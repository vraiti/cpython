#ifndef D3G_CONTAINERS_LIST_H
#define D3G_CONTAINERS_LIST_H

#include "../hashmap.h"
#include <stddef.h>

typedef struct {
    ARW *items;
    size_t len;
    size_t allocated;
} ARWList;

void arwlist_init(ARWList *l);
void arwlist_free(ARWList *l);
int arwlist_append(ARWList *l, ARW value);
int arwlist_get(const ARWList *l, size_t index, ARW *out);
int arwlist_set(ARWList *l, size_t index, ARW value);
int arwlist_insert(ARWList *l, size_t index, ARW value);
ARW arwlist_pop(ARWList *l);
size_t arwlist_len(const ARWList *l);

#ifdef Py_PYTHON_H

#include "../hook.h"

typedef struct {
    ObjectTraceData base;
    ARWList arws;
} ListTraceData;

#endif /* Py_PYTHON_H */

#endif
