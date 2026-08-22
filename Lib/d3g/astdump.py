"""AST service for the Rust postprocessor.

Reads one source-file path per line on stdin and answers each with one JSON
line describing only what the postprocessor consumes: the tree of directly
nested function/class definitions, each function's parameter names, and a
statement skeleton with line numbers, control-flow bodies, assignment
targets and the expressions whose names the dataflow resolver inspects.

The reply is ``null`` when the file cannot be read or parsed. The parsed
``ast`` tree is discarded after each reply.

    python -m d3g.astdump
"""

from __future__ import annotations

import ast
import dis
import json
import sys

_DEFS = (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
_FUNCS = (ast.FunctionDef, ast.AsyncFunctionDef)


def _names(expr: ast.AST) -> list[str]:
    return sorted({n.id for n in ast.walk(expr) if isinstance(n, ast.Name)})


def _attr_chain(expr: ast.expr) -> str | None:
    parts: list[str] = []
    node = expr
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
        parts.reverse()
        return ".".join(parts)
    return None


def _unwrap(e: ast.expr) -> ast.expr:
    # `await f()` carries the same dataflow as `f()`.
    while isinstance(e, ast.Await):
        e = e.value
    return e


def _expr(e: ast.expr) -> dict:
    e = _unwrap(e)
    if isinstance(e, ast.Name):
        return {"k": "n", "id": e.id}
    if isinstance(e, ast.Attribute):
        return {"k": "a", "c": _attr_chain(e), "l": e.lineno}
    if isinstance(e, ast.Call):
        f = e.func
        obj = f.value.id if isinstance(f, ast.Attribute) and isinstance(f.value, ast.Name) else None
        callee = f.attr if isinstance(f, ast.Attribute) else f.id if isinstance(f, ast.Name) else None
        return {
            "k": "c",
            "l": e.lineno,
            "f": callee,
            "o": obj,
            "a": [_expr(a) for a in e.args],
            "kw": [[kw.arg, _expr(kw.value)] for kw in e.keywords],
            "nm": _names(e),
        }
    return {"k": "x", "nm": _names(e)}


def _target(t: ast.expr) -> dict:
    if isinstance(t, ast.Name):
        return {"k": "n", "id": t.id}
    if isinstance(t, ast.Attribute):
        return {"k": "a", "c": _attr_chain(t)}
    return {"k": "x"}


def _name_or_none(t: ast.expr) -> str | None:
    return t.id if isinstance(t, ast.Name) else None


def _stmt(s: ast.stmt) -> dict:
    d: dict = {"l": s.lineno}
    if isinstance(s, (ast.For, ast.AsyncFor)):
        d.update(t="for", tg=_name_or_none(s.target), it=_expr(s.iter),
                 b=_body(s.body), e=_body(s.orelse))
    elif isinstance(s, ast.While):
        d.update(t="while", b=_body(s.body), e=_body(s.orelse))
    elif isinstance(s, ast.If):
        d.update(t="if", b=_body(s.body), e=_body(s.orelse))
    elif isinstance(s, (ast.With, ast.AsyncWith)):
        d.update(t="with", b=_body(s.body))
    elif isinstance(s, ast.Try) or (hasattr(ast, "TryStar") and isinstance(s, ast.TryStar)):
        d.update(t="try", b=_body(s.body))
    elif isinstance(s, ast.Assign):
        d.update(t="assign", tg=[_target(t) for t in s.targets], v=_expr(s.value))
    elif isinstance(s, ast.AnnAssign):
        d.update(t="ann", tg=_name_or_none(s.target),
                 v=_expr(s.value) if s.value is not None else None)
    elif isinstance(s, ast.AugAssign):
        d.update(t="aug", tg=_name_or_none(s.target), v=_expr(s.value))
    elif isinstance(s, ast.Return):
        d.update(t="ret", v=_expr(s.value) if s.value is not None else None)
    elif isinstance(s, ast.Expr) and isinstance(s.value, (ast.Yield, ast.YieldFrom)):
        # A generator's yielded value is what its consumer receives.
        d.update(t="ret", v=_expr(s.value.value) if s.value.value is not None else None)
    elif isinstance(s, ast.Expr) and isinstance(_unwrap(s.value), ast.Call):
        d.update(t="expr", v=_expr(s.value))
    else:
        d["t"] = "o"
    return d


def _body(stmts: list[ast.stmt]) -> list[dict]:
    return [_stmt(s) for s in stmts]


# ---- bytecode control-flow graph ------------------------------------------
#
# The tracer records one byte per executed conditional jump (0/1 = not
# taken/taken; FOR_ITER: next/exhausted) and per `async for` iteration
# attempt (0, then 2 once exhausted). Replaying those bytes needs the
# bytecode's branch structure, not the statement tree, because compound
# conditions, ternaries and comprehensions all emit bytes of their own.
#
# Each function's graph is a list of [line, kind, target] nodes: the
# instructions that change line, branch, or are branched to. `target` is
# the index of the node jumped to.

CFG_LINEAR, CFG_COND, CFG_FOR, CFG_JUMP, CFG_SEND, CFG_ANEXT, CFG_STOP = range(7)

_COND = {"POP_JUMP_IF_FALSE", "POP_JUMP_IF_TRUE", "POP_JUMP_IF_NONE", "POP_JUMP_IF_NOT_NONE"}
_JUMP = {"JUMP_FORWARD", "JUMP_BACKWARD", "JUMP_BACKWARD_NO_INTERRUPT", "JUMP", "JUMP_NO_INTERRUPT"}
_STOP = {"RETURN_VALUE", "RAISE_VARARGS", "RERAISE"}


def _cfg(co) -> list[list[int]]:
    instrs = list(dis.get_instructions(co))
    by_offset = {i.offset: n for n, i in enumerate(instrs)}
    handlers = dis._parse_exception_table(co)

    def after_end_async_for(offset: int) -> int | None:
        for h in handlers:
            if h.start <= offset < h.end and instrs[by_offset[h.target]].opname == "END_ASYNC_FOR":
                n = by_offset[h.target] + 1
                return instrs[n].offset if n < len(instrs) else None
        return None

    nodes = []  # (offset, line, kind, target_offset)
    for i in instrs:
        kind, target = CFG_LINEAR, None
        if i.opname in _COND:
            kind, target = CFG_COND, i.argval
        elif i.opname == "FOR_ITER":
            kind, target = CFG_FOR, i.argval
        elif i.opname in _JUMP:
            kind, target = CFG_JUMP, i.argval
        elif i.opname == "SEND":
            kind, target = CFG_SEND, i.argval
        elif i.opname == "GET_ANEXT":
            target = after_end_async_for(i.offset)
            kind = CFG_ANEXT if target is not None else CFG_LINEAR
        elif i.opname in _STOP:
            kind = CFG_STOP
        line = i.positions.lineno if i.positions and i.positions.lineno else 0
        nodes.append([i.offset, line, kind, target])

    targets = {n[3] for n in nodes if n[3] is not None}
    kept = []
    prev_line = None
    for n in nodes:
        if n[2] != CFG_LINEAR or n[0] in targets or n[1] != prev_line:
            kept.append(n)
        prev_line = n[1]
    index = {n[0]: k for k, n in enumerate(kept)}
    return [[n[1], n[2], index[n[3]] if n[3] is not None else -1] for n in kept]


def _cfgs(source: str, path: str) -> dict[str, list]:
    try:
        code = compile(source, path, "exec")
    except (SyntaxError, ValueError, RecursionError):
        return {}
    out = {}
    stack = [code]
    while stack:
        co = stack.pop()
        if co is not code:
            out[co.co_qualname] = _cfg(co)
        stack.extend(c for c in co.co_consts if hasattr(c, "co_code"))
    return out


def _defs(node: ast.AST, cfgs: dict, prefix: str = "") -> list[dict]:
    # Only *direct* children: a def nested inside an if/try/with block is
    # not reachable by qualname lookup, matching ast.iter_child_nodes.
    out = []
    for child in ast.iter_child_nodes(node):
        if isinstance(child, _FUNCS):
            a = child.args
            params = [p.arg for p in a.posonlyargs + a.args]
            params += [p.arg for p in a.kwonlyargs]
            if a.vararg:
                params.append(a.vararg.arg)
            if a.kwarg:
                params.append(a.kwarg.arg)
            qualname = prefix + child.name
            d = {"n": child.name, "fn": True, "p": params,
                 "b": _body(child.body), "d": _defs(child, cfgs, qualname + ".<locals>.")}
            cfg = cfgs.get(qualname)
            if cfg is not None:
                d["c"] = cfg
            out.append(d)
        elif isinstance(child, ast.ClassDef):
            out.append({"n": child.name, "fn": False,
                        "d": _defs(child, cfgs, prefix + child.name + ".")})
    return out


def dump(path: str) -> dict | None:
    try:
        with open(path) as f:
            source = f.read()
        tree = ast.parse(source, path)
    except (SyntaxError, OSError, ValueError):
        return None
    return {"d": _defs(tree, _cfgs(source, path))}


def main() -> None:
    out = sys.stdout
    for line in sys.stdin:
        path = line.rstrip("\n")
        out.write(json.dumps(dump(path), separators=(",", ":")))
        out.write("\n")
        out.flush()


if __name__ == "__main__":
    main()
