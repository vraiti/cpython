#ifndef TRACER_HOOKS_H
#define TRACER_HOOKS_H

#include "Python.h"

struct _PyInterpreterFrame;

void tracer_object_new_hook(PyObject *obj, PyTypeObject *type);
void tracer_setattr_hook(PyObject *obj, PyObject *name, PyObject *value, int result);
PyObject *tracer_getattr_hook(PyObject *obj, PyObject *name, PyObject *result);
PyObject *tracer_getitem_hook(PyObject *o, PyObject *key, PyObject *result);
void tracer_setitem_hook(PyObject *o, PyObject *key, PyObject *value, int result);
void tracer_global_store_hook(PyObject *globals, PyObject *name, PyObject *value);
void tracer_global_load_hook(PyObject *globals, PyObject *name, PyObject *value);
void tracer_global_delete_hook(PyObject *globals, PyObject *name);
void tracer_deref_store_hook(PyObject *cell, PyObject *name, PyObject *value);
void tracer_deref_load_hook(PyObject *cell, PyObject *name, PyObject *value);
void tracer_branch_hook(struct _PyInterpreterFrame *frame, int taken);
void tracer_py_call_hook(struct _PyInterpreterFrame *frame);
void tracer_py_return_hook(struct _PyInterpreterFrame *frame);
void tracer_c_call_hook(PyObject *callable, PyObject *self_obj);
void tracer_c_return_hook(struct _PyInterpreterFrame *frame);
void tracer_shm_open_hook(const char *name);

#endif
