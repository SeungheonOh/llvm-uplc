#include "runtime/builtins/helpers.h"

/*
 * Polymorphic control builtins. The "forces" on these represent the
 * type applications eaten at the UPLC level — e.g. `ifThenElse` is
 * `forall a. Bool -> a -> a -> a`, so it takes 1 force (consuming `a`)
 * followed by 3 value arguments. The dispatch runner only invokes the
 * impl when the accumulator is saturated (both forces and args), so
 * here we just read `a[0..arity-1]` directly.
 */

uplc_value uplcrt_builtin_ifThenElse(uplc_budget* b, uplc_value* a) {
    bool cond = uplcrt_unwrap_bool(b, a[0]);
    return cond ? a[1] : a[2];
}

uplc_value uplcrt_builtin_chooseUnit(uplc_budget* b, uplc_value* a) {
    /* First argument must be the unit constant; second is returned. */
    const uplc_rconstant* c = uplcrt_as_const(a[0]);
    if (!c || c->tag != UPLC_RCONST_UNIT) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    return a[1];
}

uplc_value uplcrt_builtin_trace(uplc_budget* b, uplc_value* a) {
    /* args[0] is a string (logged and discarded); args[1] is returned. */
    uint32_t len = 0;
    (void)uplcrt_unwrap_string(b, a[0], &len);
    return a[1];
}
