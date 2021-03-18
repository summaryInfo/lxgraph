/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "util.h"
#include "callgraph.h"

#include <stdio.h>

static int cmp_def(const void *a, const void *b) {
    literal alit = *(literal *)a;
    literal blit = *(literal *)b;
    if (alit < blit) return -1;
    else return (alit > blit);
}

static int cmp_def2(const void *a, const void *b) {
    literal da = *(literal *)a, db = *(literal *)b;
    literal fa = literal_get_file(da), fb = literal_get_file(db);
    if (fa < fb) return -1;
    else if (fa > fb) return 1;
    if (da < db) return -1;
    return (da > db);
}

static int cmp_call(const void *a, const void *b) {
    const struct invokation *ia = (const struct invokation *)a;
    const struct invokation *ib = (const struct invokation *)b;
    if (ia->caller < ib->caller) return -1;
    if (ia->caller > ib->caller) return 1;
    if (ia->callee < ib->callee) return -1;
    return (ia->callee > ib->callee);
}

static void dfs(literal root, struct invokation *calls, struct invokation *end) {
    uint64_t *ptr = literal_get_pdata(root);
    struct invokation *it = calls + (*ptr >> 16);
    if (*ptr & 1) return;
    *ptr |= 1;
    for (; it < end && it->caller == root; it++)
        dfs(it->callee, calls, end);
}

void remove_unused(struct callgraph *cg) {
    {
        // Clear marks and build
        // associations between edges
        // and nodes to be ables to dfs on O(n) time
        size_t i = 0, j = 0;
        while (j < cg->calls_size) {
            while (i < cg->defs_size && cg->calls[j].caller != cg->defs[i])
                *literal_get_pdata(cg->defs[i++]) = 0;
            if (i == cg->defs_size) break;
            *literal_get_pdata(cg->defs[i++]) = j++ << 16;
            while (j < cg->calls_size &&
                   cg->calls[j].caller == cg->calls[j - 1].caller) j++;
        }
        while (i < cg->defs_size)
            *literal_get_pdata(cg->defs[i++]) = 0;
    }

    {
        // Mark every function starting from roots

        // TODO Make this configurable
        const char *roots[] = {
            "main(int, char **)",
            "main()",
            "start_kernel()"
        };
        for (size_t i = 0; i < sizeof roots/sizeof *roots; i++)
            dfs(strtab_put(&cg->strtab, roots[i]), cg->calls, cg->calls + cg->calls_size);
    }

    {
        // Remove unreachable edges
        struct invokation *dst = cg->calls, *src = cg->calls + 1;
        struct invokation *end = dst + cg->calls_size;
        for (; src < end; src++) {
            if (*literal_get_pdata(src->caller) & 1)
                *dst++ = *src;
        }
        cg->calls_size = dst - cg->calls;
    }

    {
        // Remove unreachable functions
        literal *dst = cg->defs, *src = cg->defs + 1;
        literal *end = dst + cg->defs_size;
        for (; src < end; src++) {
            if (*literal_get_pdata(*src) & 1)
                *dst++ = *src;
        }
        cg->defs_size = dst - cg->defs;
    }
}

void colapse_duplicates(struct callgraph *cg) {
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

    /* Sort by caller name */
    qsort(cg->calls, cg->calls_size, sizeof cg->calls[0], cmp_call);
    /* Sort by name */
    qsort(cg->defs, cg->defs_size, sizeof cg->defs[0], cmp_def);
    colapse_duplicates(cg);
    remove_unused(cg);

    /* Sort by file */
    qsort(cg->defs, cg->defs_size, sizeof cg->defs[0], cmp_def2);

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
