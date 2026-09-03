#include "writer.h"
#include <sqlite3.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define RING_CAP   (1u << 16)   /* events; ~3 MiB of slots */
#define BATCH_MAX  4096         /* events per transaction */

#define TAINT_ID UINT64_MAX

/* ---- ring state ---- */

static TraceEvent ring[RING_CAP];
static size_t ring_head = 0;    /* next slot to read */
static size_t ring_count = 0;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t idle = PTHREAD_COND_INITIALIZER;

static pthread_t thread;
static int running = 0;     /* thread exists and has not been joined */
static int accepting = 0;   /* pushes are queued rather than discarded */
static int busy = 0;        /* thread is outside the mutex, possibly in sqlite */
static int fork_pending = 0; /* a fork is imminent: writer must park */
static pthread_cond_t resume = PTHREAD_COND_INITIALIZER;
static int atfork_registered = 0;

/* ---- writer-thread-private state ---- */

static sqlite3 *sdb = NULL;
static int opened = 0;
static int in_txn = 0;
static char db_path[4096];
static pid_t db_pid;

/* Events received before the first call. A process that never records a
 * call never opens its database (incidental subprocesses that merely
 * inherit PYTHON_D3G_CONFIG); these are buffered until a call arrives
 * and discarded at stop if none does. */
static TraceEvent *pending = NULL;
static size_t pending_len = 0, pending_cap = 0;

static struct {
    sqlite3_stmt *call, *attr_read, *object, *member, *function, *ipc,
                 *io_object, *io_op, *call_arg;
} st;

static struct {
    size_t calls, objects, ipc, io_objects;
} stats;

/* ---- helpers ---- */

static void free_event(TraceEvent *ev) {
    switch (ev->kind) {
    case EV_CALL:
        if (ev->u.call) {
            free(ev->u.call->control_flow);
            free(ev->u.call->control_flow_exc);
            free(ev->u.call->attr_reads);
            free(ev->u.call);
        }
        break;
    case EV_OBJECT:
        SMap_free(&ev->u.object.members);
        break;
    case EV_FUNCTION:
        free(ev->u.function.ref);
        free(ev->u.function.cfg);
        break;
    case EV_IPC:
        free(ev->u.ipc.name);
        break;
    case EV_IO_OBJECT:
        free(ev->u.io_object.name);
        break;
    case EV_CALL_ARG:
        free(ev->u.call_arg.name);
        break;
    default:
        break;
    }
}

static int exec_sql(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(sdb, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "d3g: SQL error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static const char *SCHEMA_SQL =
    "PRAGMA synchronous=OFF;"
    "CREATE TABLE meta (pid INTEGER);"
    "CREATE TABLE machine (machine_id TEXT NOT NULL);"
    "CREATE TABLE functions (function_id INTEGER PRIMARY KEY, ref TEXT NOT NULL, cfg BLOB);"
    "CREATE TABLE calls ("
    "    pid INTEGER NOT NULL,"
    "    call_id INTEGER NOT NULL,"
    "    function_id INTEGER NOT NULL,"
    "    caller_id INTEGER NOT NULL,"
    "    call_lineno INTEGER NOT NULL,"
    "    obj_id INTEGER NOT NULL,"
    "    control_flow BLOB,"
    "    control_flow_bits INTEGER NOT NULL DEFAULT 0,"
    "    control_flow_exc BLOB,"
    "    func_idx INTEGER NOT NULL DEFAULT 0,"
    "    created_id INTEGER NOT NULL DEFAULT 0,"
    "    created_lineno INTEGER NOT NULL DEFAULT 0,"
    "    PRIMARY KEY (pid, call_id)"
    ");"
    "CREATE TABLE call_args ("
    "    pid INTEGER NOT NULL,"
    "    call_id INTEGER NOT NULL,"
    "    name TEXT NOT NULL,"
    "    obj_idx INTEGER NOT NULL"
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
    "    call_id INTEGER NOT NULL"
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

static int prep(sqlite3_stmt **out, const char *sql) {
    if (sqlite3_prepare_v2(sdb, sql, -1, out, NULL) != SQLITE_OK) {
        fprintf(stderr, "d3g: prepare failed: %s\n", sqlite3_errmsg(sdb));
        return -1;
    }
    return 0;
}

static int is_db_file(const char *name) {
    size_t len = strlen(name);
    return len > 3 && strcmp(name + len - 3, ".db") == 0;
}

/* PYTHON_D3G_OUTDIR is claimed exclusively by d3g: a fresh trace run wipes
 * whatever *.db files (including a previous run's merged trace.db) are left
 * over from the last one, rather than accumulating numbered run directories
 * forever. Anything else found there means the directory wasn't handed to
 * d3g exclusively -- that's a configuration mistake, not something to route
 * around, so it's fatal: the process exits rather than risk deleting or
 * cohabiting with unrelated files. */
static void validate_and_clear_outdir(const char *outdir) {
    DIR *d = opendir(outdir);
    if (!d) return; /* doesn't exist yet -- nothing to validate or clear */
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (!is_db_file(ent->d_name)) {
            fprintf(stderr,
                    "d3g: PYTHON_D3G_OUTDIR '%s' contains '%s', which is not a .db file; "
                    "refusing to overwrite it\n", outdir, ent->d_name);
            closedir(d);
            exit(1);
        }
    }
    rewinddir(d);
    while ((ent = readdir(d))) {
        if (!is_db_file(ent->d_name)) continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", outdir, ent->d_name);
        unlink(path);
    }
    closedir(d);
}

static int open_db(void) {
    const char *outdir = getenv("PYTHON_D3G_OUTDIR");
    if (!outdir) return -1;

    /* A process belongs to the same trace run as its immediate parent if
     * that parent already has a "{parent_pid}.db" directly under outdir;
     * only a process with no traced parent (the root of a new trace tree)
     * clears outdir out for a fresh run. This assumes at most one trace
     * tree writes to a given outdir at a time -- unlike the old numbered
     * run directories, two unrelated root processes racing on the same
     * outdir can now clobber each other. */
    char parent_db[4096];
    snprintf(parent_db, sizeof(parent_db), "%s/%d.db", outdir, (int)getppid());
    struct stat st_buf;
    int has_parent = (stat(parent_db, &st_buf) == 0);

    mkdir(outdir, 0755);
    if (!has_parent) validate_and_clear_outdir(outdir);

    db_pid = getpid();
    snprintf(db_path, sizeof(db_path), "%s/%d.db", outdir, (int)db_pid);
    unlink(db_path);

    if (sqlite3_open(db_path, &sdb) != SQLITE_OK) {
        fprintf(stderr, "d3g: failed to open %s: %s\n", db_path,
                sqlite3_errmsg(sdb));
        sqlite3_close(sdb);
        sdb = NULL;
        return -1;
    }
    if (exec_sql(SCHEMA_SQL) < 0) goto fail;

    {
        sqlite3_stmt *s;
        if (prep(&s, "INSERT INTO meta VALUES (?)") < 0) goto fail;
        sqlite3_bind_int(s, 1, db_pid);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    {
        char machine_id[64] = "";
        FILE *f = fopen("/etc/machine-id", "r");
        if (f) {
            if (fgets(machine_id, sizeof(machine_id), f)) {
                size_t len = strlen(machine_id);
                if (len > 0 && machine_id[len - 1] == '\n')
                    machine_id[len - 1] = '\0';
            }
            fclose(f);
        }
        sqlite3_stmt *s;
        if (prep(&s, "INSERT INTO machine VALUES (?)") < 0) goto fail;
        sqlite3_bind_text(s, 1, machine_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    if (prep(&st.call, "INSERT INTO calls VALUES (?,?,?,?,?,?,?,?,?,?,?,?)") < 0 ||
        prep(&st.call_arg, "INSERT INTO call_args VALUES (?,?,?,?)") < 0 ||
        prep(&st.attr_read, "INSERT INTO attr_reads VALUES (?,?,?,?,?)") < 0 ||
        prep(&st.object, "INSERT INTO objects VALUES (?,?,?)") < 0 ||
        prep(&st.member, "INSERT INTO members VALUES (?,?,?,?)") < 0 ||
        prep(&st.function, "INSERT OR REPLACE INTO functions VALUES (?,?,?)") < 0 ||
        prep(&st.ipc, "INSERT INTO ipc VALUES (?,?,?)") < 0 ||
        prep(&st.io_object, "INSERT INTO io_objects VALUES (?,?,?,?)") < 0 ||
        prep(&st.io_op, "INSERT INTO io_ops VALUES (?,?,?,?,?,?)") < 0)
        goto fail;

    opened = 1;
    return 0;

fail:
    fprintf(stderr, "d3g: cannot initialize %s: %s\n", db_path,
            sqlite3_errmsg(sdb));
    sqlite3_close(sdb);
    sdb = NULL;
    return -1;
}

static void close_db(void) {
    if (!sdb) return;
    sqlite3_finalize(st.call);
    sqlite3_finalize(st.attr_read);
    sqlite3_finalize(st.object);
    sqlite3_finalize(st.member);
    sqlite3_finalize(st.function);
    sqlite3_finalize(st.ipc);
    sqlite3_finalize(st.io_object);
    sqlite3_finalize(st.io_op);
    sqlite3_finalize(st.call_arg);
    memset(&st, 0, sizeof(st));
    sqlite3_close(sdb);
    sdb = NULL;
    opened = 0;
}

static void write_event(const TraceEvent *ev) {
    switch (ev->kind) {
    case EV_CALL: {
        CallRecordData *rec = ev->u.call;
        uint64_t caller = rec->caller_id == TAINT_ID ? 0 : rec->caller_id;
        sqlite3_bind_int(st.call, 1, db_pid);
        sqlite3_bind_int64(st.call, 2, (sqlite3_int64)rec->call_id);
        sqlite3_bind_int(st.call, 3, rec->function_id);
        sqlite3_bind_int64(st.call, 4, (sqlite3_int64)caller);
        sqlite3_bind_int(st.call, 5, rec->call_lineno);
        sqlite3_bind_int(st.call, 6, rec->obj_id);
        if (rec->control_flow && rec->control_flow_len > 0)
            sqlite3_bind_blob(st.call, 7, rec->control_flow,
                              (int)rec->control_flow_len, SQLITE_STATIC);
        else
            sqlite3_bind_null(st.call, 7);
        sqlite3_bind_int64(st.call, 8, (sqlite3_int64)rec->control_flow_bits);
        if (rec->control_flow_exc && rec->control_flow_exc_len > 0)
            sqlite3_bind_blob(st.call, 9, rec->control_flow_exc,
                              (int)rec->control_flow_exc_len, SQLITE_STATIC);
        else
            sqlite3_bind_null(st.call, 9);
        sqlite3_bind_int(st.call, 10, rec->func_idx);
        sqlite3_bind_int64(st.call, 11, (sqlite3_int64)rec->created_id);
        sqlite3_bind_int(st.call, 12, rec->created_lineno);
        sqlite3_step(st.call);
        sqlite3_reset(st.call);

        for (Py_ssize_t j = 0; j < rec->attr_reads_len; j++) {
            AttrRecordReadData *ar = &rec->attr_reads[j];
            uint64_t ar_caller = ar->caller_id == TAINT_ID ? 0 : ar->caller_id;
            sqlite3_bind_int(st.attr_read, 1, db_pid);
            sqlite3_bind_int64(st.attr_read, 2, (sqlite3_int64)rec->call_id);
            sqlite3_bind_int64(st.attr_read, 3, (sqlite3_int64)ar_caller);
            sqlite3_bind_int(st.attr_read, 4, ar->write_call_lineno);
            sqlite3_bind_int(st.attr_read, 5, ar->read_call_lineno);
            sqlite3_step(st.attr_read);
            sqlite3_reset(st.attr_read);
        }
        stats.calls++;
        break;
    }
    case EV_OBJECT: {
        sqlite3_bind_int(st.object, 1, db_pid);
        sqlite3_bind_int64(st.object, 2, (sqlite3_int64)ev->u.object.id);
        sqlite3_bind_int64(st.object, 3, (sqlite3_int64)ev->u.object.call_id);
        sqlite3_step(st.object);
        sqlite3_reset(st.object);

        const SMap *m = &ev->u.object.members;
        if (m->entries) {
            for (size_t k = 0; k < m->capacity; k++) {
                if (!m->entries[k].occupied) continue;
                sqlite3_bind_int(st.member, 1, db_pid);
                sqlite3_bind_int64(st.member, 2, (sqlite3_int64)ev->u.object.id);
                sqlite3_bind_text(st.member, 3, m->entries[k].key, -1, SQLITE_STATIC);
                sqlite3_bind_int64(st.member, 4, (intptr_t)m->entries[k].value);
                sqlite3_step(st.member);
                sqlite3_reset(st.member);
            }
        }
        stats.objects++;
        break;
    }
    case EV_FUNCTION:
        sqlite3_bind_int(st.function, 1, ev->u.function.id);
        sqlite3_bind_text(st.function, 2, ev->u.function.ref, -1, SQLITE_STATIC);
        if (ev->u.function.cfg_len)
            sqlite3_bind_blob(st.function, 3, ev->u.function.cfg,
                              (int)ev->u.function.cfg_len, SQLITE_STATIC);
        else
            sqlite3_bind_null(st.function, 3);
        sqlite3_step(st.function);
        sqlite3_reset(st.function);
        break;
    case EV_IPC:
        sqlite3_bind_int(st.ipc, 1, db_pid);
        sqlite3_bind_text(st.ipc, 2, ev->u.ipc.name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st.ipc, 3, (sqlite3_int64)ev->u.ipc.call_id);
        sqlite3_step(st.ipc);
        sqlite3_reset(st.ipc);
        stats.ipc++;
        break;
    case EV_IO_OBJECT:
        sqlite3_bind_int(st.io_object, 1, db_pid);
        sqlite3_bind_int(st.io_object, 2, ev->u.io_object.id);
        sqlite3_bind_text(st.io_object, 3, ev->u.io_object.name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st.io_object, 4, (sqlite3_int64)ev->u.io_object.offset);
        sqlite3_step(st.io_object);
        sqlite3_reset(st.io_object);
        stats.io_objects++;
        break;
    case EV_IO_OP:
        sqlite3_bind_int(st.io_op, 1, db_pid);
        sqlite3_bind_int(st.io_op, 2, ev->u.io_op.io_object_id);
        sqlite3_bind_int64(st.io_op, 3, (sqlite3_int64)ev->u.io_op.call_id);
        sqlite3_bind_int64(st.io_op, 4, (sqlite3_int64)ev->u.io_op.offset);
        sqlite3_bind_int64(st.io_op, 5, (sqlite3_int64)ev->u.io_op.length);
        sqlite3_bind_int(st.io_op, 6, ev->u.io_op.op_type);
        sqlite3_step(st.io_op);
        sqlite3_reset(st.io_op);
        break;
    case EV_CALL_ARG:
        sqlite3_bind_int(st.call_arg, 1, db_pid);
        sqlite3_bind_int64(st.call_arg, 2, (sqlite3_int64)ev->u.call_arg.call_id);
        sqlite3_bind_text(st.call_arg, 3, ev->u.call_arg.name, -1, SQLITE_STATIC);
        sqlite3_bind_int(st.call_arg, 4, ev->u.call_arg.obj_idx);
        sqlite3_step(st.call_arg);
        sqlite3_reset(st.call_arg);
        break;
    default:
        break;
    }
}

static void begin_txn(void) {
    if (opened && !in_txn && exec_sql("BEGIN") == 0) in_txn = 1;
}

static void commit_txn(void) {
    if (in_txn) {
        exec_sql("COMMIT");
        in_txn = 0;
    }
}

/* Write or buffer one event, then release it. */
static void handle_event(TraceEvent *ev) {
    if (!opened) {
        if (ev->kind != EV_CALL) {
            if (pending_len == pending_cap) {
                size_t cap = pending_cap ? pending_cap * 2 : 64;
                TraceEvent *tmp = realloc(pending, cap * sizeof(TraceEvent));
                if (!tmp) { free_event(ev); return; }
                pending = tmp;
                pending_cap = cap;
            }
            pending[pending_len++] = *ev;
            return;
        }
        if (open_db() < 0) {
            /* Unwritable output: drop everything from now on. */
            free_event(ev);
            return;
        }
        begin_txn();
        for (size_t i = 0; i < pending_len; i++) {
            write_event(&pending[i]);
            free_event(&pending[i]);
        }
        free(pending);
        pending = NULL;
        pending_len = pending_cap = 0;
    }
    if (sdb) write_event(ev);
    free_event(ev);
}

static void drop_pending(void) {
    for (size_t i = 0; i < pending_len; i++) free_event(&pending[i]);
    free(pending);
    pending = NULL;
    pending_len = pending_cap = 0;
}

/* ---- thread ---- */

static void *writer_main(void *arg) {
    (void)arg;
    static TraceEvent batch[BATCH_MAX];
    int stop = 0;

    while (!stop) {
        pthread_mutex_lock(&mu);
        busy = 0;
        pthread_cond_broadcast(&idle);
        while (fork_pending)
            pthread_cond_wait(&resume, &mu);
        while (ring_count == 0)
            pthread_cond_wait(&not_empty, &mu);
        busy = 1;
        size_t n = ring_count < BATCH_MAX ? ring_count : BATCH_MAX;
        for (size_t i = 0; i < n; i++) {
            batch[i] = ring[ring_head];
            ring_head = (ring_head + 1) & (RING_CAP - 1);
        }
        ring_count -= n;
        pthread_cond_broadcast(&not_full);
        pthread_mutex_unlock(&mu);

        begin_txn();
        for (size_t i = 0; i < n; i++) {
            if (batch[i].kind == EV_STOP) { stop = 1; break; }
            handle_event(&batch[i]);
        }
        commit_txn();
    }

    if (opened) {
        fprintf(stderr,
                "Trace written to %s (%zu calls, %zu objects, %zu ipc, %zu io_objects)\n",
                db_path, stats.calls, stats.objects, stats.ipc, stats.io_objects);
        close_db();
    }
    drop_pending();

    pthread_mutex_lock(&mu);
    busy = 0;
    pthread_cond_broadcast(&idle);
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* ---- fork handling ---- */

/* The forking thread holds `mu` across fork() and waits first for the
 * writer to park on `not_empty`, so the child's copy of the writer is not
 * mid-way through malloc or SQLite with their internal locks held. */
static void atfork_prepare(void) {
    pthread_mutex_lock(&mu);
    fork_pending = 1;
    while (running && busy)
        pthread_cond_wait(&idle, &mu);
}

static void atfork_parent(void) {
    fork_pending = 0;
    pthread_cond_broadcast(&resume);
    pthread_mutex_unlock(&mu);
}

static void atfork_child(void) {
    writer_after_fork_child();
}

void writer_after_fork_child(void) {
    /* `mu` is held by this (the only surviving) thread since prepare. */
    for (size_t i = 0; i < ring_count; i++)
        free_event(&ring[(ring_head + i) & (RING_CAP - 1)]);
    ring_head = 0;
    ring_count = 0;
    running = 0;
    accepting = 0;
    busy = 0;
    fork_pending = 0;

    /* The parent's connection and buffered rows belong to the parent. */
    sdb = NULL;
    opened = 0;
    in_txn = 0;
    memset(&st, 0, sizeof(st));
    memset(&stats, 0, sizeof(stats));
    pending = NULL;
    pending_len = pending_cap = 0;

    pthread_mutex_unlock(&mu);
    pthread_mutex_init(&mu, NULL);
    pthread_cond_init(&not_empty, NULL);
    pthread_cond_init(&not_full, NULL);
    pthread_cond_init(&idle, NULL);
    pthread_cond_init(&resume, NULL);
}

/* ---- public API ---- */

int writer_running(void) {
    return running;
}

int writer_start(void) {
    pthread_mutex_lock(&mu);
    if (running) {
        pthread_mutex_unlock(&mu);
        return 0;
    }
    if (!atfork_registered) {
        pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
        atfork_registered = 1;
    }
    memset(&stats, 0, sizeof(stats));
    busy = 1;
    if (pthread_create(&thread, NULL, writer_main, NULL) != 0) {
        busy = 0;
        pthread_mutex_unlock(&mu);
        fprintf(stderr, "d3g: cannot start writer thread\n");
        return -1;
    }
    running = 1;
    accepting = 1;
    pthread_mutex_unlock(&mu);
    return 0;
}

void writer_push(const TraceEvent *ev) {
    pthread_mutex_lock(&mu);
    if (!accepting) {
        pthread_mutex_unlock(&mu);
        TraceEvent copy = *ev;
        free_event(&copy);
        return;
    }
    while (ring_count == RING_CAP)
        pthread_cond_wait(&not_full, &mu);
    ring[(ring_head + ring_count) & (RING_CAP - 1)] = *ev;
    ring_count++;
    pthread_cond_signal(&not_empty);
    pthread_mutex_unlock(&mu);
}

void writer_stop(void) {
    pthread_mutex_lock(&mu);
    if (!running) {
        pthread_mutex_unlock(&mu);
        return;
    }
    while (ring_count == RING_CAP)
        pthread_cond_wait(&not_full, &mu);
    ring[(ring_head + ring_count) & (RING_CAP - 1)].kind = EV_STOP;
    ring_count++;
    accepting = 0;
    pthread_cond_signal(&not_empty);
    pthread_mutex_unlock(&mu);

    pthread_join(thread, NULL);

    pthread_mutex_lock(&mu);
    running = 0;
    pthread_mutex_unlock(&mu);
}

/* ---- constructors ---- */

void writer_push_call(CallRecordData *rec) {
    TraceEvent ev = { .kind = EV_CALL };
    ev.u.call = rec;
    writer_push(&ev);
}

void writer_push_object(uint64_t id, uint64_t call_id, SMap *members) {
    TraceEvent ev = { .kind = EV_OBJECT };
    ev.u.object.id = id;
    ev.u.object.call_id = call_id;
    ev.u.object.members = *members;   /* move */
    memset(members, 0, sizeof(*members));
    writer_push(&ev);
}

void writer_push_function(int32_t id, const char *ref, const uint8_t *cfg, size_t cfg_len) {
    TraceEvent ev = { .kind = EV_FUNCTION };
    ev.u.function.id = id;
    ev.u.function.ref = strdup(ref);
    ev.u.function.cfg = NULL;
    ev.u.function.cfg_len = 0;
    if (cfg && cfg_len) {
        ev.u.function.cfg = malloc(cfg_len);
        if (ev.u.function.cfg) {
            memcpy(ev.u.function.cfg, cfg, cfg_len);
            ev.u.function.cfg_len = cfg_len;
        }
    }
    writer_push(&ev);
}

void writer_push_ipc(const char *name, int64_t call_id) {
    TraceEvent ev = { .kind = EV_IPC };
    ev.u.ipc.name = strdup(name);
    ev.u.ipc.call_id = call_id;
    writer_push(&ev);
}

void writer_push_io_object(uint32_t id, const char *name, uint64_t offset) {
    TraceEvent ev = { .kind = EV_IO_OBJECT };
    ev.u.io_object.id = id;
    ev.u.io_object.name = strdup(name);
    ev.u.io_object.offset = offset;
    writer_push(&ev);
}

void writer_push_call_arg(uint64_t call_id, const char *name, int32_t obj_idx) {
    TraceEvent ev = { .kind = EV_CALL_ARG };
    ev.u.call_arg.call_id = call_id;
    ev.u.call_arg.name = strdup(name);
    ev.u.call_arg.obj_idx = obj_idx;
    writer_push(&ev);
}

void writer_push_io_op(uint32_t io_object_id, uint64_t call_id,
                       uint64_t offset, uint64_t length, int op_type) {
    TraceEvent ev = { .kind = EV_IO_OP };
    ev.u.io_op.io_object_id = io_object_id;
    ev.u.io_op.call_id = call_id;
    ev.u.io_op.offset = offset;
    ev.u.io_op.length = length;
    ev.u.io_op.op_type = op_type;
    writer_push(&ev);
}
