#ifndef UPLCRT_BC_DISPATCH_H
#define UPLCRT_BC_DISPATCH_H

/*
 * Tail-call-threaded dispatch for the bytecode VM.
 *
 * Each opcode handler is a function with a fixed signature. A handler
 * charges its step cost, does its work, then `musttail`-calls into the
 * next handler via UPLC_BC_DISPATCH_NEXT. The dispatch table
 * UPLC_BC_OP_TABLE is indexed by the low byte of the instruction word.
 *
 * Signature is kept minimal so clang can keep pc / sp / state in
 * callee-saved registers across the tail chain. The value stack and
 * the bytecode program both live behind `st`; only pc and sp flow via
 * args because they mutate every handler.
 *
 * Failure (ERROR, OutOfBudget, machine invariant) longjmps via the
 * installed fail context — not through the return value. The normal
 * return path carries the final uplc_value up the tail-call chain
 * until the top-level RETURN hands it back to uplc_bc_run.
 */

#include <stdint.h>

#include "runtime/bytecode/frames.h"
#include "runtime/core/value.h"
#include "uplc/bytecode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uplc_bc_state uplc_bc_state;

typedef uplc_value (*uplc_bc_op_fn)(const uplc_bc_word* pc,
                                    uplc_value*         sp,
                                    uplc_bc_state*      st);

extern const uplc_bc_op_fn UPLC_BC_OP_TABLE[256];

/* Dispatch-loop state. Kept compact. The bytecode VM allocates one of
 * these on the C stack in uplc_bc_run and passes its address down the
 * tail chain.
 *
 * Call-frame layout:
 *   env[0]           = applied argument (LamAbs only; unused for Delay)
 *   env[1..n_upvals] = captured free vars for LamAbs
 *   env[0..n_upvals) = captured free vars for Delay (no arg slot)
 * The emitter is responsible for picking the right slot index per
 * variable reference; the runtime just indexes `env`. */
struct uplc_bc_state {
    uplc_value*             env;
    const uplc_bc_program*  prog;
    uplc_budget*            budget;
    uplc_arena*             arena;
    /* uplc_fail_ctx* (installed before dispatch begins). Typed void*
     * here to avoid pulling runtime/core/errors.h into every op TU. */
    void*                   fail_ctx;
    /* Value-stack bounds — uplc_bc_run passes the base/end so handlers
     * can assert on underflow in debug builds. */
    uplc_value*             stack_base;
    uplc_value*             stack_end;
    /* Frame stack. `fp` points to the next slot; `fp == frame_base`
     * means we're in the top-level entry function and a RETURN there
     * hands the result back to uplc_bc_run. */
    uplc_bc_frame*          fp;
    uplc_bc_frame*          frame_base;
    uplc_bc_frame*          frame_end;
};

#if defined(__clang__)
#  define UPLC_BC_MUSTTAIL       [[clang::musttail]]
#  define UPLC_BC_HOT            __attribute__((hot))
#  define UPLC_BC_ALWAYS_INLINE  __attribute__((always_inline))
#  define UPLC_BC_LIKELY(x)      __builtin_expect(!!(x), 1)
#  define UPLC_BC_UNLIKELY(x)    __builtin_expect(!!(x), 0)
#elif defined(__GNUC__)
#  define UPLC_BC_MUSTTAIL       __attribute__((musttail))
#  define UPLC_BC_HOT            __attribute__((hot))
#  define UPLC_BC_ALWAYS_INLINE  __attribute__((always_inline))
#  define UPLC_BC_LIKELY(x)      __builtin_expect(!!(x), 1)
#  define UPLC_BC_UNLIKELY(x)    __builtin_expect(!!(x), 0)
#else
#  define UPLC_BC_MUSTTAIL
#  define UPLC_BC_HOT
#  define UPLC_BC_ALWAYS_INLINE
#  define UPLC_BC_LIKELY(x)      (x)
#  define UPLC_BC_UNLIKELY(x)    (x)
#endif

/* Tail-call into the opcode word at *pc. Intended for inline use inside
 * an op handler after it has finished its own work and advanced pc/sp. */
#define UPLC_BC_DISPATCH_NEXT(pc_, sp_, st_) do {                          \
    const uplc_bc_word _bc_w = (pc_)[0];                                   \
    uint8_t _bc_op = (uint8_t)(_bc_w & 0xFFu);                             \
    UPLC_BC_MUSTTAIL                                                       \
    return UPLC_BC_OP_TABLE[_bc_op]((pc_), (sp_), (st_));                  \
} while (0)

/* Declarations for op handlers. Each lives in its own TU and is added to
 * UPLC_BC_OP_TABLE in vm.c. Kept extern so handlers can reference each
 * other in tail-call chains (tail_apply → apply, etc). */
uplc_value uplc_bc_op_var_local(const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_var_upval(const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_const    (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_builtin  (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_mk_lam   (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_mk_delay (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_apply        (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_force        (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_tail_apply   (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_tail_force   (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_constr   (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_case     (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_error    (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);
uplc_value uplc_bc_op_return   (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);

/* Placeholder for ops not yet implemented — raises UPLC_FAIL_MACHINE. */
uplc_value uplc_bc_op_unimpl   (const uplc_bc_word* pc, uplc_value* sp, uplc_bc_state* st);

/* Grow the malloc-backed frame stack 2× when APPLY / FORCE / CASE is
 * about to push into a full buffer. Updates st->{frame_base, fp,
 * frame_end}. Raises on hard ceiling or allocator failure. */
void uplcrt_bc_grow_frames(uplc_bc_state* st);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_BC_DISPATCH_H */
