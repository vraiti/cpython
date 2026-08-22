#ifndef TRACER_HOOKS_H
#define TRACER_HOOKS_H

#include "Python.h"

#define TRACER_API __attribute__((visibility("default")))

struct _PyInterpreterFrame;

TRACER_API void d3g_object_new_hook(PyObject *obj, PyTypeObject *type);
TRACER_API void d3g_container_dealloc_hook(PyObject *obj);
TRACER_API void d3g_setattr_hook(PyObject *obj, PyObject *name, PyObject *value, int result);
TRACER_API PyObject *d3g_getattr_hook(PyObject *obj, PyObject *name, PyObject *result);
TRACER_API PyObject *d3g_getitem_hook(PyObject *o, PyObject *key, PyObject *result);
TRACER_API void d3g_setitem_hook(PyObject *o, PyObject *key, PyObject *value, int result);
TRACER_API void d3g_global_store_hook(PyObject *globals, PyObject *name, PyObject *value);
TRACER_API void d3g_global_load_hook(PyObject *globals, PyObject *name, PyObject *value);
TRACER_API void d3g_global_delete_hook(PyObject *globals, PyObject *name);
TRACER_API void d3g_deref_store_hook(PyObject *cell, PyObject *name, PyObject *value);
TRACER_API void d3g_deref_load_hook(PyObject *cell, PyObject *name, PyObject *value);
TRACER_API void d3g_branch_hook(struct _PyInterpreterFrame *frame, int taken);
TRACER_API void d3g_py_call_hook(struct _PyInterpreterFrame *frame);
TRACER_API void d3g_py_return_hook(struct _PyInterpreterFrame *frame);
TRACER_API void d3g_c_call_hook(PyObject *callable, PyObject *self_obj);
TRACER_API void d3g_c_return_hook(struct _PyInterpreterFrame *frame);
TRACER_API void d3g_shm_open_hook(const char *name);
TRACER_API void d3g_pipe_hook(int fd_read, int fd_write);
TRACER_API void d3g_mkfifo_hook(const char *path);
TRACER_API void d3g_socket_hook(PyObject *address_repr);
TRACER_API void d3g_signal_hook(int signum);
TRACER_API void d3g_mmap_create_hook(PyObject *mmap_obj, int fd, long long offset);
TRACER_API void d3g_mmap_read_hook(PyObject *mmap_obj, long long offset, long long length);
TRACER_API void d3g_mmap_write_hook(PyObject *mmap_obj, long long offset, long long length);
/* Buffer-protocol exports of hooked io objects (memoryview over mmap):
 * export returns the IoObjectRecord of `exporter` (or NULL); the io hooks
 * take that record and a byte range relative to the exporter's base. */
TRACER_API void *d3g_buffer_export_hook(PyObject *exporter);
TRACER_API void d3g_buffer_read_hook(void *record, long long offset, long long length);
TRACER_API void d3g_buffer_write_hook(void *record, long long offset, long long length);
TRACER_API void d3g_after_fork_child_hook(void);
TRACER_API void d3g_fileio_open_hook(PyObject *fileio_obj, int fd);
TRACER_API void d3g_fileio_read_hook(PyObject *fileio_obj, int fd, Py_ssize_t n);
TRACER_API void d3g_fileio_write_hook(PyObject *fileio_obj, int fd, Py_ssize_t n);
TRACER_API void d3g_sem_acquire_hook(const char *name);
TRACER_API void d3g_sem_release_hook(const char *name);
TRACER_API void d3g_enumerate_fds(void);
TRACER_API void d3g_flush_trace(void);

/* SIG_DFL interception (Modules/_tracer/signals.c). */
TRACER_API PyOS_sighandler_t d3g_setsig_substitute(int sig, PyOS_sighandler_t handler);
TRACER_API PyOS_sighandler_t d3g_setsig_report(PyOS_sighandler_t old);
TRACER_API void d3g_check_pending_signals(void);
TRACER_API void d3g_signals_install(void);
TRACER_API void d3g_signals_uninstall(void);
TRACER_API void d3g_gen_iter_hook(struct _PyInterpreterFrame *caller, int exhausted);

#endif
