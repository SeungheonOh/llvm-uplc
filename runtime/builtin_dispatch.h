#ifndef UPLCRT_BUILTIN_DISPATCH_H
#define UPLCRT_BUILTIN_DISPATCH_H

#include <stdint.h>

#include "uplc/abi.h"
#include "uplc/budget.h"
#include "uplc/costmodel.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Central dispatcher for saturated builtin calls. Called from both
 * runtime/cek/cek.c (indirectly via builtin_state.c) and
 * runtime/compiled/apply.c on saturation.
 *
 * Responsibilities:
 *   1. Charge (cpu, mem) via uplcrt_costfn_eval using the builtin's
 *      per-argument sizes from exmem.
 *   2. Raise UPLC_FAIL_OUT_OF_BUDGET if the new budget goes negative.
 *   3. Invoke the per-builtin implementation (uplc_builtin_impl_fn).
 *   4. Raise UPLC_FAIL_EVALUATION if the impl pointer is NULL (a.k.a.
 *      "builtin not yet implemented").
 */
uplc_value uplcrt_run_builtin(uplc_budget* b, uint8_t tag,
                              uplc_value* argv, uint32_t argc);

/* Per-builtin implementation signature. Impls read `argv`, raise via
 * uplcrt_fail on any dynamic error, and return a new VCon (or VConstr /
 * VBuiltin, rarely) value. */
typedef uplc_value (*uplc_builtin_impl_fn)(uplc_budget* b, uplc_value* argv);

/* Metadata row for a single builtin. One per entry in the dispatch table. */
typedef struct uplc_builtin_meta {
    uint8_t               arity;
    uint8_t               force_count;
    uint8_t               _pad[6];
    uplc_cost_row         cost;
    uplc_builtin_impl_fn  impl;   /* NULL = not implemented yet */
} uplc_builtin_meta;

/* Look up metadata for `tag`. Returns NULL if out of range. */
const uplc_builtin_meta* uplcrt_builtin_meta(uint8_t tag);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_BUILTIN_DISPATCH_H */
