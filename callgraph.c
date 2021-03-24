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

struct parse_context {
    struct callgraph *callgraph;
    struct function *current;
};

static struct file *add_file(struct callgraph *cg, const char *file) {
    if (!strncmp(file, "./", 2)) file += 2;

    size_t len = strlen(file);
    struct file dummy = { .head.hash = hash64(file, len), .name = file };
    ht_head_t **h = ht_lookup_ptr(&cg->files, &dummy.head);
    if (*h) return container_of(*h, struct file, head);

    struct file *new = calloc(1, sizeof *new + len + 1);
    memcpy(new + 1, file, len + 1);
    new->name = (char *)(new + 1);
    new->head = dummy.head;
    list_init(&new->functions);
    list_init(&new->calls);
    list_init(&new->called);

    ht_insert_hint(&cg->files, h, &new->head);
    return new;
}

static struct function *add_function_ref(struct callgraph *cg, const char *function) {
    size_t len = strlen(function);
    struct function dummy = { .head.hash = hash64(function, len), .name = function };
    ht_head_t **h = ht_lookup_ptr(&cg->functions, &dummy.head);
    if (*h) return container_of(*h, struct function, head);

    struct function *new = calloc(1, sizeof *new + len + 1);
    memcpy(new + 1, function, len + 1);
    new->name = (char *)(new + 1);
    new->head = dummy.head;
    list_init(&new->calls);
    list_init(&new->called);
    list_init(&new->in_file);

    ht_insert_hint(&cg->functions, h, &new->head);
    return new;
}

static struct function *add_function(struct callgraph *cg, struct file *file, const char *function,
                                     int line, int col, bool is_extern, bool is_inline) {
    size_t len = strlen(function);
    struct function dummy = { .head.hash = hash64(function, len), .name = function };
    ht_head_t **h = ht_lookup_ptr(&cg->functions, &dummy.head);
    if (*h) {
        struct function *fn = container_of(*h, struct function, head);
        if (!fn->file) {
            list_append(&file->functions, &fn->in_file);
            fn->file = file;
            fn->line = line;
            fn->column = col;
            fn->is_extern = is_extern;
            fn->is_inline = is_inline;
        }
        return fn;
    }

    struct function *new = calloc(1, sizeof *new + len + 1);
    memcpy(new + 1, function, len + 1);
    new->name = (char *)(new + 1);
    new->head = dummy.head;
    list_init(&new->calls);
    list_init(&new->called);
    list_append(&file->functions, &new->in_file);
    new->file = file;
    new->line = line;
    new->column = col;
    new->is_extern = is_extern;
    new->is_inline = is_inline;

    ht_insert_hint(&cg->functions, h, &new->head);

    return new;
}

void add_function_call(struct function *from, struct function *to, int line, int col) {
    struct call *new = malloc(sizeof *new);
    list_append(&from->calls, &new->calls);
    list_append(&to->called, &new->called);
    new->caller = from;
    new->callee = to;
    new->weight = 1.;
    new->line = line;
    new->column = col;
}

static enum CXChildVisitResult visit(CXCursor cur, CXCursor parent, CXClientData data) {
    struct parse_context *context = (struct parse_context *)data;
    (void)parent;

    unsigned line, col;
    switch (clang_getCursorKind(cur)) {
    case CXCursor_CompoundStmt:
        if (context->current)
            context->current->is_definition = 1;
        break;
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_FunctionTemplate:;
        bool is_extern =  clang_Cursor_getStorageClass(cur) != CX_SC_Extern;
        bool is_inline = clang_Cursor_isFunctionInlined(cur);
        CXString name = clang_getCursorDisplayName(cur);
        CXFile cfile;
        clang_getExpansionLocation(clang_getCursorLocation(cur), &cfile, &line, &col, NULL);
        CXString filename = clang_getFileName(cfile);
        struct file *file = add_file(context->callgraph, clang_getCString(filename));
        struct function *newfn = add_function(context->callgraph, file,
                clang_getCString(name), line, col, is_extern, is_inline);
        clang_disposeString(name);
        clang_disposeString(filename);
        if (!context->current) {
            context->current = newfn;
            clang_visitChildren(cur, visit, data);
            context->current = NULL;
            return CXChildVisit_Continue;
        }
        break;
    case CXCursor_DeclRefExpr:
    case CXCursor_MemberRefExpr:;
        CXCursor decl = clang_getCursorReferenced(cur);
        enum CXCursorKind kind = clang_getCursorKind(decl);
        if (kind == CXCursor_FunctionDecl ||
                kind == CXCursor_CXXMethod ||
                kind == CXCursor_FunctionTemplate) {
            CXString fname = clang_getCursorDisplayName(decl);
            clang_getExpansionLocation(clang_getCursorLocation(cur), NULL, &line, &col, NULL);
            struct function *fn = add_function_ref(context->callgraph, clang_getCString(fname));
            clang_disposeString(fname);
            if (!context->current) {
                // TODO Static initiallization...
                break;
            }
            assert(context->current);
            add_function_call(context->current, fn, line, col);
        }
        /* fallthrough */
    default:
        break;
    }
    return CXChildVisit_Recurse;
}

void free_callgraph(struct callgraph *cg) {
    ht_iter_t itfile = ht_begin(&cg->files);
    for (ht_head_t *cur; (cur = ht_erase_current(&itfile));)
        erase_file(cg, container_of(cur, struct file, head));
    ht_free(&cg->files);

    ht_iter_t itfun = ht_begin(&cg->functions);
    for (ht_head_t *cur; (cur = ht_erase_current(&itfun));)
        erase_function(cg, container_of(cur, struct function, head));
    ht_free(&cg->functions);
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
    struct parse_context context = { arg->pres[thread_index], NULL };

    CXIndex index = clang_createIndex(1, config.log_level > 1);


    for (size_t i = arg->offset; i < arg->offset + arg->size; i++) {
        CXCompileCommand cmd = clang_CompileCommands_getCommand(arg->cmds, i);
        CXString dir = clang_CompileCommand_getDirectory(cmd);
        chdir(clang_getCString(dir));
        clang_disposeString(dir);
        if (config.log_level > 3) {
            CXString file = clang_CompileCommand_getFilename(cmd);
            syncdebug("Parsing file %zd '%s'", i, clang_getCString(file));
            clang_disposeString(file);
        }
        // TODO Additional args for compilation?
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
        clang_visitChildren(cur, visit, (CXClientData)&context);
        clang_disposeTranslationUnit(unit);
    }

    clang_disposeIndex(index);
}

static void merge_move_callgraph(struct callgraph *dst, struct callgraph *src) {
    /* Just add all information from one source
     * to another, it takes linear time */
    ht_iter_t itfun = ht_begin(&src->functions);
    for (ht_head_t *cur; (cur = ht_next(&itfun)); ) {
        struct function *sfun = container_of(cur, struct function, head);
        struct function *dfun = add_function_ref(dst, sfun->name);
        /* Collect missing information to dst */
        if (!dfun->file && sfun->file) {
            dfun->file = add_file(dst, sfun->file->name);
            list_append(&dfun->file->functions, &dfun->in_file);
            dfun->line = sfun->line;
            dfun->column = sfun->column;
            dfun->is_definition = sfun->is_definition;
            dfun->is_inline = sfun->is_inline;
            dfun->is_extern = sfun->is_extern;
        }
        /* Collect missing edges to dst */
        /* We only need to iterate through calls list,
         * but not through called list, because if element
         * is in one list it will be in the other list for some function
         * NOTE: Edges for inline functions from header files
         * into multiple source files are duplicated and fileterd
         * later in filter.c, collapse_duplicates() */
        list_iter_t it = list_begin(&sfun->calls);
        for (list_head_t *curcall; (curcall = list_next(&it)); ) {
            struct call *call = container_of(curcall, struct call, calls);
            struct function *to = add_function_ref(dst, call->callee->name);
            add_function_call(dfun, to, call->line, call->column);
        }
    }
}

struct merge_arg {
    struct callgraph *dst;
    struct callgraph *src;
};

static bool eq_file(const ht_head_t *a, const ht_head_t *b) {
    const struct file *af = container_of(a, const struct file, head);
    const struct file *bf = container_of(b, const struct file, head);
    return !strcmp(af->name, bf->name);
}

static bool eq_function(const ht_head_t *a, const ht_head_t *b) {
    const struct function *af = container_of(a, const struct function, head);
    const struct function *bf = container_of(b, const struct function, head);
    return !strcmp(af->name, bf->name);
}

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
        ht_init(&cgparts[i]->functions, HT_INIT_CAPS, eq_function);
        ht_init(&cgparts[i]->files, HT_INIT_CAPS, eq_file);
    }

    CXCompilationDatabase_Error err = CXCompilationDatabase_NoError;
    CXCompilationDatabase cdb = clang_CompilationDatabase_fromDirectory(path, &err);
    if (err) {
        warn("Cannot parse compilation database for '%s'", path);
        return NULL;
    }

    char buf[PATH_MAX + 1];
    char *res = getcwd(buf, sizeof buf - 1);
    CXCompileCommands ccmds = clang_CompilationDatabase_getAllCompileCommands(cdb);

    ssize_t ncmds = clang_CompileCommands_getSize(ccmds);
    for (ssize_t offset = 0; ncmds > offset; offset += BATCH_SIZE) {
        struct arg arg = {
            .pres = cgparts,
            .cmds = ccmds,
            .offset = offset,
            .size = MIN(BATCH_SIZE, ncmds - offset)
        };
        submit_work(do_parse, &arg, sizeof arg);
    }
    drain_work();

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

    clang_CompileCommands_dispose(ccmds);
    clang_CompilationDatabase_dispose(cdb);
    if (res) chdir(buf);
    return cgparts[0];
}
