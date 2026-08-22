#ifndef TRACER_CONTAINERS_H
#define TRACER_CONTAINERS_H

#include <Python.h>
#include <stdint.h>
#include "../hook.h"

typedef enum {
    CONTAINER_NONE,
    CONTAINER_DICT,
    CONTAINER_LIST,
    CONTAINER_DEQUE,
    CONTAINER_SET,
    CONTAINER_TUPLE,     /* immutable: read tracking only */
    CONTAINER_BYTEARRAY, /* mutable byte buffer: index read/write tracking */
    /* TUPLE and BYTEARRAY use the generic ObjectTraceData.attrs map keyed
     * by index; they have no type-specific ARW structure
     * (see new_typed_container_trace_data). */
} ContainerType;

#include "dict.h"
#include "list.h"
#include "deque.h"
#include "set.h"

ARW caller_arw(void);
void emit_read(const ARW *arw);

int containers_init(PyObject *module);

#endif
