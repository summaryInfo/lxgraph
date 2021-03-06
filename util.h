/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#ifndef UTIL_H_
#define UTIL_H_ 1

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define CACHE_LINE 64
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define SWAP(a, b) do{__typeof__(a) t__ = (a); (a) = (b); (b) = t__;}while(0)

#define PROG_NAME "lxgraph"

/* Logging */

void info(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
void warn(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
void debug(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
void syncdebug(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
_Noreturn void die(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

/* Configuration */

struct array_option {
    size_t caps;
    size_t size;
    char **data;
};

enum level_of_details {
    lod_function,
    lod_file,
};

struct config {
    char *config_path;
    char *output_path;
    char *build_dir;
    int32_t log_level;
    int32_t level_of_details;
    int32_t nthreads;
    struct array_option exclude_files;
    struct array_option exclude_functions;
    struct array_option root_files;
    struct array_option root_functions;
    struct array_option reverse_root_files;
    struct array_option reverse_root_functions;
    bool keep_inline;
    bool keep_static;
};

extern struct config config;

enum option {
    o_log_level,
    o_inline,
    o_static,
    o_config,
    o_out,
    o_path,
    o_threads,
    o_exclude_files,
    o_exclude_functions,
    o_root_files,
    o_root_functions,
    o_reverse_root_files,
    o_reverse_root_functions,
    o_lod,
    o_MAX
};

bool set_option(const char *name, const char *value);
const char *usage_string(size_t i);
void init_config(const char *path);
void fini_config(void);

/* File utils */

struct mapping {
    char *addr;
    size_t size;
};

struct mapping map_file(const char *path);
void unmap_file(struct mapping map);
bool adjust_buffer(void **buf, size_t *caps, size_t size, size_t elem);

#endif

