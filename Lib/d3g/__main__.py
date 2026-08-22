"""Run a script under the tracer.

    PYTHON_TRACER_CONFIG=config.yaml PYTHON_TRACER_OUTDIR=dir \
        python -m d3g -- script.py [args...]

Every traced process, this one included, streams its records into its own
{pid}.db in the output directory while it runs; uninstall() drains what is
still in flight at exit. Merging those files and building the dependency
graph is an offline step:

    python -m d3g.postprocess dir
"""

from __future__ import annotations

import os
import shutil
import sys


def main() -> None:
    sys.argv = sys.argv[1:]
    if sys.argv and sys.argv[0] == "--":
        sys.argv = sys.argv[1:]

    if not sys.argv:
        print("Usage: PYTHON_TRACER_CONFIG=config.yaml python -m d3g -- script.py [args...]", file=sys.stderr)
        sys.exit(1)

    from _tracer import uninstall

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

    try:
        exec(code, {"__name__": "__main__", "__file__": script})
    except SystemExit:
        pass
    finally:
        # Idempotent; the interpreter also flushes at finalization, on
        # os._exit and on SIG_DFL termination (see Modules/_tracer/signals.c).
        uninstall()


if __name__ == "__main__":
    main()
