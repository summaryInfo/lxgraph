/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#ifndef CALLGRAPH_H_
#define CALLGRAPH_H_ 1

#include "literal.h"

#include <stddef.h>

struct callgraph {
    literal function;
    literal file;
    struct hashtable strtab;

    struct invokation {
        literal caller;
        literal callee;
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

#endif

