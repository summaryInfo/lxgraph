/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "util.h"
#include "callgraph.h"

#include <math.h>
#include <stdio.h>

#define MAX_WEIGHT 16

void dump_dot(struct callgraph *cg, const char *destpath) {
    FILE *dst = destpath ? fopen(destpath, "w") : stdout;
    if (!dst) {
        warn("Cannot open output file '%s'", destpath);
        return;
    }

    debug("Writing graph to '%s'...", destpath ? destpath : "<stdout>");

    fputs("digraph \"callgraph\" {\n", dst);

    // TODO Make more of these configurable
    fprintf(dst, "\tlayout = \"%s\";\n", "fdp");
    fprintf(dst, "\tsmoothing = \"%s\";\n", "graph_dist");
    fprintf(dst, "\tesep = \"+%u\";\n", 32);
    fprintf(dst, "\toverlap = \"%s\";\n", "false");
    fprintf(dst, "\tsplines = \"%s\";\n", "true");
    fprintf(dst, "\toutputorder = \"%s\";\n", "edgesfirst");
    fprintf(dst, "\tnode[shape=\"%s\" style=\"%s\" color=\"%s\"]\n", "box", "filled", "white");

    /* Print functions for each file */
    ht_iter_t itfile = ht_begin(&cg->files);
    for (ht_head_t *cur; (cur = ht_next(&itfile)); ) {
        struct file *file = container_of(cur, struct file, head);
        if (list_is_empty(&file->functions)) continue;
        fprintf(dst, "\tsubgraph \"cluster_%s\" {\n", file->name);
        fprintf(dst, "\t\tstyle = \"%s\";\n", "dotted,filled");
        fprintf(dst, "\t\tcolor = \"%s\";\n", "lightgray");
        fprintf(dst, "\t\tlabel = \"%s\";\n", file->name);

        list_iter_t itfun = list_begin(&file->functions);
        for (list_head_t *curfun; (curfun = list_next(&itfun)); ) {
            struct function *fun = container_of(curfun, struct function, in_file);
            fprintf(dst, "\t\tn%p[label=\"%s\"];\n", (void *)fun, fun->name);

            list_iter_t itcall = list_begin(&fun->calls);
            for (list_head_t *curcall; (curcall = list_next(&itcall)); ) {
                struct call *call = container_of(curcall, struct call, calls);
                if (call->caller->file != call->callee->file) continue;
                fprintf(dst, "\t\tn%p -> n%p[style = \"setlinewidth(%f)\"];\n",
                        (void *)call->caller, (void *)call->callee, MIN(pow(call->weight, 0.6), MAX_WEIGHT));
            }
        }
        fputs("\t}\n", dst);
    }

    /* Print all edges between files */
    ht_iter_t itfun = ht_begin(&cg->functions);
    for (ht_head_t *cur; (cur = ht_next(&itfun)); ) {
        struct function *fun = container_of(cur, struct function, head);
        if (!fun->file) {
            // Built-in functions are not defined anywhere...
            fprintf(dst, "\t\tn%p[label=\"%s\"];\n", (void *)fun, fun->name);
        }

        list_iter_t itcall = list_begin(&fun->calls);
        for (list_head_t *curcall; (curcall = list_next(&itcall)); ) {
            struct call *call = container_of(curcall, struct call, calls);
            if (call->caller->file == call->callee->file) continue;
            fprintf(dst, "\t\tn%p -> n%p[style = \"setlinewidth(%f)\"];\n",
                    (void *)call->caller, (void *)call->callee, MIN(pow(call->weight, 0.6), MAX_WEIGHT));
        }
    }

    fputs("}\n", dst);

    debug("Done.");

    if (dst != stdout) fclose(dst);
}
