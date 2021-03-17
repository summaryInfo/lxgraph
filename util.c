/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#define _DEFAULT_SOURCE

#include "util.h"
#include "hashtable.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_OPTION_DESC 256

static struct options {
    const char *name;
    const char *desc;
} options[o_MAX] = {
    [o_log_level] = {"log-level", ", -L<value>\t(Verbositiy of output, 0-4)" },
    [o_config] = {"config", ", -C<value>\t(Configuration file path)" },
    [o_out] = {"out", ", -o<value>\t(Output file path)"},
    [o_path] = {"path", ", -p<value>\t(Build directory path)"},
    [o_threads] = {"threads", ", -T<value>\t(Number of threads to use, default is number of cores + 1)"},
};

struct config config;

_Noreturn void die(const char *fmt, ...) {
    if (config.log_level > 0) {
        va_list args;
        va_start(args, fmt);
        fputs("[\033[31;1mFATAL\033[m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
    exit(EXIT_FAILURE);
}

void warn(const char *fmt, ...) {
    if (config.log_level > 1) {
        va_list args;
        va_start(args, fmt);
        fputs("[\033[33;1mWARN\033[m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
}

void info(const char *fmt, ...) {
    if (config.log_level > 2) {
        va_list args;
        va_start(args, fmt);
        fputs("[\033[32;1mINFO\033[m] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
}

void debug(const char *fmt, ...) {
    if (config.log_level > 3) {
        va_list args;
        va_start(args, fmt);
        fputs("[DEBUG] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        va_end(args);
    }
}

#define CAPS_STEP(x) ((x)?4*(x)/3:8)

bool adjust_buffer(void **buf, size_t *caps, size_t size, size_t elem) {
    if (size > *caps) {
        void *tmp = realloc(*buf, elem * MAX(CAPS_STEP(*caps), size));
        if (!tmp) return 0;
        *buf = tmp;
        *caps = CAPS_STEP(*caps);
    }
    return 1;
}

#define HT_LOAD_FACTOR(x) (4*(x)/3)
#define HT_CAPS_STEP(x) (3*(x)/2)

bool ht_adjust(struct hashtable *ht, intptr_t inc) {
    ht->size += inc;

    if (HT_LOAD_FACTOR(ht->size) > ht->caps) {
        struct hashtable tmp = {
            .cmpfn = ht->cmpfn,
            .caps = HT_CAPS_STEP(ht->caps),
            .data = calloc(HT_CAPS_STEP(ht->caps), sizeof(*ht->data)),
        };
        if (!tmp.data) return 0;

        ht_iter_t it = ht_begin(ht);
        while(ht_current(&it))
            ht_insert(&tmp, ht_erase_current(&it));
        free(ht->data);
        *ht = tmp;
    }

    return 1;
}

#if 0
static bool parse_bool(const char *str, bool *val, bool dflt) {
    if (!strcasecmp(str, "default")) {
        *val = dflt;
        return 1;
    } else if (!strcasecmp(str, "true") || !strcasecmp(str, "yes") || !strcasecmp(str, "y") || !strcmp(str, "1")) {
        *val = 1;
        return 1;
    } else if (!strcasecmp(str, "false") || !strcasecmp(str, "no") || !strcasecmp(str, "n") || !strcmp(str, "0")) {
        *val = 0;
        return 1;
    }
    return 0;
}
#endif

static bool parse_int(const char *str, int64_t *val, int64_t min, int64_t max, int64_t dflt) {
    if (!strcasecmp(str, "default")) *val = dflt;
    else {
        errno = 0;
        char *end;
        *val = strtoll(str, &end, 0);
        if (errno || !end || *end) return 0;
        if (*val < min) *val = min;
        if (*val > max) *val = max;
    }
    return 1;
}

static void parse_str(char **dst, const char *str, const char *dflt) {
    char *res;
    if (!strcasecmp(str, "default")) {
        res = dflt ? strdup(dflt) : NULL;
    } else res = strdup(str);

    if (*dst) free(*dst);
    *dst = res;
}

bool set_option(const char *name, const char *value) {
    int64_t v;
    if (!strcmp(options[o_log_level].name, name)) {
        if (!parse_int(value, &v, 0, 4, 3)) goto e_value;
        config.log_level = v;
        return true;
    } else if (!strcmp(options[o_config].name, name)) {
        parse_str(&config.config_path, value, PROG_NAME".conf");
        return true;
    } else if (!strcmp(options[o_path].name, name)) {
        parse_str(&config.build_dir, value, ".");
        return true;
    } else if (!strcmp(options[o_out].name, name)) {
        parse_str(&config.output_path, value, "graph.dot");
        return true;
    } else if (!strcmp(options[o_threads].name, name)) {
        if (!parse_int(value, &v, 1, 32, 0)) goto e_value;
        config.nthreads = v;
        return true;
    }

    warn("Unknown option '%s' with value '%s'", name, value);
    return false;
e_value:
    warn("Failed to parse value '%s' of option '%s'", value, name);
    return false;
}

const char *usage_string(size_t idx) {
    static char buffer[MAX_OPTION_DESC + 1];

    if (!idx) {
        return /* argv0 here*/ " [options]\n"
            "Where options are:\n"
                "\t--help, -h\t\t\t(Print this message and exit)\n"
                "\t-q\t\t\t\t(Set log level to 0)\n";
    } else if (idx - 1 < o_MAX) {
        snprintf(buffer, sizeof buffer, "\t--%s=<value>%s\n", options[idx - 1].name, options[idx - 1].desc);
        return buffer;
    } else if (idx == o_MAX + 1) {
        return  "For every boolean option --<X>=<Y>\n"
                "\t--<X>, --<X>=yes, --<X>=y,  --<X>=true\n"
            "are equivalent to --<X>=1, and\n"
                "\t--no-<X>, --<X>=no, --<X>=n, --<X>=false\n"
            "are equivalent to --<X>=0,\n"
            "where 'yes', 'y', 'true', 'no', 'n' and 'false' are case independet\n"
            "All options are also accept special value 'default' to reset to built-in default\n";
    } else return NULL;
}

struct mapping map_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) goto e_open;

    struct stat stt;
    if (fstat(fd, &stt) < 0) goto e_open;

    char *addr = mmap(NULL, stt.st_size + 1, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) goto e_open;

    close(fd);

    return (struct mapping) { addr, stt.st_size };

e_open:
    if (fd >= 0) close(fd);
    debug("Failed to map file '%s'", path);
    return (struct mapping) { NULL, 0 };
}

void unmap_file(struct mapping map) {
    munmap(map.addr, map.size + 1);
}

static void parse_config(void) {
    struct mapping cfg = {0};
    char pathbuf[PATH_MAX];

    /* Config file is search in following places:
     * 1. sconf(SCONF_CONFIG_PATH) set with --config=
     * 2. $XDG_CONFIG_HOME/<name>.conf
     * 3. $HOME/.config/<name>.conf
     * If file is not found in those places, just give up */

    if (config.config_path)
        cfg = map_file(config.config_path);
    if (!cfg.addr) {
        const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
        if (xdg_cfg) {
            snprintf(pathbuf, sizeof pathbuf, "%s/"PROG_NAME".conf", xdg_cfg);
            cfg = map_file(pathbuf);
        }
    }
    if (!cfg.addr) {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(pathbuf, sizeof pathbuf, "%s/.config/"PROG_NAME".conf", home);
            cfg = map_file(pathbuf);
        }
    }

    if (!cfg.addr) debug("Cannot find config file anywhere");

    char *ptr = cfg.addr, *end = cfg.addr + cfg.size;
    char saved1 = '\0', saved2 = '\0';
    ssize_t line_n = 0;
    while (ptr < end) {
        line_n++;

        while (ptr < end && isspace((unsigned)*ptr)) ptr++;
        if (ptr >= end) break;

        char *start = ptr;
        if (isalpha((unsigned)*ptr)) {
            char *name_start, *name_end, *value_start, *value_end;

            name_start = ptr;

            while (ptr < end && !isspace((unsigned)*ptr) && *ptr != '#' && *ptr != '=') ptr++;
            name_end = ptr;

            while (ptr < end && isblank((unsigned)*ptr)) ptr++;
            if (ptr >= end || *ptr++ != '=') goto e_wrong_line;
            while (ptr < end && isblank((unsigned)*ptr)) ptr++;
            value_start = ptr;

            while (ptr < end && *ptr != '\n') ptr++;
            while (ptr > value_start && isblank((unsigned)ptr[-1])) ptr--;
            value_end = ptr;

            SWAP(*value_end, saved1);
            SWAP(*name_end, saved2);
            set_option(name_start, value_start);
            SWAP(*name_end, saved2);
            SWAP(*value_end, saved1);
        } else if (*ptr == '#') {
            while (ptr < end && *ptr != '\n') ptr++;
        } else {
e_wrong_line:
            ptr = start;
            while(ptr < end && *ptr != '\n') ptr++;
            SWAP(*ptr, saved1);
            warn("Can't parse config line #%zd: %s", line_n, start);
            SWAP(*ptr, saved1);
            ptr++;
        }
    }

    unmap_file(cfg);
}

void init_config(const char *path) {
    for (size_t i = 0; i < sizeof(options)/sizeof(*options); i++)
        if (i != o_config) set_option(options[i].name, "default");

    if (path) set_option(options[o_config].name, path);
    parse_config();
}

void fini_config(void) {
    free(config.config_path);
    free(config.output_path);
    free(config.build_dir);
    memset(&config, 0, sizeof config);
}
