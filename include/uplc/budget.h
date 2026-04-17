#ifndef UPLC_BUDGET_H
#define UPLC_BUDGET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Step kinds. Exactly match the TS reference's 9-step model in
 * uplc/cek/machine.ts. The order here is the order of the per-kind scratch
 * counters in `uplc_budget::scratch`.
 * ------------------------------------------------------------------------- */
typedef enum uplc_step_kind {
    UPLC_STEP_CONST    = 0,
    UPLC_STEP_VAR      = 1,
    UPLC_STEP_LAMBDA   = 2,
    UPLC_STEP_APPLY    = 3,
    UPLC_STEP_DELAY    = 4,
    UPLC_STEP_FORCE    = 5,
    UPLC_STEP_BUILTIN  = 6,
    UPLC_STEP_CONSTR   = 7,
    UPLC_STEP_CASE     = 8,
    UPLC_STEP__COUNT   = 9
} uplc_step_kind;

/* TS machine buffers per-step-kind counts and flushes into (cpu, mem) every
 * SLIPPAGE total steps. We match the same threshold exactly so OutOfBudget
 * trip points are bit-compatible. */
#define UPLC_SLIPPAGE 200u

/* ExBudget with embedded slippage buffer.
 *
 *   cpu, mem        : flushed counters (int64, saturating-subtracted from budget)
 *   scratch[0..8]   : per-step-kind counters
 *   scratch[9]      : running total of all scratch slots
 *   arena           : evaluation arena pointer (void* to avoid dragging
 *                     arena.h into this public header). Both execution
 *                     paths (CEK interpreter and compiled code) stash the
 *                     arena here so ABI entrypoints that need to heap-
 *                     allocate (make_lam, make_delay, make_constr, const_*
 *                     helpers, ...) can find it without an extra parameter.
 *
 * `uplcrt_budget_flush` empties scratch into (cpu, mem) using the baked
 * step-cost table; codegen and the direct interpreter both go through it. */
typedef struct uplc_budget {
    int64_t  cpu;
    int64_t  mem;
    uint32_t scratch[UPLC_STEP__COUNT + 1];
    void*    arena;
    int64_t  initial_cpu;      /* initial cpu — needed so OOB reasoning can
                                * match TS's saturating-consumed semantics. */
    int64_t  initial_mem;
} uplc_budget;

/* Saturating i64 arithmetic used by the cost-model evaluator. Matches the
 * behaviour of TS's satAdd/satMul in cek/costing.ts. */
int64_t uplcrt_sat_add_i64(int64_t a, int64_t b);
int64_t uplcrt_sat_mul_i64(int64_t a, int64_t b);

/* Initialise a budget. `cpu` / `mem` of INT64_MAX means unlimited; the
 * arena field is cleared to NULL — caller sets it before evaluation. */
void uplcrt_budget_init(uplc_budget* b, int64_t cpu, int64_t mem);

/* Convenience: initialise and attach an arena in one call. */
void uplcrt_budget_init_with_arena(uplc_budget* b, int64_t cpu, int64_t mem,
                                   void* arena);

/* Retrieve the arena as a forward-declared uplc_arena*. Callers that want
 * to act on it should also include runtime/arena.h. */
struct uplc_arena;
struct uplc_arena* uplcrt_budget_arena(uplc_budget* b);

/* Returns non-zero iff the program has not exceeded its initial budget.
 * Because builtin costs saturate at I64_MAX, we can't simply test
 * `remaining >= 0`: a program whose total cost saturates still consumes
 * ≤ I64_MAX, which is fine under an I64_MAX initial. The correct test
 * is `consumed <= initial` where `consumed = sat_sub(initial, remaining)`. */
int  uplcrt_budget_ok(const uplc_budget* b);

/* Charge the one-time CEK startup cost (100 cpu / 100 mem) against the
 * budget. Called exactly once per evaluation by the entry points. */
void uplcrt_budget_startup(uplc_budget* b);

/* Apply the per-kind scratch counters to (cpu, mem) via saturating
 * subtraction, then zero the scratch. Extern because it's cold — callers
 * hit it only once per UPLC_SLIPPAGE opcodes (see uplcrt_budget_step). */
void uplcrt_budget_flush(uplc_budget* b);

/* Charge one CEK step against the budget. `static inline` because this is
 * on the hot path — the bytecode VM dispatch loop and JIT-emitted IR both
 * hit it on every opcode. Keeping the body in the header lets every TU
 * inline it directly without -flto. The slow path (flush) stays extern.
 *
 * Callers that need a function pointer (e.g. an ABI-stable export for
 * JIT-time dynamic symbol lookup) can take `&uplcrt_budget_step_extern`
 * instead — it is an out-of-line wrapper with identical behaviour. */
static inline void uplcrt_budget_step(uplc_budget* b, uplc_step_kind kind) {
    unsigned k = (unsigned)kind;
    ++b->scratch[k];
    if (++b->scratch[UPLC_STEP__COUNT] >= UPLC_SLIPPAGE) {
        uplcrt_budget_flush(b);
    }
}

/* Out-of-line wrapper with ABI-stable address. Never called on the hot
 * path; exists solely so that symbol lookups against libuplcrt resolve
 * something concrete. */
void uplcrt_budget_step_extern(uplc_budget* b, uplc_step_kind kind);

#ifdef __cplusplus
}
#endif

#endif /* UPLC_BUDGET_H */
