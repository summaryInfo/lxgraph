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
#include <pthread.h>
#include <setjmp.h>

#define MAX_OPTION_DESC 256

static struct options {
    const char *name;
    const char *desc;
} options[o_MAX] = {
    [o_log_level] = {"log-level", ", -L<value>\t(Verbositiy of output, 0-4)" },
    [o_inline] = {"inline", "\t(Keep inline functions)"},
    [o_static] = {"static", "\t(Keep static functions)"},
    [o_lod] = {"lod", "\t\t(Set level of details, [function]/file)"},
    [o_config] = {"config", ", -C<value>\t(Configuration file path)" },
    [o_out] = {"out", ", -o<value>\t(Output file path)"},
    [o_path] = {"path", ", -p<value>\t(Build directory path)"},
    [o_threads] = {"threads", ", -T<value>\t(Number of threads to use, default is number of cores + 1)"},
    [o_exclude_files] = {"exclude-files", "\t\t(List of files to exclude from the graph)"},
    [o_exclude_functions] = {"exclude-functions", "\t\t(List of functions to exclude from the graph)"},
    [o_root_files] = {"root-files", "\t\t(List of files to mark as roots of the graph)"},
    [o_root_functions] = {"root-functions", "\t\t(List of functions to mark as roots of the graph)"},
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

void syncdebug(const char *fmt, ...) {
    if (config.log_level > 3) {
        static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        va_list args;
        va_start(args, fmt);
        pthread_mutex_lock(&mutex);
        fputs("[DEBUG] ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
        pthread_mutex_unlock(&mutex);
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

static bool parse_enum(const char *str, int64_t *val, int dflt, int start, ...) {
    if (!strcasecmp(str, "default")) *val = dflt;
    else {
        va_list va;
        va_start(va, start);
        const char *s;
        bool valset = 0;
        while ((s = va_arg(va, const char *))) {
            if (!strcasecmp(str, s)) {
                *val = start;
                valset = 1;
                break;
            }
            start++;
        }
        va_end(va);
        return valset;
    }
    return 1;
}

static void fini_array_option(struct array_option *current) {
    for (size_t i = 0; i < current->size; i++)
        free(current->data[i]);
    free(current->data);
    *current = (struct array_option) { 0 };
}

bool set_option(const char *name, const char *value) {
    static struct array_option *current;
    if (name) {
        if (value) debug("Setting option %s=\"%s\"", name, value);
        int64_t v;
        bool bv;
        if (!strcmp(options[o_log_level].name, name)) {
            if (!parse_int(value, &v, 0, 4, 3)) goto e_value;
            config.log_level = v;
            return true;
        } else if (!strcmp(options[o_config].name, name)) {
            parse_str(&config.config_path, value, PROG_NAME".conf");
            return true;
        } else if (!strcmp(options[o_inline].name, name)) {
            if (!parse_bool(value, &bv, 1)) goto e_value;
            config.keep_inline = bv;
            return true;
        } else if (!strcmp(options[o_static].name, name)) {
            if (!parse_bool(value, &bv, 1)) goto e_value;
            config.keep_static = bv;
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
        } else if (!strcmp(options[o_lod].name, name)) {
            if (!parse_enum(value, &v, lod_function,
                    lod_function, "function", "file", NULL)) goto e_value;
            config.level_of_details = v;
            return true;
        } else if (!strcmp(options[o_exclude_files].name, name)) {
            current = &config.exclude_files;
        } else if (!strcmp(options[o_exclude_functions].name, name)) {
            current = &config.exclude_functions;
        } else if (!strcmp(options[o_root_functions].name, name)) {
            current = &config.root_functions;
        } else if (!strcmp(options[o_root_files].name, name)) {
            current = &config.root_files;
        } else current = NULL;
    }

    if (current) {
        if (!value || !*value) {
            debug("  Clearing option array");
            fini_array_option(current);
        } else {
            debug("  Appending option %s", value);
            bool res = adjust_buffer((void **)&current->data, &current->caps, current->size + 1, sizeof value);
            assert(res);
            current->data[current->size++] = strdup(value);
            assert(current->data[current->size - 1]);
        }
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
            "All non-array options are also accept special value 'default' to reset to built-in default\n"
            "Array options accept one value at a time and append to the current value.\n"
            "Specify empty value string to clear the array option\n";
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

#define MAX_VAL_LEN 1024

struct parse_state {
    const char *start;
    const char *end;

    const char *line_start;
    size_t line_n;
    bool skip_to_quote;
    bool skip_to_bracket;

    jmp_buf escape_path;
};

inline static void skip_spaces(struct parse_state *ps) {
    while (ps->start < ps->end) {
        if (*ps->start == '#') {
            do ps->start++;
            while (ps->start < ps->end && *ps->start != '\n');
        } else if (isspace((unsigned)*ps->start)) {
            if (*ps->start == '\n') {
                ps->line_start = ps->start + 1;
                ps->line_n++;
            }
            ps->start++;
        } else break;
    }
}

inline static bool is_end(struct parse_state *ps) {
    return ps->start >= ps->end;
}

_Noreturn static void complain(struct parse_state *ps, const char *msg) {
    int col = ps->start - ps->line_start;
    warn("%s at line %zd column %d:", msg, ps->line_n, col);
    warn("\t%s\n%*c", ps->line_start, col + 1, '^');
    longjmp(ps->escape_path, 1);
}

inline static char comsume_hex_digit(struct parse_state *ps) {
    unsigned h = *ps->start++;
    if (h - '0' < 10) return h - '0';
    if (h - 'A' < 6) return h - 'A';
    if (h - 'a' < 6) return h - 'a';

    ps->start--;
    complain(ps, "Expected hex digit");
}

inline static char comsume_oct_digit(struct parse_state *ps) {
    unsigned h = *ps->start++;
    if (h - '0' < 8) return h - '0';

    ps->start--;
    complain(ps, "Expected octal digit");
}

inline static char unescaped(struct parse_state *ps) {
    char ch = *ps->start++;
    if (!ch) {
        ps->start--;
        complain(ps, "Unexpected end of file");
    } else if (ch == '\\') {
        switch(ch = *ps->start++) {
        case '\0':
            ps->start--;
            complain(ps, "Unexpected end of file");
        case '0':
            if ((unsigned)*ps->start - '0' > 7)
                return '\0';
            // fallthrough
        case '1': case '2': case '3':
        case '4': case '5': case '6':
        case '7': case '8': case '9': {
            break;
        }
        case 'x': {
            unsigned h0 = comsume_hex_digit(ps);
            unsigned h1 = comsume_hex_digit(ps);
            return (h0 << 4) | h1;
        }
        case 'u':
        case 'U':
            complain(ps, "Unicode escapes are not implemeneted");
            break;
        case 'a': return '\a';
        case 'b': return '\b';
        case 'e': return '\033';
        case 'f': return '\f';
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case 'v': return '\v';
        }
    }
    if (ch == '\n') {
        ps->line_start = ps->start;
        ps->line_n++;
    }
    return ch;
}

inline static bool is_word_break(struct parse_state *ps) {
    return isspace((unsigned)*ps->start) || !*ps->start || strchr("#=\"][", *ps->start);
}

inline static bool is_quoted_word_break(struct parse_state *ps) {
    return !*ps->start || *ps->start == '"';
}

inline static bool comsume_if(struct parse_state *ps, char c) {
    if (*ps->start == c) {
        ps->start++;
        return 1;
    }
    return 0;
}

static void consume(struct parse_state *ps, char c) {
    skip_spaces(ps);
    if (*ps->start++ != c) {
        ps->start--;
        complain(ps, "Unexpected character");
    }
}

static bool parse_value(struct parse_state *ps, char *dst, char *end, bool allow_array) {
    skip_spaces(ps);
    if (is_end(ps)) complain(ps, "Unexpected end of file, expected value");

    if (ps->skip_to_bracket && comsume_if(ps, ']')) {
        ps->skip_to_bracket = 0;
        return 1;
    } else if (comsume_if(ps, '[')) {
        ps->skip_to_bracket = 1;
        if (!allow_array) complain(ps, "Nested arrays are not supported");
        return 1;
    } else if (comsume_if(ps, '"')) {
        ps->skip_to_quote = 1;
        while (dst < end) {
            if (is_quoted_word_break(ps)) break;
            *dst++ = unescaped(ps);
        }
        ps->skip_to_quote = 0;
        if (!comsume_if(ps, '"')) complain(ps, "Unexpected end of file, expected \"");
    } else {
        while (dst < end) {
            if (is_word_break(ps)) break;
            *dst++ = unescaped(ps);
        }
    }
    *dst = '\0';
    return 0;
}

static void parse_config(void) {
    struct mapping cfg = {0};
    static char buf1[MAX(PATH_MAX, MAX_VAL_LEN)];
    static char buf2[MAX_VAL_LEN];
    const char *config_taken =  NULL;

    /* Config file is search in following places:
     * 1. --config=/-C
     * 2. Project directory
     * 3. Current directory
     * If file is not found in those places, just give up */

    if (config.config_path)
        cfg = map_file(config_taken = config.config_path);
    if (!cfg.addr) {
        snprintf(buf1, sizeof buf1, "%s/"PROG_NAME".conf", config.build_dir);
        cfg = map_file(config_taken = buf1);
    }
    if (!cfg.addr)
        cfg = map_file(config_taken = PROG_NAME".conf");

    if (!cfg.addr) {
        debug("Cannot find config file anywhere");
        return;
    } else {
        debug("Picked config file '%s'", config_taken);
    }

    struct parse_state ps = {
        .start = cfg.addr,
        .end = cfg.addr + cfg.size,
        .line_start = cfg.addr,
    };

start_from_next_line:
    if (setjmp(ps.escape_path)) {
        // Skip to the place where we know the state
        if (ps.skip_to_bracket) {
            while (ps.start < ps.end && *ps.start != ']') ps.start++;
            ps.skip_to_bracket = 0;
        } else if (ps.skip_to_quote) {
            while (ps.start < ps.end && *ps.start != '"') ps.start++;
            ps.skip_to_quote = 0;
        } else {
            while (ps.start < ps.end && !isspace((unsigned)*ps.start)) ps.start++;
        }
        if (ps.start < ps.end) goto start_from_next_line;
        unmap_file(cfg);
        return;
    }

    while (!is_end(&ps)) {
        parse_value(&ps, buf2, buf2 + sizeof buf2 - 1, 0);
        consume(&ps, '=');
        if (parse_value(&ps, buf1, buf1 + sizeof buf1 - 1, 1)) {
            set_option(buf2, NULL);
            while (!parse_value(&ps, buf1, buf1 + sizeof buf1 - 1, 0))
                set_option(NULL, buf1);
        } else {
            set_option(buf2, buf1);
        }
        skip_spaces(&ps);
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
    fini_array_option(&config.exclude_files);
    fini_array_option(&config.exclude_functions);
    fini_array_option(&config.root_files);
    fini_array_option(&config.root_functions);
    free(config.config_path);
    free(config.output_path);
    free(config.build_dir);
    memset(&config, 0, sizeof config);
}
