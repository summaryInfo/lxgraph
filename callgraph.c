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

struct strtab_item {
    ht_head_t head;
    const char *name;
};

static bool strtab_item_cmp(const ht_head_t *a, const ht_head_t *b) {
    const struct strtab_item *ai = (const struct strtab_item *)a;
    const struct strtab_item *bi = (const struct strtab_item *)b;
    return !strcmp(ai->name, bi->name);
}

const char *strtab_get(struct callgraph *cg, literal lit) {
    (void)cg;
    return lit->name;
}

literal strtab_put(struct callgraph *cg, const char *str) {
    size_t len = strlen(str);
    struct strtab_item key = { .head = { .hash = hash64(str, len) }, .name = str };
    ht_head_t **h = ht_lookup_ptr(&cg->strtab, (ht_head_t *)&key);
    if (*h) return (literal)*h;

    struct strtab_item *new = malloc(sizeof *new + len + 1);
    assert(new);
    new->head = key.head;
    new->name = (char *)(new + 1);
    strcpy((char *)new->name, str);

    ht_insert_hint(&cg->strtab, h, (ht_head_t *)new);
    return (literal)new;
}

inline static void set_current(struct callgraph *cg, const char *fun, const char *file) {
    assert(!cg->function || !fun); // No nested functions allowed
    cg->function = fun ? strtab_put(cg, fun) : NULL;
    cg->file = strtab_put(cg, file);
}

inline static void add_function_declaration(struct callgraph *cg, literal id) {
    if (adjust_buffer((void **)&cg->defs, &cg->defs_caps, cg->defs_size + 1, sizeof *cg->defs)) {
        // TODO Store source location
        cg->defs[cg->defs_size++] = (struct definition) { .name = id };
    }
}

inline static void add_function_call(struct callgraph *cg, const char *str) {
    literal id = strtab_put(cg, str), fn = cg->function;
    if (adjust_buffer((void **)&cg->calls, &cg->calls_caps, cg->calls_size + 1, sizeof *cg->calls)) {
        // TODO Store source location
        if (!fn) fn = strtab_put(cg, "<static expr>");
        cg->calls[cg->calls_size++] = (struct invokation) { fn, id };
    }
}

inline static void mark_as_definition(struct callgraph *cg, literal func) {
    struct definition *def = &cg->defs[cg->defs_size - 1];
    assert(def->name == func);
    assert(!def->file || def->file == cg->file);
    def->file = cg->file;
}

static enum CXChildVisitResult visit(CXCursor cur, CXCursor parent, CXClientData data) {
    struct callgraph *cg = (struct callgraph *)data;
    (void)parent;

    switch (clang_getCursorKind(cur)) {
    case CXCursor_CompoundStmt:
        if (cg->function)
            mark_as_definition(cg, cg->function);
        break;
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_FunctionTemplate:;
        CXString name = clang_getCursorDisplayName(cur);
        CXString tu = clang_getTranslationUnitSpelling(clang_Cursor_getTranslationUnit(cur));
        set_current(cg, clang_getCString(name), clang_getCString(name));
        add_function_declaration(cg, cg->function);
        clang_visitChildren(cur, visit, data);
        set_current(cg, NULL, clang_getCString(tu));
        clang_disposeString(name);
        clang_disposeString(tu);
        break;
    case CXCursor_DeclRefExpr:
    case CXCursor_MemberRefExpr:;
        CXCursor decl = clang_getCursorReferenced(cur);
        enum CXCursorKind kind = clang_getCursorKind(decl);
        if (kind == CXCursor_FunctionDecl ||
                kind == CXCursor_CXXMethod ||
                kind == CXCursor_FunctionTemplate) {
            CXString fname = clang_getCursorDisplayName(decl);
            add_function_call(cg, clang_getCString(fname));
            clang_disposeString(fname);
        }
        /* fallthrough */
    default:
        break;
    }
    return CXChildVisit_Recurse;
}

void free_callgraph(struct callgraph *cg) {
    ht_iter_t it = ht_begin(&cg->strtab);
    while (ht_current(&it))
        free(ht_erase_current(&it));
    ht_free(&cg->strtab);
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

static void do_parse(void *varg) {
    struct arg *arg = varg;
    struct callgraph *cg = calloc(1, sizeof *cg);
    ht_init(&cg->strtab, HT_INIT_CAPS, strtab_item_cmp);
    assert(cg->strtab.data);
    *arg->pres = cg;

    CXIndex index = clang_createIndex(1, config.log_level > 1);

    for (size_t i = arg->offset; i < arg->offset + arg->size; i++) {
        CXCompileCommand cmd = clang_CompileCommands_getCommand(arg->cmds, i);
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

static int cmp_def(const void *a, const void *b) {
    const struct definition *da = (const struct definition *)a;
    const struct definition *db = (const struct definition *)b;
    if (da->file < db->file) return -1;
    if (da->file > db->file) return 1;
    if (da->name < db->name) return -1;
    if (da->name > db->name) return 1;
    return 0;
}

static void merge_move_callgraph(struct callgraph *dst, struct callgraph *src) {
    /* Move string table */
    ht_iter_t it = ht_begin(&src->strtab);
    while (ht_current(&it)) {
        literal cur = (literal)ht_erase_current(&it);
        literal old = (literal)ht_insert(&dst->strtab, (ht_head_t *)cur);
        if (old) /* Need to replace references to the duplicate string */ {
            for (size_t i = 0; i < src->calls_size; i++) {
                if (src->calls[i].callee == cur)
                    src->calls[i].callee = old;
                if (src->calls[i].caller == cur)
                    src->calls[i].caller = old;
            }
            for (size_t i = 0; i < src->defs_size; i++) {
                if (src->defs[i].name == cur)
                    src->defs[i].name = old;
                if (src->defs[i].file == cur)
                    src->defs[i].file = old;
            }
            free(cur);
        }
    }

    /* Merge calls vector (just append it) */

    bool res = adjust_buffer((void **)&dst->calls, &dst->calls_caps,
                             dst->calls_size + src->calls_size, sizeof *dst->calls);
    assert(res);
    memcpy(dst->calls + dst->calls_size, src->calls, src->calls_size * sizeof *dst->calls);
    dst->calls_size += src->calls_size;
    free(src->calls);
    src->calls = NULL;
    src->calls_caps = 0;
    src->calls_caps = 0;

    /* Merge definitions vector (append, then deduplicate) */

    if (!dst->defs_size) {
        dst->defs = src->defs;
        dst->defs_size = src->defs_size;
        dst->defs_caps = src->defs_caps;
    } else {
        /* O(n) copy */
        res = adjust_buffer((void **)&dst->defs, &dst->defs_caps,
                                 dst->defs_size + src->defs_size, sizeof *dst->defs);
        assert(res);
        memcpy(dst->defs + dst->defs_size, src->defs, src->defs_size * sizeof *dst->defs);
        dst->defs_size += src->defs_size;
        free(src->defs);

        /* O(nlogn) sort */
        qsort(dst->defs, dst->defs_size, sizeof *dst->defs, cmp_def);

        /* O(n) deduplication */
        struct definition *ddef = dst->defs, *sdef, *enddef = dst->defs + dst->defs_size;
        for (sdef = ddef + 1; sdef < enddef; sdef++) {
            if (ddef->name == sdef->name) {
                /* all functions are globally unique (I might be wrong here...) */
                assert(!ddef->file || !sdef->file || ddef->file == sdef->file);
                if (!ddef->file) ddef->file = sdef->file;
            } else {
                *++ddef = *sdef;
            }
        }
        dst->defs_size = ddef - dst->defs + 1;
    }
    src->defs_caps = 0;
    src->defs_size = 0;
    src->defs = NULL;
}

struct callgraph *parse_directory(const char *path) {
    struct callgraph *cgparts[nproc];

    CXCompilationDatabase_Error err = CXCompilationDatabase_NoError;
    CXCompilationDatabase cdb = clang_CompilationDatabase_fromDirectory(path, &err);
    if (err) {
        warn("Cannot parse compilation database for '%s'", path);
        return NULL;
    }

    char buf[PATH_MAX + 1];
    char *res = getcwd(buf, sizeof buf -1);
    CXCompileCommands ccmds = clang_CompilationDatabase_getAllCompileCommands(cdb);

    /* Parsing should be performed
     * in project directory so chdir() temporaly */
    chdir(path);

    ssize_t ncmds = clang_CompileCommands_getSize(ccmds);
    ssize_t cmds_per_worker = ncmds/nproc, tail = ncmds%nproc;
    for (ssize_t i = 0; i < nproc; i++) {
        struct arg arg = {
            .pres = cgparts + i,
            .cmds = ccmds,
            .offset = cmds_per_worker*i + MIN(tail, i),
            .size = cmds_per_worker + (i < tail)
        };
        submit_work(do_parse, &arg, sizeof arg);
    }
    drain_work();

    for (ssize_t i = 1; i < nproc; i++) {
        merge_move_callgraph(cgparts[0], cgparts[i]);
        free_callgraph(cgparts[i]);
    }

    clang_CompileCommands_dispose(ccmds);
    clang_CompilationDatabase_dispose(cdb);
    if (res) chdir(buf);
    return cgparts[0];
}
