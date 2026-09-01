/* D3G tracer: plain data structures that other internal structs embed.
 *
 * Kept free of interpreter dependencies so pycore_tstate.h can include it.
 * The inline accessors live in pycore_d3g_frame.h.
 */

#ifndef Py_INTERNAL_D3G_STRUCTS_H
#define Py_INTERNAL_D3G_STRUCTS_H

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _PyInterpreterFrame;

/* One entry per frame allocated on the thread's data stack, in push
 * order. The frame pointer identifies the slot; call_id is the tracer's
 * per-call identity (0: untraced, UINT64_MAX: taint-excluded). */
typedef struct {
    struct _PyInterpreterFrame *frame;
    uint64_t call_id;
} _PyD3GFrameSlot;

/* Per-thread parallel stack, embedded in _PyThreadStateImpl. Slots are
 * pushed and popped in lockstep with the data stack (see
 * _PyD3G_PushFrame/_PyD3G_PopFrame in pycore_d3g_frame.h). */
typedef struct {
    _PyD3GFrameSlot *slots;
    size_t count;
    size_t cap;
} _PyD3GFrameStack;

/* Tracer data for a generator/coroutine/async generator, stored in extra
 * slots appended to the object's own variable-size allocation, after the
 * embedded frame's localsplus array (see _PyD3G_GenExtras). */
typedef struct {
    uint64_t call_id;          /* the body's call, assigned at first RESUME */
    uint64_t created_id;       /* call that executed RETURN_GENERATOR */
    int32_t created_lineno;    /* line of the creating call expression */
    int32_t _reserved;
} _PyD3GGenExtras;

#ifdef __cplusplus
}
#endif
#endif  // !Py_INTERNAL_D3G_STRUCTS_H
