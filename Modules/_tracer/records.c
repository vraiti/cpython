#include "records.h"
#include "hook.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INITIAL_CAP 256

/* ========== C array helpers ========== */

CallRecordData *db_add_call(DatabaseObject *db,
                            uint64_t call_id, int32_t function_id,
                            uint64_t caller_id, int32_t call_lineno,
                            int32_t obj_id) {
    if (db->calls_len >= db->calls_cap) {
        Py_ssize_t new_cap = db->calls_cap ? db->calls_cap * 2 : INITIAL_CAP;
        CallRecordData *tmp = realloc(db->calls, new_cap * sizeof(CallRecordData));
        if (!tmp) return NULL;
        db->calls = tmp;
        db->calls_cap = new_cap;
    }
    CallRecordData *rec = &db->calls[db->calls_len++];
    rec->call_id = call_id;
    rec->function_id = function_id;
    rec->caller_id = caller_id;
    rec->call_lineno = call_lineno;
    rec->obj_id = obj_id;
    rec->control_flow = NULL;
    rec->control_flow_len = 0;
    rec->attr_reads = NULL;
    rec->attr_reads_len = 0;
    rec->attr_reads_cap = 0;
    return rec;
}

Py_ssize_t db_add_object(DatabaseObject *db, uint64_t call_id) {
    if (db->objects_len >= db->objects_cap) {
        Py_ssize_t new_cap = db->objects_cap ? db->objects_cap * 2 : INITIAL_CAP;
        ObjectRecordData *tmp = realloc(db->objects, new_cap * sizeof(ObjectRecordData));
        if (!tmp) return -1;
        db->objects = tmp;
        db->objects_cap = new_cap;
    }
    Py_ssize_t idx = db->objects_len++;
    ObjectRecordData *obj = &db->objects[idx];
    obj->call_id = call_id;
    SMap_init(&obj->members, 8);
    return idx;
}

void db_add_ipc_entry(DatabaseObject *db, const char *name, int64_t obj_idx) {
    if (db->ipc_len >= db->ipc_cap) {
        Py_ssize_t new_cap = db->ipc_cap ? db->ipc_cap * 2 : 16;
        IpcRecordData *tmp = realloc(db->ipc, new_cap * sizeof(IpcRecordData));
        if (!tmp) return;
        db->ipc = tmp;
        db->ipc_cap = new_cap;
    }
    IpcRecordData *entry = &db->ipc[db->ipc_len++];
    entry->name = strdup(name);
    entry->obj_idx = obj_idx;
}

IoObjectRecord *db_add_io_object(DatabaseObject *db, const char *name,
                                 uint64_t offset) {
    if (db->io_objects_len >= db->io_objects_cap) {
        Py_ssize_t new_cap = db->io_objects_cap ? db->io_objects_cap * 2 : 16;
        IoObjectRecord **tmp = realloc(db->io_objects, new_cap * sizeof(IoObjectRecord *));
        if (!tmp) return NULL;
        db->io_objects = tmp;
        db->io_objects_cap = new_cap;
    }
    IoObjectRecord *rec = malloc(sizeof(IoObjectRecord));
    if (!rec) return NULL;
    rec->name = strdup(name);
    rec->offset = offset;
    rec->id = db->next_io_object_id++;
    db->io_objects[db->io_objects_len++] = rec;
    return rec;
}

void db_add_io_op(DatabaseObject *db, IoObjectRecord *io_object, uint64_t call_id,
                  uint64_t offset, uint64_t length, IoOpType op_type) {
    if (db->io_ops_len >= db->io_ops_cap) {
        Py_ssize_t new_cap = db->io_ops_cap ? db->io_ops_cap * 2 : 64;
        IoOpRecord *tmp = realloc(db->io_ops, new_cap * sizeof(IoOpRecord));
        if (!tmp) return;
        db->io_ops = tmp;
        db->io_ops_cap = new_cap;
    }
    IoOpRecord *rec = &db->io_ops[db->io_ops_len++];
    rec->io_object = io_object;
    rec->call_id = call_id;
    rec->offset = offset;
    rec->length = length;
    rec->op_type = op_type;
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

void db_set_arw(DatabaseObject *db, int32_t obj_id, const char *attr_name,
                uint64_t caller_id, int32_t call_lineno) {
    char key[300];
    snprintf(key, sizeof(key), "%d:%s", obj_id, attr_name);

    AttrRecordWriteData *existing;
    if (SMap_get(&db->arw_map, key, (void **)&existing)) {
        existing->caller_id = caller_id;
        existing->call_lineno = call_lineno;
    } else {
        AttrRecordWriteData *arw = malloc(sizeof(AttrRecordWriteData));
        if (!arw) return;
        arw->caller_id = caller_id;
        arw->call_lineno = call_lineno;
        SMap_set(&db->arw_map, key, arw);
    }
}

AttrRecordWriteData *db_get_arw(DatabaseObject *db, int32_t obj_id,
                                const char *attr_name) {
    char key[300];
    snprintf(key, sizeof(key), "%d:%s", obj_id, attr_name);
    AttrRecordWriteData *out;
    if (SMap_get(&db->arw_map, key, (void **)&out))
        return out;
    return NULL;
}

void db_clear_records(DatabaseObject *db) {
    for (Py_ssize_t i = 0; i < db->calls_len; i++) {
        free(db->calls[i].control_flow);
        free(db->calls[i].attr_reads);
    }
    db->calls_len = 0;

    for (Py_ssize_t i = 0; i < db->ipc_len; i++)
        free(db->ipc[i].name);
    db->ipc_len = 0;

    db->io_ops_len = 0;

    smap_free_values(&db->arw_map);
    SMap_free(&db->arw_map);
    SMap_init(&db->arw_map, 256);
}

/* ========== Database Python type ========== */

PyTypeObject *DatabaseType = NULL;

static int Database_init(PyObject *self, PyObject *args, PyObject *kw) {
    DatabaseObject *o = (DatabaseObject *)self;
    o->calls = NULL;
    o->calls_len = 0;
    o->calls_cap = 0;
    o->objects = NULL;
    o->objects_len = 0;
    o->objects_cap = 0;
    o->ipc = NULL;
    o->ipc_len = 0;
    o->ipc_cap = 0;
    o->io_objects = NULL;
    o->io_objects_len = 0;
    o->io_objects_cap = 0;
    o->io_ops = NULL;
    o->io_ops_len = 0;
    o->io_ops_cap = 0;
    o->next_io_object_id = 0;
    SMap_init(&o->arw_map, 256);
    return 0;
}

static void Database_dealloc(PyObject *self) {
    DatabaseObject *o = (DatabaseObject *)self;

    for (Py_ssize_t i = 0; i < o->calls_len; i++) {
        free(o->calls[i].control_flow);
        free(o->calls[i].attr_reads);
    }
    free(o->calls);

    for (Py_ssize_t i = 0; i < o->objects_len; i++)
        SMap_free(&o->objects[i].members);
    free(o->objects);

    for (Py_ssize_t i = 0; i < o->ipc_len; i++)
        free(o->ipc[i].name);
    free(o->ipc);

    for (Py_ssize_t i = 0; i < o->io_objects_len; i++) {
        free(o->io_objects[i]->name);
        free(o->io_objects[i]);
    }
    free(o->io_objects);
    free(o->io_ops);

    smap_free_values(&o->arw_map);
    SMap_free(&o->arw_map);

    Py_TYPE(self)->tp_free(self);
}

/* ---- Python methods ---- */

static PyObject *Database_add_ipc(PyObject *self, PyObject *args) {
    DatabaseObject *o = (DatabaseObject *)self;
    const char *name;
    long long obj_idx;
    if (!PyArg_ParseTuple(args, "sL", &name, &obj_idx))
        return NULL;
    db_add_ipc_entry(o, name, (int64_t)obj_idx);
    Py_RETURN_NONE;
}

/* ---- serialize ---- */

static int exec_sql(sqlite3 *sdb, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(sdb, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static const char *SCHEMA_SQL =
    "CREATE TABLE meta (pid INTEGER);"
    "CREATE TABLE machine (machine_id TEXT NOT NULL);"
    "CREATE TABLE functions (function_id INTEGER PRIMARY KEY, ref TEXT NOT NULL);"
    "CREATE TABLE calls ("
    "    pid INTEGER NOT NULL,"
    "    call_id INTEGER NOT NULL,"
    "    function_id INTEGER NOT NULL,"
    "    caller_id INTEGER NOT NULL,"
    "    call_lineno INTEGER NOT NULL,"
    "    obj_id INTEGER NOT NULL,"
    "    control_flow BLOB,"
    "    PRIMARY KEY (pid, call_id)"
    ");"
    "CREATE TABLE attr_reads ("
    "    pid INTEGER NOT NULL,"
    "    call_id INTEGER NOT NULL,"
    "    caller_id INTEGER NOT NULL,"
    "    write_call_lineno INTEGER NOT NULL,"
    "    read_call_lineno INTEGER NOT NULL"
    ");"
    "CREATE TABLE objects ("
    "    pid INTEGER NOT NULL,"
    "    obj_idx INTEGER NOT NULL,"
    "    call_id INTEGER NOT NULL,"
    "    PRIMARY KEY (pid, obj_idx)"
    ");"
    "CREATE TABLE members ("
    "    pid INTEGER NOT NULL,"
    "    obj_idx INTEGER NOT NULL,"
    "    attr TEXT NOT NULL,"
    "    child_idx INTEGER NOT NULL"
    ");"
    "CREATE TABLE ipc ("
    "    pid INTEGER NOT NULL,"
    "    name TEXT NOT NULL,"
    "    obj_idx INTEGER NOT NULL"
    ");"
    "CREATE TABLE io_objects ("
    "    pid INTEGER NOT NULL,"
    "    io_object_id INTEGER NOT NULL,"
    "    name TEXT NOT NULL,"
    "    offset INTEGER NOT NULL,"
    "    PRIMARY KEY (pid, io_object_id)"
    ");"
    "CREATE TABLE io_ops ("
    "    pid INTEGER NOT NULL,"
    "    io_object_id INTEGER NOT NULL,"
    "    call_id INTEGER NOT NULL,"
    "    offset INTEGER NOT NULL,"
    "    length INTEGER NOT NULL,"
    "    op_type INTEGER NOT NULL"
    ");";

#define TAINT_ID UINT64_MAX

int tracer_serialize_db(DatabaseObject *o, const char *path) {
    unlink(path);

    sqlite3 *sdb;
    if (sqlite3_open(path, &sdb) != SQLITE_OK) {
        fprintf(stderr, "tracer: failed to open %s: %s\n", path, sqlite3_errmsg(sdb));
        sqlite3_close(sdb);
        return -1;
    }

    if (exec_sql(sdb, SCHEMA_SQL) < 0)
        goto fail;
    if (exec_sql(sdb, "BEGIN TRANSACTION") < 0)
        goto fail;

    /* meta */
    {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(sdb, "INSERT INTO meta VALUES (?)", -1, &st, NULL);
        sqlite3_bind_int(st, 1, getpid());
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    /* machine */
    {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(sdb, "INSERT INTO machine VALUES (?)", -1, &st, NULL);
        char machine_id[64] = "";
        FILE *f = fopen("/etc/machine-id", "r");
        if (f) {
            if (fgets(machine_id, sizeof(machine_id), f)) {
                size_t len = strlen(machine_id);
                if (len > 0 && machine_id[len-1] == '\n')
                    machine_id[len-1] = '\0';
            }
            fclose(f);
        }
        sqlite3_bind_text(st, 1, machine_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    /* functions from g_state.func_to_id */
    {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(sdb,
            "INSERT INTO functions VALUES (?, ?)", -1, &st, NULL);
        SMap *fm = &g_state.func_to_id;
        if (fm->entries) {
            for (size_t i = 0; i < fm->capacity; i++) {
                if (!fm->entries[i].occupied) continue;
                int32_t fid = (int32_t)(intptr_t)fm->entries[i].value;
                sqlite3_bind_int(st, 1, fid);
                sqlite3_bind_text(st, 2, fm->entries[i].key, -1, SQLITE_STATIC);
                sqlite3_step(st);
                sqlite3_reset(st);
            }
        }
        sqlite3_finalize(st);
    }

    pid_t pid = getpid();

    /* calls + attr_reads */
    {
        sqlite3_stmt *call_st, *ar_st;
        sqlite3_prepare_v2(sdb,
            "INSERT INTO calls VALUES (?,?,?,?,?,?,?)", -1, &call_st, NULL);
        sqlite3_prepare_v2(sdb,
            "INSERT INTO attr_reads VALUES (?,?,?,?,?)", -1, &ar_st, NULL);

        for (Py_ssize_t i = 0; i < o->calls_len; i++) {
            CallRecordData *rec = &o->calls[i];
            uint64_t caller = rec->caller_id == TAINT_ID ? 0 : rec->caller_id;

            sqlite3_bind_int(call_st, 1, pid);
            sqlite3_bind_int64(call_st, 2, (sqlite3_int64)rec->call_id);
            sqlite3_bind_int(call_st, 3, rec->function_id);
            sqlite3_bind_int64(call_st, 4, (sqlite3_int64)caller);
            sqlite3_bind_int(call_st, 5, rec->call_lineno);
            sqlite3_bind_int(call_st, 6, rec->obj_id);
            if (rec->control_flow && rec->control_flow_len > 0)
                sqlite3_bind_blob(call_st, 7, rec->control_flow,
                                  (int)rec->control_flow_len, SQLITE_STATIC);
            else
                sqlite3_bind_null(call_st, 7);
            sqlite3_step(call_st);
            sqlite3_reset(call_st);

            for (Py_ssize_t j = 0; j < rec->attr_reads_len; j++) {
                AttrRecordReadData *ar = &rec->attr_reads[j];
                uint64_t ar_caller = ar->caller_id == TAINT_ID ? 0 : ar->caller_id;
                sqlite3_bind_int(ar_st, 1, pid);
                sqlite3_bind_int64(ar_st, 2, (sqlite3_int64)rec->call_id);
                sqlite3_bind_int64(ar_st, 3, (sqlite3_int64)ar_caller);
                sqlite3_bind_int(ar_st, 4, ar->write_call_lineno);
                sqlite3_bind_int(ar_st, 5, ar->read_call_lineno);
                sqlite3_step(ar_st);
                sqlite3_reset(ar_st);
            }
        }
        sqlite3_finalize(call_st);
        sqlite3_finalize(ar_st);
    }

    /* objects + members */
    {
        sqlite3_stmt *obj_st, *mem_st;
        sqlite3_prepare_v2(sdb,
            "INSERT INTO objects VALUES (?,?,?)", -1, &obj_st, NULL);
        sqlite3_prepare_v2(sdb,
            "INSERT INTO members VALUES (?,?,?,?)", -1, &mem_st, NULL);

        for (Py_ssize_t i = 0; i < o->objects_len; i++) {
            ObjectRecordData *obj = &o->objects[i];
            sqlite3_bind_int(obj_st, 1, pid);
            sqlite3_bind_int64(obj_st, 2, (sqlite3_int64)i);
            sqlite3_bind_int64(obj_st, 3, (sqlite3_int64)obj->call_id);
            sqlite3_step(obj_st);
            sqlite3_reset(obj_st);

            SMap *m = &obj->members;
            if (m->entries) {
                for (size_t k = 0; k < m->capacity; k++) {
                    if (!m->entries[k].occupied) continue;
                    sqlite3_bind_int(mem_st, 1, pid);
                    sqlite3_bind_int64(mem_st, 2, (sqlite3_int64)i);
                    sqlite3_bind_text(mem_st, 3, m->entries[k].key, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(mem_st, 4, (intptr_t)m->entries[k].value);
                    sqlite3_step(mem_st);
                    sqlite3_reset(mem_st);
                }
            }
        }
        sqlite3_finalize(obj_st);
        sqlite3_finalize(mem_st);
    }

    /* ipc */
    {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(sdb,
            "INSERT INTO ipc VALUES (?,?,?)", -1, &st, NULL);
        for (Py_ssize_t i = 0; i < o->ipc_len; i++) {
            IpcRecordData *entry = &o->ipc[i];
            sqlite3_bind_int(st, 1, pid);
            sqlite3_bind_text(st, 2, entry->name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)entry->obj_idx);
            sqlite3_step(st);
            sqlite3_reset(st);
        }
        sqlite3_finalize(st);
    }

    /* io_objects */
    {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(sdb,
            "INSERT INTO io_objects VALUES (?,?,?,?)", -1, &st, NULL);
        for (Py_ssize_t i = 0; i < o->io_objects_len; i++) {
            IoObjectRecord *rec = o->io_objects[i];
            sqlite3_bind_int(st, 1, pid);
            sqlite3_bind_int(st, 2, rec->id);
            sqlite3_bind_text(st, 3, rec->name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)rec->offset);
            sqlite3_step(st);
            sqlite3_reset(st);
        }
        sqlite3_finalize(st);
    }

    /* io_ops */
    {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(sdb,
            "INSERT INTO io_ops VALUES (?,?,?,?,?,?)", -1, &st, NULL);
        for (Py_ssize_t i = 0; i < o->io_ops_len; i++) {
            IoOpRecord *rec = &o->io_ops[i];
            sqlite3_bind_int(st, 1, pid);
            sqlite3_bind_int(st, 2, rec->io_object->id);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)rec->call_id);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)rec->offset);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)rec->length);
            sqlite3_bind_int(st, 6, rec->op_type);
            sqlite3_step(st);
            sqlite3_reset(st);
        }
        sqlite3_finalize(st);
    }

    if (exec_sql(sdb, "COMMIT") < 0)
        goto fail;

    sqlite3_close(sdb);

    fprintf(stderr, "Trace written to %s (%zd calls, %zd objects, %zd ipc, %zd io_objects)\n",
            path, o->calls_len, o->objects_len, o->ipc_len, o->io_objects_len);
    return 0;

fail:
    fprintf(stderr, "tracer: serialize failed: %s\n", sqlite3_errmsg(sdb));
    sqlite3_close(sdb);
    return -1;
}

static PyObject *Database_serialize(PyObject *self, PyObject *args) {
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path))
        return NULL;
    if (tracer_serialize_db((DatabaseObject *)self, path) < 0) {
        PyErr_SetString(PyExc_RuntimeError, "serialize failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyMethodDef Database_methods[] = {
    {"add_ipc",    Database_add_ipc,    METH_VARARGS, NULL},
    {"serialize",  Database_serialize,  METH_VARARGS, NULL},
    {NULL}
};

static PyType_Slot Database_slots[] = {
    {Py_tp_init,     Database_init},
    {Py_tp_dealloc,  Database_dealloc},
    {Py_tp_methods,  Database_methods},
    {0, NULL}
};

static PyType_Spec Database_spec = {
    .name = "tracer._tracer.Database",
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
