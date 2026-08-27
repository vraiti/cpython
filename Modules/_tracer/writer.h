#ifndef TRACER_WRITER_H
#define TRACER_WRITER_H

/* Online trace serialization.
 *
 * Completed records are handed to a bounded ring buffer and consumed by a
 * dedicated writer thread that appends them to $PYTHON_TRACER_OUTDIR/{pid}.db
 * while the program runs, so process memory holds only records that are
 * still being built (active calls, live objects) plus the ring itself.
 *
 * Producers run with the GIL held (every hook does); the writer thread never
 * touches the interpreter, only private C memory and SQLite. A full ring
 * blocks the producer until the writer drains it, which bounds memory at the
 * cost of back-pressure on the traced program.
 *
 * Every push transfers ownership of the event payload to the writer, which
 * frees it after writing. If the writer is not accepting (never started,
 * already stopped, or failed), the payload is freed immediately. */

#include <stdint.h>
#include <stddef.h>
#include "hashmap.h"
#include "records.h"

typedef enum {
    EV_CALL,
    EV_OBJECT,
    EV_FUNCTION,
    EV_IPC,
    EV_IO_OBJECT,
    EV_IO_OP,
    EV_CALL_ARG,
    EV_STOP,
} EventKind;

typedef struct {
    EventKind kind;
    union {
        CallRecordData *call;                      /* owned */
        struct { uint64_t id; uint64_t call_id; SMap members; } object;
        struct { int32_t id; char *ref; uint8_t *cfg; size_t cfg_len; } function; /* ref, cfg owned */
        struct { char *name; int64_t call_id; } ipc;              /* name owned */
        struct { uint32_t id; char *name; uint64_t offset; } io_object; /* name owned */
        struct { uint32_t io_object_id; uint64_t call_id, offset, length;
                 int op_type; } io_op;
        struct { uint64_t call_id; char *name; int32_t obj_idx; } call_arg; /* name owned */
    } u;
} TraceEvent;

/* Start the writer thread for this process. Safe to call when already
 * running (no-op). Returns 0 on success, -1 if the thread could not be
 * created; in that case pushes free their payloads and nothing is written. */
int writer_start(void);

/* Hand an event to the writer. Takes ownership of the payload. Blocks while
 * the ring is full. */
void writer_push(const TraceEvent *ev);

/* Push the stop sentinel and join the writer thread. Everything pushed
 * before this call is written before it returns. Idempotent. */
void writer_stop(void);

/* Discard the ring and the parent's thread bookkeeping in a forked child.
 * The inherited SQLite handle is abandoned, never closed: the parent still
 * owns the file. Called from the pthread_atfork child handler. */
void writer_after_fork_child(void);

int writer_running(void);

/* Convenience constructors; each pushes immediately. */
void writer_push_call(CallRecordData *rec);
void writer_push_object(uint64_t id, uint64_t call_id, SMap *members);
void writer_push_function(int32_t id, const char *ref, const uint8_t *cfg, size_t cfg_len);
void writer_push_ipc(const char *name, int64_t call_id);
void writer_push_io_object(uint32_t id, const char *name, uint64_t offset);
void writer_push_io_op(uint32_t io_object_id, uint64_t call_id,
                       uint64_t offset, uint64_t length, int op_type);
void writer_push_call_arg(uint64_t call_id, const char *name, int32_t obj_idx);

#endif
