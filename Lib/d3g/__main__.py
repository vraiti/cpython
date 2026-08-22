from __future__ import annotations

import glob
import os
import shutil
import signal
import sqlite3
import sys
import tempfile
from typing import Any


def _wait_for_traces(output_dir: str) -> None:
    import time
    while True:
        pids = []
        for name in os.listdir(output_dir):
            if name.endswith(".db"):
                try:
                    pids.append(int(name[:-3]))
                except ValueError:
                    continue
        alive = [pid for pid in pids if os.path.exists(f"/proc/{pid}")]
        if not alive:
            return
        time.sleep(1)


def _merge_dbs(output_dir: str, all_dbs: list[str]) -> None:
    merged_path = os.path.join(output_dir, "trace.db")
    fd, tmp_path = tempfile.mkstemp(suffix=".db", dir=output_dir)
    os.close(fd)
    first, rest = all_dbs[0], all_dbs[1:]
    shutil.copy2(first, tmp_path)
    conn = sqlite3.connect(tmp_path)
    for db_path in rest:
        try:
            conn.execute("ATTACH DATABASE ? AS child", (db_path,))

            # function_id values are interned per process, so the child's
            # ids collide with the parent's. Re-key the child's functions by
            # ref and rewrite its calls to the merged ids.
            fmap: dict[int, int] = {}
            for fid, ref in conn.execute(
                    "SELECT function_id, ref FROM child.functions").fetchall():
                row = conn.execute(
                    "SELECT function_id FROM functions WHERE ref = ?", (ref,)).fetchone()
                if row is None:
                    cur = conn.execute("INSERT INTO functions (ref) VALUES (?)", (ref,))
                    fmap[fid] = cur.lastrowid
                else:
                    fmap[fid] = row[0]

            conn.execute("INSERT OR IGNORE INTO meta SELECT * FROM child.meta")
            for pid, call_id, fid, caller_id, lineno, obj_id, cf in conn.execute(
                    "SELECT pid, call_id, function_id, caller_id, call_lineno, "
                    "obj_id, control_flow FROM child.calls").fetchall():
                conn.execute(
                    "INSERT INTO calls VALUES (?, ?, ?, ?, ?, ?, ?)",
                    (pid, call_id, fmap.get(fid, fid), caller_id, lineno, obj_id, cf))
            for table in ("attr_reads", "objects", "members", "ipc",
                          "io_objects", "io_ops"):
                conn.execute(f"INSERT INTO {table} SELECT * FROM child.{table}")

            # SQLite refuses to DETACH a database used by the open transaction.
            conn.commit()
            conn.execute("DETACH DATABASE child")
        except sqlite3.Error as e:
            print(f"Failed to merge {db_path}: {e}", file=sys.stderr)
            try:
                conn.rollback()
                conn.execute("DETACH DATABASE child")
            except Exception:
                pass
    conn.commit()
    conn.close()
    for db_path in all_dbs:
        os.remove(db_path)
    os.rename(tmp_path, merged_path)


def main() -> None:
    sys.argv = sys.argv[1:]
    if sys.argv and sys.argv[0] == "--":
        sys.argv = sys.argv[1:]

    if not sys.argv:
        print("Usage: PYTHON_TRACER_CONFIG=config.yaml python -m d3g -- script.py [args...]", file=sys.stderr)
        sys.exit(1)

    from _tracer import uninstall

    output_dir = os.environ.get("PYTHON_TRACER_OUTDIR", "traces")

    script = sys.argv[0]
    if not os.path.exists(script):
        resolved = shutil.which(script)
        if resolved:
            script = resolved

    # Compile under the absolute path: the scope filter compares
    # co_filename against absolute module prefixes, so a relative script
    # path would leave the script's own code untraced.
    script = os.path.abspath(script)
    with open(script) as f:
        code = compile(f.read(), script, "exec")

    _uninstalled = False

    _orig_signal = signal.signal
    def _wrap_signal(signum: int, handler: Any) -> Any:
        if signum == signal.SIGTERM and callable(handler):
            real_handler = handler
            def _wrapper(s: int, f: Any) -> Any:
                nonlocal _uninstalled
                if not _uninstalled:
                    uninstall()
                    _uninstalled = True
                return real_handler(s, f)
            return _orig_signal(signum, _wrapper)
        return _orig_signal(signum, handler)
    signal.signal = _wrap_signal  # type: ignore

    def _default_sigterm(signum: int, frame: Any) -> None:
        nonlocal _uninstalled
        if not _uninstalled:
            uninstall()
            _uninstalled = True
        raise SystemExit(0)
    _orig_signal(signal.SIGTERM, _default_sigterm)

    try:
        exec(code, {"__name__": "__main__", "__file__": script})
    except SystemExit:
        pass
    finally:
        if not _uninstalled:
            uninstall()
            _uninstalled = True
        signal.signal = _orig_signal  # type: ignore

        _wait_for_traces(output_dir)

        from d3g._bootstrap import db
        db.serialize(os.path.join(output_dir, f"{os.getpid()}.db"))

        # Always leave exactly one artifact, trace.db: merge when several
        # processes wrote traces, rename when only this one did. A stale
        # trace.db from an earlier run is excluded from the inputs and
        # overwritten by the result.
        merged_path = os.path.join(output_dir, "trace.db")
        all_dbs = sorted(
            p for p in glob.glob(os.path.join(output_dir, "*.db"))
            if os.path.basename(p) != "trace.db"
        )
        if len(all_dbs) == 1:
            os.replace(all_dbs[0], merged_path)
        elif all_dbs:
            _merge_dbs(output_dir, all_dbs)


if __name__ == "__main__":
    main()
