/*
 * APPLY — pop arg, pop fn, dispatch by fn tag:
 *   V_LAM (subtag BYTECODE) : push caller frame, switch to callee's body
 *   V_BUILTIN                : consume the arg via builtin state machine
 *   V_LAM (INTERP/COMPILED)  : cross-mode not supported → EvaluationFailure
 *   anything else            : EvaluationFailure
 *
 * The StepApply charge fires once per opcode, matching the TS CEK.
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

/* Max builtin arity in the Plutus spec is 6 (chooseData). */
#define UPLC_BC_MAX_BUILTIN_ARITY 8

UPLC_BC_HOT
uplc_value uplc_bc_op_apply(const uplc_bc_word* pc, uplc_value* sp,
                            uplc_bc_state* st) {
    uplcrt_budget_step(st->budget, UPLC_STEP_APPLY);
    uplc_value arg = *(--sp);
    uplc_value fn  = *(--sp);

    /* HOT PATH: applying a bytecode VLam. The vast majority of APPLY
     * sites in real scripts hit this branch. Branch hints keep the
     * VBuiltin / cross-mode / error paths as cold fall-through. */
    if (UPLC_BC_LIKELY(fn.tag == UPLC_V_LAM &&
                       fn.subtag == (uint8_t)UPLC_VLAM_BYTECODE)) {
        uplc_bc_closure*  c      = uplc_bc_closure_of(fn);
        const uplc_bc_fn* target = c->fn;

        if (UPLC_BC_UNLIKELY(st->fp >= st->frame_end)) {
            uplcrt_bc_grow_frames(st);
        }
        st->fp->ret_pc  = pc + 1;
        st->fp->ret_env = st->env;
        ++st->fp;

        /* env = [arg, upvals...] — single allocation, no intermediate
         * buffer. The arena's inline fast path turns this into a pure
         * bump in the common case. */
        uint32_t nfree    = c->n_upvals;
        uint32_t env_size = 1u + nfree;
        uplc_value* new_env = (uplc_value*)uplc_arena_alloc(
            st->arena, sizeof(uplc_value) * env_size, _Alignof(uplc_value));
        new_env[0] = arg;
        if (nfree > 0) {
            memcpy(&new_env[1], c->upvals, sizeof(uplc_value) * nfree);
        }
        st->env = new_env;

        UPLC_BC_DISPATCH_NEXT(target->opcodes, sp, st);
    }

    if (fn.tag == UPLC_V_BUILTIN) {
        uplc_builtin_state* bs = uplcrt_builtin_state_of(fn);
        /* Hot path: this apply saturates the builtin (last arg, all
         * forces done). Skip clone_state's arena allocation entirely
         * by assembling the arg vector on the C stack and calling
         * run_builtin directly. Saves ~50-100 ns per saturating call
         * on hot arithmetic loops. Budget parity: run_builtin charges
         * the cost-model cost exactly as the slow path would. */
        if (UPLC_BC_LIKELY(bs->forces_applied == bs->total_forces &&
                           (uint32_t)bs->args_applied + 1u ==
                           (uint32_t)bs->total_args)) {
            uplc_value args_buf[UPLC_BC_MAX_BUILTIN_ARITY];
            for (uint8_t i = 0; i < bs->args_applied; ++i) {
                args_buf[i] = bs->args[i];
            }
            args_buf[bs->args_applied] = arg;
            *sp++ = uplcrt_run_builtin(st->budget, bs->tag,
                                        args_buf, bs->total_args);
            UPLC_BC_DISPATCH_NEXT(pc + 1, sp, st);
        }
        /* Non-saturating: the new VBuiltin value must survive beyond
         * this call (possibly shared across branches), so we need the
         * copy-on-write clone. */
        *sp++ = uplcrt_builtin_consume_arg(st->budget, bs, arg);
        UPLC_BC_DISPATCH_NEXT(pc + 1, sp, st);
    }
    if (fn.tag == UPLC_V_LAM) {
        uplcrt_raise(UPLC_FAIL_EVALUATION,
                     "bytecode VM: apply: cross-mode VLam");
    }
    uplcrt_raise(UPLC_FAIL_EVALUATION,
                 "bytecode VM: apply: not a function");
    /* unreachable */
    uplc_value dummy = {0};
    return dummy;
}

/*
 * TAIL_APPLY — emitted when an APPLY is immediately followed by
 * RETURN. For the bytecode-lambda fast path we do NOT push a new
 * return frame; the callee's eventual RETURN pops the existing frame
 * (which belongs to OUR caller) and transfers directly to them,
 * saving a push + pop per call.
 *
 * For VBuiltin we fall through to the equivalent of APPLY + RETURN:
 * consume the arg to produce the result, push it, then execute the
 * same RETURN logic op_return uses.
 *
 * Budget parity: same single StepApply charge — RETURN is free.
 */
UPLC_BC_HOT
uplc_value uplc_bc_op_tail_apply(const uplc_bc_word* pc, uplc_value* sp,
                                 uplc_bc_state* st) {
    (void)pc;
    uplcrt_budget_step(st->budget, UPLC_STEP_APPLY);
    uplc_value arg = *(--sp);
    uplc_value fn  = *(--sp);

    if (UPLC_BC_LIKELY(fn.tag == UPLC_V_LAM &&
                       fn.subtag == (uint8_t)UPLC_VLAM_BYTECODE)) {
        uplc_bc_closure*  c      = uplc_bc_closure_of(fn);
        const uplc_bc_fn* target = c->fn;

        /* No frame push — the frame currently on top belongs to OUR
         * caller, and when `target`'s body eventually RETURNs it will
         * pop that frame and dispatch directly to our caller. */
        uint32_t nfree    = c->n_upvals;
        uint32_t env_size = 1u + nfree;
        uplc_value* new_env = (uplc_value*)uplc_arena_alloc(
            st->arena, sizeof(uplc_value) * env_size, _Alignof(uplc_value));
        new_env[0] = arg;
        if (nfree > 0) {
            memcpy(&new_env[1], c->upvals, sizeof(uplc_value) * nfree);
        }
        st->env = new_env;

        UPLC_BC_DISPATCH_NEXT(target->opcodes, sp, st);
    }

    if (fn.tag == UPLC_V_BUILTIN) {
        uplc_builtin_state* bs = uplcrt_builtin_state_of(fn);
        /* Saturating fast path — skip clone_state, assemble args on
         * the C stack, dispatch via run_builtin directly. Same
         * optimisation as op_apply. */
        if (UPLC_BC_LIKELY(bs->forces_applied == bs->total_forces &&
                           (uint32_t)bs->args_applied + 1u ==
                           (uint32_t)bs->total_args)) {
            uplc_value args_buf[UPLC_BC_MAX_BUILTIN_ARITY];
            for (uint8_t i = 0; i < bs->args_applied; ++i) {
                args_buf[i] = bs->args[i];
            }
            args_buf[bs->args_applied] = arg;
            *sp++ = uplcrt_run_builtin(st->budget, bs->tag,
                                        args_buf, bs->total_args);
        } else {
            *sp++ = uplcrt_builtin_consume_arg(st->budget, bs, arg);
        }
        /* Inline RETURN logic: the builtin produced the result, now
         * hand it back to our caller the same way op_return would. */
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

    if (fn.tag == UPLC_V_LAM) {
        uplcrt_raise(UPLC_FAIL_EVALUATION,
                     "bytecode VM: tail_apply: cross-mode VLam");
    }
    uplcrt_raise(UPLC_FAIL_EVALUATION,
                 "bytecode VM: tail_apply: not a function");
    uplc_value dummy = {0};
    return dummy;
}
