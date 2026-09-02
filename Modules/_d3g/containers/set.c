#include "containers.h"
#include <stdlib.h>
#include <string.h>


/* ======================================================================== */
/* ARWSet — open-addressing hash table keyed by Py_hash_t                    */
/* ======================================================================== */

#define USABLE_FRACTION(n) (((n) << 1) / 3)

static size_t
arwset_lookup(const ARWSet *s, Py_hash_t hash)
{
    size_t i = (size_t)hash & s->mask;
    size_t perturb = (size_t)hash;

    for (;;) {
        ARWSetEntry *entry = &s->table[i];
        if (entry->hash == ARWSET_EMPTY)
            return i;
        if (entry->hash == hash)
            return i;
        if (entry->hash == ARWSET_DUMMY) {
            perturb >>= 5;
            i = (5 * i + perturb + 1) & s->mask;
            continue;
        }
        perturb >>= 5;
        i = (5 * i + perturb + 1) & s->mask;
    }
}

static int arwset_resize(ARWSet *s, size_t minused);

void
arwset_init(ARWSet *s)
{
    memset(s->smalltable, 0, sizeof(s->smalltable));
    s->table = s->smalltable;
    s->mask = ARWSET_MINSIZE - 1;
    s->fill = 0;
    s->used = 0;
    s->finger = 0;
}

void
arwset_free(ARWSet *s)
{
    if (s->table != s->smalltable)
        free(s->table);
    s->table = NULL;
    s->mask = 0;
    s->fill = 0;
    s->used = 0;
}

int
arwset_add(ARWSet *s, Py_hash_t hash, ARW value)
{
    if (hash == ARWSET_EMPTY || hash == ARWSET_DUMMY)
        hash = hash ^ 0x12345678;

    size_t slot = arwset_lookup(s, hash);
    ARWSetEntry *entry = &s->table[slot];

    if (entry->hash == hash) {
        entry->value = value;
        return 0;
    }

    if (entry->hash == ARWSET_EMPTY)
        s->fill++;
    entry->hash = hash;
    entry->value = value;
    s->used++;

    if (s->fill * 3 > (s->mask + 1) * 2) {
        if (arwset_resize(s, s->used > 50000 ? s->used * 2 : s->used * 4) < 0)
            return -1;
    }
    return 0;
}

int
arwset_get(const ARWSet *s, Py_hash_t hash, ARW *out)
{
    if (s->used == 0)
        return -1;
    if (hash == ARWSET_EMPTY || hash == ARWSET_DUMMY)
        hash = hash ^ 0x12345678;

    size_t slot = arwset_lookup(s, hash);
    ARWSetEntry *entry = &s->table[slot];
    if (entry->hash != hash)
        return -1;
    *out = entry->value;
    return 0;
}

int
arwset_del(ARWSet *s, Py_hash_t hash)
{
    if (s->used == 0)
        return -1;
    if (hash == ARWSET_EMPTY || hash == ARWSET_DUMMY)
        hash = hash ^ 0x12345678;

    size_t slot = arwset_lookup(s, hash);
    ARWSetEntry *entry = &s->table[slot];
    if (entry->hash != hash)
        return -1;
    entry->hash = ARWSET_DUMMY;
    s->used--;
    return 0;
}

int
arwset_contains(const ARWSet *s, Py_hash_t hash)
{
    if (s->used == 0)
        return 0;
    if (hash == ARWSET_EMPTY || hash == ARWSET_DUMMY)
        hash = hash ^ 0x12345678;
    size_t slot = arwset_lookup(s, hash);
    return s->table[slot].hash == hash;
}

int
arwset_pop(ARWSet *s, ARW *out)
{
    if (s->used == 0)
        return -1;
    size_t i = s->finger;
    while (i <= s->mask) {
        if (s->table[i].hash != ARWSET_EMPTY &&
            s->table[i].hash != ARWSET_DUMMY) {
            *out = s->table[i].value;
            s->table[i].hash = ARWSET_DUMMY;
            s->used--;
            s->finger = i + 1;
            return 0;
        }
        i++;
    }
    i = 0;
    while (i < s->finger) {
        if (s->table[i].hash != ARWSET_EMPTY &&
            s->table[i].hash != ARWSET_DUMMY) {
            *out = s->table[i].value;
            s->table[i].hash = ARWSET_DUMMY;
            s->used--;
            s->finger = i + 1;
            return 0;
        }
        i++;
    }
    return -1;
}

size_t
arwset_len(const ARWSet *s)
{
    return s->used;
}

static int
arwset_resize(ARWSet *s, size_t minused)
{
    size_t newsize = ARWSET_MINSIZE;
    while (newsize <= minused)
        newsize <<= 1;

    ARWSetEntry *oldtable = s->table;
    size_t oldmask = s->mask;

    ARWSetEntry *newtable;
    if (newsize == ARWSET_MINSIZE && oldtable != s->smalltable) {
        newtable = s->smalltable;
    } else {
        newtable = malloc(newsize * sizeof(ARWSetEntry));
        if (!newtable)
            return -1;
    }
    memset(newtable, 0, newsize * sizeof(ARWSetEntry));

    s->table = newtable;
    s->mask = newsize - 1;
    s->fill = s->used;
    s->finger = 0;

    for (size_t i = 0; i <= oldmask; i++) {
        if (oldtable[i].hash != ARWSET_EMPTY &&
            oldtable[i].hash != ARWSET_DUMMY) {
            size_t j = (size_t)oldtable[i].hash & s->mask;
            while (newtable[j].hash != ARWSET_EMPTY)
                j = (j + 1) & s->mask;
            newtable[j] = oldtable[i];
        }
    }

    if (oldtable != s->smalltable)
        free(oldtable);
    return 0;
}

