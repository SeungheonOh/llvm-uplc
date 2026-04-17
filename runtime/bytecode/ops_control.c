/*
 * Control / leaf ops for the bytecode VM.
 *
 *   VAR_LOCAL  push env[imm24]                       — StepVar
 *   CONST      push prog->consts[imm24]              — StepConst
 *   BUILTIN    push fresh VBuiltin for tag imm24     — StepBuiltin
 *   ERROR      raise UPLC_FAIL_EVALUATION            — no step charge
 *   RETURN     flush budget, return top-of-stack     — no step charge
 *
 * All handlers match uplc_bc_op_fn and dispatch the next opcode via
 * UPLC_BC_DISPATCH_NEXT. RETURN is the only handler that does NOT tail-
 * call further — it returns the final value up the chain to uplc_bc_run.
 */

#include "runtime/bytecode/dispatch.h"

#include <stdint.h>
#include <stddef.h>

#include "runtime/core/builtin_state.h"
#include "runtime/core/errors.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"

UPLC_BC_HOT
uplc_value uplc_bc_op_var_local(const uplc_bc_word* pc, uplc_value* sp,
                                uplc_bc_state* st) {
    uplcrt_budget_step(st->budget, UPLC_STEP_VAR);
    uint32_t slot = uplc_bc_imm24_of(pc[0]);
    *sp++ = st->env[slot];
    UPLC_BC_DISPATCH_NEXT(pc + 1, sp, st);
}

/* VAR_UPVAL is a semantic alias of VAR_LOCAL at the runtime level — the
 * emitter resolves absolute slot indices into the flat env array; the
 * distinction between "arg" and "upval" exists only for emit-time
 * bookkeeping and may later drive inline-cache decisions. */
UPLC_BC_HOT
uplc_value uplc_bc_op_var_upval(const uplc_bc_word* pc, uplc_value* sp,
                                uplc_bc_state* st) {
    uplcrt_budget_step(st->budget, UPLC_STEP_VAR);
    uint32_t slot = uplc_bc_imm24_of(pc[0]);
    *sp++ = st->env[slot];
    UPLC_BC_DISPATCH_NEXT(pc + 1, sp, st);
}

UPLC_BC_HOT
uplc_value uplc_bc_op_const(const uplc_bc_word* pc, uplc_value* sp,
                            uplc_bc_state* st) {
    uplcrt_budget_step(st->budget, UPLC_STEP_CONST);
    uint32_t idx = uplc_bc_imm24_of(pc[0]);
    *sp++ = st->prog->consts[idx];
    UPLC_BC_DISPATCH_NEXT(pc + 1, sp, st);
}

UPLC_BC_HOT
uplc_value uplc_bc_op_builtin(const uplc_bc_word* pc, uplc_value* sp,
                              uplc_bc_state* st) {
    uplcrt_budget_step(st->budget, UPLC_STEP_BUILTIN);
    uint32_t tag = uplc_bc_imm24_of(pc[0]);
    *sp++ = uplcrt_builtin_fresh(st->budget, (uint8_t)tag);
    UPLC_BC_DISPATCH_NEXT(pc + 1, sp, st);
}

uplc_value uplc_bc_op_error(const uplc_bc_word* pc, uplc_value* sp,
                            uplc_bc_state* st) {
    (void)pc;
    (void)sp;
    /* Match the tree-walking CEK reference (runtime/cek/cek.c step_for_tag):
     * every CEK compute step charges one machine step regardless of the
     * term's outcome, and ERROR charges under the STEP_CONST bucket as a
     * placeholder. Preserving this keeps bit-exact budget parity. */
    uplcrt_budget_step(st->budget, UPLC_STEP_CONST);
    uplcrt_raise(UPLC_FAIL_EVALUATION, "error");
    /* unreachable — uplcrt_raise is noreturn */
    uplc_value dummy = {0};
    return dummy;
}

uplc_value uplc_bc_op_return(const uplc_bc_word* pc, uplc_value* sp,
                             uplc_bc_state* st) {
    (void)pc;
    if (st->fp <= st->frame_base) {
        /* Top-level return — flush slippage so caller sees bit-exact
         * (cpu, mem), then hand the result up through the tail chain. */
        uplcrt_budget_flush(st->budget);
        return *(sp - 1);
    }
    /* Pop one frame. If the popped frame has ret_pc == NULL it's a
     * sub-dispatch boundary — return up the tail chain to the C caller
     * that set it up (CASE, for example). Otherwise tail-call back into
     * the caller's opcode stream. Env is restored in both cases so any
     * C-level caller sees a consistent machine state. */
    --st->fp;
    st->env = st->fp->ret_env;
    if (st->fp->ret_pc == NULL) {
        return *(sp - 1);
    }
    UPLC_BC_DISPATCH_NEXT(st->fp->ret_pc, sp, st);
}

uplc_value uplc_bc_op_unimpl(const uplc_bc_word* pc, uplc_value* sp,
                             uplc_bc_state* st) {
    (void)pc;
    (void)sp;
    (void)st;
    uplcrt_raise(UPLC_FAIL_MACHINE, "bytecode VM: opcode not implemented yet");
    uplc_value dummy = {0};
    return dummy;
}
