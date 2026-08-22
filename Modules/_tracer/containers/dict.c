#include "containers.h"
#include <stdlib.h>
#include <string.h>


/* ======================================================================== */
/* ARWDict — compact string-keyed hash table for ARW values                  */
/* ======================================================================== */

#define USABLE_FRACTION(n) (((n) << 1) / 3)

static size_t
arwdict_hash(const char *key)
{
    size_t h = 5381;
    for (const char *p = key; *p; p++)
        h = ((h << 5) + h) ^ (unsigned char)*p;
    return h;
}

static int8_t
calculate_log2_size(size_t minsize)
{
    int8_t log2 = 3;
    while (((size_t)1 << log2) < minsize)
        log2++;
    return log2;
}

static int arwdict_resize(ARWDict *d, size_t minsize);

void
arwdict_init(ARWDict *d)
{
    d->log2_size = 0;
    d->usable = 0;
    d->nentries = 0;
    d->used = 0;
    d->indices = NULL;
    d->entries = NULL;
}

void
arwdict_free(ARWDict *d)
{
    for (size_t i = 0; i < d->nentries; i++)
        free(d->entries[i].key);
    free(d->entries);
    free(d->indices);
    d->indices = NULL;
    d->entries = NULL;
    d->log2_size = 0;
    d->usable = 0;
    d->nentries = 0;
    d->used = 0;
}

static int
arwdict_ensure_alloc(ARWDict *d)
{
    if (d->indices != NULL)
        return 0;
    int8_t log2 = 3;
    size_t size = (size_t)1 << log2;
    d->indices = malloc(size * sizeof(int32_t));
    if (!d->indices)
        return -1;
    memset(d->indices, 0xff, size * sizeof(int32_t));
    d->entries = malloc(USABLE_FRACTION(size) * sizeof(ARWDictEntry));
    if (!d->entries) {
        free(d->indices);
        d->indices = NULL;
        return -1;
    }
    d->log2_size = log2;
    d->usable = USABLE_FRACTION(size);
    d->nentries = 0;
    d->used = 0;
    return 0;
}

static size_t
arwdict_lookup(const ARWDict *d, const char *key, size_t hash)
{
    size_t mask = ((size_t)1 << d->log2_size) - 1;
    size_t i = hash & mask;
    size_t perturb = hash;

    for (;;) {
        int32_t ix = d->indices[i];
        if (ix == ARWDICT_EMPTY)
            return i;
        if (ix != ARWDICT_DUMMY && strcmp(d->entries[ix].key, key) == 0)
            return i;
        perturb >>= 5;
        i = (5 * i + perturb + 1) & mask;
    }
}

int
arwdict_get(const ARWDict *d, const char *key, ARW *out)
{
    if (d->indices == NULL || d->used == 0)
        return -1;
    size_t hash = arwdict_hash(key);
    size_t slot = arwdict_lookup(d, key, hash);
    int32_t ix = d->indices[slot];
    if (ix < 0)
        return -1;
    *out = d->entries[ix].value;
    return 0;
}

int
arwdict_set(ARWDict *d, const char *key, ARW value)
{
    if (arwdict_ensure_alloc(d) < 0)
        return -1;

    size_t hash = arwdict_hash(key);
    size_t slot = arwdict_lookup(d, key, hash);
    int32_t ix = d->indices[slot];

    if (ix >= 0) {
        d->entries[ix].value = value;
        return 0;
    }

    if (d->usable == 0) {
        if (arwdict_resize(d, d->used * 3) < 0)
            return -1;
        slot = arwdict_lookup(d, key, hash);
    }

    char *dup = strdup(key);
    if (!dup)
        return -1;

    int32_t new_ix = (int32_t)d->nentries;
    d->indices[slot] = new_ix;
    d->entries[new_ix].key = dup;
    d->entries[new_ix].value = value;
    d->nentries++;
    d->usable--;
    d->used++;
    return 0;
}

int
arwdict_del(ARWDict *d, const char *key)
{
    if (d->indices == NULL || d->used == 0)
        return -1;

    size_t hash = arwdict_hash(key);
    size_t slot = arwdict_lookup(d, key, hash);
    int32_t ix = d->indices[slot];
    if (ix < 0)
        return -1;

    d->indices[slot] = ARWDICT_DUMMY;
    free(d->entries[ix].key);
    d->entries[ix].key = NULL;
    d->used--;
    return 0;
}

static int
arwdict_resize(ARWDict *d, size_t minsize)
{
    if (minsize < ARWDICT_MINSIZE)
        minsize = ARWDICT_MINSIZE;
    int8_t log2 = calculate_log2_size(minsize);
    size_t newsize = (size_t)1 << log2;

    int32_t *new_indices = malloc(newsize * sizeof(int32_t));
    if (!new_indices)
        return -1;
    memset(new_indices, 0xff, newsize * sizeof(int32_t));

    size_t new_usable = USABLE_FRACTION(newsize);
    ARWDictEntry *new_entries = malloc(new_usable * sizeof(ARWDictEntry));
    if (!new_entries) {
        free(new_indices);
        return -1;
    }

    size_t new_nentries = 0;
    size_t mask = newsize - 1;
    for (size_t i = 0; i < d->nentries; i++) {
        if (d->entries[i].key == NULL)
            continue;
        size_t hash = arwdict_hash(d->entries[i].key);
        size_t slot = hash & mask;
        size_t perturb = hash;
        while (new_indices[slot] != ARWDICT_EMPTY) {
            perturb >>= 5;
            slot = (5 * slot + perturb + 1) & mask;
        }
        new_indices[slot] = (int32_t)new_nentries;
        new_entries[new_nentries] = d->entries[i];
        new_nentries++;
    }

    free(d->indices);
    free(d->entries);
    d->indices = new_indices;
    d->entries = new_entries;
    d->log2_size = log2;
    d->usable = new_usable - new_nentries;
    d->nentries = new_nentries;
    d->used = new_nentries;
    return 0;
}

int
arwdict_contains(const ARWDict *d, const char *key)
{
    if (d->indices == NULL || d->used == 0)
        return 0;
    size_t hash = arwdict_hash(key);
    size_t slot = arwdict_lookup(d, key, hash);
    return d->indices[slot] >= 0;
}

size_t
arwdict_len(const ARWDict *d)
{
    return d->used;
}

