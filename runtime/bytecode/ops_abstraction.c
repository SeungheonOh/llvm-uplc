/*
 * MK_LAM / MK_DELAY — create a bytecode closure by capturing values
 * from the current env and wrap them in a V_LAM / V_DELAY value with
 * subtag UPLC_VLAM_BYTECODE.
 *
 * Instruction layout (shared by both):
 *   word 0: [op:8 | fn_id:24]
 *   word 1: [n_upvals]
 *   word 2..2+n_upvals-1: slot indices into the current env
 */

#include "runtime/bytecode/closure.h"
#include "runtime/bytecode/dispatch.h"

#include <stddef.h>
#include <stdint.h>

#include "runtime/core/arena.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

/* Allocate the bc_closure directly in the arena and populate its
 * flexible `upvals[]` array in place. Saves the intermediate
 * `upvals_buf` allocation + memcpy that the older path did, cutting
 * MK_LAM / MK_DELAY arena work roughly in half. */
static uplc_value build_closure_value(const uplc_bc_word* pc, uplc_bc_state* st,
                                      uint8_t value_tag) {
    uint32_t fn_id = uplc_bc_imm24_of(pc[0]);
    uint32_t nfree = pc[1];
    const uplc_bc_fn* fn = &st->prog->functions[fn_id];

    size_t bytes = sizeof(uplc_bc_closure) + sizeof(uplc_value) * nfree;
    uplc_bc_closure* c = (uplc_bc_closure*)uplc_arena_alloc(
        st->arena, bytes, _Alignof(uplc_bc_closure));
    c->fn       = fn;
    c->n_upvals = nfree;
    c->_pad     = 0;
    for (uint32_t i = 0; i < nfree; ++i) {
        c->upvals[i] = st->env[pc[2 + i]];
    }

    uplc_value v = {
        .tag     = value_tag,
        .subtag  = (uint8_t)UPLC_VLAM_BYTECODE,
        ._pad    = {0},
        .payload = (uint64_t)(uintptr_t)c,
    };
    return v;
}

UPLC_BC_HOT
uplc_value uplc_bc_op_mk_lam(const uplc_bc_word* pc, uplc_value* sp,
                             uplc_bc_state* st) {
    uplcrt_budget_step(st->budget, UPLC_STEP_LAMBDA);
    *sp++ = build_closure_value(pc, st, (uint8_t)UPLC_V_LAM);
    const uplc_bc_word* next_pc = pc + 2 + pc[1];
    UPLC_BC_DISPATCH_NEXT(next_pc, sp, st);
}

UPLC_BC_HOT
uplc_value uplc_bc_op_mk_delay(const uplc_bc_word* pc, uplc_value* sp,
                               uplc_bc_state* st) {
    uplcrt_budget_step(st->budget, UPLC_STEP_DELAY);
    *sp++ = build_closure_value(pc, st, (uint8_t)UPLC_V_DELAY);
    const uplc_bc_word* next_pc = pc + 2 + pc[1];
    UPLC_BC_DISPATCH_NEXT(next_pc, sp, st);
}
