/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "util.h"
#include "callgraph.h"

#include <stdio.h>

void remove_unused(struct callgraph *cg) {
    for (size_t i = 0; i < cg->defs_size; i++)
        *literal_get_pdata(cg->defs[i]) = 0;
    for (size_t i = 0; i < cg->calls_size; i++) {
        (*literal_get_pdata(cg->calls[i].callee))++;
        (*literal_get_pdata(cg->calls[i].caller))++;
    }
    literal *dst = cg->defs, *src = cg->defs;
    literal *end = src + cg->defs_size;
    for (; src < end; src++) {
        if (*literal_get_pdata(*src))
            *dst++ = *src;
    }
    cg->defs_size = dst - cg->defs;
}

static int cmp_call(const void *a, const void *b) {
    const struct invokation *ia = (const struct invokation *)a;
    const struct invokation *ib = (const struct invokation *)b;
    if (ia->caller < ib->caller) return -1;
    if (ia->caller > ib->caller) return 1;
    if (ia->callee < ib->callee) return -1;
    return (ia->callee > ib->callee);
}

void colapse_duplicates(struct callgraph *cg) {
    qsort(cg->calls, cg->calls_size, sizeof cg->calls[0], cmp_call);
    struct invokation *dst = cg->calls, *src = cg->calls + 1;
    struct invokation *end = dst + cg->calls_size;
    for (; src < end; src++) {
        // TODO Increase weight of duplicate node
        if (dst->callee != src->callee || dst->caller != src->caller)
            *++dst = *src;
    }
    cg->calls_size = dst - cg->calls + 1;
}

void dump_dot(struct callgraph *cg, const char *destpath) {
    FILE *dst = destpath ? fopen(destpath, "w") : stdout;
    if (!dst) {
        warn("Cannot open output file '%s'", destpath);
        return;
    }

    remove_unused(cg);
    colapse_duplicates(cg);

    fputs("digraph \"callgraph\" {\n", dst);
    fprintf(dst, "\tlayout = \"circo\";\n");
    literal old_file = (void *)-1;
    for (size_t i = 0; i < cg->defs_size; i++) {
        literal file = literal_get_file(cg->defs[i]);
        if (old_file != file) {
            if (old_file != (void *)-1) fputs("\t}\n", dst);
            fprintf(dst, "\tsubgraph \"%s\" {\n", file ? literal_get_name(file) : "<external>");
            fprintf(dst, "\t\tlayout = \"sfdp\";\n");
            old_file = literal_get_file(cg->defs[i]);
        }
        fprintf(dst, "\t\tn%p[label=\"%s\"];\n", (void *)cg->defs[i], literal_get_name(cg->defs[i]));
    }
    if (old_file != (void *)-1) fputs("\t}\n", dst);

    for (size_t i = 0; i < cg->calls_size; i++)
        fprintf(dst, "\tn%p -> n%p;\n", (void *)cg->calls[i].caller, (void *)cg->calls[i].callee);
    fputs("}\n", dst);

    if (dst != stdout) fclose(dst);
}
