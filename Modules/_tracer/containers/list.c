#include "containers.h"
#include <stdlib.h>
#include <string.h>


/* ======================================================================== */
/* ARWList — dynamic array with CPython's resize algorithm                   */
/* ======================================================================== */

static int
arwlist_resize(ARWList *l, size_t newsize)
{
    size_t new_allocated;
    ARW *items;

    if (l->allocated >= newsize && newsize >= (l->allocated >> 1)) {
        l->len = newsize;
        return 0;
    }

    new_allocated = ((size_t)newsize + (newsize >> 3) + 6) & ~(size_t)3;

    if (newsize - l->len > new_allocated - newsize)
        new_allocated = ((size_t)newsize + 3) & ~(size_t)3;

    if (newsize == 0)
        new_allocated = 0;

    items = realloc(l->items, new_allocated * sizeof(ARW));
    if (items == NULL && new_allocated != 0)
        return -1;

    l->items = items;
    l->len = newsize;
    l->allocated = new_allocated;
    return 0;
}

void
arwlist_init(ARWList *l)
{
    l->items = NULL;
    l->len = 0;
    l->allocated = 0;
}

void
arwlist_free(ARWList *l)
{
    free(l->items);
    l->items = NULL;
    l->len = 0;
    l->allocated = 0;
}

int
arwlist_append(ARWList *l, ARW value)
{
    if (arwlist_resize(l, l->len + 1) < 0)
        return -1;
    l->items[l->len - 1] = value;
    return 0;
}

int
arwlist_get(const ARWList *l, size_t index, ARW *out)
{
    if (index >= l->len)
        return -1;
    *out = l->items[index];
    return 0;
}

int
arwlist_set(ARWList *l, size_t index, ARW value)
{
    if (index >= l->len)
        return -1;
    l->items[index] = value;
    return 0;
}

int
arwlist_insert(ARWList *l, size_t index, ARW value)
{
    if (index > l->len)
        return -1;
    if (arwlist_resize(l, l->len + 1) < 0)
        return -1;
    if (index < l->len - 1)
        memmove(&l->items[index + 1], &l->items[index],
                (l->len - 1 - index) * sizeof(ARW));
    l->items[index] = value;
    return 0;
}

ARW
arwlist_pop(ARWList *l)
{
    ARW value = l->items[l->len - 1];
    arwlist_resize(l, l->len - 1);
    return value;
}

size_t
arwlist_len(const ARWList *l)
{
    return l->len;
}

