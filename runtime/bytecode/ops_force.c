/*
 * FORCE — pop value, dispatch by tag:
 *   V_DELAY (subtag BYTECODE)     : push caller frame, switch to body
 *   V_BUILTIN                      : consume a Force via the state machine
 *   V_DELAY (INTERP/COMPILED)      : cross-mode not supported → EvaluationFailure
 *   anything else                  : EvaluationFailure
 *
 * For Delay, env is just the captured upvals — no arg slot. The emitter
 * is responsible for picking the right slot indices inside the body.
 */

#include "runtime/bytecode/closure.h"
#include "runtime/bytecode/dispatch.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

#include "runtime/core/arena.h"
#include "runtime/core/builtin_dispatch.h"
#include "runtime/core/builtin_state.h"
#include "runtime/core/errors.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

UPLC_BC_HOT
uplc_value uplc_bc_op_force(const uplc_bc_word* pc, uplc_value* sp,
                            uplc_bc_state* st) {
    uplcrt_budget_step(st->budget, UPLC_STEP_FORCE);
    uplc_value v = *(--sp);

    if (UPLC_BC_LIKELY(v.tag == UPLC_V_DELAY &&
                       v.subtag == (uint8_t)UPLC_VLAM_BYTECODE)) {
        uplc_bc_closure*  c      = uplc_bc_closure_of(v);
        const uplc_bc_fn* target = c->fn;

        if (UPLC_BC_UNLIKELY(st->fp >= st->frame_end)) {
            uplcrt_bc_grow_frames(st);
        }
        st->fp->ret_pc  = pc + 1;
        st->fp->ret_env = st->env;
        ++st->fp;

        /* Delay has no arg slot — env is just the upvals. */
        uint32_t nfree = c->n_upvals;
        uplc_value* new_env = NULL;
        if (nfree > 0) {
            new_env = (uplc_value*)uplc_arena_alloc(
                st->arena, sizeof(uplc_value) * nfree, _Alignof(uplc_value));
            memcpy(new_env, c->upvals, sizeof(uplc_value) * nfree);
        }
        st->env = new_env;

        UPLC_BC_DISPATCH_NEXT(target->opcodes, sp, st);
    }

    if (v.tag == UPLC_V_BUILTIN) {
        uplc_builtin_state* bs = uplcrt_builtin_state_of(v);
        /* Saturating-force fast path: last force, all args already in.
         * Rare in practice (most builtins take forces first, args
         * last), but trivial to cover. */
        if (UPLC_BC_LIKELY((uint32_t)bs->forces_applied + 1u ==
                           (uint32_t)bs->total_forces &&
                           bs->args_applied == bs->total_args)) {
            *sp++ = uplcrt_run_builtin(st->budget, bs->tag,
                                        bs->args, bs->total_args);
        } else {
            *sp++ = uplcrt_builtin_consume_force(st->budget, bs);
        }
        UPLC_BC_DISPATCH_NEXT(pc + 1, sp, st);
    }
    if (v.tag == UPLC_V_DELAY) {
        uplcrt_raise(UPLC_FAIL_EVALUATION,
                     "bytecode VM: force: cross-mode VDelay");
    }
    uplcrt_raise(UPLC_FAIL_EVALUATION,
                 "bytecode VM: force: not a delay/builtin");
    /* unreachable */
    uplc_value dummy = {0};
    return dummy;
}

/*
 * TAIL_FORCE — emitted when a FORCE is immediately followed by
 * RETURN. Skips the frame push on the VDelay bytecode fast path, so
 * the delayed body's RETURN pops our caller's frame directly.
 *
 * VBuiltin cold path inlines RETURN semantics after consume_force.
 * Budget parity preserved (single StepForce charge).
 */
UPLC_BC_HOT
uplc_value uplc_bc_op_tail_force(const uplc_bc_word* pc, uplc_value* sp,
                                 uplc_bc_state* st) {
    (void)pc;
    uplcrt_budget_step(st->budget, UPLC_STEP_FORCE);
    uplc_value v = *(--sp);

    if (UPLC_BC_LIKELY(v.tag == UPLC_V_DELAY &&
                       v.subtag == (uint8_t)UPLC_VLAM_BYTECODE)) {
        uplc_bc_closure*  c      = uplc_bc_closure_of(v);
        const uplc_bc_fn* target = c->fn;

        uint32_t nfree = c->n_upvals;
        uplc_value* new_env = NULL;
        if (nfree > 0) {
            new_env = (uplc_value*)uplc_arena_alloc(
                st->arena, sizeof(uplc_value) * nfree, _Alignof(uplc_value));
            memcpy(new_env, c->upvals, sizeof(uplc_value) * nfree);
        }
        st->env = new_env;

        UPLC_BC_DISPATCH_NEXT(target->opcodes, sp, st);
    }

    if (v.tag == UPLC_V_BUILTIN) {
        uplc_builtin_state* bs = uplcrt_builtin_state_of(v);
        if (UPLC_BC_LIKELY((uint32_t)bs->forces_applied + 1u ==
                           (uint32_t)bs->total_forces &&
                           bs->args_applied == bs->total_args)) {
            *sp++ = uplcrt_run_builtin(st->budget, bs->tag,
                                        bs->args, bs->total_args);
        } else {
            *sp++ = uplcrt_builtin_consume_force(st->budget, bs);
        }
        if (st->fp <= st->frame_base) {
            uplcrt_budget_flush(st->budget);
            return *(sp - 1);
        }
        --st->fp;
        st->env = st->fp->ret_env;
        if (st->fp->ret_pc == NULL) {
            return *(sp - 1);
        }
        UPLC_BC_DISPATCH_NEXT(st->fp->ret_pc, sp, st);
    }

    if (v.tag == UPLC_V_DELAY) {
        uplcrt_raise(UPLC_FAIL_EVALUATION,
                     "bytecode VM: tail_force: cross-mode VDelay");
    }
    uplcrt_raise(UPLC_FAIL_EVALUATION,
                 "bytecode VM: tail_force: not a delay/builtin");
    uplc_value dummy = {0};
    return dummy;
}
