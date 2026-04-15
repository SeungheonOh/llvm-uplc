#ifndef UPLCRT_READBACK_H
#define UPLCRT_READBACK_H

#include "runtime/arena.h"
#include "runtime/cek/rterm.h"
#include "uplc/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Readback: value -> uplc_rterm. Converts an evaluated value back into a
 * term that can be pretty-printed and compared against the TS conformance
 * reference output.
 *
 * Coverage:
 *   VCon      → UPLC_RTERM_CONSTANT wrapping the original uplc_rconstant.
 *   VConstr   → UPLC_RTERM_CONSTR with recursively-read-back fields.
 *   VBuiltin  → builtin term wrapped in the right number of forces /
 *               applys to reflect the partial-application state.
 *   VLam      → lambda term whose body re-reads captured env vars
 *               (minimal: produces a placeholder body "error" — closure
 *               readback lands in M4 alongside conformance).
 *   VDelay    → delay wrapped around a placeholder body (same caveat).
 *
 * Allocations come from `arena`. The returned term is valid as long as
 * the arena is alive.
 */
uplc_rterm* uplcrt_readback(uplc_arena* arena, uplc_value v);

/* Internal helper exposed for the VLam/VDelay arms and for future callers
 * that want to print intermediate closures. Substitutes env into the
 * free de Bruijn indices of `body`. `initial_depth` = 1 for a closure
 * body under a lambda, 0 under a delay. */
struct uplc_env_cell;
uplc_rterm* uplcrt_readback_close(uplc_arena* arena, uplc_rterm* body,
                                  struct uplc_env_cell* env,
                                  uint32_t initial_depth);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_READBACK_H */
