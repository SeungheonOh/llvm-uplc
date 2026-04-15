#include "uplc/budget.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Budget implementation matching the TS reference's slippage scheme:
 *
 *   - `scratch[0..8]`  per-step-kind unbudgeted counters
 *   - `scratch[9]`     running total across all step kinds
 *
 * Each uplc_budget_step() bumps the relevant counter plus the total; once
 * the total reaches UPLC_SLIPPAGE we call uplcrt_budget_flush() which
 * converts the scratch values into (cpu, mem) charges using the step-cost
 * table and zeroes the scratch slots.
 *
 * Saturating arithmetic keeps the counters well-defined at overflow so
 * pathologically expensive programs can't wrap into "budget remaining".
 */

int64_t uplcrt_sat_add_i64(int64_t a, int64_t b) {
    int64_t r;
    if (__builtin_add_overflow(a, b, &r)) {
        return (a < 0) == (b < 0)
            ? (a < 0 ? INT64_MIN : INT64_MAX)
            : r;
    }
    return r;
}

int64_t uplcrt_sat_mul_i64(int64_t a, int64_t b) {
    int64_t r;
    if (__builtin_mul_overflow(a, b, &r)) {
        return ((a < 0) ^ (b < 0)) ? INT64_MIN : INT64_MAX;
    }
    return r;
}

void uplcrt_budget_init(uplc_budget* b, int64_t cpu, int64_t mem) {
    b->cpu = cpu;
    b->mem = mem;
    for (unsigned i = 0; i < UPLC_STEP__COUNT + 1; ++i) b->scratch[i] = 0;
    b->arena = NULL;
    b->initial_cpu = cpu;
    b->initial_mem = mem;
}

void uplcrt_budget_init_with_arena(uplc_budget* b, int64_t cpu, int64_t mem,
                                   void* arena) {
    uplcrt_budget_init(b, cpu, mem);
    b->arena = arena;
}

struct uplc_arena* uplcrt_budget_arena(uplc_budget* b) {
    return (struct uplc_arena*)b->arena;
}

/* TS-matching OOB semantics: consumed = sat_add(initial, -remaining),
 * saturating at I64_MAX. OOB iff consumed > initial. Under an I64_MAX
 * initial, consumed can at worst cap at I64_MAX, so no OOB. Under a
 * smaller initial, the saturation just makes it "very much over". */
int uplcrt_budget_ok(const uplc_budget* b) {
    int64_t cpu_consumed = uplcrt_sat_add_i64(b->initial_cpu, -b->cpu);
    int64_t mem_consumed = uplcrt_sat_add_i64(b->initial_mem, -b->mem);
    return (cpu_consumed <= b->initial_cpu &&
            mem_consumed <= b->initial_mem) ? 1 : 0;
}

/* Per-step machine costs. Matches TS cek/machine.ts MACHINE_COSTS — at the
 * current Conway parameters every step kind charges (16000 cpu, 100 mem).
 * Once M3 lands the full cost-model evaluator these numbers will be read
 * from the baked uplc_cost_model instead of hard-coded here. */
static const int64_t kStepCpu[UPLC_STEP__COUNT] = {
    16000, 16000, 16000, 16000, 16000, 16000, 16000, 16000, 16000
};
static const int64_t kStepMem[UPLC_STEP__COUNT] = {
    100, 100, 100, 100, 100, 100, 100, 100, 100
};

/* One-time startup cost charged on entry to the CEK / compiled entry.
 * Matches TS CekMachine's initial machineCosts.startup charge; without it
 * simple fixtures come in 100/100 under the expected totals. */
void uplcrt_budget_startup(uplc_budget* b) {
    b->cpu = uplcrt_sat_add_i64(b->cpu, -100);
    b->mem = uplcrt_sat_add_i64(b->mem, -100);
}

/* Apply the per-kind scratch counters to (cpu, mem) via saturating
 * subtraction, then zero the scratch. Called from uplcrt_budget_step once
 * the running total crosses UPLC_SLIPPAGE, and again at termination to
 * settle any residual scratch. */
void uplcrt_budget_flush(uplc_budget* b) {
    for (unsigned k = 0; k < UPLC_STEP__COUNT; ++k) {
        uint32_t n = b->scratch[k];
        if (n == 0) continue;
        int64_t cpu = uplcrt_sat_mul_i64((int64_t)n, kStepCpu[k]);
        int64_t mem = uplcrt_sat_mul_i64((int64_t)n, kStepMem[k]);
        b->cpu = uplcrt_sat_add_i64(b->cpu, -cpu);
        b->mem = uplcrt_sat_add_i64(b->mem, -mem);
        b->scratch[k] = 0;
    }
    b->scratch[UPLC_STEP__COUNT] = 0;
}

/* Charge one machine step of the given kind. Defined as inlinable for the
 * interpreter hot path; compiler-emitted IR in M7 will inline this via the
 * same header. */
void uplcrt_budget_step(uplc_budget* b, uplc_step_kind kind) {
    unsigned k = (unsigned)kind;
    ++b->scratch[k];
    if (++b->scratch[UPLC_STEP__COUNT] >= UPLC_SLIPPAGE) {
        uplcrt_budget_flush(b);
    }
}
