/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#ifndef CALLGRAPH_H_
#define CALLGRAPH_H_ 1

#include "literal.h"

#include <stddef.h>

struct callgraph {
    literal function;
    literal fn_file;
    int fn_line;
    int fn_col;
    struct hashtable strtab;

    struct invokation {
        literal caller;
        literal callee;
        float weight;
        int line, col;
    } *calls;
    size_t calls_caps;
    size_t calls_size;

    literal *defs;
    size_t defs_caps;
    size_t defs_size;
};

void free_callgraph(struct callgraph *cg);
struct callgraph *parse_directory(const char *path);

void dump_dot(struct callgraph *cg, const char *destpath);
void filter_graph(struct callgraph *cg);
void renew_graph(struct callgraph *cg);


int cmp_def_by_file(const void *, const void *);
int cmp_def_by_addr(const void *, const void *);
int cmp_call_by_callee(const void *, const void *);
int cmp_call_by_caller(const void *, const void *);

#endif

