/* D3G: where a frame's call_id lives.
 *
 * Nothing is added to _PyInterpreterFrame or to the generator objects that
 * embed one: both structs are compiled into prebuilt extensions that read
 * their fields by offset (PyTorch's Dynamo includes
 * pycore_interpframe_structs.h directly), so their layout must stay
 * byte-identical to upstream CPython. The per-frame data is kept
 * elsewhere, chosen by who owns the frame:
 *
 * - FRAME_OWNED_BY_THREAD: the frame lives on the thread's data stack,
 *   whose memory is reused as soon as the frame is popped. A parallel
 *   stack on the thread state (_PyThreadStateImpl.d3g_frames) gets one
 *   slot per data-stack allocation, pushed by the three allocation
 *   helpers and popped by _PyThreadState_PopFrame. Lookups walk down from
 *   the top; the executing frame is the top slot, its caller the next.
 *
 * - FRAME_OWNED_BY_GENERATOR: the frame is embedded by value in the
 *   generator object for the object's whole lifetime and is never on the
 *   data stack. The generator is allocated with _PyD3G_GEN_EXTRA_SLOTS
 *   extra items after the frame's localsplus array, and the data lives
 *   there (_PyD3GGenExtras). It is freed with the object, so no dealloc
 *   hook or side table is needed.
 *
 * - Everything else (interpreter entry frames, frame objects that took
 *   ownership of a finished frame): no storage; reads yield 0 and writes
 *   are dropped. No hook records against those.
 *
 * Frames allocated on the data stack by code that bypasses the helpers
 * (an extension's own copy of them) have no slot: reads yield 0, and the
 * pointer-checked pop leaves the stack consistent.
 *
 * This header is included by pycore_interpframe.h once
 * _PyFrame_NumSlotsForCodeObject is defined; include that header, not
 * this one.
 */

#ifndef Py_INTERNAL_D3G_FRAME_H
#define Py_INTERNAL_D3G_FRAME_H

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#ifndef Py_INTERNAL_INTERP_FRAME_H
#  error "include pycore_interpframe.h instead of this header"
#endif

#include "pycore_d3g_structs.h"
#include "pycore_tstate.h"          // _PyThreadStateImpl

#ifdef __cplusplus
extern "C" {
#endif

/* Out-of-line parts (Python/pystate.c). */
extern int _PyD3G_FrameStackGrow(_PyD3GFrameStack *st);
extern void _PyD3G_FrameStackPopSlow(_PyD3GFrameStack *st,
                                     struct _PyInterpreterFrame *frame);
extern _PyD3GFrameSlot *_PyD3G_FrameStackFind(_PyD3GFrameStack *st,
                                              struct _PyInterpreterFrame *frame);
extern void _PyD3G_FrameStackClear(_PyD3GFrameStack *st);

static inline _PyD3GFrameStack *
_PyD3G_FrameStackOf(PyThreadState *tstate)
{
    return &((_PyThreadStateImpl *)tstate)->d3g_frames;
}

/* Called by every data-stack frame allocation, right after datastack_top
 * is bumped. On allocation failure the frame simply has no slot. */
static inline void
_PyD3G_PushFrame(PyThreadState *tstate, struct _PyInterpreterFrame *frame)
{
    _PyD3GFrameStack *st = _PyD3G_FrameStackOf(tstate);
    if (st->count == st->cap && _PyD3G_FrameStackGrow(st) < 0) {
        return;
    }
    st->slots[st->count].frame = frame;
    st->slots[st->count].call_id = 0;
    st->count++;
}

/* Called by _PyThreadState_PopFrame. The popped frame is normally the
 * top slot; otherwise the slow path searches for it and discards
 * everything above (or does nothing if the frame was never recorded). */
static inline void
_PyD3G_PopFrame(PyThreadState *tstate, struct _PyInterpreterFrame *frame)
{
    _PyD3GFrameStack *st = _PyD3G_FrameStackOf(tstate);
    if (st->count > 0 && st->slots[st->count - 1].frame == frame) {
        st->count--;
        return;
    }
    _PyD3G_FrameStackPopSlow(st, frame);
}

/* Generator extras: items appended to the generator's allocation. */
#define _PyD3G_GEN_EXTRA_SLOTS \
    ((int)((sizeof(_PyD3GGenExtras) + sizeof(_PyStackRef) - 1) / sizeof(_PyStackRef)))

/* For use before the frame has been copied into the generator (make_gen),
 * when gi_iframe.f_executable is not yet the code object. */
static inline _PyD3GGenExtras *
_PyD3G_GenExtrasForCode(PyGenObject *gen, PyCodeObject *code)
{
    return (_PyD3GGenExtras *)
        &gen->gi_iframe.localsplus[_PyFrame_NumSlotsForCodeObject(code)];
}

static inline _PyD3GGenExtras *
_PyD3G_GenExtras(PyGenObject *gen)
{
    return _PyD3G_GenExtrasForCode(gen, _PyFrame_GetCode(&gen->gi_iframe));
}

#ifdef __cplusplus
}
#endif
#endif  // !Py_INTERNAL_D3G_FRAME_H
