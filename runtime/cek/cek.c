#include "runtime/cek/cek.h"

#include <setjmp.h>
#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include "runtime/builtin_dispatch.h"
#include "runtime/builtin_state.h"
#include "runtime/case_decompose.h"
#include "runtime/cek/closure.h"
#include "runtime/cek/env.h"
#include "runtime/cek/stack.h"
#include "runtime/errors.h"
#include "runtime/value.h"

/* VBuiltin state is shared with the compiled path — defined in
 * runtime/builtin_state.{h,c}. The CEK loop only sees the opaque
 * uplc_builtin_state* payload and routes every Force/Apply event
 * through uplcrt_builtin_consume_force / uplcrt_builtin_consume_arg,
 * which dispatch into runtime/builtin_dispatch.c on saturation. */

/* --------------------------------------------------------------------- */
/* Frame kinds                                                            */
/* --------------------------------------------------------------------- */

/* APPLY_TO: payload is a pre-evaluated argument value. When the current
 * computation returns, apply that returned value (as fn) to the stored
 * argument. Used by Case to feed constructor fields into the matched
 * branch body, left-to-right. */
#define UPLC_FRAME_APPLY_TO 5

/* --------------------------------------------------------------------- */
/* Small helpers                                                          */
/* --------------------------------------------------------------------- */

static inline uplc_step_kind step_for_tag(uint8_t tag) {
    switch (tag) {
        case UPLC_RTERM_VAR:      return UPLC_STEP_VAR;
        case UPLC_RTERM_LAMBDA:   return UPLC_STEP_LAMBDA;
        case UPLC_RTERM_APPLY:    return UPLC_STEP_APPLY;
        case UPLC_RTERM_DELAY:    return UPLC_STEP_DELAY;
        case UPLC_RTERM_FORCE:    return UPLC_STEP_FORCE;
        case UPLC_RTERM_CONSTANT: return UPLC_STEP_CONST;
        case UPLC_RTERM_BUILTIN:  return UPLC_STEP_BUILTIN;
        case UPLC_RTERM_CONSTR:   return UPLC_STEP_CONSTR;
        case UPLC_RTERM_CASE:     return UPLC_STEP_CASE;
        case UPLC_RTERM_ERROR:    return UPLC_STEP_CONST;  /* unused; will raise */
        default:                  return UPLC_STEP_CONST;
    }
}

static void charge_and_check(uplc_budget* b, uplc_step_kind k) {
    uplcrt_budget_step(b, k);
    if (!uplcrt_budget_ok(b)) {
        uplcrt_raise(UPLC_FAIL_OUT_OF_BUDGET, "out of budget");
    }
}

/* Apply an already-evaluated function value to an already-evaluated
 * argument. Returns via *next_term / *next_env / *next_mode — the caller
 * then continues the loop. For interpreter closures this resumes compute
 * mode with the body; for VBuiltin this accumulates args (and raises on
 * saturation in M2). */
typedef enum {
    DISPATCH_COMPUTE,  /* *next_term + *next_env */
    DISPATCH_RETURN    /* *next_value is the result (already set) */
} dispatch_kind;

static dispatch_kind dispatch_apply(uplc_budget* budget,
                                    uplc_value fn,
                                    uplc_value arg,
                                    uplc_rterm** next_term,
                                    uplc_env* next_env,
                                    uplc_value* next_value) {
    if (fn.tag == UPLC_V_LAM && fn.subtag == UPLC_VLAM_INTERP) {
        uplc_arena* arena = uplcrt_budget_arena(budget);
        uplc_interp_closure* c = uplc_closure_of(fn);
        *next_term = c->body;
        *next_env  = uplc_env_extend(arena, c->env, arg);
        return DISPATCH_COMPUTE;
    }
    if (fn.tag == UPLC_V_BUILTIN) {
        const uplc_builtin_state* s = uplcrt_builtin_state_of(fn);
        *next_value = uplcrt_builtin_consume_arg(budget, s, arg);
        return DISPATCH_RETURN;
    }
    uplcrt_raise(UPLC_FAIL_EVALUATION, "apply: non-function value");
    __builtin_unreachable();
}

static dispatch_kind dispatch_force(uplc_budget* budget,
                                    uplc_value v,
                                    uplc_rterm** next_term,
                                    uplc_env* next_env,
                                    uplc_value* next_value) {
    (void)next_env;
    if (v.tag == UPLC_V_DELAY && v.subtag == UPLC_VLAM_INTERP) {
        uplc_interp_closure* c = uplc_closure_of(v);
        *next_term = c->body;
        *next_env  = c->env;
        return DISPATCH_COMPUTE;
    }
    if (v.tag == UPLC_V_BUILTIN) {
        const uplc_builtin_state* s = uplcrt_builtin_state_of(v);
        *next_value = uplcrt_builtin_consume_force(budget, s);
        return DISPATCH_RETURN;
    }
    uplcrt_raise(UPLC_FAIL_EVALUATION, "force: non-delayed value");
    __builtin_unreachable();
}

/* --------------------------------------------------------------------- */
/* Main loop                                                              */
/* --------------------------------------------------------------------- */

uplc_cek_result uplc_cek_run(uplc_arena* arena, uplc_rterm* root,
                             uplc_budget* budget) {
    uplc_cek_result result;
    result.ok = 0;
    result.fail_kind = UPLC_FAIL_MACHINE;
    result.fail_message = NULL;
    memset(&result.value, 0, sizeof(result.value));

    /* Attach the arena to the budget so builtin_state / builtin_dispatch /
     * helper make_* routines can heap-allocate without an extra parameter.
     * (Matches the compiled path's uplcrt_run_compiled setup.) */
    void* saved_arena = budget->arena;
    budget->arena = arena;

    /* Charge the one-time startup cost before entering the step loop.
     * Matches TS CekMachine's MACHINE_COSTS.startup. */
    uplcrt_budget_startup(budget);

    uplc_fail_ctx ctx;
    if (setjmp(ctx.env) != 0) {
        uplcrt_fail_install(NULL);
        result.ok = 0;
        result.fail_kind = ctx.kind;
        result.fail_message = ctx.message;
        /* Flush any pending slippage so caller observes the final charges. */
        uplcrt_budget_flush(budget);
        budget->arena = saved_arena;
        return result;
    }
    uplcrt_fail_install(&ctx);

    uplc_stack stack;
    uplc_stack_init(&stack, arena);

    enum { MODE_COMPUTE, MODE_RETURN } mode = MODE_COMPUTE;
    uplc_rterm* term = root;
    uplc_env    env  = NULL;
    uplc_value  value; memset(&value, 0, sizeof(value));

    for (;;) {
        if (mode == MODE_COMPUTE) {
            charge_and_check(budget, step_for_tag(term->tag));

            switch (term->tag) {
                case UPLC_RTERM_VAR: {
                    if (!uplc_env_lookup(env, term->var.index, &value)) {
                        uplcrt_raise(UPLC_FAIL_EVALUATION, "free variable");
                    }
                    mode = MODE_RETURN;
                    break;
                }
                case UPLC_RTERM_LAMBDA: {
                    value = uplc_make_lam_interp(arena, term->lambda.body, env);
                    mode = MODE_RETURN;
                    break;
                }
                case UPLC_RTERM_DELAY: {
                    value = uplc_make_delay_interp(arena, term->delay.term, env);
                    mode = MODE_RETURN;
                    break;
                }
                case UPLC_RTERM_FORCE: {
                    uplc_frame f = { .kind = UPLC_FRAME_FORCE };
                    uplc_stack_push(&stack, &f);
                    term = term->force.term;
                    break;
                }
                case UPLC_RTERM_APPLY: {
                    uplc_frame f;
                    f.kind = UPLC_FRAME_APP_LEFT;
                    f.app_left.arg = term->apply.arg;
                    f.app_left.env = env;
                    uplc_stack_push(&stack, &f);
                    term = term->apply.fn;
                    break;
                }
                case UPLC_RTERM_CONSTANT: {
                    value = uplc_make_rcon(term->constant.value);
                    mode = MODE_RETURN;
                    break;
                }
                case UPLC_RTERM_BUILTIN: {
                    value = uplcrt_builtin_fresh(budget, term->builtin.tag);
                    mode = MODE_RETURN;
                    break;
                }
                case UPLC_RTERM_ERROR: {
                    uplcrt_raise(UPLC_FAIL_EVALUATION, "error");
                }
                case UPLC_RTERM_CONSTR: {
                    uint32_t n = term->constr.n_fields;
                    if (n == 0) {
                        value = uplc_make_constr_vals(arena, term->constr.tag_index,
                                                      NULL, 0);
                        mode = MODE_RETURN;
                        break;
                    }
                    /* Pre-allocate the collected array for the whole constr. */
                    uplc_value* collected = (uplc_value*)uplc_arena_alloc(
                        arena, sizeof(uplc_value) * n, alignof(uplc_value));
                    uplc_frame f;
                    f.kind = UPLC_FRAME_CONSTR;
                    f.constr.tag_index = term->constr.tag_index;
                    f.constr.remaining_fields = term->constr.fields + 1;
                    f.constr.remaining_count = n - 1;
                    f.constr.collected = collected;
                    f.constr.collected_count = 0;
                    f.constr.env = env;
                    uplc_stack_push(&stack, &f);
                    term = term->constr.fields[0];
                    break;
                }
                case UPLC_RTERM_CASE: {
                    uplc_frame f;
                    f.kind = UPLC_FRAME_CASE;
                    f.case_.branches = term->case_.branches;
                    f.case_.n_branches = term->case_.n_branches;
                    f.case_.env = env;
                    uplc_stack_push(&stack, &f);
                    term = term->case_.scrutinee;
                    break;
                }
                default:
                    uplcrt_raise(UPLC_FAIL_MACHINE, "cek: unknown term tag");
            }
        } else {
            /* MODE_RETURN */
            if (uplc_stack_empty(&stack)) {
                uplcrt_fail_install(NULL);
                /* Settle any residual slippage. If the final budget ends
                 * up negative the program exhausted its allowance inside
                 * the buffer window — convert to OutOfBudget. */
                uplcrt_budget_flush(budget);
                if (!uplcrt_budget_ok(budget)) {
                    result.ok = 0;
                    result.fail_kind = UPLC_FAIL_OUT_OF_BUDGET;
                    result.fail_message = "out of budget";
                    budget->arena = saved_arena;
                    return result;
                }
                result.ok = 1;
                result.value = value;
                budget->arena = saved_arena;
                return result;
            }
            uplc_frame f = uplc_stack_pop(&stack);
            switch (f.kind) {
                case UPLC_FRAME_FORCE: {
                    uplc_rterm* next_term;
                    uplc_env next_env;
                    uplc_value next_value;
                    dispatch_kind dk = dispatch_force(
                        budget, value, &next_term, &next_env, &next_value);
                    if (dk == DISPATCH_COMPUTE) {
                        term = next_term;
                        env  = next_env;
                        mode = MODE_COMPUTE;
                    } else {
                        value = next_value;
                    }
                    break;
                }
                case UPLC_FRAME_APP_LEFT: {
                    /* `value` is the evaluated function. Stash it and go
                     * compute the argument. */
                    uplc_frame right;
                    right.kind = UPLC_FRAME_APP_RIGHT;
                    right.app_right.fn = value;
                    uplc_stack_push(&stack, &right);
                    term = f.app_left.arg;
                    env  = f.app_left.env;
                    mode = MODE_COMPUTE;
                    break;
                }
                case UPLC_FRAME_APP_RIGHT: {
                    uplc_rterm* next_term;
                    uplc_env next_env;
                    uplc_value next_value;
                    dispatch_kind dk = dispatch_apply(
                        budget, f.app_right.fn, value,
                        &next_term, &next_env, &next_value);
                    if (dk == DISPATCH_COMPUTE) {
                        term = next_term;
                        env  = next_env;
                        mode = MODE_COMPUTE;
                    } else {
                        value = next_value;
                    }
                    break;
                }
                case UPLC_FRAME_CONSTR: {
                    f.constr.collected[f.constr.collected_count++] = value;
                    if (f.constr.remaining_count == 0) {
                        value = uplc_make_constr_vals(
                            arena,
                            f.constr.tag_index,
                            f.constr.collected,
                            f.constr.collected_count);
                    } else {
                        uplc_frame next;
                        next.kind = UPLC_FRAME_CONSTR;
                        next.constr.tag_index = f.constr.tag_index;
                        next.constr.remaining_fields = f.constr.remaining_fields + 1;
                        next.constr.remaining_count = f.constr.remaining_count - 1;
                        next.constr.collected = f.constr.collected;
                        next.constr.collected_count = f.constr.collected_count;
                        next.constr.env = f.constr.env;
                        uplc_stack_push(&stack, &next);
                        term = f.constr.remaining_fields[0];
                        env  = f.constr.env;
                        mode = MODE_COMPUTE;
                    }
                    break;
                }
                case UPLC_FRAME_CASE: {
                    /* Decompose the scrutinee (VConstr or a constant that
                     * can be coerced to one: bool / unit / pair / list /
                     * non-negative integer). */
                    uplc_case_decomp dec = uplcrt_case_decompose(
                        budget, value, f.case_.n_branches);
                    for (uint32_t i = dec.n_fields; i > 0; --i) {
                        uplc_frame at;
                        at.kind = UPLC_FRAME_APPLY_TO;
                        at.app_right.fn = dec.fields[i - 1];
                        uplc_stack_push(&stack, &at);
                    }
                    term = f.case_.branches[dec.tag];
                    env  = f.case_.env;
                    mode = MODE_COMPUTE;
                    break;
                }
                case UPLC_FRAME_APPLY_TO: {
                    uplc_rterm* next_term;
                    uplc_env next_env;
                    uplc_value next_value;
                    dispatch_kind dk = dispatch_apply(
                        budget, value, f.app_right.fn,
                        &next_term, &next_env, &next_value);
                    if (dk == DISPATCH_COMPUTE) {
                        term = next_term;
                        env  = next_env;
                        mode = MODE_COMPUTE;
                    } else {
                        value = next_value;
                    }
                    break;
                }
                default:
                    uplcrt_raise(UPLC_FAIL_MACHINE, "cek: unknown frame kind");
            }
        }
    }
}
