#include <stdint.h>

#include "runtime/core/arena.h"
#include "runtime/core/errors.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

/*
 * uplcrt_make_constr — delegates to the shared `uplc_make_constr_vals`
 * helper. Both the CEK walker and compiled code produce VConstr values
 * with the same on-heap layout, so there is no mode-specific state.
 */
uplc_value uplcrt_make_constr(uplc_budget* b, uint64_t tag,
                              const uplc_value* fields, uint32_t n) {
    uplc_arena* a = uplcrt_budget_arena(b);
    if (!a) uplcrt_fail(b, UPLC_FAIL_MACHINE);
    return uplc_make_constr_vals(a, tag, fields, n);
}
