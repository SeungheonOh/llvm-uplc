#include <stdint.h>

#include "runtime/arena.h"
#include "runtime/builtin_state.h"
#include "runtime/compiled/closure.h"
#include "runtime/errors.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

/*
 * uplcrt_force_slow — mirror of uplcrt_apply_slow for V_DELAY values.
 *
 * The HOT path (V_DELAY + COMPILED) is normally inlined directly into
 * the generated IR by emit_force_inline in compiler/codegen/llvm_codegen.cc,
 * so this entry is only reached for V_BUILTIN force-consumption or
 * evaluation-failure cases.
 *
 *   V_DELAY, subtag == COMPILED
 *     Cast payload to uplc_closure, call closure->fn(closure->free, b)
 *     where closure->fn is a uplc_delay_fn (no `arg` parameter). Kept
 *     for completeness; not normally hit from generated code.
 *
 *   V_DELAY, subtag == INTERP
 *     Not applicable from compiled code. Fail.
 *
 *   V_BUILTIN
 *     Consume one force step.
 */
uplc_value uplcrt_force_slow(uplc_value thunk, uplc_budget* b) {
    if (thunk.tag == UPLC_V_DELAY) {
        if (thunk.subtag == UPLC_VLAM_COMPILED) {
            uplc_closure* c = uplcrt_closure_of(thunk);
            uplc_delay_fn body = (uplc_delay_fn)c->fn;
            return body(c->free, b);
        }
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    if (thunk.tag == UPLC_V_BUILTIN) {
        return uplcrt_builtin_consume_force(b, uplcrt_builtin_state_of(thunk));
    }
    uplcrt_fail(b, UPLC_FAIL_EVALUATION);
}

/* Backwards-compat alias for non-codegen consumers (CEK readback, tests). */
uplc_value uplcrt_force(uplc_value thunk, uplc_budget* b) {
    return uplcrt_force_slow(thunk, b);
}
