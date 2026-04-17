/*
 * Bytecode VM entry. Sets up the dispatch state, arms the failure
 * trampoline, charges CEK startup, and hands off to the first opcode of
 * the entry function.
 *
 * The opcode table is defined here as the single source of truth. Ops
 * not yet implemented in this milestone (M-bc-2) route to
 * uplc_bc_op_unimpl which raises UPLC_FAIL_MACHINE — this lets us
 * incrementally fill in opcodes in later milestones without silently
 * running wrong semantics.
 */

#include "runtime/bytecode/dispatch.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/core/arena.h"
#include "runtime/core/errors.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/bytecode.h"
#include "uplc/budget.h"

/* Value-stack size. The stack is SHARED across all nested sub-
 * dispatches (callee bodies re-use the caller's sp via
 * run_bytecode_subfn in op_case, op_apply's callee, etc.), so a
 * per-function high-water mark can't bound the total: deeply nested
 * applies compound each callee's stack usage on top of the caller's.
 *
 * 16 K slots × 16 bytes = 256 KiB per eval. Freed wholesale on arena
 * destroy, so steady-state memory is bounded regardless of iteration
 * count. Overflow is checked at the push site (op_apply's env build
 * etc.) and reports UPLC_FAIL_MACHINE cleanly. */
#define UPLC_BC_DEFAULT_STACK  (16u * 1024u)

/* Frame-stack capacity. Lives in the evaluation arena so it's freed
 * wholesale when the arena is destroyed — no malloc/realloc churn and
 * no longjmp/indeterminate-local hazards.
 *
 * 32 K frames × 16 bytes = 512 KiB per eval. Chosen to cover the
 * deepest real-world validator in the SAIB benchmark corpus (gun-*,
 * useCasing, useOld) while keeping per-iteration memory bounded.
 * Arena destroy at the end of each iteration releases it wholesale,
 * so steady-state memory is flat regardless of iteration count. */
#define UPLC_BC_DEFAULT_FRAMES (32u * 1024u)

/* ------------------------------------------------------------------ */
/* Opcode table                                                        */
/* ------------------------------------------------------------------ */

const uplc_bc_op_fn UPLC_BC_OP_TABLE[256] = {
    [UPLC_BC_VAR_LOCAL]  = uplc_bc_op_var_local,
    [UPLC_BC_VAR_UPVAL]  = uplc_bc_op_var_upval,
    [UPLC_BC_CONST]      = uplc_bc_op_const,
    [UPLC_BC_BUILTIN]    = uplc_bc_op_builtin,
    [UPLC_BC_MK_LAM]     = uplc_bc_op_mk_lam,
    [UPLC_BC_MK_DELAY]   = uplc_bc_op_mk_delay,
    [UPLC_BC_APPLY]      = uplc_bc_op_apply,
    [UPLC_BC_FORCE]      = uplc_bc_op_force,
    [UPLC_BC_CONSTR]     = uplc_bc_op_constr,
    [UPLC_BC_CASE]       = uplc_bc_op_case,
    [UPLC_BC_RETURN]     = uplc_bc_op_return,
    [UPLC_BC_ERROR]      = uplc_bc_op_error,
    [UPLC_BC_TAIL_APPLY] = uplc_bc_op_tail_apply,
    [UPLC_BC_TAIL_FORCE] = uplc_bc_op_tail_force,
};

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

uplc_bc_result uplc_bc_run(uplc_arena* arena,
                           const uplc_bc_program* prog,
                           uplc_budget* budget) {
    uplc_bc_result r;
    r.ok           = 0;
    r.fail_kind    = UPLC_FAIL_MACHINE;
    r.fail_message = NULL;
    uplc_value zero = {0};
    r.value        = zero;

    if (!prog || !prog->functions || prog->n_functions == 0) {
        r.fail_message = "bytecode VM: empty program";
        return r;
    }

    const uplc_bc_fn* entry = &prog->functions[0];
    if (!entry->opcodes || entry->n_opcodes == 0) {
        r.fail_message = "bytecode VM: empty entry function";
        return r;
    }

    /* Allocate value stack and frame stack in the arena. Value stack
     * is a fixed generous size; see UPLC_BC_DEFAULT_STACK comment. */
    uint32_t stack_slots = UPLC_BC_DEFAULT_STACK;
    uplc_value* stack = (uplc_value*)uplc_arena_alloc(
        arena, sizeof(uplc_value) * stack_slots, _Alignof(uplc_value));
    if (!stack) {
        r.fail_message = "bytecode VM: value stack allocation failed";
        return r;
    }

    uint32_t frame_slots = UPLC_BC_DEFAULT_FRAMES;
    uplc_bc_frame* frames = (uplc_bc_frame*)uplc_arena_alloc(
        arena, sizeof(uplc_bc_frame) * frame_slots, _Alignof(uplc_bc_frame));
    if (!frames) {
        r.fail_message = "bytecode VM: frame stack allocation failed";
        return r;
    }

    uplc_fail_ctx fctx = {0};
    uplc_bc_state st;
    st.env        = NULL;           /* top-level entry has no args/upvals */
    st.prog       = prog;
    st.budget     = budget;
    st.arena      = arena;
    st.fail_ctx   = &fctx;
    st.stack_base = stack;
    st.stack_end  = stack + stack_slots;
    st.fp         = frames;
    st.frame_base = frames;
    st.frame_end  = frames + frame_slots;

    if (setjmp(fctx.env) != 0) {
        /* Failure path — budget scratch may be dirty; flush so caller
         * sees bit-exact (cpu, mem) at the moment of failure. */
        uplcrt_budget_flush(budget);
        r.ok           = 0;
        r.fail_kind    = fctx.kind;
        r.fail_message = fctx.message;
        return r;
    }
    uplcrt_fail_install(&fctx);

    /* Charge the one-time CEK startup cost. Matches TS reference. */
    uplcrt_budget_startup(budget);

    /* Kick off dispatch on the first opcode of the entry function. */
    const uplc_bc_word* pc = entry->opcodes;
    uint8_t first_op = (uint8_t)(pc[0] & 0xFFu);
    uplc_bc_op_fn first = UPLC_BC_OP_TABLE[first_op];
    if (!first) {
        uplcrt_raise(UPLC_FAIL_MACHINE, "bytecode VM: null opcode at entry");
    }
    r.value = first(pc, stack, &st);
    r.ok   = 1;
    return r;
}

/* Frame stack lives in the per-evaluation arena, freed wholesale on
 * arena destroy. Overflow raises UPLC_FAIL_MACHINE cleanly. Growing
 * in place (realloc) is available as a future optimisation but is
 * deferred: the longjmp/indeterminate-local interaction made it
 * fragile, and 32 K frames is enough for all known real workloads. */
void uplcrt_bc_grow_frames(uplc_bc_state* st) {
    (void)st;
    uplcrt_raise(UPLC_FAIL_MACHINE,
                 "bytecode VM: frame stack overflow (depth > 32768)");
}
