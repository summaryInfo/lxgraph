/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "util.h"
#include "callgraph.h"

#include <stdio.h>

void clear_marks(struct callgraph *cg) {
    ht_iter_t it = ht_begin(&cg->functions);
    for (ht_head_t *cur; (cur = ht_next(&it)); ) {
        struct function *function = container_of(cur, struct function, head);
        function->mark = 0;
    }
}

static void exclude_exceptions(struct callgraph *cg) {
    for (size_t i = 0; i < config.exclude_files.size; i++) {
        struct file *file = find_file(cg, config.exclude_files.data[i]);
        if (!file) continue;

        debug("Excluding file '%s'", file ? file->name : "<NULL>");
        erase_file(cg, file);
    }

    for (size_t i = 0; i < config.exclude_functions.size; i++) {
        struct function *function = find_function(cg, config.exclude_functions.data[i]);
        if (!function) continue;

        debug("Excluding function '%s'", function ? function->name : "<NULL>");
        erase_function(cg, function);
    }
}

static void dfs(struct function *root) {
    if (root->mark) return;
    root->mark = 1;
    list_iter_t it = list_begin(&root->calls);
    for (list_head_t *cur; (cur = list_next(&it)); ) {
        struct call *edge = container_of(cur, struct call, calls);
        dfs(edge->callee);
    }
}

static void remove_unused(struct callgraph *cg) {
    /* Mark every function starting from roots */

    for (size_t i = 0; i < config.root_files.size; i++) {
        struct file *file = find_file(cg, config.root_files.data[i]);
        if (file) {
            list_iter_t it = list_begin(&file->functions);
            for (list_head_t *cur; (cur = list_next(&it)); ) {
                struct function *fun = container_of(cur, struct function, in_file);
                debug("Makring root '%s'", fun->name);
                dfs(fun);
            }
        }
    }

    for (size_t i = 0; i < config.root_functions.size; i++) {
        struct function *fun = find_function(cg, config.root_functions.data[i]);
        if (fun) {
            debug("Makring root '%s'", fun->name);
            dfs(fun);
        }
    }

    debug("Removing unreachable functions...");

    ht_iter_t it = ht_begin(&cg->functions);
    for (ht_head_t *cur; (cur = ht_current(&it)); ) {
        struct function *fun = container_of(cur, struct function, head);
        if (!fun->mark) {
            ht_erase_current(&it);
            erase_function(cg, fun);
        } else {
            ht_next(&it);
        }
    }
}

static int cmp_call(const void *a, const void *b) {
    struct call *call_a = (*(struct call **)a);
    struct call *call_b = (*(struct call **)b);

    if (call_a->callee < call_b->callee) return -1;
    if (call_a->callee > call_b->callee) return 1;

    // Just compare files by address, they are unique
    if (call_a->caller->file < call_b->caller->file) return -1;
    if (call_a->caller->file > call_b->caller->file) return 1;

    if (call_a->line < call_b->line) return -1;
    if (call_a->line > call_b->line) return 1;

    if (call_a->column < call_b->column) return -1;
    if (call_a->column > call_b->column) return 1;

    return 0;
}

static void collapse_duplicates(struct callgraph *cg) {
    struct call **buffer = NULL;
    size_t bufsize = 0, bufcaps = 0;

    debug("Collapsing duplicate edges...");
    ht_iter_t it = ht_begin(&cg->functions);
    for (ht_head_t *cur; (cur = ht_next(&it)); ) {
        struct function *fun = container_of(cur, struct function, head);
        bufsize = 0;

        /* Add all edges to temporary buffer */
        list_iter_t edge = list_begin(&fun->calls);
        for (list_head_t *curcall; (curcall = list_next(&edge)); ) {
            struct call *call = container_of(curcall, struct call, calls);
            bool res = adjust_buffer((void **)&buffer, &bufcaps, bufsize + 1, sizeof *buffer);
            assert(res);
            buffer[bufsize++] = call;
        }

        if (!bufsize) continue;

        /* Sort them by source location: callee-file-function-line-column */
        qsort(buffer, bufsize, sizeof *buffer, cmp_call);

        /* And remove duplicates linearally */
        for (size_t i = 1; i < bufsize; i++) {
            if (!cmp_call(&buffer[i - 1], &buffer[i]))
                erase_call(buffer[i - 1]);
            else if (buffer[i -1]->callee == buffer[i]->callee) {
                buffer[i]->weight += buffer[i - 1]->weight;
                erase_call(buffer[i - 1]);
            }
        }
    }
    free(buffer);
}

void filter_graph(struct callgraph *cg) {
    clear_marks(cg);
    exclude_exceptions(cg);
    collapse_duplicates(cg);
    remove_unused(cg);
}
