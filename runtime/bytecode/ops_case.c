/*
 * CASE — decompose a scrutinee into (tag, fields), dispatch into the
 * selected alt's body, then apply fields left-to-right.
 *
 * Instruction layout (variable length):
 *   word 0: [op:8 | n_alts:24]
 *   for each alt in tag order:
 *     word: fn_id (u32)
 *     word: nfree (u32)
 *     words[nfree]: env-slot indices in the enclosing scope
 *
 * Each alt carries its own capture plan so the selected alt's body can
 * see free variables from the enclosing scope — mirroring how MK_LAM /
 * MK_DELAY work. Only the selected alt's env is materialised; alts not
 * chosen have zero runtime cost (laziness preserved).
 *
 * Semantics match the TS CEK reference:
 *   - Charges StepCase once.
 *   - Only the selected alt is evaluated.
 *   - Field applications do NOT charge StepApply (they are synthetic
 *     FApplyTo frames in the CEK, not source-level Apply terms).
 *
 * Sub-dispatch mechanism: to run the alt body to a value and to apply
 * fields, we push a boundary frame (ret_pc = NULL) and invoke the
 * dispatch table directly (not via musttail). uplc_bc_op_return treats
 * a frame with ret_pc == NULL as "return up the tail chain to the C
 * caller", which comes back into this handler.
 */

#include "runtime/bytecode/closure.h"
#include "runtime/bytecode/dispatch.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

#include "runtime/core/arena.h"
#include "runtime/core/builtin_state.h"
#include "runtime/core/case_decompose.h"
#include "runtime/core/errors.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

/* Run a bytecode function with the given env until it hits a RETURN
 * whose frame carries ret_pc == NULL, and return that RETURN's value. */
static uplc_value run_bytecode_subfn(uplc_bc_state*     st,
                                     const uplc_bc_fn*  target,
                                     uplc_value*        env,
                                     uplc_value*        sp) {
    if (st->fp >= st->frame_end) {
        uplcrt_bc_grow_frames(st);
    }
    st->fp->ret_pc  = NULL;           /* boundary marker */
    st->fp->ret_env = st->env;
    ++st->fp;
    st->env = env;

    uint8_t op0 = (uint8_t)(target->opcodes[0] & 0xFFu);
    return UPLC_BC_OP_TABLE[op0](target->opcodes, sp, st);
}

/* Apply `picked` to `arg` and return the result. Same dispatch rules as
 * op_apply but implemented synchronously (regular function call, not
 * musttail) so it can be looped inside op_case.
 *
 * Intentionally does NOT charge StepApply: in the TS CEK reference,
 * case-field application is synthesised by FCase -> APPLY_TO frames
 * that are not backed by a source-level Apply term, and the budget
 * charge only fires once per source Apply. This helper is used only
 * from op_case; standalone APPLY goes through op_apply which does
 * charge. */
static uplc_value apply_one(uplc_bc_state* st, uplc_value picked,
                            uplc_value arg, uplc_value* sp) {
    if (picked.tag == UPLC_V_LAM) {
        if (picked.subtag != (uint8_t)UPLC_VLAM_BYTECODE) {
            uplcrt_raise(UPLC_FAIL_EVALUATION,
                         "bytecode VM: case: cross-mode VLam in alt");
        }
        uplc_bc_closure*  c      = uplc_bc_closure_of(picked);
        const uplc_bc_fn* target = c->fn;

        /* Build callee env: [arg, upvals...]. */
        uint32_t env_size = 1u + c->n_upvals;
        uplc_value* new_env = (uplc_value*)uplc_arena_alloc(
            st->arena, sizeof(uplc_value) * env_size, _Alignof(uplc_value));
        new_env[0] = arg;
        if (c->n_upvals > 0) {
            memcpy(&new_env[1], c->upvals,
                   sizeof(uplc_value) * c->n_upvals);
        }
        return run_bytecode_subfn(st, target, new_env, sp);
    }
    if (picked.tag == UPLC_V_BUILTIN) {
        uplc_builtin_state* bs = uplcrt_builtin_state_of(picked);
        return uplcrt_builtin_consume_arg(st->budget, bs, arg);
    }
    uplcrt_raise(UPLC_FAIL_EVALUATION,
                 "bytecode VM: case: apply field to non-function");
    /* unreachable */
    uplc_value dummy = {0};
    return dummy;
}

UPLC_BC_HOT
uplc_value uplc_bc_op_case(const uplc_bc_word* pc, uplc_value* sp,
                           uplc_bc_state* st) {
    uplcrt_budget_step(st->budget, UPLC_STEP_CASE);
    uint32_t n_alts = uplc_bc_imm24_of(pc[0]);

    uplc_value scrutinee = *(--sp);
    uplc_case_decomp dec = uplcrt_case_decompose(st->budget, scrutinee, n_alts);

    /* Walk per-alt headers to find alt[tag]'s plan and the end of the
     * CASE instruction. Each header is {fn_id, nfree, slots[nfree]}. */
    const uplc_bc_word* p       = pc + 1;
    const uplc_bc_word* alt_hdr = NULL;
    for (uint32_t i = 0; i < n_alts; ++i) {
        if (i == dec.tag) alt_hdr = p;
        uint32_t nfree_i = (uint32_t)p[1];
        p += 2u + nfree_i;
    }
    const uplc_bc_word* after = p;

    uint32_t alt_fn_id = (uint32_t)alt_hdr[0];
    uint32_t alt_nfree = (uint32_t)alt_hdr[1];
    const uplc_bc_word* alt_slots = alt_hdr + 2;
    const uplc_bc_fn* alt_fn = &st->prog->functions[alt_fn_id];

    /* Build the alt's env by resolving each capture slot against the
     * current env — same shape as MK_LAM / MK_DELAY construction. Alts
     * are Delay-shaped (no arg slot) so env is exactly [upvals...]. */
    uplc_value* alt_env = NULL;
    if (alt_nfree > 0) {
        alt_env = (uplc_value*)uplc_arena_alloc(
            st->arena, sizeof(uplc_value) * alt_nfree, _Alignof(uplc_value));
        for (uint32_t i = 0; i < alt_nfree; ++i) {
            alt_env[i] = st->env[alt_slots[i]];
        }
    }

    uplc_value alt_value = run_bytecode_subfn(st, alt_fn, alt_env, sp);

    /* Apply fields to the alt value. apply_one deliberately does NOT
     * charge StepApply — these are synthetic FApplyTo dispatches in the
     * CEK reference. */
    for (uint32_t i = 0; i < dec.n_fields; ++i) {
        alt_value = apply_one(st, alt_value, dec.fields[i], sp);
    }

    *sp++ = alt_value;
    UPLC_BC_DISPATCH_NEXT(after, sp, st);
}
