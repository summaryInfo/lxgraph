/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#ifndef CALLGRAPH_H_
#define CALLGRAPH_H_ 1

#include "hashtable.h"
#include "list.h"
#include "util.h"

#include <stddef.h>


struct file {
    ht_head_t head;
    list_head_t functions;
    const char *name;
};

struct function {
    ht_head_t head;
    struct file *file;
    list_head_t in_file;
    list_head_t called;
    list_head_t calls;
    int line;
    int16_t column;
    bool is_definition : 1;
    bool is_extern : 1;
    bool is_inline : 1;
    bool mark : 1;
    const char *name;
};

struct call {
    struct function *caller;
    struct function *callee;
    list_head_t called;
    list_head_t calls;
    int line;
    int column;
    float weight;
};

struct callgraph {
    struct hashtable functions;
    struct hashtable files;
};

inline static void erase_call(struct call *call) {
    list_erase(&call->called);
    list_erase(&call->calls);
    free(call);
}

inline static void erase_function(struct callgraph *cg, struct function *fun) {
    list_iter_t it = list_begin(&fun->calls);
    for (list_head_t *cur; (cur = list_next(&it)); )
        erase_call(container_of(cur, struct call, calls));
    it = list_begin(&fun->called);
    for (list_head_t *cur; (cur = list_next(&it)); )
        erase_call(container_of(cur, struct call, called));
    list_erase(&fun->in_file);
    ht_erase(&cg->functions, &fun->head);
    free(fun);
}

inline static void erase_file(struct callgraph *cg, struct file *file) {
    list_iter_t it = list_begin(&file->functions);
    for (list_head_t *cur; (cur = list_next(&it)); )
        erase_function(cg, container_of(cur, struct function, in_file));
    ht_erase(&cg->files, &file->head);
    free(file);
}

inline static struct file *find_file(struct callgraph *cg, const char *file) {
    size_t len = strlen(file);
    struct file dummy = { .head.hash = hash64(file, len), .name = file };
    return container_of(ht_find(&cg->files, &dummy.head), struct file, head);
}

inline static  struct function *find_function(struct callgraph *cg, const char *function) {
    size_t len = strlen(function);
    struct function dummy = { .head.hash = hash64(function, len), .name = function };
    return container_of(ht_find(&cg->functions, &dummy.head), struct function, head);
}

void free_callgraph(struct callgraph *cg);
struct callgraph *parse_directory(const char *path);

void dump_dot(struct callgraph *cg, const char *destpath);
void filter_graph(struct callgraph *cg);
void clear_marks(struct callgraph *cg);
#endif

