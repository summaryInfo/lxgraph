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

static int cmp_call(const void *a, const void *b) {
    const struct invokation *ia = (const struct invokation *)a;
    const struct invokation *ib = (const struct invokation *)b;
    if (ia->caller < ib->caller) return -1;
    if (ia->caller > ib->caller) return 1;
    if (ia->callee < ib->callee) return -1;
    if (ia->callee > ib->callee) return 1;
    if (ia->line < ib->line) return -1;
    if (ia->line > ib->line) return 1;
    if (ia->col < ib->col) return -1;
    if (ia->col > ib->col) return 1;
    return 0;
}

static void dfs(literal root, struct invokation *calls, struct invokation *end) {
    uint64_t *ptr = literal_get_pdata(root);
    struct invokation *it = calls + (*ptr >> 16);
    if (*ptr & 1) return;
    *ptr |= 1;
    for (; it < end && it->caller == root; it++)
        dfs(it->callee, calls, end);
}

static void filter_function(struct callgraph *cg, literal fun) {
    debug("Excluding function '%s'", fun ? literal_get_name(fun) : "<NULL>");
    {
        // Remove edges
        struct invokation *dst = cg->calls, *src = cg->calls;
        struct invokation *end = dst + cg->calls_size;
        for (; src < end; src++) {
            if (src->callee != fun && src->caller != fun)
                *dst++ = *src;
        }
        cg->calls_size = dst - cg->calls;
    }

    {
        // Remove functions
        literal *dst = cg->defs, *src = cg->defs;
        literal *end = dst + cg->defs_size;
        for (; src < end; src++) {
            if (*src != fun)
                *dst++ = *src;
        }
        cg->defs_size = dst - cg->defs;
    }
}

static void filter_file(struct callgraph *cg, literal file) {
    debug("Excluding file '%s'", file ? literal_get_name(file) : "<NULL>");
    {
        // Remove edges
        struct invokation *dst = cg->calls, *src = cg->calls;
        struct invokation *end = dst + cg->calls_size;
        for (; src < end; src++) {
            if (literal_get_file(src->callee) != file &&
                    literal_get_file(src->caller) != file)
                *dst++ = *src;
        }
        cg->calls_size = dst - cg->calls;
    }

    {
        // Remove functions
        literal *dst = cg->defs, *src = cg->defs;
        literal *end = dst + cg->defs_size;
        for (; src < end; src++) {
            if (literal_get_file(*src) != file)
                *dst++ = *src;
            else debug("    Removed '%s'", literal_get_name(*src));
        }
        cg->defs_size = dst - cg->defs;
    }
}

void renew_graph(struct callgraph *cg) {
    // Clear marks and build
    // associations between edges
    // and nodes
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

static void remove_unused(struct callgraph *cg) {
    renew_graph(cg);
    {
        // Mark every function starting from roots

        for (size_t i = 0; i < config.root_functions.size; i++) {
            debug("Makring root '%s'", config.root_functions.data[i]);
            dfs(strtab_put(&cg->strtab, config.root_functions.data[i]), cg->calls, cg->calls + cg->calls_size);
        }
    }

    debug("Removing unreachable functions");

    {
        // Remove unreachable edges
        struct invokation *dst = cg->calls, *src = cg->calls;
        struct invokation *end = dst + cg->calls_size;
        for (; src < end; src++) {
            if (*literal_get_pdata(src->caller) & 1)
                *dst++ = *src;
        }
        cg->calls_size = dst - cg->calls;
    }

    {
        // Remove unreachable functions
        literal *dst = cg->defs, *src = cg->defs;
        literal *end = dst + cg->defs_size;
        for (; src < end; src++) {
            if (*literal_get_pdata(*src) & 1) *dst++ = *src;
            else debug("Removing unused function '%s'", literal_get_name(*src));
        }
        cg->defs_size = dst - cg->defs;
    }
}

static void collapse_duplicates(struct callgraph *cg) {
    debug("Collapsing duplicate edges");
    struct invokation *dst = cg->calls, *src = cg->calls + 1;
    struct invokation *end = dst + cg->calls_size;
    int last_line = 0, last_col = 0;
    for (; src < end; src++) {
        if (dst->callee != src->callee || dst->caller != src->caller) {
            *++dst = *src;
            last_line = 0;
            last_col = 0;
        } else if (last_line != src->line || last_col != src->col) {
            last_line = src->line, last_col = src->col;
            dst->weight++;
        }
    }
    cg->calls_size = dst - cg->calls + 1;
}

void filter_graph(struct callgraph *cg) {
    for (size_t i = 0; i < config.exclude_files.size; i++)
        filter_file(cg, strtab_put(&cg->strtab, config.exclude_files.data[i]));

    for (size_t i = 0; i < config.exclude_functions.size; i++)
        filter_function(cg, strtab_put(&cg->strtab, config.exclude_functions.data[i]));

    /* Sort by caller name */
    qsort(cg->calls, cg->calls_size, sizeof cg->calls[0], cmp_call);
    /* Sort by name */
    qsort(cg->defs, cg->defs_size, sizeof cg->defs[0], cmp_def);
    collapse_duplicates(cg);
    remove_unused(cg);

    renew_graph(cg);
}
