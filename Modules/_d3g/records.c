#include "records.h"
#include "writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== live-call list ========== */

CallRecordData *db_add_call(DatabaseObject *db,
                            uint64_t call_id, int32_t function_id,
                            uint64_t caller_id, int32_t call_lineno,
                            int32_t obj_id) {
    CallRecordData *rec = malloc(sizeof(CallRecordData));
    if (!rec) return NULL;
    rec->call_id = call_id;
    rec->function_id = function_id;
    rec->caller_id = caller_id;
    rec->call_lineno = call_lineno;
    rec->obj_id = obj_id;
    rec->func_idx = 0;
    rec->created_id = 0;
    rec->created_lineno = 0;
    rec->control_flow = NULL;
    rec->control_flow_len = 0;
    rec->control_flow_bits = 0;
    rec->control_flow_exc = NULL;
    rec->control_flow_exc_len = 0;
    rec->attr_reads = NULL;
    rec->attr_reads_len = 0;
    rec->attr_reads_cap = 0;

    rec->prev = NULL;
    rec->next = db->live_head;
    if (db->live_head) db->live_head->prev = rec;
    db->live_head = rec;
    db->live_count++;
    db->calls_total++;
    return rec;
}

static void unlink_call(DatabaseObject *db, CallRecordData *rec) {
    if (rec->prev) rec->prev->next = rec->next;
    else db->live_head = rec->next;
    if (rec->next) rec->next->prev = rec->prev;
    rec->prev = rec->next = NULL;
    db->live_count--;
}

void db_complete_call(DatabaseObject *db, CallRecordData *rec) {
    unlink_call(db, rec);
    writer_push_call(rec);
}

void db_complete_all_calls(DatabaseObject *db) {
    while (db->live_head)
        db_complete_call(db, db->live_head);
}

IoObjectRecord *db_add_io_object(DatabaseObject *db, const char *name,
                                 uint64_t offset) {
    IoObjectRecord *rec = malloc(sizeof(IoObjectRecord));
    if (!rec) return NULL;
    rec->name = strdup(name);
    rec->offset = offset;
    rec->id = db->next_io_object_id++;
    writer_push_io_object(rec->id, rec->name, rec->offset);
    return rec;
}

void db_add_attr_read(CallRecordData *rec,
                      uint64_t caller_id,
                      int32_t write_call_lineno,
                      int32_t read_call_lineno) {
    if (rec->attr_reads_len >= rec->attr_reads_cap) {
        Py_ssize_t new_cap = rec->attr_reads_cap ? rec->attr_reads_cap * 2 : 8;
        AttrRecordReadData *tmp = realloc(rec->attr_reads,
                                          new_cap * sizeof(AttrRecordReadData));
        if (!tmp) return;
        rec->attr_reads = tmp;
        rec->attr_reads_cap = new_cap;
    }
    AttrRecordReadData *ar = &rec->attr_reads[rec->attr_reads_len++];
    ar->caller_id = caller_id;
    ar->write_call_lineno = write_call_lineno;
    ar->read_call_lineno = read_call_lineno;
}

void db_clear_records(DatabaseObject *db) {
    /* Callers holding CallRecordData* (FrameEntry.record of frames still
     * active in a forked child) must re-create their records afterwards;
     * see d3g_after_fork_child_hook. */
    CallRecordData *rec = db->live_head;
    while (rec) {
        CallRecordData *next = rec->next;
        free(rec->control_flow);
        free(rec->control_flow_exc);
        free(rec->attr_reads);
        free(rec);
        rec = next;
    }
    db->live_head = NULL;
    db->live_count = 0;
}

/* ========== Database Python type ========== */

PyTypeObject *DatabaseType = NULL;

static int Database_init(PyObject *self, PyObject *args, PyObject *kw) {
    DatabaseObject *o = (DatabaseObject *)self;
    o->live_head = NULL;
    o->live_count = 0;
    o->calls_total = 0;
    o->next_io_object_id = 0;
    return 0;
}

static void Database_dealloc(PyObject *self) {
    db_clear_records((DatabaseObject *)self);
    Py_TYPE(self)->tp_free(self);
}

static PyObject *Database_stats(PyObject *self, PyObject *Py_UNUSED(args)) {
    DatabaseObject *o = (DatabaseObject *)self;
    return Py_BuildValue("{s:K,s:n}",
                         "calls_total", (unsigned long long)o->calls_total,
                         "live_calls", o->live_count);
}

static PyMethodDef Database_methods[] = {
    {"stats", Database_stats, METH_NOARGS, NULL},
    {NULL}
};

static PyType_Slot Database_slots[] = {
    {Py_tp_init,     Database_init},
    {Py_tp_dealloc,  Database_dealloc},
    {Py_tp_methods,  Database_methods},
    {0, NULL}
};

static PyType_Spec Database_spec = {
    .name = "d3g._d3g.Database",
    .basicsize = sizeof(DatabaseObject),
    .flags = Py_TPFLAGS_DEFAULT,
    .slots = Database_slots,
};

/* ========== Module registration ========== */

int records_init(PyObject *module) {
    DatabaseType = (PyTypeObject *)PyType_FromSpec(&Database_spec);
    if (!DatabaseType) return -1;
    if (PyModule_AddObject(module, "Database", (PyObject *)DatabaseType) < 0) {
        Py_DECREF(DatabaseType);
        return -1;
    }
    return 0;
}
