#include "runtime/builtin_state.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "runtime/builtin_dispatch.h"
#include "runtime/errors.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

/*
 * VBuiltin state machine. Each Force/Apply event produces a *new* state
 * in the arena (copy-on-write) so partial-application chains can branch
 * without aliasing.
 *
 * When a transition leaves the state fully saturated (all forces and
 * args consumed), we immediately dispatch into uplcrt_run_builtin and
 * return the produced value. Callers never observe a saturated VBuiltin.
 */

static uplc_value wrap(uplc_builtin_state* s) {
    uplc_value v;
    v.tag    = UPLC_V_BUILTIN;
    v.subtag = 0;
    for (int i = 0; i < 6; ++i) v._pad[i] = 0;
    v.payload = (uint64_t)(uintptr_t)s;
    return v;
}

static uplc_builtin_state* clone_state(uplc_arena* a,
                                       const uplc_builtin_state* src) {
    uplc_builtin_state* s = (uplc_builtin_state*)uplc_arena_alloc(
        a, sizeof(uplc_builtin_state), alignof(uplc_builtin_state));
    *s = *src;
    if (src->total_args > 0) {
        uplc_value* args = (uplc_value*)uplc_arena_alloc(
            a, sizeof(uplc_value) * (size_t)src->total_args, alignof(uplc_value));
        for (uint8_t i = 0; i < src->total_args; ++i) args[i] = src->args[i];
        s->args = args;
    } else {
        s->args = NULL;
    }
    return s;
}

static bool is_saturated(const uplc_builtin_state* s) {
    return s->forces_applied == s->total_forces &&
           s->args_applied == s->total_args;
}

uplc_value uplcrt_builtin_fresh(uplc_budget* b, uint8_t tag) {
    const uplc_builtin_meta* meta = uplcrt_builtin_meta(tag);
    if (!meta) uplcrt_fail(b, UPLC_FAIL_EVALUATION);

    uplc_arena* a = uplcrt_budget_arena(b);
    if (!a) uplcrt_fail(b, UPLC_FAIL_MACHINE);

    uplc_builtin_state* s = (uplc_builtin_state*)uplc_arena_alloc(
        a, sizeof(uplc_builtin_state), alignof(uplc_builtin_state));
    memset(s, 0, sizeof(*s));
    s->tag = tag;
    s->total_forces = meta->force_count;
    s->total_args = meta->arity;
    if (meta->arity > 0) {
        s->args = (uplc_value*)uplc_arena_alloc(
            a, sizeof(uplc_value) * (size_t)meta->arity, alignof(uplc_value));
        memset(s->args, 0, sizeof(uplc_value) * (size_t)meta->arity);
    }
    return wrap(s);
}

uplc_builtin_state* uplcrt_builtin_state_of(uplc_value v) {
    return (uplc_builtin_state*)(uintptr_t)v.payload;
}

uplc_value uplcrt_builtin_consume_force(uplc_budget* b,
                                        const uplc_builtin_state* s) {
    if (s->forces_applied >= s->total_forces) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    uplc_arena* a = uplcrt_budget_arena(b);
    if (!a) uplcrt_fail(b, UPLC_FAIL_MACHINE);

    uplc_builtin_state* s2 = clone_state(a, s);
    s2->forces_applied = (uint8_t)(s2->forces_applied + 1);
    if (is_saturated(s2)) {
        return uplcrt_run_builtin(b, s2->tag, s2->args, s2->total_args);
    }
    return wrap(s2);
}

uplc_value uplcrt_builtin_consume_arg(uplc_budget* b,
                                      const uplc_builtin_state* s,
                                      uplc_value arg) {
    /* Plutus builtins require all forces to be consumed before value
     * arguments start arriving. Apply-before-force is an evaluation
     * failure per the spec (`builtin not saturated`). */
    if (s->forces_applied < s->total_forces) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    if (s->args_applied >= s->total_args) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    uplc_arena* a = uplcrt_budget_arena(b);
    if (!a) uplcrt_fail(b, UPLC_FAIL_MACHINE);

    uplc_builtin_state* s2 = clone_state(a, s);
    s2->args[s2->args_applied] = arg;
    s2->args_applied = (uint8_t)(s2->args_applied + 1);
    if (is_saturated(s2)) {
        return uplcrt_run_builtin(b, s2->tag, s2->args, s2->total_args);
    }
    return wrap(s2);
}
