/*
 * CONSTR — build a VConstr from the top n_fields values on the stack.
 *
 * Instruction layout:
 *   word 0: [op:8 | n_fields:24]
 *   word 1: tag_lo (uint32)
 *   word 2: tag_hi (uint32)
 *
 * Step cost: StepConstr.
 *
 * After the opcode, the n_fields slots of the stack are consumed and a
 * single VConstr value is pushed in their place.
 */

#include "runtime/bytecode/dispatch.h"

#include <stdint.h>

#include "runtime/core/arena.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

UPLC_BC_HOT
uplc_value uplc_bc_op_constr(const uplc_bc_word* pc, uplc_value* sp,
                             uplc_bc_state* st) {
    uplcrt_budget_step(st->budget, UPLC_STEP_CONSTR);
    uint32_t n_fields = uplc_bc_imm24_of(pc[0]);
    uint64_t tag = (uint64_t)pc[1] | ((uint64_t)pc[2] << 32);

    uplc_value* fields = sp - n_fields;
    uplc_value v = uplc_make_constr_vals(st->arena, tag, fields, n_fields);
    sp -= n_fields;
    *sp++ = v;

    UPLC_BC_DISPATCH_NEXT(pc + 3, sp, st);
}
