#include <stdint.h>

#include "runtime/core/builtin_state.h"
#include "uplc/abi.h"
#include "uplc/budget.h"
#include "uplc/term.h"

/*
 * ABI entry called from LLVM-emitted IR when evaluating a `builtin` term.
 * Delegates to the shared VBuiltin state machine (runtime/builtin_state.c)
 * so the CEK walker and compiled code produce identical VBuiltin values.
 */
uplc_value uplcrt_make_builtin(uplc_budget* b, uplc_builtin_tag tag) {
    return uplcrt_builtin_fresh(b, (uint8_t)tag);
}
