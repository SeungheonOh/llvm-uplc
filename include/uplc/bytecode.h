#ifndef UPLC_BYTECODE_H
#define UPLC_BYTECODE_H

/*
 * Bytecode ISA for the uplci interpreter path.
 *
 * Instruction word: [opcode:8 | imm24:24], little-endian.
 *
 *   UPLC_BC_VAR_LOCAL  imm24 = slot in current env (env[0]=arg, env[1..]=upvals)
 *   UPLC_BC_VAR_UPVAL  imm24 = upval index in the enclosing closure
 *   UPLC_BC_CONST      imm24 = index into uplc_bc_program.consts
 *   UPLC_BC_BUILTIN    imm24 = builtin tag (uplc_builtin_tag)
 *   UPLC_BC_MK_LAM     imm24 = fn_id; +1 nfree word; +nfree slot words
 *   UPLC_BC_MK_DELAY   imm24 = fn_id; +1 nfree word; +nfree slot words
 *   UPLC_BC_APPLY      (no operands) — pops arg, pops fn
 *   UPLC_BC_FORCE      (no operands) — pops value
 *   UPLC_BC_CONSTR     imm24 = tag_lo_24; +1 word with n_fields and tag high bits
 *   UPLC_BC_CASE       imm24 = n_alts; +n_alts fn_id words
 *   UPLC_BC_RETURN     terminates the current function, top-of-stack = result
 *   UPLC_BC_ERROR      raises EvaluationFailure
 *   UPLC_BC_TAIL_APPLY tail-position apply (reuses frame)
 *   UPLC_BC_TAIL_FORCE tail-position force
 *
 * Semantics: every opcode corresponds 1-to-1 with a CEK step kind and
 * charges one step via uplcrt_budget_step (see uplc/budget.h). Bit-exact
 * budget parity with the TS reference falls out automatically.
 */

#include <stdint.h>

#include "uplc/abi.h"
#include "uplc/budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declared; concrete layout lives in runtime/core/arena.h. */
typedef struct uplc_arena uplc_arena;

/* Forward-declared; readback uses the original rterm body + upval
 * deBruijn plan to reconstruct lambda/delay terms. Concrete rterm
 * layout lives in runtime/core/rterm.h. */
typedef struct uplc_rterm uplc_rterm;

typedef uint32_t uplc_bc_word;

typedef enum uplc_bc_op {
    UPLC_BC_VAR_LOCAL  = 0x01,
    UPLC_BC_VAR_UPVAL  = 0x02,
    UPLC_BC_CONST      = 0x03,
    UPLC_BC_BUILTIN    = 0x04,
    UPLC_BC_MK_LAM     = 0x05,
    UPLC_BC_MK_DELAY   = 0x06,
    UPLC_BC_APPLY      = 0x07,
    UPLC_BC_FORCE      = 0x08,
    UPLC_BC_CONSTR     = 0x09,
    UPLC_BC_CASE       = 0x0A,
    UPLC_BC_RETURN     = 0x0B,
    UPLC_BC_ERROR      = 0x0C,
    UPLC_BC_TAIL_APPLY = 0x0D,
    UPLC_BC_TAIL_FORCE = 0x0E,

    UPLC_BC__OP_MAX    = 0xFF
} uplc_bc_op;

/* Build one instruction word from (opcode, imm24). */
static inline uplc_bc_word uplc_bc_mk(uint8_t op, uint32_t imm24) {
    return (uint32_t)op | (imm24 << 8);
}

/* Extract parts. */
static inline uint8_t  uplc_bc_op_of   (uplc_bc_word w) { return (uint8_t)(w & 0xFFu); }
static inline uint32_t uplc_bc_imm24_of(uplc_bc_word w) { return w >> 8; }

/* A bytecode function body. Points into the bytecode arena.
 *
 * `body_rterm` and `upval_outer_db` are populated for Lambda / Delay
 * sub-functions to let readback reconstruct the original term. The
 * top-level entry function and case-alt thunks set them to NULL / 0;
 * readback never encounters top-level closures (programs always return
 * a value, not the program itself), and case-alt closures never escape
 * the CASE opcode's sub-dispatch. */
typedef struct uplc_bc_fn {
    uint32_t            n_upvals;     /* captured free vars count */
    uint32_t            n_opcodes;    /* total 32-bit words including operand tails */
    const uplc_bc_word* opcodes;
    uint16_t            n_args;       /* 1 for LamAbs, 0 for Delay */
    uint16_t            max_stack;    /* emit-time computed upper bound */
    const uplc_rterm*   body_rterm;   /* original rterm body for readback; NULL if N/A */
    const uint32_t*     upval_outer_db;  /* length = n_upvals; each entry is the
                                          * deBruijn index (0-based) in the enclosing
                                          * scope that this upval slot captures. */
} uplc_bc_fn;

/* A bytecode program: a top-level function (index 0), its nested
 * sub-functions, and a shared constant pool. */
typedef struct uplc_bc_program {
    const uplc_bc_fn*  functions;
    uint32_t           n_functions;
    const uplc_value*  consts;
    uint32_t           n_consts;
    uint32_t           version_major;
    uint32_t           version_minor;
    uint32_t           version_patch;
} uplc_bc_program;

typedef struct uplc_bc_result {
    int            ok;              /* 1 on success, 0 on failure */
    uplc_fail_kind fail_kind;
    const char*    fail_message;
    uplc_value     value;           /* valid only if ok == 1 */
} uplc_bc_result;

/* Run the bytecode program's entry function (functions[0]). The budget
 * slippage buffer is flushed before return so callers always see the
 * final (cpu, mem). On failure `fail_kind` discriminates evaluation
 * failure, out-of-budget, and machine error. */
uplc_bc_result uplc_bc_run(uplc_arena* arena,
                           const uplc_bc_program* prog,
                           uplc_budget* budget);

#ifdef __cplusplus
}
#endif

#endif  /* UPLC_BYTECODE_H */
