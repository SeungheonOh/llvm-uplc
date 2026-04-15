#ifndef UPLCRT_CASE_DECOMPOSE_H
#define UPLCRT_CASE_DECOMPOSE_H

#include <stdint.h>

#include "runtime/arena.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared case scrutinee decomposition. UPLC's `case` can match on either
 * a VConstr or one of the "constant-as-constructor" types (bool, unit,
 * pair, list, non-negative integer). TS's constantToConstr (machine.ts:446)
 * is the reference.
 *
 * `uplcrt_case_decompose` returns the tag to select from the branches
 * array and the (already-evaluated) field values to apply the selected
 * branch against, left-to-right. It raises UPLC_FAIL_EVALUATION when
 *   - `scrutinee` is a non-decomposable value,
 *   - `n_branches` exceeds the type's maximum (e.g. >2 for bool),
 *   - the computed tag is not strictly less than `n_branches`,
 *   - an integer scrutinee is negative or doesn't fit in uint64.
 */
typedef struct uplc_case_decomp {
    uint64_t     tag;
    uint32_t     n_fields;
    uplc_value*  fields;   /* NULL iff n_fields == 0, else arena-allocated */
} uplc_case_decomp;

uplc_case_decomp uplcrt_case_decompose(uplc_budget* b,
                                       uplc_value   scrutinee,
                                       uint32_t     n_branches);

/* JIT-friendly variant: writes results into out-params instead of returning
 * the struct (avoids struct-return ABI differences between C and LLVM IR).
 * Semantics are identical to uplcrt_case_decompose; raises on invalid input. */
void uplcrt_case_decompose_out(uplc_budget*  b,
                               uplc_value    scrutinee,
                               uint32_t      n_branches,
                               uint64_t*     out_tag,
                               uint32_t*     out_n_fields,
                               uplc_value**  out_fields);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_CASE_DECOMPOSE_H */
