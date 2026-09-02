#ifndef D3G_CFG_H
#define D3G_CFG_H

#include "Python.h"
#include <stdint.h>
#include <stddef.h>

/* Bytecode control-flow graph of a code object, for replaying the branch
 * bytes recorded by d3g_branch_hook. The blob is a sequence of 9-byte
 * little-endian records {u32 line, u8 kind, u32 target}; `target` indexes
 * a record, 0xFFFFFFFF when absent. Records cover the instructions that
 * branch, are branched to, or start a new line.
 *
 * kind: 0 linear, 1 conditional jump (byte 1 = taken), 2 FOR_ITER (byte
 * 1 = exhausted), 3 unconditional jump, 4 SEND (await; continue at the
 * target), 5 GET_ANEXT (byte 0 per attempt; a following 2 means the loop
 * is exhausted, continue at the target), 6 return/raise, 7 exception
 * handler entry (PUSH_EXC_INFO; `target` holds the code-unit offset d3g
 * records when the handler is entered, not a record index).
 *
 * Returns a malloc'd buffer (NULL if empty or on failure) and its length. */
uint8_t *d3g_build_cfg(PyCodeObject *co, size_t *len_out);

#endif
