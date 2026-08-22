#include "containers.h"
#include <string.h>
#include <stdlib.h>

extern uint64_t get_frame_call_id(PyFrameObject *frame);
extern ObjectTraceData *get_trace_data(PyObject *obj);
extern CallRecordData *current_record(void);

ARW caller_arw(void) {
    ARW e = {0, 0};
    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return e;
    e.caller_id = get_frame_call_id(frame);
    e.call_lineno = PyFrame_GetLineNumber(frame);
    return e;
}

void emit_read(const ARW *arw) {
    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return;
    CallRecordData *rec = current_record();
    if (!rec) return;
    uint64_t caller_id = get_frame_call_id(frame);
    int lineno = PyFrame_GetLineNumber(frame);
    db_add_attr_read(rec, caller_id, arw->call_lineno, lineno);
}

int containers_init(PyObject *module) {
    return 0;
}
