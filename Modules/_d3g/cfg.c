#include "cfg.h"
#include "pycore_code.h"             /* _Py_GetBaseCodeUnit, address ranges */
#include "pycore_opcode_metadata.h"  /* _PyOpcode_Caches */
#include "opcode_ids.h"
#include <stdlib.h>
#include <string.h>

#define CFG_LINEAR 0
#define CFG_COND   1
#define CFG_FOR    2
#define CFG_JUMP   3
#define CFG_SEND   4
#define CFG_ANEXT  5
#define CFG_STOP   6
#define CFG_HANDLER 7

#define NO_TARGET (-1)

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

uint8_t *d3g_build_cfg(PyCodeObject *co, size_t *len_out) {
    *len_out = 0;
    int n = (int)Py_SIZE(co);           /* code units */
    if (n <= 0) return NULL;

    int32_t *line = malloc(sizeof(int32_t) * n);
    int32_t *target = malloc(sizeof(int32_t) * n);
    uint8_t *kind = calloc((size_t)n, 1);
    uint8_t *is_instr = calloc((size_t)n, 1);
    uint8_t *is_target = calloc((size_t)n, 1);
    int32_t *kept_index = malloc(sizeof(int32_t) * (n + 1));
    if (!line || !target || !kind || !is_instr || !is_target || !kept_index) goto fail;
    for (int i = 0; i < n; i++) { line[i] = 0; target[i] = NO_TARGET; }

    /* Lines from the location table. */
    PyCodeAddressRange range;
    _PyCode_InitAddressRange(co, &range);
    while (_PyLineTable_NextAddressRange(&range)) {
        int lo = range.ar_start / 2, hi = range.ar_end / 2;
        for (int a = lo; a < hi && a < n; a++) {
            if (a >= 0) line[a] = range.ar_line > 0 ? range.ar_line : 0;
        }
    }

    /* Instructions. An EXTENDED_ARG prefix belongs to the instruction it
     * extends; the node lives at the prefix so jump targets resolve. */
    int i = 0;
    while (i < n) {
        int start = i;
        _Py_CODEUNIT u = _Py_GetBaseCodeUnit(co, i);
        int op = u.op.code;
        int ext = 0;
        while (op == EXTENDED_ARG && i + 1 < n) {
            ext = (ext << 8) | u.op.arg;
            i++;
            u = _Py_GetBaseCodeUnit(co, i);
            op = u.op.code;
        }
        int arg = (ext << 8) | u.op.arg;
        int next = i + 1 + _PyOpcode_Caches[op];
        is_instr[start] = 1;
        switch (op) {
        case POP_JUMP_IF_FALSE: case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_NONE: case POP_JUMP_IF_NOT_NONE:
            kind[start] = CFG_COND; target[start] = next + arg; break;
        case FOR_ITER:
            kind[start] = CFG_FOR; target[start] = next + arg; break;
        case JUMP_FORWARD:
            kind[start] = CFG_JUMP; target[start] = next + arg; break;
        case JUMP_BACKWARD: case JUMP_BACKWARD_NO_INTERRUPT:
            kind[start] = CFG_JUMP; target[start] = next - arg; break;
        case SEND:
            kind[start] = CFG_SEND; target[start] = next + arg; break;
        case GET_ANEXT:
            kind[start] = CFG_ANEXT; break;          /* resolved below */
        case END_ASYNC_FOR:
            /* Not a jump, but its oparg points back at the loop's END_SEND;
             * stash it so the matching GET_ANEXT can be found. */
            target[start] = next - arg; break;
        case RETURN_VALUE: case RAISE_VARARGS: case RERAISE:
            kind[start] = CFG_STOP; break;
        case PUSH_EXC_INFO:
            kind[start] = CFG_HANDLER; target[start] = start; break;
        default: break;
        }
        i = next;
    }

    /* GET_ANEXT -> instruction after the END_ASYNC_FOR that ends its loop:
     * END_ASYNC_FOR's back-reference names the END_SEND, the SEND jumping
     * there precedes it, and GET_ANEXT sits just before that SEND. */
    for (int e = 0; e < n; e++) {
        _Py_CODEUNIT u;
        if (!is_instr[e] || target[e] == NO_TARGET || kind[e] != CFG_LINEAR) continue;
        u = _Py_GetBaseCodeUnit(co, e);
        if (u.op.code != END_ASYNC_FOR && u.op.code != EXTENDED_ARG) continue;
        int end_send = target[e];
        target[e] = NO_TARGET;
        for (int s = 0; s < n; s++) {
            if (!is_instr[s] || kind[s] != CFG_SEND || target[s] != end_send) continue;
            for (int a = s - 1; a >= 0 && a >= s - 8; a--) {
                if (is_instr[a] && kind[a] == CFG_ANEXT && target[a] == NO_TARGET) {
                    target[a] = e + 1;
                    break;
                }
            }
            break;
        }
    }
    for (int a = 0; a < n; a++) {
        if (is_instr[a] && kind[a] != CFG_HANDLER && target[a] >= 0 && target[a] < n)
            is_target[target[a]] = 1;
    }

    /* Keep branch nodes, branch targets and line starts. */
    int kept = 0;
    int32_t prev_line = -1;
    for (int a = 0; a < n; a++) {
        kept_index[a] = -1;
        if (!is_instr[a]) continue;
        if (kind[a] != CFG_LINEAR || is_target[a] || line[a] != prev_line) {
            kept_index[a] = kept++;
        }
        prev_line = line[a];
    }
    kept_index[n] = kept;   /* jump past the end */

    size_t len = (size_t)kept * 9;
    uint8_t *blob = malloc(len ? len : 1);
    if (!blob) goto fail;
    uint8_t *p = blob;
    for (int a = 0; a < n; a++) {
        if (kept_index[a] < 0) continue;
        int32_t t = target[a];
        uint32_t ti = 0xFFFFFFFFu;
        if (kind[a] == CFG_HANDLER) {
            ti = (uint32_t)t;      /* raw code-unit offset */
        } else if (kind[a] != CFG_LINEAR && t >= 0 && t <= n) {
            int32_t k = kept_index[t];
            ti = k >= 0 ? (uint32_t)k : 0xFFFFFFFFu;
        }
        put_u32(p, (uint32_t)line[a]);
        p[4] = kind[a];
        put_u32(p + 5, ti);
        p += 9;
    }
    free(line); free(target); free(kind); free(is_instr); free(is_target); free(kept_index);
    *len_out = len;
    return blob;

fail:
    free(line); free(target); free(kind); free(is_instr); free(is_target); free(kept_index);
    return NULL;
}
