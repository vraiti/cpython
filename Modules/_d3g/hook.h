#ifndef D3G_HOOK_H
#define D3G_HOOK_H

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

/* Per-object trace data, keyed by PyObject* in g_state.object_extras.
 * `members` is the object record (objects/members tables) built up over
 * the object's lifetime; at deallocation (or at flush for objects still
 * alive) it is moved to the writer thread. */
typedef struct {
    uint64_t id;
    ARWMap attrs;
    char type;
    uint64_t call_id;   /* constructing call; currently always 0 */
    SMap members;       /* attr/key -> child object id */
} ObjectTraceData;

/* Per-frame entry on the trace stack.
 *
 * Control flow is recorded as two independent, never-interleaved streams:
 * `bits` (1 bit per branch/FOR/SEND decision, LSB-first within each byte)
 * and `exc` (a varint-tuple per exception-handler entry: bit position delta
 * since the previous exception in this same stream, handler offset from the
 * function's start, and unwind depth -- how many nested traced calls below
 * this frame were abandoned by the exception). */
typedef struct {
    uint64_t call_id;
    CallRecordData *record;
    uint8_t *bits;
    size_t bit_pos;             /* bits written so far */
    size_t bits_cap;            /* bytes allocated for `bits` */
    uint8_t *exc;
    size_t exc_len;             /* bytes written so far */
    size_t exc_cap;
    size_t last_exc_bit_pos;    /* for delta-encoding the next record's bit_pos */
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
    int traceall;             /* trace every call and class regardless of
                                 prefixes/classes; taint still applies */

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
