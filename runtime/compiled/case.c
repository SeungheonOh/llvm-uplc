#include <stdint.h>

#include "runtime/case_decompose.h"
#include "runtime/errors.h"
#include "runtime/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

/*
 * uplcrt_case_dispatch — compiled-mode implementation of the Case term.
 * Decomposes the scrutinee via the shared runtime/case_decompose helper
 * (which handles both VConstr and coercible constants like bool/list/…)
 * then applies the selected alt to each resulting field left-to-right.
 *
 * NOTE: Not called by LLVM-emitted IR. The codegen now emits a switch
 * over the tag so that only the selected branch is evaluated (lazy
 * semantics). This function is kept as a reference implementation.
 */
uplc_value uplcrt_case_dispatch(uplc_value scrutinee,
                                const uplc_value* alts,
                                uint32_t n_alts,
                                uplc_budget* b) {
    uplc_case_decomp dec = uplcrt_case_decompose(b, scrutinee, n_alts);
    uplc_value current = alts[dec.tag];
    for (uint32_t i = 0; i < dec.n_fields; ++i) {
        current = uplcrt_apply(current, dec.fields[i], b);
    }
    return current;
}

/*
 * uplcrt_apply_fields — apply branch to fields[0..n_fields-1] left-to-right.
 * Called by LLVM-emitted Case code after the switch selects the right branch.
 */
uplc_value uplcrt_apply_fields(uplc_value branch, const uplc_value* fields,
                               uint32_t n_fields, uplc_budget* b) {
    uplc_value current = branch;
    for (uint32_t i = 0; i < n_fields; ++i) {
        current = uplcrt_apply(current, fields[i], b);
    }
    return current;
}
