from __future__ import annotations

import os
import threading

db = None


def init() -> None:
    global db
    import yaml
    with open(os.environ["PYTHON_TRACER_CONFIG"]) as f:
        cfg = yaml.safe_load(f) or {}

    os.makedirs(os.environ["PYTHON_TRACER_OUTDIR"], exist_ok=True)

    modules = cfg.get("modules") or []
    tracked_classes = cfg.get("classes") or []
    taint_patterns = cfg.get("taint-functions") or None
    # traceall: trace every call and every class regardless of `modules`
    # and `classes`; `taint-functions` still excludes their subtrees.
    traceall = bool(cfg.get("traceall", False))

    prefixes = []
    if modules:
        import importlib.util
        for mod_name in modules:
            spec = importlib.util.find_spec(mod_name)
            if spec is None:
                continue
            if spec.submodule_search_locations:
                prefixes.append(spec.submodule_search_locations[0])
            elif spec.origin:
                prefixes.append(os.path.dirname(os.path.abspath(spec.origin)))

    from _tracer import Database, PathFilter, install, install_thread

    path_filter = PathFilter(prefixes=prefixes, tracked_classes=tracked_classes)
    db = Database()  # noqa: F841 — stored as module global
    install(prefixes, db, path_filter, taint_patterns=taint_patterns,
            traceall=traceall)

    _original_run = threading.Thread.run
    def _patched_run(self: threading.Thread) -> None:
        install_thread()
        _original_run(self)
    threading.Thread.run = _patched_run  # type: ignore
