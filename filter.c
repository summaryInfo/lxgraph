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

static void reverse_dfs(struct function *root) {
    if (root->mark) return;
    root->mark = 1;
    list_iter_t it = list_begin(&root->calls);
    for (list_head_t *cur; (cur = list_next(&it)); ) {
        struct call *edge = container_of(cur, struct call, calls);
        reverse_dfs(edge->callee);
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

    for (size_t i = 0; i < config.reverse_root_files.size; i++) {
        struct file *file = find_file(cg, config.reverse_root_files.data[i]);
        if (file) {
            list_iter_t it = list_begin(&file->functions);
            for (list_head_t *cur; (cur = list_next(&it)); ) {
                struct function *fun = container_of(cur, struct function, in_file);
                debug("Makring reverse root '%s'", fun->name);
                reverse_dfs(fun);
            }
        }
    }

    for (size_t i = 0; i < config.reverse_root_functions.size; i++) {
        struct function *fun = find_function(cg, config.reverse_root_functions.data[i]);
        if (fun) {
            debug("Makring reverse root '%s'", fun->name);
            reverse_dfs(fun);
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

static int cmp_dep(const void *a, const void *b) {
    struct call *call_a = (*(struct call **)a);
    struct call *call_b = (*(struct call **)b);

    if (call_a->to_file < call_b->to_file) return -1;
    if (call_a->to_file > call_b->to_file) return 1;

    // Just compare files by address, they are unique
    if (call_a->from_file < call_b->from_file) return -1;
    if (call_a->from_file > call_b->from_file) return 1;
    return 0;
}

static void collapse_one_entry(list_iter_t edge, int (*cmp)(const void *, const void *)) {
    static struct call **buffer = NULL;
    static size_t bufsize = 0, bufcaps = 0;

    if (!cmp) {
        free(buffer);
        bufcaps = 0;
        buffer = NULL;
        return;
    }

    bufsize = 0;

    /* Add all edges to temporary buffer */
    for (list_head_t *curcall; (curcall = list_next(&edge)); ) {
        struct call *call = container_of(curcall, struct call, calls);
        bool res = adjust_buffer((void **)&buffer, &bufcaps, bufsize + 1, sizeof *buffer);
        assert(res);
        buffer[bufsize++] = call;
    }

    if (!bufsize) return;

    /* Sort them by source location: callee-file-function-line-column */
    qsort(buffer, bufsize, sizeof *buffer, cmp);

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

static void collapse_duplicates(struct callgraph *cg) {
    debug("Collapsing duplicate edges...");
    ht_iter_t it = ht_begin(&cg->functions);
    for (ht_head_t *cur; (cur = ht_next(&it)); ) {
        struct function *fun = container_of(cur, struct function, head);
        collapse_one_entry(list_begin(&fun->calls), cmp_call);
    }
    collapse_one_entry((list_iter_t){ 0 }, NULL);
}


static void collapse_file_duplicates(struct callgraph *cg) {
    debug("Collapsing duplicate edges for files...");
    ht_iter_t it = ht_begin(&cg->files);
    for (ht_head_t *cur; (cur = ht_next(&it)); ) {
        struct file *file = container_of(cur, struct file, head);
        collapse_one_entry(list_begin(&file->calls), cmp_dep);
    }
    collapse_one_entry((list_iter_t){ 0 }, NULL);
}


inline static void add_file_edge(struct file *from, struct file *to, float weight) {
    struct call *new = malloc(sizeof *new);
    list_append(&from->calls, &new->calls);
    list_append(&to->called, &new->called);
    new->from_file = from;
    new->to_file = to;
    new->weight = weight;
}

static void condence_file_graph(struct callgraph *cg) {
    debug("Collapsing function nodes...");
    ht_iter_t it = ht_begin(&cg->files);
    for (ht_head_t *cur; (cur = ht_next(&it)); ) {
        struct file *file = container_of(cur, struct file, head);
        list_iter_t itfun = list_begin(&file->functions);
        for (list_head_t *curfun; (curfun = list_next(&itfun)); ) {
            struct function *fun = container_of(curfun, struct function, in_file);
            list_iter_t itcall = list_begin(&fun->calls);
            for (list_head_t *curcall; (curcall = list_next(&itcall)); ) {
                struct call *call = container_of(curcall, struct call, calls);
                if (call->callee->file != file && call->callee->file)
                    add_file_edge(file, call->callee->file, call->weight);
            }
        }
    }
}

static void collapse_inline(struct callgraph *cg) {
    debug("Collapsing inline functions...");
    ht_iter_t it = ht_begin(&cg->functions);
    for (ht_head_t *cur; (cur = ht_current(&it)); ) {
        struct function *fun = container_of(cur, struct function, head);
        if (!fun->is_inline) {
            ht_next(&it);
            continue;
        }
        list_iter_t itfrom = list_begin(&fun->called);
        for (list_head_t *curfrom; (curfrom = list_erase_current(&itfrom)); ) {
            struct call *from = container_of(curfrom, struct call, called);
            if (from->caller != fun) {
                list_iter_t itto = list_begin(&fun->calls);
                for (list_head_t *curto; (curto = list_next(&itto)); ) {
                    struct call *to = container_of(curfrom, struct call, called);
                    if (to->callee == fun) continue;
                    add_function_call(from->caller, to->callee, from->line, from->column);
                }
            }
            erase_call(from);
        }
        ht_erase_current(&it);
        erase_function(cg, fun);
    }
}

void filter_graph(struct callgraph *cg) {
    clear_marks(cg);
    exclude_exceptions(cg);
    collapse_duplicates(cg);

    // TODO static
    if (!config.keep_inline)
        collapse_inline(cg);

    remove_unused(cg);

    if (config.level_of_details == lod_file) {
        condence_file_graph(cg);
        collapse_file_duplicates(cg);
    }
}
