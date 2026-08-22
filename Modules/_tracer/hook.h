#ifndef TRACER_HOOK_H
#define TRACER_HOOK_H

#include <Python.h>
#include <stdint.h>
#include "hashmap.h"
#include "records.h"
#include "filter.h"

#define Py_IS_PRIMITIVE(obj) ( \
    PyLong_Check(obj) || PyFloat_Check(obj) || PyBool_Check(obj) || \
    PyUnicode_Check(obj) || PyBytes_Check(obj) || \
    (obj) == Py_None || (obj) == Py_True || (obj) == Py_False \
)

/* Per-object trace data stored in PyTrace_Hashtable */
typedef struct {
    uint64_t id;
    ARWMap attrs;
    char type;
} ObjectTraceData;

/* Per-frame entry on the trace stack */
typedef struct {
    uint64_t call_id;
    CallRecordData *record;
    uint8_t *branch_buf;
    size_t branch_len;
    size_t branch_cap;
} FrameEntry;

/* Per-(thread, coroutine) frame stack */
typedef struct {
    FrameEntry *entries;
    size_t count;
    size_t cap;
} FrameStack;

/* Global trace state */
typedef struct {
    uint64_t next_call_id;
    int enabled;

    PyObject *db;             /* DatabaseObject* */
    PyObject *filter;         /* PathFilterObject* */

    char **prefixes;
    Py_ssize_t prefix_count;
    UMap scope_cache;

    char **taint_patterns;
    Py_ssize_t taint_count;

    /* AST data */
    SMap func_to_id;          /* ref_str -> int32_t (cast from void*) */
    int32_t next_func_id;

    /* Object extras: (uintptr_t)PyObject* -> (intptr_t)ObjectTraceData* */
    UMap object_extras;

    /* I/O object tracking: (uintptr_t)PyObject* -> (intptr_t)IoObjectRecord* */
    UMap io_object_records;
} TraceState;

extern TraceState g_state;

int hook_init(PyObject *module);

#endif
