#ifndef D3G_RECORDS_H
#define D3G_RECORDS_H

#include <Python.h>
#include <stdint.h>
#include "hashmap.h"

/* ---- Plain C record structs (no PyObject_HEAD) ---- */

typedef struct {
    uint64_t caller_id;
    int32_t write_call_lineno;
    int32_t read_call_lineno;
} AttrRecordReadData;

/* A call record lives in the database's live list from the call's RESUME
 * until its return, accumulating attr_reads and (at return) control_flow.
 * On return it is unlinked and handed to the writer thread (writer.h),
 * which owns and frees it after serialization. Records never move while
 * live: FrameEntry.record holds a pointer for the call's lifetime. */
typedef struct CallRecordData {
    uint64_t call_id;
    int32_t function_id;
    uint64_t caller_id;
    int32_t call_lineno;
    int32_t obj_id;
    int32_t func_idx;      /* trace id of the function object executed, 0 if untracked */
    uint64_t created_id;   /* generator/coroutine bodies: the creating call */
    int32_t created_lineno; /* line of the creating call expression */
    uint8_t *control_flow;
    Py_ssize_t control_flow_len;
    AttrRecordReadData *attr_reads;
    Py_ssize_t attr_reads_len;
    Py_ssize_t attr_reads_cap;
    struct CallRecordData *prev, *next;   /* live list links */
} CallRecordData;

typedef struct IoObjectRecord {
    char *name;
    uint64_t offset;
    uint32_t id;
} IoObjectRecord;

typedef enum {
    IO_OP_READ,
    IO_OP_WRITE,
} IoOpType;

/* ---- Database (Python type holding the in-flight state) ---- */

typedef struct {
    PyObject_HEAD
    CallRecordData *live_head;     /* calls that have not returned */
    Py_ssize_t live_count;
    uint64_t calls_total;          /* calls recorded in this process */
    uint32_t next_io_object_id;
} DatabaseObject;

extern PyTypeObject *DatabaseType;

/* ---- Database helpers ---- */

/* Allocate a call record and link it into the live list. */
CallRecordData *db_add_call(DatabaseObject *db,
                            uint64_t call_id, int32_t function_id,
                            uint64_t caller_id, int32_t call_lineno,
                            int32_t obj_id);

/* Unlink a returned call and hand it to the writer. */
void db_complete_call(DatabaseObject *db, CallRecordData *rec);

/* Hand every live call to the writer without unlinking semantics mattering
 * (used at flush; tracing must be disabled first so no hook touches the
 * records afterwards). */
void db_complete_all_calls(DatabaseObject *db);

/* Allocate an io object record, assign its id, and emit its row. The
 * returned record stays owned by the caller (it is referenced from the
 * io_object_records map for the lifetime of the Python object). */
IoObjectRecord *db_add_io_object(DatabaseObject *db, const char *name,
                                 uint64_t offset);

void db_add_attr_read(CallRecordData *rec,
                      uint64_t caller_id,
                      int32_t write_call_lineno,
                      int32_t read_call_lineno);

/* Free every live call record (forked child: the parent owns that history). */
void db_clear_records(DatabaseObject *db);

int records_init(PyObject *module);

#endif
