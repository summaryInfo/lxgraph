/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "util.h"
#include "callgraph.h"

#include <math.h>
#include <stdio.h>

static int cmp_def2(const void *a, const void *b) {
    literal da = *(literal *)a, db = *(literal *)b;
    literal fa = literal_get_file(da), fb = literal_get_file(db);
    if (fa < fb) return -1;
    else if (fa > fb) return 1;
    if (da < db) return -1;
    return (da > db);
}

void dump_dot(struct callgraph *cg, const char *destpath) {
    FILE *dst = destpath ? fopen(destpath, "w") : stdout;
    if (!dst) {
        warn("Cannot open output file '%s'", destpath);
        return;
    }

    /* Sort by file */
    renew_graph(cg);
    qsort(cg->defs, cg->defs_size, sizeof cg->defs[0], cmp_def2);

    debug("Writing graph to '%s'...", destpath ? destpath : "<stdout>");

    fputs("digraph \"callgraph\" {\n", dst);
    fprintf(dst, "\tlayout = \"%s\";\n", "fdp");
    fprintf(dst, "\tsmoothing = \"%s\";\n", "graph_dist");
    fprintf(dst, "\tesep = \"+%u\";\n", 32);
    fprintf(dst, "\toverlap = \"%s\";\n", "false");
    fprintf(dst, "\tsplines = \"%s\";\n", "compound");
    fprintf(dst, "\toutputorder = \"%s\";\n", "edgesfirst");
    fprintf(dst, "\tnode[shape=\"%s\" style=\"%s\" color=\"%s\"]\n", "box", "filled", "white");

    literal old_file = (void *)-1, *firstdef = NULL, *edef = cg->defs + cg->defs_size;

    /* Print functions for each file */
    for (size_t i = 0; i < cg->defs_size; i++) {
        literal file = literal_get_file(cg->defs[i]);
        if (old_file != file) {
            fprintf(dst, "\tsubgraph \"cluster_%s\" {\n", file ? literal_get_name(file) : "<external>");
            fprintf(dst, "\t\tstyle = \"%s\";\n", "dotted,filled");
            fprintf(dst, "\t\tcolor = \"%s\";\n", "lightgray");
            fprintf(dst, "\t\tlabel = \"%s\";\n", file ? literal_get_name(file) : "<external>");
            old_file = literal_get_file(cg->defs[i]);
            firstdef = cg->defs + i;
        }
        fprintf(dst, "\t\tn%p[label=\"%s\"];\n", (void *)cg->defs[i], literal_get_name(cg->defs[i]));
        if (i < cg->defs_size && file != literal_get_file(cg->defs[i + 1])) {
            /* Print edges within current file */
            for (literal *def = firstdef; def < edef && literal_get_file(*def) == literal_get_file(*firstdef); def++) {
                struct invokation *it = cg->calls + (*literal_get_pdata(*def) >> 16);
                for (; it < cg->calls + cg->calls_size && it->caller == *def; it++)
                    if (literal_get_file(it->caller) == literal_get_file(it->callee))
                        fprintf(dst, "\t\tn%p -> n%p[style = \"setlinewidth(%f)\"];\n", (void *)it->caller, (void *)it->callee, MIN(pow(cg->calls[i].weight, 0.6), 20));
            }
            fputs("\t}\n", dst);
        }
    }

    /* Print all edges between files */
    for (size_t i = 0; i < cg->calls_size; i++) {
        if (literal_get_file(cg->calls[i].caller) != literal_get_file(cg->calls[i].callee))
            fprintf(dst, "\tn%p -> n%p[style = \"setlinewidth(%f)\"];\n",
                (void *)cg->calls[i].caller, (void *)cg->calls[i].callee, MIN(pow(cg->calls[i].weight, 0.6), 20));
    }
    fputs("}\n", dst);

    debug("Done.");

    if (dst != stdout) fclose(dst);
}
