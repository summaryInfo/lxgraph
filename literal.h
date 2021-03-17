#ifndef LITERAL_H_
#define LITERAL_H_ 1

#include "hashtable.h"

typedef struct strtab_item *literal;

void init_strtab(struct hashtable *strtab);
void fini_strtab(struct hashtable *strtab);
literal strtab_put(struct hashtable *ht, const char *str);
/* duplicates are left in src */
void strtab_merge(struct hashtable *restrict dst, struct hashtable *restrict src);

const char *literal_get_name(literal literal);
void literal_set_file(literal lit, literal file);
literal literal_get_file(literal lit);
uint64_t *literal_get_pdata(literal lit);
bool literal_eq(literal a, literal b);

#endif

