#ifndef LITERAL_H_
#define LITERAL_H_ 1

#include "hashtable.h"

typedef struct strtab_item *literal;

enum literal_flags {
    lf_global = 1 << 0,
    lf_inline = 1 << 1,
    lf_defined = 1 << 2,
    lf_function = 1 << 3,
    lf_file = 1 << 4,
    lf_dup = 1 << 5,
};

void init_strtab(struct hashtable *strtab);
void fini_strtab(struct hashtable *strtab);
literal strtab_put(struct hashtable *ht, const char *str);
literal strtab_put2(struct hashtable *ht, const char *str, enum literal_flags fl);
/* duplicates are left in src */
void strtab_merge(struct hashtable *restrict dst, struct hashtable *restrict src);

void literal_set_flags(literal lit, enum literal_flags flags);
enum literal_flags literal_get_flags(literal lit);
const char *literal_get_name(literal literal);
void literal_set_file(literal lit, literal file);
literal literal_get_file(literal lit);
uint64_t *literal_get_pdata(literal lit);
bool literal_eq(literal a, literal b);

#endif

