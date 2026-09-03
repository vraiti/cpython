"""Merge per-process traces and build the dependency graph.

    python -m d3g.postprocess DIR      # merge DIR/{pid}.db into DIR/trace.db, then postprocess
    python -m d3g.postprocess trace.db # postprocess one database

The work is done by the Rust binary `d3g-postprocess` (source in
postprocess/ at the repository root; build with `cargo build --release`).
It parses source files via `$VIRTUAL_ENV/bin/python3` (deliberately a plain,
uninstrumented interpreter, not this one, so parsing source for the
postprocessor doesn't itself get traced) through `d3g.astdump`, so the
statement structure replayed against the recorded control flow is the one
the traced program actually ran. $VIRTUAL_ENV and $PYTHON_D3G_OUTDIR must
be set by the caller.

The binary is located via $D3G_POSTPROCESS, then $PATH, then the
repository's postprocess/target/release directory.
"""

from __future__ import annotations

import os
import shutil
import sys


def find_binary() -> str | None:
    explicit = os.environ.get("D3G_POSTPROCESS")
    if explicit:
        return explicit
    found = shutil.which("d3g-postprocess")
    if found:
        return found
    # Lib/d3g/postprocess.py -> Lib/d3g -> Lib -> cpython -> repository root
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    candidate = os.path.join(repo, "postprocess", "target", "release", "d3g-postprocess")
    if os.access(candidate, os.X_OK):
        return candidate
    return None


def main() -> None:
    binary = find_binary()
    if binary is None:
        print(
            "d3g.postprocess: d3g-postprocess binary not found; build it with\n"
            "  cargo build --release --manifest-path postprocess/Cargo.toml\n"
            "or set $D3G_POSTPROCESS to its path.",
            file=sys.stderr,
        )
        sys.exit(1)
    argv = [binary, *sys.argv[1:]]
    os.execv(binary, argv)


if __name__ == "__main__":
    main()
