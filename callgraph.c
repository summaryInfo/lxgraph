/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "util.h"
#include "hashtable.h"
#include "callgraph.h"
#include "worker.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <clang-c/Index.h>
#include <clang-c/CXCompilationDatabase.h>

#define BATCH_SIZE 16

inline static void set_current(struct callgraph *cg, const char *fun, const char *file, int line, int col) {
    assert(!cg->function || !fun); // No nested functions allowed
    if (!strncmp(file, "./", 2)) file += 2;
    cg->function = fun ? strtab_put2(&cg->strtab, fun, lf_function) : NULL;
    cg->fn_file = strtab_put2(&cg->strtab, file, lf_file);
    cg->fn_line = line;
    cg->fn_col = col;
}

inline static void add_function_declaration(struct callgraph *cg, literal id, bool global, bool inline_) {
    if (adjust_buffer((void **)&cg->defs, &cg->defs_caps, cg->defs_size + 1, sizeof *cg->defs)) {
        cg->defs[cg->defs_size++] = id;
        literal_set_flags(id, lf_function | (global ? lf_global : 0) | (inline_ ? lf_inline : 0));
    }
}

inline static void add_function_call(struct callgraph *cg, const char *str, int line, int col) {
    literal id = strtab_put(&cg->strtab, str), fn = cg->function;
    if (adjust_buffer((void **)&cg->calls, &cg->calls_caps, cg->calls_size + 1, sizeof *cg->calls)) {
        if (!fn) fn = strtab_put2(&cg->strtab, "<static expr>", lf_function);
        cg->calls[cg->calls_size++] = (struct invokation) { fn, id, 1.f, line, col };
    }
}

inline static void mark_as_definition(struct callgraph *cg, literal func) {
    literal def = cg->defs[cg->defs_size - 1];
    assert(def == func);
    literal_set_flags(func, literal_get_flags(func) | lf_defined);
    literal_set_location(func, cg->fn_file, cg->fn_line, cg->fn_col);
}

static enum CXChildVisitResult visit(CXCursor cur, CXCursor parent, CXClientData data) {
    struct callgraph *cg = (struct callgraph *)data;
    (void)parent;

    unsigned line, col;
    switch (clang_getCursorKind(cur)) {
    case CXCursor_CompoundStmt:
        if (cg->function)
            mark_as_definition(cg, cg->function);
        break;
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_FunctionTemplate:;
        if (cg->function) break;
        bool is_global =  clang_Cursor_getStorageClass(cur) != CX_SC_Extern;
        bool is_inline = clang_Cursor_isFunctionInlined(cur);
        CXString name = clang_getCursorDisplayName(cur);
        CXFile file;
        clang_getExpansionLocation(clang_getCursorLocation(cur), &file, &line, &col, NULL);
        CXString filename = clang_getFileName(file);
        set_current(cg, clang_getCString(name), clang_getCString(filename), line, col);
        add_function_declaration(cg, cg->function, is_global, is_inline);
        clang_visitChildren(cur, visit, data);
        set_current(cg, NULL, clang_getCString(filename), 0, 0);
        clang_disposeString(name);
        clang_disposeString(filename);
        return CXChildVisit_Continue;
    case CXCursor_DeclRefExpr:
    case CXCursor_MemberRefExpr:;
        CXCursor decl = clang_getCursorReferenced(cur);
        enum CXCursorKind kind = clang_getCursorKind(decl);
        if (kind == CXCursor_FunctionDecl ||
                kind == CXCursor_CXXMethod ||
                kind == CXCursor_FunctionTemplate) {
            CXString fname = clang_getCursorDisplayName(decl);
            clang_getExpansionLocation(clang_getCursorLocation(cur), NULL, &line, &col, NULL);
            add_function_call(cg, clang_getCString(fname), line, col);
            clang_disposeString(fname);
        }
        /* fallthrough */
    default:
        break;
    }
    return CXChildVisit_Recurse;
}

void free_callgraph(struct callgraph *cg) {
    fini_strtab(&cg->strtab);
    free(cg->defs);
    free(cg->calls);
    free(cg);
}

struct arg {
    struct callgraph **pres;
    CXCompileCommands cmds;
    size_t offset;
    size_t size;
};

static void do_parse(int thread_index, void *varg) {
    struct arg *arg = varg;
    struct callgraph *cg = arg->pres[thread_index];

    CXIndex index = clang_createIndex(1, config.log_level > 1);


    for (size_t i = arg->offset; i < arg->offset + arg->size; i++) {
        CXCompileCommand cmd = clang_CompileCommands_getCommand(arg->cmds, i);
        CXString dir = clang_CompileCommand_getDirectory(cmd);
        chdir(clang_getCString(dir));
        clang_disposeString(dir);
        if (config.log_level > 3) {
            CXString file = clang_CompileCommand_getFilename(cmd);
            syncdebug("Parsing file '%s'", clang_getCString(file));
            clang_disposeString(file);
        }
        size_t nargs = clang_CompileCommand_getNumArgs(cmd);
        CXString args[nargs];
        const char *cargs[nargs];
        for (size_t j = 0; j < nargs; j++) {
            args[j] = clang_CompileCommand_getArg(cmd, j);
            cargs[j] = clang_getCString(args[j]);
        }
        CXTranslationUnit unit = clang_parseTranslationUnit(index, NULL, cargs, nargs, NULL, 0, CXTranslationUnit_None);
        for (size_t j = 0; j < nargs; j++)
            clang_disposeString(args[j]);
        if (!unit) {
            CXString file = clang_CompileCommand_getFilename(cmd);
            warn("Cannot parse file '%s'", clang_getCString(file));
            clang_disposeString(file);
            continue;
        }

        CXCursor cur = clang_getTranslationUnitCursor(unit);
        clang_visitChildren(cur, visit, (CXClientData)cg);
        clang_disposeTranslationUnit(unit);
    }

    clang_disposeIndex(index);
}

int cmp_def_by_file(const void *a, const void *b) {
    literal da = *(literal *)a, db = *(literal *)b;
    literal fa = literal_get_file(da), fb = literal_get_file(db);
    // May be I should sort by actual string to make locations persistent?
    if (fa < fb) return -1;
    else if (fa > fb) return 1;
    if (da < db) return -1;
    return (da > db);
}

int cmp_def_by_addr(const void *a, const void *b) {
    literal da = *(literal *)a, db = *(literal *)b;
    literal fa = literal_get_file(da), fb = literal_get_file(db);
    if (da < db) return -1;
    else if (da > db) return 1;
    if (fa < fb) return -1;
    return (fa > fb);
}

int cmp_call_by_caller(const void *a, const void *b) {
    const struct invokation *ia = (const struct invokation *)a;
    const struct invokation *ib = (const struct invokation *)b;
    if (ia->caller < ib->caller) return -1;
    if (ia->caller > ib->caller) return 1;
    if (ia->callee < ib->callee) return -1;
    return (ia->callee > ib->callee);
}

int cmp_call_by_callee(const void *a, const void *b) {
    const struct invokation *ia = (const struct invokation *)a;
    const struct invokation *ib = (const struct invokation *)b;
    if (ia->callee < ib->callee) return -1;
    if (ia->callee > ib->callee) return 1;
    if (ia->caller < ib->caller) return -1;
    return (ia->caller > ib->caller);
}

static void merge_move_callgraph(struct callgraph *dst, struct callgraph *src) {
    /* Move string table */

    strtab_merge(&dst->strtab, &src->strtab);
    size_t size = src->strtab.size;
    literal *tmp = malloc(size*sizeof *tmp);
    ht_iter_t it = ht_begin(&src->strtab);
    for (literal *ptr = tmp; ht_current(&it); ptr++)
        *ptr = (literal)ht_erase_current(&it);
    qsort(tmp, size, sizeof *tmp, cmp_def_by_addr);

    if (src->calls && size) {
        qsort(src->calls, src->calls_size, sizeof *src->calls, cmp_call_by_callee);
        struct invokation *it1 = src->calls, *it1_end = src->calls + src->calls_size;
        for (literal *it2 = tmp; it1 < it1_end; it1++) {
            while (it2 < tmp + size && *it2 < it1->callee) it2++;
            if (it2 >= tmp + size) break;
            if (*it2 == it1->callee) it1->callee = literal_get_file(*it2);
        }

        qsort(src->calls, src->calls_size, sizeof *src->calls, cmp_call_by_caller);
        it1 = src->calls, it1_end = src->calls + src->calls_size;
        for (literal *it2 = tmp; it1 < it1_end; it1++) {
            while (it2 < tmp + size && *it2 < it1->caller) it2++;
            if (it2 >= tmp + size) break;
            if (*it2 == it1->caller) it1->caller = literal_get_file(*it2);
        }
    }

    if (src->defs && size) {
        qsort(src->defs, src->defs_size, sizeof *src->defs, cmp_def_by_addr);
        literal *it3 = src->defs, *it3_end = src->defs + src->defs_size;
        for (literal *it2 = tmp; it3 < it3_end; it3++) {
            while (it2 < tmp + size && *it2 < *it3) it2++;
            if (it2 >= tmp + size) break;
            if (*it2 == *it3) *it3 = literal_get_file(*it2);
        }
    }

    for (size_t i = 0; i < size; i++)
        free(tmp[i]);
    free(tmp);

    dst->fn_file = NULL;
    dst->function = NULL;
    src->fn_file = NULL;
    src->function = NULL;

    /* Merge calls vector (just append it) */
    if (!dst->calls_size) {
        dst->calls = src->calls;
        dst->calls_size = src->calls_size;
        dst->calls_caps = src->calls_caps;
    } else if (src->calls_size) {
        bool res = adjust_buffer((void **)&dst->calls, &dst->calls_caps,
                                 dst->calls_size + src->calls_size, sizeof *dst->calls);
        assert(res);
        memcpy(dst->calls + dst->calls_size, src->calls, src->calls_size * sizeof *dst->calls);
        dst->calls_size += src->calls_size;
        free(src->calls);
    }
    src->calls_caps = 0;
    src->calls_size = 0;
    src->calls = NULL;

    /* Merge definitions vector (append, then deduplicate) */

    if (!dst->defs_size) {
        dst->defs = src->defs;
        dst->defs_size = src->defs_size;
        dst->defs_caps = src->defs_caps;
    } else {
        if (src->defs_size) {
            bool res = adjust_buffer((void **)&dst->defs, &dst->defs_caps,
                                     dst->defs_size + src->defs_size, sizeof *dst->defs);
            assert(res);
            memcpy(dst->defs + dst->defs_size, src->defs, src->defs_size * sizeof *dst->defs);
            dst->defs_size += src->defs_size;
            free(src->defs);
        }

        /* O(nlogn) sort */
        qsort(dst->defs, dst->defs_size, sizeof *dst->defs, cmp_def_by_file);

        /* O(n) deduplication */
        literal *ddef = dst->defs, *sdef, *enddef = dst->defs + dst->defs_size;
        for (sdef = ddef + 1; sdef < enddef; sdef++) {
            if (!literal_eq(*ddef, *sdef)) *++ddef = *sdef;
            else if (!literal_get_file(*ddef) && literal_get_file(*sdef))
                literal_set_location(*ddef, literal_get_file(*sdef), literal_get_line(*sdef), literal_get_column(*sdef));
        }

        dst->defs_size = ddef - dst->defs + 1;
    }

    src->defs_caps = 0;
    src->defs_size = 0;
    src->defs = NULL;
}

struct merge_arg {
    struct callgraph *dst;
    struct callgraph *src;
};

static void do_merge_parallel(int thread_index, void *varg) {
    struct merge_arg *arg = varg;
    (void)thread_index;
    merge_move_callgraph(arg->dst, arg->src);
    free_callgraph(arg->src);
}

struct callgraph *parse_directory(const char *path) {
    struct callgraph *cgparts[nproc];
    for (ssize_t i = 0; i < nproc; i++) {
        cgparts[i] = calloc(1, sizeof *cgparts[0]);
        init_strtab(&cgparts[i]->strtab);
    }

    CXCompilationDatabase_Error err = CXCompilationDatabase_NoError;
    CXCompilationDatabase cdb = clang_CompilationDatabase_fromDirectory(path, &err);
    if (err) {
        warn("Cannot parse compilation database for '%s'", path);
        return NULL;
    }

    char buf[PATH_MAX + 1];
    char *res = getcwd(buf, sizeof buf -1);
    CXCompileCommands ccmds = clang_CompilationDatabase_getAllCompileCommands(cdb);

    ssize_t ncmds = clang_CompileCommands_getSize(ccmds);
    for (ssize_t offset = 0; ncmds > offset; offset += BATCH_SIZE) {
        struct arg arg = {
            .pres = cgparts,
            .cmds = ccmds,
            .offset = offset,
            .size = MIN(BATCH_SIZE, ncmds - offset)
        };
        offset += BATCH_SIZE;
        submit_work(do_parse, &arg, sizeof arg);
    }
    drain_work();

    if (nproc == 1) {
        qsort(cgparts[0]->defs, cgparts[0]->defs_size,
              sizeof *cgparts[0]->defs, cmp_def_by_file);
    } else {
        size_t n = nproc;
        while (n > 1) {
            size_t cnt = n/2, off = (n+1)/2;
            do {
                debug("Merging %zd into %zd", cnt + off - 1, cnt - 1);
                struct merge_arg arg = { cgparts[cnt - 1], cgparts[cnt + off - 1] };
                submit_work(do_merge_parallel, &arg, sizeof arg);
            } while (--cnt > 0);
            drain_work();
            n = off;
        }
    }

    clang_CompileCommands_dispose(ccmds);
    clang_CompilationDatabase_dispose(cdb);
    if (res) chdir(buf);
    return cgparts[0];
}
