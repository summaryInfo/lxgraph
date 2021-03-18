/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#include "hashtable.h"
#include "literal.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

struct strtab_item {
    ht_head_t head;
    const char *name;
    literal file;
    uint64_t data;
    enum literal_flags flags;
};

static bool strtab_item_cmp(const ht_head_t *a, const ht_head_t *b) {
    const struct strtab_item *ai = (const struct strtab_item *)a;
    const struct strtab_item *bi = (const struct strtab_item *)b;
    return !strcmp(ai->name, bi->name);
}

void init_strtab(struct hashtable *strtab) {
    ht_init(strtab, HT_INIT_CAPS, strtab_item_cmp);
    assert(strtab->data);
}

void fini_strtab(struct hashtable *strtab) {
    ht_iter_t it = ht_begin(strtab);
    while (ht_current(&it))
        free(ht_erase_current(&it));
    ht_free(strtab);
}

bool literal_eq(literal a, literal b) {
    return a->head.hash == b->head.hash &&
           !strcmp(a->name, b->name);
}

const char *literal_get_name(literal lit) {
    return lit->name;
}

uint64_t *literal_get_pdata(literal lit) {
    return &lit->data;
}

void literal_set_file(literal lit, literal file) {
    assert(lit);
    assert(file->flags & lf_file);
    assert(lit->flags & lf_function);
    if (lit->file) lit->flags |= lf_dup;
    lit->file = file;
}

literal literal_get_file(literal lit) {
    assert(lit->flags & lf_function);
    return lit->file;
}

literal strtab_put(struct hashtable *ht, const char *str) {
    if (!str) return NULL;

    size_t len = strlen(str);
    struct strtab_item key = { .head = { .hash = hash64(str, len) }, .name = str };
    ht_head_t **h = ht_lookup_ptr(ht, (ht_head_t *)&key);
    if (*h) return (literal)*h;

    struct strtab_item *new = malloc(sizeof *new + len + 1);
    assert(new);
    new->head = key.head;
    new->name = (char *)(new + 1);
    new->file = NULL;
    new->data = 0;
    strcpy((char *)new->name, str);

    ht_insert_hint(ht, h, (ht_head_t *)new);
    return (literal)new;
}

literal strtab_put2(struct hashtable *ht, const char *str, enum literal_flags fl) {
    literal id = strtab_put(ht, str);
    if (id) id->flags |= fl;
    return id;
}

void strtab_merge(struct hashtable *restrict dst, struct hashtable *restrict src) {
    ht_iter_t it = ht_begin(src);
    ht_iter_t it2 = it;
    for (ht_head_t *cur; (cur = ht_next(&it2)); ) {
        literal clit = (literal)cur;
        if (clit->file) {
            ht_head_t *ex = ht_find(dst, (ht_head_t *)clit->file);
            if (ex) clit->file = (literal)ex;
        }
    }

    for (ht_head_t *cur; (cur = ht_current(&it)); ) {
        ht_head_t **old = ht_lookup_ptr(dst, cur);
        if (!*old) ht_insert_hint(dst, old, ht_erase_current(&it));
        else {
            literal olit = (literal)*old, clit = (literal)cur;
            if (!olit->file)
                olit->file = clit->file;
            else if (clit->file)
                olit->flags |= lf_dup;
            clit->file = olit;
            ht_next(&it);
        }
    }
}

void literal_set_flags(literal lit, enum literal_flags flags) {
    lit->flags = flags;
}

enum literal_flags literal_get_flags(literal lit) {
    return lit->flags;
}
