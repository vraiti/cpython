#ifndef TRACER_HOOKS_H
#define TRACER_HOOKS_H

#include "Python.h"

#define TRACER_API __attribute__((visibility("default")))

struct _PyInterpreterFrame;

TRACER_API void tracer_object_new_hook(PyObject *obj, PyTypeObject *type);
TRACER_API void tracer_setattr_hook(PyObject *obj, PyObject *name, PyObject *value, int result);
TRACER_API PyObject *tracer_getattr_hook(PyObject *obj, PyObject *name, PyObject *result);
TRACER_API PyObject *tracer_getitem_hook(PyObject *o, PyObject *key, PyObject *result);
TRACER_API void tracer_setitem_hook(PyObject *o, PyObject *key, PyObject *value, int result);
TRACER_API void tracer_global_store_hook(PyObject *globals, PyObject *name, PyObject *value);
TRACER_API void tracer_global_load_hook(PyObject *globals, PyObject *name, PyObject *value);
TRACER_API void tracer_global_delete_hook(PyObject *globals, PyObject *name);
TRACER_API void tracer_deref_store_hook(PyObject *cell, PyObject *name, PyObject *value);
TRACER_API void tracer_deref_load_hook(PyObject *cell, PyObject *name, PyObject *value);
TRACER_API void tracer_branch_hook(struct _PyInterpreterFrame *frame, int taken);
TRACER_API void tracer_py_call_hook(struct _PyInterpreterFrame *frame);
TRACER_API void tracer_py_return_hook(struct _PyInterpreterFrame *frame);
TRACER_API void tracer_c_call_hook(PyObject *callable, PyObject *self_obj);
TRACER_API void tracer_c_return_hook(struct _PyInterpreterFrame *frame);
TRACER_API void tracer_shm_open_hook(const char *name);
TRACER_API void tracer_pipe_hook(int fd_read, int fd_write);
TRACER_API void tracer_mkfifo_hook(const char *path);
TRACER_API void tracer_socket_hook(PyObject *address_repr);
TRACER_API void tracer_signal_hook(int signum);
TRACER_API void tracer_mmap_create_hook(PyObject *mmap_obj, int fd, long long offset);
TRACER_API void tracer_mmap_read_hook(PyObject *mmap_obj, long long offset, long long length);
TRACER_API void tracer_mmap_write_hook(PyObject *mmap_obj, long long offset, long long length);
TRACER_API void tracer_after_fork_child_hook(void);
TRACER_API void tracer_fileio_open_hook(PyObject *fileio_obj, int fd);
TRACER_API void tracer_fileio_read_hook(PyObject *fileio_obj, int fd, Py_ssize_t n);
TRACER_API void tracer_fileio_write_hook(PyObject *fileio_obj, int fd, Py_ssize_t n);
TRACER_API void tracer_sem_acquire_hook(const char *name);
TRACER_API void tracer_sem_release_hook(const char *name);
TRACER_API void tracer_enumerate_fds(void);

#endif
