/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "util.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SKIP_OPT ((void*)-1)

static _Noreturn void usage(const char *argv0, int code) {
    if (config.log_level > 0 || code == EXIT_SUCCESS) {
        size_t i = 0;
        do fputs(argv0, stdout);
        while((argv0 = usage_string(i++)));
    }
    exit(code);
}

static void parse_options(char **argv) {
    size_t ind = 1;

    char *arg;
    const char *opt;
    while (argv[ind] && argv[ind][0] == '-') {
        size_t cind = 0;
        if (!argv[ind][1]) usage(argv[0], EXIT_FAILURE);
        if (argv[ind][1] == '-') {
            if (!argv[ind][2]) {
                ind++;
                break;
            }

            //Long options

            opt = argv[ind] + 2; /* skip '--' */

            if ((arg = strchr(argv[ind], '='))) {
                *arg++ = '\0';
                if (!*arg) arg = argv[++ind];

                if (strcmp(opt, "config") && !set_option(opt, arg))
                    usage(argv[0], EXIT_FAILURE);
            } else {
                if (!strcmp(opt, "help"))
                    usage(argv[0], EXIT_SUCCESS);
                else {
                    const char *val = "true";
                    if (!strncmp(opt, "no-", 3)) opt += 3, val = "false";
                    if (!set_option(opt, val))
                        usage(argv[0], EXIT_FAILURE);
                }
            }
        } else while (argv[ind] && argv[ind][++cind]) {
            char letter = argv[ind][cind];
            // One letter options
            switch (letter) {
            case 'Q':
                config.log_level = 0;
                break;
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
            default:;
                opt = NULL;
                switch (letter) {
                case 'L': opt = "log-level"; break;
                }

                if (opt) {
                    // Has arguments
                    if (!argv[ind][++cind]) ind++, cind = 0;
                    if (!argv[ind]) usage(argv[0], EXIT_FAILURE);
                    arg = argv[ind] + cind;

                    if (opt != SKIP_OPT)
                        set_option(opt, arg);

                    goto next;
                }

                warn("Unknown option -%c", letter);
            }
        }
    next:
        if (argv[ind]) ind++;
    }
}

int main(int argc, char **argv) {
    (void) argc;

    // Parse --config/-C argument before parsing config file
    char *cpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--config=", sizeof "--config=" - 1) ||
                !strncmp(argv[i], "-C", sizeof "-C" - 1)) {
            char *arg = argv[i] + (argv[i][1] == '-' ? sizeof "--config=" : sizeof "-C") - 1;
            if (!*arg) arg = argv[++i];
            if (!arg) usage(argv[0], EXIT_FAILURE);
            cpath = arg;
        }
    }

    init_config(cpath);
    parse_options(argv);

    return EXIT_SUCCESS;
}
