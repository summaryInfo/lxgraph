#ifndef CALLGRAPH_H_
#define CALLGRAPH_H_ 1

#include "hashtable.h"
#include "stddef.h"

typedef struct strtab_item *literal;

struct callgraph {
    literal function;
    literal file;
    struct hashtable strtab;

    struct invokation {
        literal caller;
        literal callee;
    } *calls;
    size_t calls_caps;
    size_t calls_size;

    struct definition {
        literal file;
        literal name;
    } *defs;
    size_t defs_caps;
    size_t defs_size;
};

void free_callgraph(struct callgraph *cg);
struct callgraph *parse_file(struct callgraph *cg, const char *path);
literal strtab_put(struct callgraph *cg, const char *str);
const char *strtab_get(struct callgraph *cg, literal literal);

#endif

