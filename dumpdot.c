/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "util.h"
#include "callgraph.h"

#include <stdio.h>

void dump_dot(struct callgraph *cg, const char *destpath) {
    FILE *dst = destpath ? fopen(destpath, "w") : stdout;
    if (!dst) {
        warn("Cannot open output file '%s'", destpath);
        return;
    }

    // TODO Clustering
    fputs("digraph \"callgraph\" {\n", dst);
    for (size_t i = 0; i < cg->defs_size; i++) {
        fprintf(dst, "\t%p[label=\"%s\"];\n", (void *)cg->defs[i].name, strtab_get(cg, cg->defs[i].name));
    }
    for (size_t i = 0; i < cg->calls_size; i++) {
        fprintf(dst, "\t%p -> %p;\n", (void *)cg->calls[i].caller, (void *)cg->calls[i].callee);
    }
    fputs("}\n", dst);

    if (dst != stdout) fclose(dst);
}
