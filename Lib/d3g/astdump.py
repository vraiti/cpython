"""AST service for the Rust postprocessor.

Reads one request per line on stdin -- a source path followed by the
tab-separated qualnames of the functions wanted from it -- and answers with
one length-prefixed binary record holding only what the dataflow resolver
consumes for those functions: parameter names and a statement skeleton
with line numbers, control-flow bodies, assignment targets and the
name/attribute/call expressions the resolver inspects. Control flow is
replayed from the bytecode graph d3g stored in the trace, so
nothing about branches is sent here.

Wire format (little-endian; NONE = 0xFFFFFFFF for optional indices):

    u32 payload length, then:
    u8  status              0 = file unreadable/unparsable (nothing follows)
    u32 nstrings, nstrings * (u16 len, utf-8 bytes)     string table
    u32 nfuncs, nfuncs * function

    function:  u32 qualname, u16 nparams, nparams * u32, body
    body:      u32 nstmts, nstmts * stmt
    stmt:      u32 line, u8 kind, payload by kind:
        0 other
        1 for     u32 target|NONE, expr iter, body, body(else)
        2 while   expr test, body, body(else)
        3 if      expr test, body, body(else)
        4 with    body
        5 try     body, body(handlers + else + finally, flattened)
        6 assign  u16 ntargets, ntargets * target, expr
        7 ann     u32 target|NONE, u8 has_value, [expr]
        8 aug     u32 target|NONE, expr
        9 return  u8 has_value, [expr]
        10 expr   expr                     (expression statement that is a call)
    target:    u8 tag: 0 name u32 | 1 attr u32 chain|NONE | 2 other
    expr:      u8 tag:
        0 name    u32 id
        1 attr    u32 chain|NONE, u32 line
        2 call    u32 line, u32 callee|NONE, u32 obj|NONE,
                  u16 nargs, nargs * expr, u16 nkw, nkw * (u32 name|NONE, expr),
                  u16 nnames, nnames * u32
        3 other   u16 nnames, nnames * u32

    python -m d3g.astdump
"""

from __future__ import annotations

import ast
import struct
import sys

_DEFS = (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
_FUNCS = (ast.FunctionDef, ast.AsyncFunctionDef)
NONE = 0xFFFFFFFF


class _Writer:
    def __init__(self) -> None:
        self.buf = bytearray()
        self.strings: list[bytes] = []
        self.index: dict[str, int] = {}

    def s(self, value: str | None) -> int:
        if value is None:
            return NONE
        i = self.index.get(value)
        if i is None:
            i = self.index[value] = len(self.strings)
            self.strings.append(value.encode("utf-8", "surrogateescape"))
        return i

    def u8(self, v: int) -> None:
        self.buf.append(v)

    def u16(self, v: int) -> None:
        self.buf += struct.pack("<H", v)

    def u32(self, v: int) -> None:
        self.buf += struct.pack("<I", v)

    def finish(self, status: int) -> bytes:
        head = bytearray([status])
        if status:
            head += struct.pack("<I", len(self.strings))
            for b in self.strings:
                head += struct.pack("<H", len(b)) + b
        payload = bytes(head) + bytes(self.buf)
        return struct.pack("<I", len(payload)) + payload


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
    # `await f()` and `not f()` carry the same dataflow as `f()`.
    while isinstance(e, (ast.Await, ast.UnaryOp)):
        e = e.value if isinstance(e, ast.Await) else e.operand
    return e


def _expr(w: _Writer, e: ast.expr) -> None:
    e = _unwrap(e)
    if isinstance(e, ast.Name):
        w.u8(0)
        w.u32(w.s(e.id))
    elif isinstance(e, ast.Attribute) and _attr_chain(e) is not None:
        w.u8(1)
        w.u32(w.s(_attr_chain(e)))
        w.u32(e.lineno)
    elif isinstance(e, ast.Call):
        f = e.func
        obj = f.value.id if isinstance(f, ast.Attribute) and isinstance(f.value, ast.Name) else None
        callee = f.attr if isinstance(f, ast.Attribute) else f.id if isinstance(f, ast.Name) else None
        w.u8(2)
        w.u32(e.lineno)
        w.u32(w.s(callee))
        w.u32(w.s(obj))
        w.u16(len(e.args))
        for a in e.args:
            _expr(w, a)
        w.u16(len(e.keywords))
        for kw in e.keywords:
            w.u32(w.s(kw.arg))
            _expr(w, kw.value)
        _name_list(w, e)
    else:
        w.u8(3)
        _name_list(w, e)


def _name_list(w: _Writer, e: ast.AST) -> None:
    names = _names(e)
    w.u16(len(names))
    for n in names:
        w.u32(w.s(n))


def _target(w: _Writer, t: ast.expr) -> None:
    if isinstance(t, ast.Name):
        w.u8(0)
        w.u32(w.s(t.id))
    elif isinstance(t, ast.Attribute):
        w.u8(1)
        w.u32(w.s(_attr_chain(t)))
    else:
        w.u8(2)


def _flatten_target(t: ast.expr) -> list[ast.expr]:
    if isinstance(t, (ast.Tuple, ast.List)):
        return [x for e in t.elts for x in _flatten_target(e)]
    if isinstance(t, ast.Starred):
        return _flatten_target(t.value)
    return [t]


def _name_or_none(w: _Writer, t: ast.expr) -> int:
    return w.s(t.id) if isinstance(t, ast.Name) else NONE


def _opt_expr(w: _Writer, e: ast.expr | None) -> None:
    if e is None:
        w.u8(0)
    else:
        w.u8(1)
        _expr(w, e)


def _stmt(w: _Writer, s: ast.stmt) -> None:
    w.u32(s.lineno)
    if isinstance(s, (ast.For, ast.AsyncFor)):
        w.u8(1)
        w.u32(_name_or_none(w, s.target))
        _expr(w, s.iter)
        _body(w, s.body)
        _body(w, s.orelse)
    elif isinstance(s, ast.While):
        w.u8(2)
        _expr(w, s.test)
        _body(w, s.body)
        _body(w, s.orelse)
    elif isinstance(s, ast.If):
        w.u8(3)
        _expr(w, s.test)
        _body(w, s.body)
        _body(w, s.orelse)
    elif isinstance(s, (ast.With, ast.AsyncWith)):
        w.u8(4)
        _body(w, s.body)
    elif isinstance(s, ast.Try) or (hasattr(ast, "TryStar") and isinstance(s, ast.TryStar)):
        w.u8(5)
        _body(w, s.body)
        rest = [st for h in s.handlers for st in h.body] + list(s.orelse) + list(s.finalbody)
        _body(w, rest)
    elif isinstance(s, ast.Assign):
        # `a, b = value` binds every element to the value's sources.
        targets = [t for tgt in s.targets for t in _flatten_target(tgt)]
        w.u8(6)
        w.u16(len(targets))
        for t in targets:
            _target(w, t)
        _expr(w, s.value)
    elif isinstance(s, ast.AnnAssign):
        w.u8(7)
        w.u32(_name_or_none(w, s.target))
        _opt_expr(w, s.value)
    elif isinstance(s, ast.AugAssign):
        w.u8(8)
        w.u32(_name_or_none(w, s.target))
        _expr(w, s.value)
    elif isinstance(s, ast.Return):
        w.u8(9)
        _opt_expr(w, s.value)
    elif isinstance(s, ast.Expr) and isinstance(s.value, (ast.Yield, ast.YieldFrom)):
        # A generator's yielded value is what its consumer receives.
        w.u8(9)
        _opt_expr(w, s.value.value)
    elif isinstance(s, ast.Expr) and isinstance(_unwrap(s.value), ast.Call):
        w.u8(10)
        _expr(w, s.value)
    else:
        w.u8(0)


def _body(w: _Writer, stmts: list[ast.stmt]) -> None:
    w.u32(len(stmts))
    for s in stmts:
        _stmt(w, s)


def _index_defs(node: ast.AST, prefix: str, out: dict[str, ast.AST]) -> None:
    # Only *direct* children: a def nested inside an if/try/with block is
    # not reachable by qualname lookup, matching ast.iter_child_nodes.
    for child in ast.iter_child_nodes(node):
        if isinstance(child, _FUNCS):
            qualname = prefix + child.name
            out[qualname] = child
            _index_defs(child, qualname + ".<locals>.", out)
        elif isinstance(child, ast.ClassDef):
            _index_defs(child, prefix + child.name + ".", out)
    # Lambdas share the qualname `<lambda>` within a scope; the first one
    # in source order stands for all of them. Nested defs keep their own.
    lam_prefix = prefix if isinstance(node, ast.Module) else prefix
    for lam in _scope_lambdas(node):
        out.setdefault(lam_prefix + "<lambda>", lam)


def _scope_lambdas(node: ast.AST) -> list[ast.Lambda]:
    found: list[ast.Lambda] = []
    stack = list(ast.iter_child_nodes(node))
    while stack:
        n = stack.pop(0)
        if isinstance(n, _DEFS):
            continue
        if isinstance(n, ast.Lambda):
            found.append(n)
            continue
        stack.extend(ast.iter_child_nodes(n))
    return found


def _function_body(fn: ast.AST) -> list[ast.stmt]:
    if isinstance(fn, ast.Lambda):
        ret = ast.Return(value=fn.body)
        ret.lineno = fn.lineno
        return [ret]
    return fn.body


def _params(fn: ast.AST) -> list[str]:
    a = fn.args
    params = [p.arg for p in a.posonlyargs + a.args]
    params += [p.arg for p in a.kwonlyargs]
    if a.vararg:
        params.append(a.vararg.arg)
    if a.kwarg:
        params.append(a.kwarg.arg)
    return params


def dump(path: str, qualnames: list[str]) -> bytes:
    w = _Writer()
    try:
        with open(path) as f:
            source = f.read()
        tree = ast.parse(source, path)
    except (SyntaxError, OSError, ValueError):
        return w.finish(0)
    defs: dict[str, ast.AST] = {}
    _index_defs(tree, "", defs)
    found = [(q, defs[q]) for q in qualnames if q in defs]
    w.u32(len(found))
    for q, fn in found:
        w.u32(w.s(q))
        params = _params(fn)
        w.u16(len(params))
        for p in params:
            w.u32(w.s(p))
        _body(w, _function_body(fn))
    return w.finish(1)


def main() -> None:
    out = sys.stdout.buffer
    for line in sys.stdin:
        fields = line.rstrip("\n").split("\t")
        out.write(dump(fields[0], fields[1:]))
        out.flush()


if __name__ == "__main__":
    main()
