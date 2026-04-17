#include <stdint.h>

#include "runtime/core/arena.h"
#include "runtime/core/builtin_state.h"
#include "runtime/compiled/closure.h"
#include "runtime/core/errors.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

/*
 * uplcrt_apply — full-dispatch apply, used as a fallback when generated
 * code can't tell statically that the callee is a compiled lambda.
 *
 * The HOT path (V_LAM + COMPILED) is normally inlined directly into the
 * generated IR by emit_apply_inline in compiler/codegen/llvm_codegen.cc,
 * so this entry is only reached for V_BUILTIN partial application or
 * for evaluation-failure cases.
 *
 * Dispatch rules:
 *   V_LAM, subtag == COMPILED
 *     Cast payload to uplc_closure, call closure->fn(closure->free, arg, b).
 *     Kept here so the slow path stays semantically equivalent to the
 *     pre-inlined entry in case some call site doesn't take the inline
 *     fast path (e.g. tests that link against the runtime directly).
 *
 *   V_LAM, subtag == INTERP
 *     Produced by the CEK walker, not applicable from compiled code.
 *     Raise UPLC_FAIL_EVALUATION.
 *
 *   V_BUILTIN
 *     Accumulate one value argument via the builtin state machine.
 *
 *   Everything else
 *     Evaluation failure: "applied a non-function value".
 */
uplc_value uplcrt_apply_slow(uplc_value fn, uplc_value arg, uplc_budget* b) {
    if (fn.tag == UPLC_V_LAM) {
        if (fn.subtag == UPLC_VLAM_COMPILED) {
            uplc_closure* c = uplcrt_closure_of(fn);
            uplc_lam_fn body = (uplc_lam_fn)c->fn;
            return body(c->free, arg, b);
        }
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    if (fn.tag == UPLC_V_BUILTIN) {
        return uplcrt_builtin_consume_arg(b, uplcrt_builtin_state_of(fn), arg);
    }
    uplcrt_fail(b, UPLC_FAIL_EVALUATION);
}

/*
 * Backwards-compat alias. Kept so existing C consumers (CEK readback,
 * unit tests that call the runtime directly) don't have to change.
 * Generated code no longer references this symbol — it goes through
 * uplcrt_apply_slow when the inline fast path falls through.
 */
uplc_value uplcrt_apply(uplc_value fn, uplc_value arg, uplc_budget* b) {
    return uplcrt_apply_slow(fn, arg, b);
}
