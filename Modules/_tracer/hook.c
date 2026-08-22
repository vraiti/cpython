#include "hook.h"
#include "tracer_hooks.h"
#include "containers/containers.h"
#include "writer.h"
#include "pycore_frame.h"
#include "pycore_interpframe.h"
#include "pycore_interpframe_structs.h"
#include "pycore_genobject.h"
#include "pycore_stackref.h"
#include "pycore_code.h"          /* _Py_GetBaseCodeUnit */
#include "opcode_ids.h"           /* FOR_ITER */
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>

/* forward declarations */
CallRecordData *current_record(void);


TraceState g_state = {0};

/* ---- object extras helpers ---- */

/* ---- frame call_id helpers ---- */

void set_frame_call_id(PyFrameObject *frame, uint64_t cid) {
    frame->f_frame->call_id = cid;
}

uint64_t get_frame_call_id(PyFrameObject *frame) {
    return frame->f_frame->call_id;
}

/* ---- object extras helpers ---- */

ObjectTraceData *get_trace_data(PyObject *obj) {
    intptr_t val;
    if (umap_get(&g_state.object_extras, (uintptr_t)obj, &val))
        return (ObjectTraceData *)val;
    return NULL;
}

static void free_trace_data(ObjectTraceData *data) {
    switch (data->type) {
    case CONTAINER_DICT: {
        DictTraceData *td = (DictTraceData *)data;
        arwdict_free(&td->arws);
        break;
    }
    case CONTAINER_LIST: {
        ListTraceData *td = (ListTraceData *)data;
        arwlist_free(&td->arws);
        break;
    }
    case CONTAINER_DEQUE: {
        DequeTraceData *td = (DequeTraceData *)data;
        arwdeque_free(&td->arws);
        break;
    }
    case CONTAINER_SET: {
        SetTraceData *td = (SetTraceData *)data;
        arwset_free(&td->arws);
        break;
    }
    default:
        break;
    }
    ARWMap_free(&data->attrs);
    SMap_free(&data->members);
    free(data);
}

/* Hand the object's record (objects/members rows) to the writer. The
 * members map is moved out, leaving an empty map behind. */
static void emit_object_record(ObjectTraceData *td) {
    writer_push_object(td->id, td->call_id, &td->members);
}

/* Tracked objects are finalized from their type's tp_dealloc
 * (d3g_container_dealloc_hook): subtype_dealloc for heap-type instances,
 * and the per-type dealloc functions for dict/list/set/deque/tuple/
 * bytearray. A weakref callback was used previously, but the weakref was
 * released immediately after creation, so its callback never ran and
 * entries outlived their objects. */


/* ---- per-coroutine frame stacks (thread-local) ---- */

static _Thread_local UMap tl_coro_stacks = {0};
static _Thread_local int tl_stacks_init = 0;

static uintptr_t get_coroutine_id(PyFrameObject *frame) {
    _PyInterpreterFrame *iframe = frame->f_frame;
    if (iframe->owner == FRAME_OWNED_BY_GENERATOR)
        return (uintptr_t)_PyGen_GetGeneratorFromFrame(iframe);
    return 0;
}

static FrameStack *get_frame_stack(PyFrameObject *frame) {
    if (!tl_stacks_init) {
        umap_init(&tl_coro_stacks, 4);
        tl_stacks_init = 1;
    }
    uintptr_t coro_id = get_coroutine_id(frame);
    intptr_t val;
    if (umap_get(&tl_coro_stacks, coro_id, &val))
        return (FrameStack *)val;
    FrameStack *stack = calloc(1, sizeof(FrameStack));
    umap_set(&tl_coro_stacks, coro_id, (intptr_t)stack);
    return stack;
}

static void frame_stack_push(PyFrameObject *frame, FrameEntry *entry) {
    FrameStack *stack = get_frame_stack(frame);
    if (stack->count >= stack->cap) {
        stack->cap = stack->cap ? stack->cap * 2 : 64;
        stack->entries = realloc(stack->entries, stack->cap * sizeof(FrameEntry));
    }
    stack->entries[stack->count++] = *entry;
}

static FrameEntry *frame_stack_peek(PyFrameObject *frame) {
    FrameStack *stack = get_frame_stack(frame);
    if (stack->count == 0) return NULL;
    return &stack->entries[stack->count - 1];
}

static void frame_stack_pop(PyFrameObject *frame) {
    FrameStack *stack = get_frame_stack(frame);
    if (stack->count > 0) {
        FrameEntry *e = &stack->entries[--stack->count];
        free(e->branch_buf);
    }
}

/* ---- scope check ---- */

static int check_scope(uintptr_t filename_ptr, const char *filename) {
    if (g_state.traceall) return 1;
    intptr_t cached;
    if (umap_get(&g_state.scope_cache, filename_ptr, &cached))
        return (int)cached;
    int result = 0;
    for (Py_ssize_t i = 0; i < g_state.prefix_count; i++) {
        if (strncmp(filename, g_state.prefixes[i], strlen(g_state.prefixes[i])) == 0) {
            result = 1;
            break;
        }
    }
    umap_set(&g_state.scope_cache, filename_ptr, result);
    return result;
}

/* ---- get_or_assign_function_id ---- */

static int32_t get_or_assign_function_id(const char *ref_str) {
    void *val;
    if (SMap_get(&g_state.func_to_id, ref_str, &val))
        return (int32_t)(intptr_t)val;
    int32_t id = g_state.next_func_id++;
    SMap_set(&g_state.func_to_id, ref_str, (void *)(intptr_t)id);
    writer_push_function(id, ref_str);
    return id;
}

/* ---- get_self_obj_id ---- */

static PyObject *get_self_obj(PyObject *py_frame, PyCodeObject *code) {
    if (code->co_argcount < 1)
        return NULL;
    PyFrameObject *frame = (PyFrameObject *)py_frame;
    _PyInterpreterFrame *iframe = (_PyInterpreterFrame *)frame->f_frame;
    if (!iframe) return NULL;
    PyObject *self_obj = PyStackRef_AsPyObjectBorrow(iframe->localsplus[0]);
    return self_obj;
}

static int32_t get_obj_id(PyObject *self_obj) {
    if (!self_obj) return 0;
    ObjectTraceData *td = get_trace_data(self_obj);
    if (!td) return 0;
    return (int32_t)td->id;
}

/* ---- is_tracked_type: check type without needing a code object ---- */

static int is_tracked_type(PyTypeObject *type) {
    if (g_state.traceall) return 1;
    PathFilterObject *pf = (PathFilterObject *)g_state.filter;
    if (!pf) return 0;

    /* check explicit tracked_classes list */
    PyObject *module = PyObject_GetAttrString((PyObject *)type, "__module__");
    PyObject *qualname_attr = PyObject_GetAttrString((PyObject *)type, "__qualname__");
    if (module && qualname_attr) {
        const char *mod_str = PyUnicode_AsUTF8(module);
        const char *qual_str = PyUnicode_AsUTF8(qualname_attr);
        if (mod_str && qual_str) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s.%s", mod_str, qual_str);
            if (SMap_contains(&pf->tracked_classes, buf)) {
                Py_DECREF(module);
                Py_DECREF(qualname_attr);
                return 1;
            }
        }
    }
    Py_XDECREF(module);
    Py_XDECREF(qualname_attr);

    /* check if type.__init__'s source file is under a traced prefix */
    PyObject *init = PyObject_GetAttrString((PyObject *)type, "__init__");
    if (!init) { PyErr_Clear(); return 0; }
    PyObject *code_obj = PyObject_GetAttrString(init, "__code__");
    Py_DECREF(init);
    if (!code_obj) { PyErr_Clear(); return 0; }

    PyCodeObject *code = (PyCodeObject *)code_obj;
    const char *filename = PyUnicode_AsUTF8(code->co_filename);
    int result = 0;
    if (filename) {
        for (Py_ssize_t i = 0; i < pf->prefix_count; i++) {
            if (strncmp(filename, pf->prefixes[i], strlen(pf->prefixes[i])) == 0) {
                result = 1;
                break;
            }
        }
    }
    Py_DECREF(code_obj);
    return result;
}

static uint64_t next_trace_data_id = 0;

static void init_trace_data(ObjectTraceData *trace_data, ContainerType type) {
    trace_data->id = next_trace_data_id++;
    ARWMap_init(&trace_data->attrs, 16);
    trace_data->type = type;
    trace_data->call_id = 0;
    SMap_init(&trace_data->members, 8);
}

static ObjectTraceData *new_trace_data(PyObject *obj) {
    ObjectTraceData *trace_data = malloc(sizeof(ObjectTraceData));
    init_trace_data(trace_data, CONTAINER_NONE);
    umap_set(&g_state.object_extras, (uintptr_t)obj, (intptr_t)trace_data);
    return trace_data;
}

/* collections.deque, resolved lazily on first use. Not exposed via any
 * public type-check macro, so this is the only way to identify it (and
 * its subclasses) by actual type identity rather than by name. */
static PyTypeObject *get_deque_type(void) {
    static PyTypeObject *cached = NULL;
    static int resolved = 0;
    if (!resolved) {
        resolved = 1;
        PyObject *collections_mod = PyImport_ImportModule("collections");
        if (collections_mod) {
            PyObject *deque_type = PyObject_GetAttrString(collections_mod, "deque");
            Py_DECREF(collections_mod);
            if (deque_type && PyType_Check(deque_type)) {
                cached = (PyTypeObject *)deque_type;
            } else {
                Py_XDECREF(deque_type);
            }
        } else {
            PyErr_Clear();
        }
    }
    return cached;
}

/* classify a container instance for tracking (dict/list/set get
 * append/add/etc. dataflow via classify_container_call; tuple is
 * read-only tracking since it can never receive a setitem). */
static ContainerType classify_container_type(PyObject *obj) {
    if (PyDict_Check(obj)) return CONTAINER_DICT;
    if (PyList_Check(obj)) return CONTAINER_LIST;
    if (PyAnySet_Check(obj)) return CONTAINER_SET;
    PyTypeObject *deque_type = get_deque_type();
    if (deque_type && PyObject_TypeCheck(obj, deque_type))
        return CONTAINER_DEQUE;
    if (PyTuple_Check(obj)) return CONTAINER_TUPLE;
    if (PyByteArray_Check(obj)) return CONTAINER_BYTEARRAY;
    return CONTAINER_NONE;
}

static ObjectTraceData *new_typed_container_trace_data(PyObject *obj, ContainerType type) {
    size_t size;
    switch (type) {
    case CONTAINER_DICT:  size = sizeof(DictTraceData); break;
    case CONTAINER_LIST:  size = sizeof(ListTraceData); break;
    case CONTAINER_SET:   size = sizeof(SetTraceData); break;
    case CONTAINER_DEQUE: size = sizeof(DequeTraceData); break;
    default:              size = sizeof(ObjectTraceData); break;
    }
    ObjectTraceData *trace_data = malloc(size);
    init_trace_data(trace_data, type);
    switch (type) {
    case CONTAINER_DICT:  arwdict_init(&((DictTraceData *)trace_data)->arws); break;
    case CONTAINER_LIST:  arwlist_init(&((ListTraceData *)trace_data)->arws); break;
    case CONTAINER_SET:   arwset_init(&((SetTraceData *)trace_data)->arws); break;
    case CONTAINER_DEQUE: arwdeque_init(&((DequeTraceData *)trace_data)->arws); break;
    default: break;
    }
    umap_set(&g_state.object_extras, (uintptr_t)obj, (intptr_t)trace_data);
    return trace_data;
}

/* Attach container-specific tracking to a plain dict/list/set/deque the
 * first time it becomes reachable from a tracked object (as an attribute
 * value); relies on d3g_container_dealloc_hook for finalization, same as
 * attach_container_trace_data. */
static ObjectTraceData *attach_typed_container_trace_data(PyObject *obj, ContainerType type) {
    return new_typed_container_trace_data(obj, type);
}

/* For heap-type instances tracked via object_new. Finalized from
 * subtype_dealloc (Objects/typeobject.c) through d3g_container_dealloc_hook. */
static void attach_trace_data(PyObject *obj) {
    new_trace_data(obj);
}

/* For dict/list/set/deque instances: dict and list don't support weakrefs
 * at all, so all four container types are instead finalized directly from
 * their type's dealloc function (see d3g_container_dealloc_hook). */
static void attach_container_trace_data(PyObject *obj) {
    new_trace_data(obj);
}

/* ---- object_new_hook: CPython hook called at end of object_new ---- */

void d3g_object_new_hook(PyObject *obj, PyTypeObject *type) {
    if (!g_state.enabled) return;
    if (get_trace_data(obj)) return;
    if (!is_tracked_type(type)) return;

    attach_trace_data(obj);
}

/* ---- setattr hook: CPython hook called after PyObject_GenericSetAttr succeeds ---- */

void d3g_setattr_hook(PyObject *obj, PyObject *name, PyObject *value, int result) {
    if (!g_state.enabled) return;

    ObjectTraceData *td = get_trace_data(obj);
    if (!td) return;

    const char *name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return;

    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return;

    uint64_t caller_id = get_frame_call_id(frame);
    int call_lineno = PyFrame_GetLineNumber(frame);
    ARW arw = {caller_id, call_lineno};
    ARWMap_set(&td->attrs, name_str, arw);

    if (!value || Py_IS_PRIMITIVE(value)) return;

    ObjectTraceData *val_td = get_trace_data(value);
    if (!val_td) {
        ContainerType ctype = classify_container_type(value);
        if (ctype != CONTAINER_NONE) {
            val_td = attach_typed_container_trace_data(value, ctype);
        }
    }
    if (val_td) {
        SMap_set(&td->members, name_str, (void *)(intptr_t)val_td->id);
    }
}

/* ---- getattr hook: CPython hook called after PyObject_GenericGetAttr succeeds ---- */

PyObject *d3g_getattr_hook(PyObject *obj, PyObject *name, PyObject *result) {
    if (!g_state.enabled) return result;

    const char *name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return result;

    if (name_str[0] == '_' && name_str[1] == '_')
        return result;

    ObjectTraceData *td = get_trace_data(obj);
    if (!td) return result;

    ARW arw;
    if (!ARWMap_get(&td->attrs, name_str, &arw))
        return result;

    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return result;

    CallRecordData *rec = current_record();
    if (rec) {
        uint64_t caller_id = get_frame_call_id(frame);
        int read_lineno = PyFrame_GetLineNumber(frame);
        db_add_attr_read(rec, caller_id, arw.call_lineno, read_lineno);
    }

    return result;
}

/* ---- global variable hooks: treat f_globals as a tracked object ---- */

static ObjectTraceData *get_or_create_globals_trace(PyObject *globals) {
    ObjectTraceData *td = get_trace_data(globals);
    if (td) return td;

    attach_container_trace_data(globals);
    return get_trace_data(globals);
}

/* ---- container dealloc hook: called from dict/list/set/deque's own
 * tp_dealloc, since dict and list can't hold weakrefs and it's simpler to
 * finalize all four container types the same way. ---- */

void d3g_container_dealloc_hook(PyObject *obj) {
    intptr_t val;
    if (umap_get(&g_state.object_extras, (uintptr_t)obj, &val)) {
        umap_delete(&g_state.object_extras, (uintptr_t)obj);
        ObjectTraceData *td = (ObjectTraceData *)val;
        /* The record is final once the object is gone. After flush the
         * writer discards the payload, so objects already emitted at
         * flush are not written twice. */
        emit_object_record(td);
        free_trace_data(td);
    }
}

void d3g_global_store_hook(PyObject *globals, PyObject *name, PyObject *value) {
    if (!g_state.enabled) return;

    ObjectTraceData *td = get_or_create_globals_trace(globals);
    if (!td) return;

    const char *name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return;

    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return;

    uint64_t caller_id = get_frame_call_id(frame);
    int call_lineno = PyFrame_GetLineNumber(frame);
    ARW arw = {caller_id, call_lineno};
    ARWMap_set(&td->attrs, name_str, arw);

    if (!value || Py_IS_PRIMITIVE(value)) return;

    ObjectTraceData *val_td = get_trace_data(value);
    if (val_td) {
        SMap_set(&td->members, name_str, (void *)(intptr_t)val_td->id);
    }
}

void d3g_global_load_hook(PyObject *globals, PyObject *name, PyObject *value) {
    if (!g_state.enabled) return;

    const char *name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return;

    ObjectTraceData *td = get_trace_data(globals);
    if (!td) return;

    ARW arw;
    if (!ARWMap_get(&td->attrs, name_str, &arw))
        return;

    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return;

    CallRecordData *rec = current_record();
    if (rec) {
        uint64_t caller_id = get_frame_call_id(frame);
        int read_lineno = PyFrame_GetLineNumber(frame);
        db_add_attr_read(rec, caller_id, arw.call_lineno, read_lineno);
    }
}

void d3g_global_delete_hook(PyObject *globals, PyObject *name) {
    if (!g_state.enabled) return;

    ObjectTraceData *td = get_trace_data(globals);
    if (!td) return;

    const char *name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return;

    ARWMap_delete(&td->attrs, name_str);
}

/* ---- branch hook ---- */

void d3g_branch_hook(_PyInterpreterFrame *frame, int taken) {
    if (!g_state.enabled) return;
    /* 0: untraced frame; UINT64_MAX: taint-excluded frame, whose decisions
     * must not be appended to the enclosing traced call. */
    if (frame->call_id == 0 || frame->call_id == UINT64_MAX) return;

    PyFrameObject *frame_obj = frame->frame_obj;
    if (!frame_obj) return;

    FrameEntry *entry = frame_stack_peek(frame_obj);
    if (!entry) return;

    if (entry->branch_len >= entry->branch_cap) {
        entry->branch_cap = entry->branch_cap ? entry->branch_cap * 2 : 32;
        entry->branch_buf = realloc(entry->branch_buf, entry->branch_cap);
        if (!entry->branch_buf) return;
    }
    entry->branch_buf[entry->branch_len++] = taken ? 1 : 0;
}

/* ---- item hooks (subscript operations) ---- */

/* Item keys are arbitrary objects, not just strings, so the ARWMap key
 * used for attribute tracking (a C string) has to be derived: strings are
 * used directly, everything else falls back to repr(). *repr_out is set
 * to a new reference the caller must Py_XDECREF once done with the
 * returned pointer, or left NULL if no repr was needed/possible. */
static const char *item_key_str(PyObject *key, PyObject **repr_out) {
    *repr_out = NULL;
    if (PyUnicode_Check(key)) {
        return PyUnicode_AsUTF8(key);
    }
    PyObject *repr = PyObject_Repr(key);
    if (!repr) {
        PyErr_Clear();
        return NULL;
    }
    const char *s = PyUnicode_AsUTF8(repr);
    if (!s) {
        Py_DECREF(repr);
        PyErr_Clear();
        return NULL;
    }
    *repr_out = repr;
    return s;
}

PyObject *d3g_getitem_hook(PyObject *o, PyObject *key, PyObject *result) {
    if (!g_state.enabled) return result;

    ObjectTraceData *td = get_trace_data(o);
    if (!td) return result;

    PyObject *repr = NULL;
    const char *key_str = item_key_str(key, &repr);
    if (!key_str) return result;

    ARW arw;
    int found = ARWMap_get(&td->attrs, key_str, &arw);
    Py_XDECREF(repr);
    if (!found) return result;

    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return result;

    CallRecordData *rec = current_record();
    if (rec) {
        uint64_t caller_id = get_frame_call_id(frame);
        int read_lineno = PyFrame_GetLineNumber(frame);
        db_add_attr_read(rec, caller_id, arw.call_lineno, read_lineno);
    }

    return result;
}

void d3g_setitem_hook(PyObject *o, PyObject *key, PyObject *value, int result) {
    if (!g_state.enabled) return;
    if (result != 0) return;

    ObjectTraceData *td = get_trace_data(o);
    if (!td) return;

    PyObject *repr = NULL;
    const char *key_str = item_key_str(key, &repr);
    if (!key_str) return;

    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) {
        Py_XDECREF(repr);
        return;
    }

    uint64_t caller_id = get_frame_call_id(frame);
    int call_lineno = PyFrame_GetLineNumber(frame);
    ARW arw = {caller_id, call_lineno};
    ARWMap_set(&td->attrs, key_str, arw);

    if (!value || Py_IS_PRIMITIVE(value)) {
        Py_XDECREF(repr);
        return;
    }

    ObjectTraceData *val_td = get_trace_data(value);
    if (!val_td) {
        ContainerType ctype = classify_container_type(value);
        if (ctype != CONTAINER_NONE) {
            val_td = attach_typed_container_trace_data(value, ctype);
        }
    }
    if (val_td) {
        SMap_set(&td->members, key_str, (void *)(intptr_t)val_td->id);
    }
    Py_XDECREF(repr);
}

/* ---- deref hooks (closure variable access) ---- */

static ObjectTraceData *get_or_create_cell_trace(PyObject *cell) {
    ObjectTraceData *td = get_trace_data(cell);
    if (td) return td;
    return new_trace_data(cell);
}

void d3g_deref_store_hook(PyObject *cell, PyObject *name, PyObject *value) {
    if (!g_state.enabled) return;

    ObjectTraceData *td = get_or_create_cell_trace(cell);
    if (!td) return;

    const char *name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return;

    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return;

    uint64_t caller_id = get_frame_call_id(frame);
    int call_lineno = PyFrame_GetLineNumber(frame);
    ARW arw = {caller_id, call_lineno};
    ARWMap_set(&td->attrs, name_str, arw);

    if (!value || Py_IS_PRIMITIVE(value)) return;

    ObjectTraceData *val_td = get_trace_data(value);
    if (val_td) {
        SMap_set(&td->members, name_str, (void *)(intptr_t)val_td->id);
    }
}

void d3g_deref_load_hook(PyObject *cell, PyObject *name, PyObject *value) {
    if (!g_state.enabled) return;

    const char *name_str = PyUnicode_AsUTF8(name);
    if (!name_str) return;

    ObjectTraceData *td = get_trace_data(cell);
    if (!td) return;

    ARW arw;
    if (!ARWMap_get(&td->attrs, name_str, &arw))
        return;

    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return;

    CallRecordData *rec = current_record();
    if (rec) {
        uint64_t caller_id = get_frame_call_id(frame);
        int read_lineno = PyFrame_GetLineNumber(frame);
        db_add_attr_read(rec, caller_id, arw.call_lineno, read_lineno);
    }
}

/* ---- push a traced frame ---- */

static void push_traced_frame(PyFrameObject *frame, uint64_t call_id,
                              CallRecordData *record, const char *ref_str) {
    FrameEntry entry;
    entry.call_id = call_id;
    entry.record = record;
    entry.branch_buf = NULL;
    entry.branch_len = 0;
    entry.branch_cap = 0;

    frame_stack_push(frame, &entry);
}

/* ---- trace callback ---- */

static int handle_call(PyObject *py_frame, PyFrameObject *frame_obj) {
    /* taint propagation */
    PyFrameObject *back = PyFrame_GetBack(frame_obj);
    if (back) {
        uint64_t caller_cid = get_frame_call_id(back);
        Py_DECREF((PyObject *)back);
        if (caller_cid == UINT64_MAX) {
            set_frame_call_id((PyFrameObject *)py_frame, UINT64_MAX);

            return 0;
        }
    }

    PyCodeObject *code = PyFrame_GetCode(frame_obj);
    if (!code) return 0;

    PyObject *filename_obj = code->co_filename;
    uintptr_t filename_ptr = (uintptr_t)filename_obj;
    Py_ssize_t fname_size;
    const char *filename = PyUnicode_AsUTF8AndSize(filename_obj, &fname_size);
    if (!filename) {
        PyErr_Clear();
        Py_DECREF(code);
        return 0;
    }

    int in_scope = check_scope(filename_ptr, filename);

    /* taint origination */
    if (g_state.taint_count > 0) {
        PyObject *qualname_obj = code->co_qualname;
        Py_ssize_t qn_size;
        const char *qualname = PyUnicode_AsUTF8AndSize(qualname_obj, &qn_size);
        if (qualname) {
            for (Py_ssize_t i = 0; i < g_state.taint_count; i++) {
                if (strstr(qualname, g_state.taint_patterns[i])) {
                    set_frame_call_id((PyFrameObject *)py_frame, UINT64_MAX);
        
                    Py_DECREF(code);
                    return 0;
                }
            }
        }
    }

    if (!in_scope) {
        set_frame_call_id((PyFrameObject *)py_frame, 0);

        /* check for __init__ on tracked class */
        PyObject *co_name = code->co_name;
        Py_ssize_t name_size;
        const char *name = PyUnicode_AsUTF8AndSize(co_name, &name_size);
        if (name && name_size == 8 && memcmp(name, "__init__", 8) == 0) {
            PyObject *self_obj = get_self_obj(py_frame, code);
            if (self_obj && get_trace_data(self_obj)) {
                uint64_t call_id = g_state.next_call_id++;
                uint64_t caller_id = 0;
                int call_lineno = 0;
                PyFrameObject *back2 = PyFrame_GetBack(frame_obj);
                if (back2) {
                    caller_id = get_frame_call_id(back2);
                    call_lineno = PyFrame_GetLineNumber(back2);
                    Py_DECREF((PyObject *)back2);
                }

                Py_ssize_t qn_sz;
                const char *qn = PyUnicode_AsUTF8AndSize(code->co_qualname, &qn_sz);
                if (qn) {
                    char ref_buf[1024];
                    snprintf(ref_buf, sizeof(ref_buf), "%s:%s", filename, qn);
                    int32_t function_id = get_or_assign_function_id(ref_buf);

                    set_frame_call_id((PyFrameObject *)py_frame, call_id);
        

                    DatabaseObject *db = (DatabaseObject *)g_state.db;
                    CallRecordData *rec = db_add_call(db, call_id, function_id,
                                                      caller_id, call_lineno, 0);
                    if (rec) {
                        rec->obj_id = get_obj_id(self_obj);
                        push_traced_frame(frame_obj, call_id, rec, ref_buf);
                    }
                }
            }
        }

        Py_DECREF(code);
        return 0;
    }

    /* in-scope call */
    uint64_t call_id = g_state.next_call_id++;
    set_frame_call_id((PyFrameObject *)py_frame, call_id);

    uint64_t caller_id = 0;
    int call_lineno = 0;
    PyFrameObject *back3 = PyFrame_GetBack(frame_obj);
    if (back3) {
        caller_id = get_frame_call_id(back3);
        call_lineno = PyFrame_GetLineNumber(back3);
        Py_DECREF((PyObject *)back3);
    }

    Py_ssize_t qn_size;
    const char *qualname = PyUnicode_AsUTF8AndSize(code->co_qualname, &qn_size);
    if (!qualname) {
        PyErr_Clear();
        Py_DECREF(code);
        return 0;
    }

    char ref_buf[1024];
    snprintf(ref_buf, sizeof(ref_buf), "%s:%s", filename, qualname);
    int32_t function_id = get_or_assign_function_id(ref_buf);

    PyObject *self_obj = get_self_obj(py_frame, code);
    int32_t obj_id = get_obj_id(self_obj);

    DatabaseObject *db = (DatabaseObject *)g_state.db;
    CallRecordData *rec = db_add_call(db, call_id, function_id,
                                       caller_id, call_lineno, obj_id);
    if (!rec) { Py_DECREF(code); return 0; }

    push_traced_frame(frame_obj, call_id, rec, ref_buf);

    Py_DECREF(code);
    return 0;
}

static int handle_return(PyObject *py_frame, PyFrameObject *frame_obj) {
    uint64_t cid = get_frame_call_id((PyFrameObject *)py_frame);
    if (cid == 0 || cid == UINT64_MAX) return 0;

    FrameStack *stack = get_frame_stack(frame_obj);
    while (stack->count > 0) {
        FrameEntry *entry = &stack->entries[stack->count - 1];

        uint64_t entry_cid = entry->call_id;

        if (entry->branch_len > 0 && entry->record) {
            entry->record->control_flow = malloc(entry->branch_len);
            if (entry->record->control_flow) {
                memcpy(entry->record->control_flow, entry->branch_buf, entry->branch_len);
                entry->record->control_flow_len = entry->branch_len;
            }
        }

        /* The record is complete: move it out of process memory. */
        if (entry->record) {
            db_complete_call((DatabaseObject *)g_state.db, entry->record);
            entry->record = NULL;
        }

        frame_stack_pop(frame_obj);
        if (entry_cid == cid) break;
    }
    return 0;
}

/* ---- profile hook for C_CALL/C_RETURN container dispatch ---- */

typedef enum {
    STASH_NONE = 0,
    STASH_LIST_APPEND,
    STASH_LIST_EXTEND,
    STASH_LIST_INSERT,
    STASH_LIST_POP,
    STASH_LIST_REMOVE,
    STASH_LIST_CLEAR,
    STASH_DICT_CLEAR,
    STASH_SET_ADD,
    STASH_SET_DISCARD,
    STASH_SET_REMOVE,
    STASH_SET_POP,
    STASH_SET_UPDATE,
    STASH_SET_CLEAR,
    STASH_DEQUE_APPEND,
    STASH_DEQUE_APPENDLEFT,
    STASH_DEQUE_EXTEND,
    STASH_DEQUE_EXTENDLEFT,
    STASH_DEQUE_POP,
    STASH_DEQUE_POPLEFT,
    STASH_DEQUE_REMOVE,
    STASH_DEQUE_CLEAR,
} StashType;

typedef struct {
    StashType type;
    PyObject *self_obj;
    Py_ssize_t pre_len;
} CCallStash;

static _Thread_local CCallStash tl_stash = {0};

static StashType classify_container_call(ObjectTraceData *td, const char *name) {
    switch (td->type) {
    case CONTAINER_LIST:
        if (strcmp(name, "append") == 0) return STASH_LIST_APPEND;
        if (strcmp(name, "extend") == 0) return STASH_LIST_EXTEND;
        if (strcmp(name, "insert") == 0) return STASH_LIST_INSERT;
        if (strcmp(name, "pop") == 0)    return STASH_LIST_POP;
        if (strcmp(name, "remove") == 0) return STASH_LIST_REMOVE;
        if (strcmp(name, "clear") == 0)  return STASH_LIST_CLEAR;
        break;
    case CONTAINER_DICT:
        if (strcmp(name, "clear") == 0)  return STASH_DICT_CLEAR;
        break;
    case CONTAINER_SET:
        if (strcmp(name, "add") == 0)     return STASH_SET_ADD;
        if (strcmp(name, "discard") == 0) return STASH_SET_DISCARD;
        if (strcmp(name, "remove") == 0)  return STASH_SET_REMOVE;
        if (strcmp(name, "pop") == 0)     return STASH_SET_POP;
        if (strcmp(name, "update") == 0)  return STASH_SET_UPDATE;
        if (strcmp(name, "clear") == 0)   return STASH_SET_CLEAR;
        break;
    case CONTAINER_DEQUE:
        if (strcmp(name, "append") == 0)      return STASH_DEQUE_APPEND;
        if (strcmp(name, "appendleft") == 0)  return STASH_DEQUE_APPENDLEFT;
        if (strcmp(name, "extend") == 0)      return STASH_DEQUE_EXTEND;
        if (strcmp(name, "extendleft") == 0)  return STASH_DEQUE_EXTENDLEFT;
        if (strcmp(name, "pop") == 0)         return STASH_DEQUE_POP;
        if (strcmp(name, "popleft") == 0)     return STASH_DEQUE_POPLEFT;
        if (strcmp(name, "remove") == 0)      return STASH_DEQUE_REMOVE;
        if (strcmp(name, "clear") == 0)       return STASH_DEQUE_CLEAR;
        break;
    default:
        break;
    }
    return STASH_NONE;
}

static void handle_c_call(PyObject *callable, PyObject *self_obj) {
    const char *name = NULL;

    if (PyCFunction_Check(callable)) {
        PyCFunctionObject *cfunc = (PyCFunctionObject *)callable;
        if (!self_obj) self_obj = cfunc->m_self;
        name = cfunc->m_ml->ml_name;
    } else if (Py_IS_TYPE(callable, &PyMethodDescr_Type)) {
        PyMethodDescrObject *descr = (PyMethodDescrObject *)callable;
        name = descr->d_method->ml_name;
    } else {
        return;
    }

    if (!self_obj) return;

    ObjectTraceData *td = get_trace_data(self_obj);
    if (!td || td->type == CONTAINER_NONE) return;
    StashType st = classify_container_call(td, name);
    if (st == STASH_NONE) return;

    tl_stash.type = st;
    tl_stash.self_obj = self_obj;
    switch (td->type) {
    case CONTAINER_LIST:
        tl_stash.pre_len = PyList_GET_SIZE(self_obj);
        break;
    case CONTAINER_SET:
        tl_stash.pre_len = PySet_GET_SIZE(self_obj);
        break;
    default:
        tl_stash.pre_len = 0;
        break;
    }
}

static void handle_c_return(PyFrameObject *frame_obj) {
    StashType st = tl_stash.type;
    if (st == STASH_NONE) return;
    tl_stash.type = STASH_NONE;

    PyObject *self_obj = tl_stash.self_obj;
    ObjectTraceData *td = get_trace_data(self_obj);
    if (!td) return;

    ARW arw = {0, 0};
    arw.caller_id = get_frame_call_id(frame_obj);
    arw.call_lineno = PyFrame_GetLineNumber(frame_obj);

    switch (st) {
    case STASH_LIST_APPEND: {
        ListTraceData *lt = (ListTraceData *)td;
        arwlist_append(&lt->arws, arw);
        break;
    }
    case STASH_LIST_EXTEND: {
        ListTraceData *lt = (ListTraceData *)td;
        Py_ssize_t new_len = PyList_GET_SIZE(self_obj);
        for (Py_ssize_t i = tl_stash.pre_len; i < new_len; i++)
            arwlist_append(&lt->arws, arw);
        break;
    }
    case STASH_LIST_INSERT: {
        ListTraceData *lt = (ListTraceData *)td;
        Py_ssize_t new_len = PyList_GET_SIZE(self_obj);
        if (new_len > tl_stash.pre_len) {
            Py_ssize_t idx = new_len - 1;
            for (Py_ssize_t i = (Py_ssize_t)lt->arws.len; i > idx && i > 0; i--) {}
            arwlist_append(&lt->arws, arw);
        }
        break;
    }
    case STASH_LIST_POP: {
        ListTraceData *lt = (ListTraceData *)td;
        if (lt->arws.len > 0) {
            ARW read_arw = arwlist_pop(&lt->arws);
            emit_read(&read_arw);
        }
        break;
    }
    case STASH_LIST_REMOVE: {
        ListTraceData *lt = (ListTraceData *)td;
        Py_ssize_t new_len = PyList_GET_SIZE(self_obj);
        if (new_len < tl_stash.pre_len && lt->arws.len > (size_t)new_len) {
            lt->arws.len = (size_t)new_len;
        }
        break;
    }
    case STASH_LIST_CLEAR: {
        ListTraceData *lt = (ListTraceData *)td;
        arwlist_free(&lt->arws);
        arwlist_init(&lt->arws);
        break;
    }
    case STASH_DICT_CLEAR: {
        DictTraceData *dt = (DictTraceData *)td;
        arwdict_free(&dt->arws);
        arwdict_init(&dt->arws);
        break;
    }
    case STASH_SET_ADD: {
        SetTraceData *st_data = (SetTraceData *)td;
        if (PySet_GET_SIZE(self_obj) > tl_stash.pre_len) {
            /* New element was added — we don't have the hash,
               but the set grew, so record a generic ARW */
        }
        break;
    }
    case STASH_SET_CLEAR: {
        SetTraceData *st_data = (SetTraceData *)td;
        arwset_free(&st_data->arws);
        arwset_init(&st_data->arws);
        break;
    }
    case STASH_DEQUE_APPEND: {
        DequeTraceData *dq = (DequeTraceData *)td;
        arwdeque_append(&dq->arws, arw);
        break;
    }
    case STASH_DEQUE_APPENDLEFT: {
        DequeTraceData *dq = (DequeTraceData *)td;
        arwdeque_appendleft(&dq->arws, arw);
        break;
    }
    case STASH_DEQUE_EXTEND: {
        DequeTraceData *dq = (DequeTraceData *)td;
        Py_ssize_t added = (Py_ssize_t)arwdeque_len(&dq->arws);
        /* Can't determine exact count without pre-len for deques,
           but the deque has already grown */
        break;
    }
    case STASH_DEQUE_POP: {
        DequeTraceData *dq = (DequeTraceData *)td;
        if (dq->arws.len > 0) {
            ARW read_arw;
            if (arwdeque_pop(&dq->arws, &read_arw) == 0)
                emit_read(&read_arw);
        }
        break;
    }
    case STASH_DEQUE_POPLEFT: {
        DequeTraceData *dq = (DequeTraceData *)td;
        if (dq->arws.len > 0) {
            ARW read_arw;
            if (arwdeque_popleft(&dq->arws, &read_arw) == 0)
                emit_read(&read_arw);
        }
        break;
    }
    case STASH_DEQUE_CLEAR: {
        DequeTraceData *dq = (DequeTraceData *)td;
        arwdeque_free(&dq->arws);
        arwdeque_init(&dq->arws);
        break;
    }
    default:
        break;
    }
}

/* ---- direct eval-loop hooks ---- */

void d3g_py_call_hook(_PyInterpreterFrame *frame) {
    if (!g_state.enabled) return;
    PyFrameObject *frame_obj = _PyFrame_GetFrameObject(frame);
    if (!frame_obj) return;
    handle_call((PyObject *)frame_obj, frame_obj);
}

void d3g_py_return_hook(_PyInterpreterFrame *frame) {
    if (!g_state.enabled) return;
    PyFrameObject *frame_obj = frame->frame_obj;
    if (!frame_obj) return;
    handle_return((PyObject *)frame_obj, frame_obj);
}

void d3g_c_call_hook(PyObject *callable, PyObject *self_obj) {
    if (!g_state.enabled) return;
    handle_c_call(callable, self_obj);
}

void d3g_c_return_hook(_PyInterpreterFrame *frame) {
    if (!g_state.enabled) return;
    PyFrameObject *frame_obj = frame->frame_obj;
    if (!frame_obj) return;
    handle_c_return(frame_obj);
}

static void record_ipc(const char *name) {
    if (!g_state.enabled) return;
    if (!g_state.db) return;
    /* Attribute to the nearest traced call, so events reached through
     * library wrappers (SharedMemory.__init__, Lib/signal.py, io layers)
     * are not dropped; current_record() returns NULL under taint. */
    CallRecordData *cur = current_record();
    if (!cur) return;
    writer_push_ipc(name, (int64_t)cur->call_id);
}

void d3g_shm_open_hook(const char *name) {
    char buf[300];
    snprintf(buf, sizeof(buf), "shm:%s", name);
    record_ipc(buf);
}

void d3g_pipe_hook(int fd_read, int fd_write) {
    char buf[64];
    snprintf(buf, sizeof(buf), "pipe:%d,%d", fd_read, fd_write);
    record_ipc(buf);
}

void d3g_mkfifo_hook(const char *path) {
    char buf[300];
    snprintf(buf, sizeof(buf), "fifo:%s", path);
    record_ipc(buf);
}

void d3g_socket_hook(PyObject *address) {
    PyObject *repr = PyObject_Repr(address);
    if (!repr) { PyErr_Clear(); return; }
    const char *s = PyUnicode_AsUTF8(repr);
    if (s) {
        char buf[300];
        snprintf(buf, sizeof(buf), "sock:%s", s);
        record_ipc(buf);
    }
    Py_DECREF(repr);
}

void d3g_signal_hook(int signum) {
    char buf[32];
    snprintf(buf, sizeof(buf), "signal:%d", signum);
    record_ipc(buf);
}

void d3g_mmap_create_hook(PyObject *mmap_obj, int fd, long long offset) {
    if (!g_state.enabled) return;
    if (!g_state.db) return;

    char path_buf[4096];
    const char *backing;
    if (fd == -1) {
        static uint64_t anon_serial = 0;
        snprintf(path_buf, sizeof(path_buf), "ANONYMOUS_%llu",
                 (unsigned long long)anon_serial++);
        backing = path_buf;
    } else {
        char proc_path[64];
        snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
        ssize_t len = readlink(proc_path, path_buf, sizeof(path_buf) - 1);
        if (len > 0) {
            path_buf[len] = '\0';
            backing = path_buf;
        } else {
            snprintf(path_buf, sizeof(path_buf), "fd:%d", fd);
            backing = path_buf;
        }
    }

    DatabaseObject *db = (DatabaseObject *)g_state.db;
    IoObjectRecord *rec = db_add_io_object(db, backing, (uint64_t)offset);
    if (rec) {
        umap_set(&g_state.io_object_records, (uintptr_t)mmap_obj, (intptr_t)rec);
    }
}

static IoObjectRecord *get_io_object_record(PyObject *obj) {
    intptr_t val;
    if (umap_get(&g_state.io_object_records, (uintptr_t)obj, &val))
        return (IoObjectRecord *)val;
    return NULL;
}

void d3g_mmap_read_hook(PyObject *mmap_obj, long long offset, long long length) {
    if (!g_state.enabled) return;
    if (!g_state.db) return;

    IoObjectRecord *rec = get_io_object_record(mmap_obj);
    if (!rec) return;

    /* Attribute to the nearest traced call, so events reached through
     * library wrappers (SharedMemory.__init__, Lib/signal.py, io layers)
     * are not dropped; current_record() returns NULL under taint. */
    CallRecordData *cur = current_record();
    if (!cur) return;
    uint64_t call_id = cur->call_id;

    writer_push_io_op(rec->id, call_id, (uint64_t)offset, (uint64_t)length,
                      IO_OP_READ);
}

void d3g_mmap_write_hook(PyObject *mmap_obj, long long offset, long long length) {
    if (!g_state.enabled) return;
    if (!g_state.db) return;

    IoObjectRecord *rec = get_io_object_record(mmap_obj);
    if (!rec) return;

    /* Attribute to the nearest traced call, so events reached through
     * library wrappers (SharedMemory.__init__, Lib/signal.py, io layers)
     * are not dropped; current_record() returns NULL under taint. */
    CallRecordData *cur = current_record();
    if (!cur) return;
    uint64_t call_id = cur->call_id;

    writer_push_io_op(rec->id, call_id, (uint64_t)offset, (uint64_t)length,
                      IO_OP_WRITE);
}

/* ---- inherited fd enumeration ---- */

void d3g_enumerate_fds(void) {
    if (!g_state.enabled) return;
    if (!g_state.db) return;

    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return;

    DatabaseObject *db = (DatabaseObject *)g_state.db;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        int fd = atoi(entry->d_name);
        int dir_fd = dirfd(dir);
        if (fd == dir_fd) continue;

        char proc_path[64];
        char path_buf[4096];
        snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
        ssize_t len = readlink(proc_path, path_buf, sizeof(path_buf) - 1);
        if (len <= 0) continue;
        path_buf[len] = '\0';

        db_add_io_object(db, path_buf, 0);
    }
    closedir(dir);
}

/* ---- semaphore hooks ---- */

void d3g_sem_acquire_hook(const char *name) {
    char buf[300];
    /* Channel identity only: resolve_ipc_edges pairs rows by exact name
     * across processes, so acquire and release must share it. */
    snprintf(buf, sizeof(buf), "sem:%s", name ? name : "?");
    record_ipc(buf);
}

void d3g_sem_release_hook(const char *name) {
    char buf[300];
    snprintf(buf, sizeof(buf), "sem:%s", name ? name : "?");
    record_ipc(buf);
}

/* ---- file I/O hooks ---- */

void d3g_fileio_open_hook(PyObject *fileio_obj, int fd) {
    if (!g_state.enabled) return;
    if (!g_state.db) return;

    char path_buf[4096];
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(proc_path, path_buf, sizeof(path_buf) - 1);
    if (len > 0) {
        path_buf[len] = '\0';
    } else {
        snprintf(path_buf, sizeof(path_buf), "fd:%d", fd);
    }

    DatabaseObject *db = (DatabaseObject *)g_state.db;
    IoObjectRecord *rec = db_add_io_object(db, path_buf, 0);
    if (rec) {
        umap_set(&g_state.io_object_records, (uintptr_t)fileio_obj, (intptr_t)rec);
    }
}

void d3g_fileio_read_hook(PyObject *fileio_obj, int fd, Py_ssize_t n) {
    if (!g_state.enabled) return;
    if (!g_state.db) return;
    if (n <= 0) return;

    IoObjectRecord *rec = get_io_object_record(fileio_obj);
    if (!rec) return;

    /* Attribute to the nearest traced call, so events reached through
     * library wrappers (SharedMemory.__init__, Lib/signal.py, io layers)
     * are not dropped; current_record() returns NULL under taint. */
    CallRecordData *cur = current_record();
    if (!cur) return;
    uint64_t call_id = cur->call_id;

    off_t pos = lseek(fd, 0, SEEK_CUR);
    uint64_t offset = (pos >= 0) ? (uint64_t)(pos - n) : 0;

    writer_push_io_op(rec->id, call_id, offset, (uint64_t)n, IO_OP_READ);
}

void d3g_fileio_write_hook(PyObject *fileio_obj, int fd, Py_ssize_t n) {
    if (!g_state.enabled) return;
    if (!g_state.db) return;
    if (n <= 0) return;

    IoObjectRecord *rec = get_io_object_record(fileio_obj);
    if (!rec) return;

    /* Attribute to the nearest traced call, so events reached through
     * library wrappers (SharedMemory.__init__, Lib/signal.py, io layers)
     * are not dropped; current_record() returns NULL under taint. */
    CallRecordData *cur = current_record();
    if (!cur) return;
    uint64_t call_id = cur->call_id;

    off_t pos = lseek(fd, 0, SEEK_CUR);
    uint64_t offset = (pos >= 0) ? (uint64_t)(pos - n) : 0;

    writer_push_io_op(rec->id, call_id, offset, (uint64_t)n, IO_OP_WRITE);
}

/* ---- fork child cleanup ---- */

void d3g_after_fork_child_hook(void) {
    if (!g_state.enabled) return;
    if (!g_state.db) return;
    DatabaseObject *db = (DatabaseObject *)g_state.db;

    /* The child discards the parent's call history, but the calls that were
     * active at fork time are still active here and their FrameEntry.record
     * pointers would dangle once db_clear_records frees the records. Only
     * the forking thread survives in the child, so its thread-local frame
     * stacks are the complete set. Snapshot each active record's header,
     * clear, then re-create the records in order and repoint the entries,
     * so the child's trace is self-contained and every live entry is
     * backed by valid storage. */
    typedef struct { FrameEntry *entry; CallRecordData header; } ActiveCall;
    ActiveCall *active = NULL;
    size_t n_active = 0, cap_active = 0;
    if (tl_stacks_init) {
        for (size_t i = 0; i < tl_coro_stacks.capacity; i++) {
            UMapEntry *me = &tl_coro_stacks.entries[i];
            if (!me->occupied) continue;
            FrameStack *stack = (FrameStack *)me->value;
            for (size_t j = 0; j < stack->count; j++) {
                FrameEntry *fe = &stack->entries[j];
                if (!fe->record) continue;
                if (n_active == cap_active) {
                    cap_active = cap_active ? cap_active * 2 : 16;
                    ActiveCall *tmp = realloc(active, cap_active * sizeof(ActiveCall));
                    if (!tmp) { free(active); active = NULL; n_active = 0; goto clear; }
                    active = tmp;
                }
                active[n_active].entry = fe;
                active[n_active].header = *fe->record;
                n_active++;
            }
        }
    }

clear:
    db_clear_records(db);

    /* The ring was reset by the pthread_atfork child handler (writer.c);
     * start a fresh writer for this pid. The function table is interned
     * per process, and the child inherits the parent's entries, so replay
     * them: nothing else would ever emit those rows here. */
    if (writer_start() == 0) {
        SMap *fm = &g_state.func_to_id;
        if (fm->entries) {
            for (size_t i = 0; i < fm->capacity; i++) {
                if (!fm->entries[i].occupied) continue;
                writer_push_function((int32_t)(intptr_t)fm->entries[i].value,
                                     fm->entries[i].key);
            }
        }
    }

    for (size_t i = 0; i < n_active; i++) {
        CallRecordData *h = &active[i].header;
        active[i].entry->record = db_add_call(db, h->call_id, h->function_id,
                                              h->caller_id, h->call_lineno,
                                              h->obj_id);
    }
    free(active);
}

/* ---- generator-driven for loops (FOR_ITER_GEN) ---- */

/* FOR_ITER_GEN inlines the generator's frame instead of calling
 * tp_iternext, so the branch decision is only known later: a YIELD_VALUE
 * back into the loop means another iteration (exhausted=0), a RETURN_VALUE
 * from the generator frame means the loop exits (exhausted=1). Both sites
 * pass the *caller* frame. The decision is recorded only when that frame is
 * a real Python frame currently suspended at a FOR_ITER-family instruction;
 * this excludes the generic FOR_ITER path (where the generator runs under
 * an interpreter entry frame and _FOR_ITER records the bit itself) and
 * SEND-driven suspension (yield from / await), which is not a loop. Bit
 * polarity matches _FOR_ITER: 1 = exhausted, 0 = body executes. */
void d3g_gen_iter_hook(_PyInterpreterFrame *caller, int exhausted) {
    if (!g_state.enabled) return;
    if (!caller || caller->owner == FRAME_OWNED_BY_INTERPRETER) return;
    if (PyStackRef_IsNone(caller->f_executable)) return;
    PyCodeObject *code = _PyFrame_GetCode(caller);
    int i = (int)(caller->instr_ptr - _PyFrame_GetBytecode(caller));
    if (_Py_GetBaseCodeUnit(code, i).op.code != FOR_ITER) return;
    d3g_branch_hook(caller, exhausted);
}

/* ---- at-exit serialization ---- */

/* Finish this process's trace: disable tracing, hand every still-live
 * call and object record to the writer, and join the writer thread so
 * $PYTHON_TRACER_OUTDIR/{pid}.db is complete on return. Idempotent.
 * Called from uninstall(), the interpreter's normal exit path
 * (Modules/main.c), and os._exit(), which otherwise skips all cleanup and
 * would lose a forked child's in-flight records.
 *
 * Completed records were streamed while the program ran, so a process
 * killed before reaching this point still leaves every returned call in
 * its database; only the records in flight at that moment are lost.
 *
 * A process that recorded no calls never ran traced code (e.g.
 * multiprocessing's resource tracker); the writer never opens a file for
 * it, so such processes don't litter the output directory. */
void d3g_flush_trace(void) {
    if (!g_state.enabled) return;
    g_state.enabled = 0;
    d3g_signals_uninstall();

    DatabaseObject *db = (DatabaseObject *)g_state.db;
    if (!db || !writer_running()) return;

    db_complete_all_calls(db);

    UMap *ex = &g_state.object_extras;
    if (ex->entries) {
        for (size_t i = 0; i < ex->capacity; i++) {
            if (!ex->entries[i].occupied) continue;
            ObjectTraceData *td = (ObjectTraceData *)ex->entries[i].value;
            emit_object_record(td);
        }
    }

    writer_stop();
}

/* ---- Python-visible functions ---- */

static PyObject *py_install(PyObject *self, PyObject *args, PyObject *kw) {
    static char *kwlist[] = {"prefixes", "db",
                             "path_filter", "taint_patterns", "traceall", NULL};
    PyObject *prefixes_list, *db, *path_filter;
    PyObject *taint_patterns = Py_None;
    int traceall = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOO|Op", kwlist,
            &prefixes_list, &db, &path_filter, &taint_patterns, &traceall))
        return NULL;

    /* clean up old state */
    Py_XDECREF(g_state.db);
    Py_XDECREF(g_state.filter);
    if (g_state.prefixes) {
        for (Py_ssize_t i = 0; i < g_state.prefix_count; i++)
            free(g_state.prefixes[i]);
        free(g_state.prefixes);
    }
    if (g_state.taint_patterns) {
        for (Py_ssize_t i = 0; i < g_state.taint_count; i++)
            free(g_state.taint_patterns[i]);
        free(g_state.taint_patterns);
    }
    umap_free(&g_state.scope_cache);
    umap_free(&g_state.object_extras);
    umap_free(&g_state.io_object_records);

    /* set up new state */
    Py_INCREF(db);
    g_state.db = db;
    Py_INCREF(path_filter);
    g_state.filter = path_filter;

    Py_ssize_t n = PyList_GET_SIZE(prefixes_list);
    g_state.prefixes = malloc(n * sizeof(char *));
    g_state.prefix_count = n;
    for (Py_ssize_t i = 0; i < n; i++) {
        const char *s = PyUnicode_AsUTF8(PyList_GET_ITEM(prefixes_list, i));
        g_state.prefixes[i] = strdup(s);
    }

    umap_init(&g_state.scope_cache, 256);
    umap_init(&g_state.object_extras, 1024);
    umap_init(&g_state.io_object_records, 16);
    SMap_free(&g_state.func_to_id);
    SMap_init(&g_state.func_to_id, 512);
    g_state.next_func_id = 0;

    g_state.taint_patterns = NULL;
    g_state.taint_count = 0;
    if (taint_patterns != Py_None && PyList_Check(taint_patterns)) {
        Py_ssize_t tn = PyList_GET_SIZE(taint_patterns);
        g_state.taint_patterns = malloc(tn * sizeof(char *));
        for (Py_ssize_t i = 0; i < tn; i++) {
            const char *s = PyUnicode_AsUTF8(PyList_GET_ITEM(taint_patterns, i));
            if (s && *s) {
                g_state.taint_patterns[g_state.taint_count++] = strdup(s);
            }
        }
    }

    g_state.traceall = traceall;
    g_state.next_call_id = 1;

    if (writer_start() < 0) {
        PyErr_SetString(PyExc_RuntimeError, "cannot start trace writer thread");
        return NULL;
    }
    g_state.enabled = 1;
    d3g_signals_install();

    Py_RETURN_NONE;
}

static PyObject *py_install_thread(PyObject *self, PyObject *Py_UNUSED(args)) {
    Py_RETURN_NONE;
}

static PyObject *py_uninstall(PyObject *self, PyObject *Py_UNUSED(args)) {
    d3g_flush_trace();
    Py_RETURN_NONE;
}

static PyObject *py_get_call_id(PyObject *self, PyObject *frame) {
    uint64_t cid = get_frame_call_id((PyFrameObject *)frame);
    return PyLong_FromUnsignedLongLong(cid);
}

static PyObject *py_set_call_id(PyObject *self, PyObject *args) {
    PyObject *frame;
    uint64_t cid;
    if (!PyArg_ParseTuple(args, "OK", &frame, &cid))
        return NULL;
    set_frame_call_id((PyFrameObject *)frame, cid);
    Py_RETURN_NONE;
}

CallRecordData *current_record(void) {
    PyFrameObject *frame = PyEval_GetFrame();
    if (!frame) return NULL;
    /* Inside a taint-excluded subtree nothing is attributed, not even to
     * the nearest traced caller. Untraced library frames (call_id 0) do
     * attribute to it: that is how dataflow through library code is
     * credited to the traced call that invoked it. */
    if (get_frame_call_id(frame) == UINT64_MAX) return NULL;
    FrameEntry *entry = frame_stack_peek(frame);
    if (!entry) return NULL;
    return entry->record;
}

static PyMethodDef hook_methods[] = {
    {"install",        (PyCFunction)py_install,        METH_VARARGS | METH_KEYWORDS, NULL},
    {"install_thread", py_install_thread,              METH_NOARGS, NULL},
    {"uninstall",      py_uninstall,                   METH_NOARGS, NULL},
    {"get_call_id",    py_get_call_id,                 METH_O, NULL},
    {"set_call_id",    (PyCFunction)py_set_call_id,    METH_VARARGS, NULL},
    {NULL}
};

int hook_init(PyObject *module) {

    for (PyMethodDef *m = hook_methods; m->ml_name; m++) {
        PyObject *func = PyCFunction_NewEx(m, NULL, NULL);
        if (!func) return -1;
        if (PyModule_AddObject(module, m->ml_name, func) < 0) {
            Py_DECREF(func);
            return -1;
        }
    }
    return 0;
}
