// Bytecode VM unit tests (M-bc-2).
//
// Programs here are hand-built bytecode — no frontend/emitter yet (that
// arrives in M-bc-5). The goal is to verify that the dispatch loop, the
// CONST and RETURN opcodes, step-charging, and the startup-cost accounting
// behave as the TS CEK reference would.
//
// Harness style matches tests/unit/cek_test.cc — no gtest, plain main().

#include <climits>
#include <cstdint>
#include <cstdio>

#include "runtime/core/arena.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"
#include "uplc/bytecode.h"

namespace {

int g_failures = 0;
int g_total    = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_total;                                                       \
        if (!(cond)) {                                                   \
            ++g_failures;                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
        }                                                                \
    } while (0)

#define CHECK_EQ_I64(actual, expected)                                    \
    do {                                                                  \
        ++g_total;                                                        \
        long long a_ = (long long)(actual);                               \
        long long e_ = (long long)(expected);                             \
        if (a_ != e_) {                                                   \
            ++g_failures;                                                 \
            std::fprintf(stderr,                                          \
                         "FAIL %s:%d: got %lld, want %lld\n",             \
                         __FILE__, __LINE__, a_, e_);                     \
        }                                                                 \
    } while (0)

// Per-kind per-step machine costs baked into runtime/core/budget.c.
// Conway-era params uniformly charge (16000, 100) for each step kind.
constexpr int64_t kStepCpu   = 16000;
constexpr int64_t kStepMem   = 100;
constexpr int64_t kStartCpu  = 100;   // uplcrt_budget_startup
constexpr int64_t kStartMem  = 100;

// -----------------------------------------------------------------------
// Test: `(con integer 42)` — just a constant, RETURN.
// Expected budget: startup + 1 * StepConst.
// -----------------------------------------------------------------------
void test_const_integer() {
    uplc_arena* arena = uplc_arena_create();
    CHECK(arena != nullptr);

    // Constant pool: a single inline integer 42.
    uplc_value consts[1];
    consts[0] = uplc_make_int_inline(42);

    // Bytecode: CONST 0 ; RETURN
    uplc_bc_word opcodes[2];
    opcodes[0] = uplc_bc_mk(UPLC_BC_CONST,  0);
    opcodes[1] = uplc_bc_mk(UPLC_BC_RETURN, 0);

    uplc_bc_fn fn{};
    fn.n_upvals  = 0;
    fn.n_opcodes = 2;
    fn.opcodes   = opcodes;
    fn.n_args    = 0;
    fn.max_stack = 1;

    uplc_bc_program prog{};
    prog.functions     = &fn;
    prog.n_functions   = 1;
    prog.consts        = consts;
    prog.n_consts      = 1;
    prog.version_major = 1;
    prog.version_minor = 1;
    prog.version_patch = 0;

    uplc_budget budget;
    uplcrt_budget_init_with_arena(&budget, INT64_MAX, INT64_MAX, arena);

    uplc_bc_result r = uplc_bc_run(arena, &prog, &budget);

    CHECK(r.ok == 1);
    CHECK(uplc_value_is_int_inline(r.value));
    if (uplc_value_is_int_inline(r.value)) {
        CHECK_EQ_I64(uplc_value_int_inline(r.value), 42);
    }

    // Budget: unlimited initial minus (startup + 1*StepConst).
    int64_t spent_cpu = INT64_MAX - budget.cpu;
    int64_t spent_mem = INT64_MAX - budget.mem;
    CHECK_EQ_I64(spent_cpu, kStartCpu + kStepCpu);
    CHECK_EQ_I64(spent_mem, kStartMem + kStepMem);

    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: ERROR opcode surfaces UPLC_FAIL_EVALUATION.
// -----------------------------------------------------------------------
void test_error_raises() {
    uplc_arena* arena = uplc_arena_create();

    uplc_bc_word opcodes[1];
    opcodes[0] = uplc_bc_mk(UPLC_BC_ERROR, 0);

    uplc_bc_fn fn{};
    fn.n_upvals  = 0;
    fn.n_opcodes = 1;
    fn.opcodes   = opcodes;
    fn.n_args    = 0;
    fn.max_stack = 0;

    uplc_bc_program prog{};
    prog.functions     = &fn;
    prog.n_functions   = 1;
    prog.consts        = nullptr;
    prog.n_consts      = 0;
    prog.version_major = 1;
    prog.version_minor = 1;
    prog.version_patch = 0;

    uplc_budget budget;
    uplcrt_budget_init_with_arena(&budget, INT64_MAX, INT64_MAX, arena);

    uplc_bc_result r = uplc_bc_run(arena, &prog, &budget);

    CHECK(r.ok == 0);
    CHECK_EQ_I64(r.fail_kind, UPLC_FAIL_EVALUATION);

    // ERROR charges one STEP_CONST to match the CEK reference (see
    // runtime/cek/cek.c step_for_tag: error is a placeholder charge).
    int64_t spent_cpu = INT64_MAX - budget.cpu;
    int64_t spent_mem = INT64_MAX - budget.mem;
    CHECK_EQ_I64(spent_cpu, kStartCpu + kStepCpu);
    CHECK_EQ_I64(spent_mem, kStartMem + kStepMem);

    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: `((lam x x) (con integer 7))` — identity applied to 7 returns 7.
// Steps: StartLamAbs → StartApply → (callee) VAR → return.
// Bytecode:
//   top fn (fn 0): MK_LAM 1 0 ; CONST 0 ; APPLY ; RETURN
//   body  (fn 1): VAR_LOCAL 0 ; RETURN
// Budget: startup + Lambda + Const + Apply + Var = (16100 + 4*16000 ...).
// -----------------------------------------------------------------------
void test_identity_apply() {
    uplc_arena* arena = uplc_arena_create();

    uplc_value consts[1];
    consts[0] = uplc_make_int_inline(7);

    // fn 1 (body of the identity lambda): VAR_LOCAL 0 ; RETURN
    uplc_bc_word body_ops[2];
    body_ops[0] = uplc_bc_mk(UPLC_BC_VAR_LOCAL, 0);
    body_ops[1] = uplc_bc_mk(UPLC_BC_RETURN,    0);

    // fn 0 (entry): MK_LAM fn_id=1 nfree=0 ; CONST 0 ; APPLY ; RETURN
    uplc_bc_word entry_ops[5];
    entry_ops[0] = uplc_bc_mk(UPLC_BC_MK_LAM, /*fn_id*/ 1);
    entry_ops[1] = 0;                                 // n_upvals = 0
    entry_ops[2] = uplc_bc_mk(UPLC_BC_CONST,  0);
    entry_ops[3] = uplc_bc_mk(UPLC_BC_APPLY,  0);
    entry_ops[4] = uplc_bc_mk(UPLC_BC_RETURN, 0);

    uplc_bc_fn fns[2]{};
    fns[0].n_upvals = 0;
    fns[0].n_opcodes = 5;
    fns[0].opcodes   = entry_ops;
    fns[0].n_args    = 0;
    fns[0].max_stack = 4;

    fns[1].n_upvals = 0;
    fns[1].n_opcodes = 2;
    fns[1].opcodes   = body_ops;
    fns[1].n_args    = 1;
    fns[1].max_stack = 1;

    uplc_bc_program prog{};
    prog.functions     = fns;
    prog.n_functions   = 2;
    prog.consts        = consts;
    prog.n_consts      = 1;
    prog.version_major = 1;
    prog.version_minor = 1;
    prog.version_patch = 0;

    uplc_budget budget;
    uplcrt_budget_init_with_arena(&budget, INT64_MAX, INT64_MAX, arena);

    uplc_bc_result r = uplc_bc_run(arena, &prog, &budget);

    CHECK(r.ok == 1);
    CHECK(uplc_value_is_int_inline(r.value));
    if (uplc_value_is_int_inline(r.value)) {
        CHECK_EQ_I64(uplc_value_int_inline(r.value), 7);
    }

    // Budget: startup + 1 Lambda + 1 Const + 1 Apply + 1 Var
    int64_t spent_cpu = INT64_MAX - budget.cpu;
    int64_t spent_mem = INT64_MAX - budget.mem;
    CHECK_EQ_I64(spent_cpu, kStartCpu + 4 * kStepCpu);
    CHECK_EQ_I64(spent_mem, kStartMem + 4 * kStepMem);

    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: `(force (delay (con integer 11)))` — StepForce + StepDelay on top
// of StepConst + startup.
// -----------------------------------------------------------------------
void test_force_delay() {
    uplc_arena* arena = uplc_arena_create();

    uplc_value consts[1];
    consts[0] = uplc_make_int_inline(11);

    // fn 1 (delayed body): CONST 0 ; RETURN
    uplc_bc_word body_ops[2];
    body_ops[0] = uplc_bc_mk(UPLC_BC_CONST,  0);
    body_ops[1] = uplc_bc_mk(UPLC_BC_RETURN, 0);

    // fn 0 (entry): MK_DELAY fn_id=1 nfree=0 ; FORCE ; RETURN
    uplc_bc_word entry_ops[4];
    entry_ops[0] = uplc_bc_mk(UPLC_BC_MK_DELAY, 1);
    entry_ops[1] = 0;
    entry_ops[2] = uplc_bc_mk(UPLC_BC_FORCE,    0);
    entry_ops[3] = uplc_bc_mk(UPLC_BC_RETURN,   0);

    uplc_bc_fn fns[2]{};
    fns[0].n_opcodes = 4;
    fns[0].opcodes   = entry_ops;
    fns[0].max_stack = 2;

    fns[1].n_opcodes = 2;
    fns[1].opcodes   = body_ops;
    fns[1].max_stack = 1;

    uplc_bc_program prog{};
    prog.functions     = fns;
    prog.n_functions   = 2;
    prog.consts        = consts;
    prog.n_consts      = 1;
    prog.version_major = 1;
    prog.version_minor = 1;
    prog.version_patch = 0;

    uplc_budget budget;
    uplcrt_budget_init_with_arena(&budget, INT64_MAX, INT64_MAX, arena);

    uplc_bc_result r = uplc_bc_run(arena, &prog, &budget);

    CHECK(r.ok == 1);
    CHECK(uplc_value_is_int_inline(r.value));
    if (uplc_value_is_int_inline(r.value)) {
        CHECK_EQ_I64(uplc_value_int_inline(r.value), 11);
    }

    // Budget: startup + Delay + Force + Const
    int64_t spent_cpu = INT64_MAX - budget.cpu;
    int64_t spent_mem = INT64_MAX - budget.mem;
    CHECK_EQ_I64(spent_cpu, kStartCpu + 3 * kStepCpu);
    CHECK_EQ_I64(spent_mem, kStartMem + 3 * kStepMem);

    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: `((lam x (lam y x)) 3 4)` — constant function picks x=3, ignores y.
// Exercises a nested lambda capturing an upval.
//   fn 0 (entry):
//       MK_LAM fn_id=1 nfree=0     -- outer
//       CONST 0                    -- push 3
//       APPLY                      -- apply outer to 3 → returns inner-closure capturing 3
//       CONST 1                    -- push 4
//       APPLY                      -- apply inner to 4 → returns 3
//       RETURN
//   fn 1 (outer body): lam x . lam y . x
//       MK_LAM fn_id=2 nfree=1 ; slot=0   -- capture current env[0]=x
//       RETURN
//   fn 2 (inner body): y → x    (x is upval 0, lives in env[1+0]=env[1])
//       VAR_UPVAL 1                -- upvals start at env[1] after arg
//       RETURN
// -----------------------------------------------------------------------
void test_const_lambda_capture() {
    uplc_arena* arena = uplc_arena_create();

    uplc_value consts[2];
    consts[0] = uplc_make_int_inline(3);
    consts[1] = uplc_make_int_inline(4);

    // fn 2 (innermost): VAR_UPVAL 1 ; RETURN
    uplc_bc_word fn2_ops[2];
    fn2_ops[0] = uplc_bc_mk(UPLC_BC_VAR_UPVAL, 1);
    fn2_ops[1] = uplc_bc_mk(UPLC_BC_RETURN,    0);

    // fn 1 (outer lam body): MK_LAM fn_id=2 nfree=1 slot=0 ; RETURN
    uplc_bc_word fn1_ops[4];
    fn1_ops[0] = uplc_bc_mk(UPLC_BC_MK_LAM, 2);
    fn1_ops[1] = 1;           // n_upvals
    fn1_ops[2] = 0;           // slot = env[0] (our arg)
    fn1_ops[3] = uplc_bc_mk(UPLC_BC_RETURN, 0);

    // fn 0 (entry): MK_LAM fn_id=1 nfree=0 ; CONST 0 ; APPLY ; CONST 1 ; APPLY ; RETURN
    uplc_bc_word entry_ops[7];
    entry_ops[0] = uplc_bc_mk(UPLC_BC_MK_LAM, 1);
    entry_ops[1] = 0;
    entry_ops[2] = uplc_bc_mk(UPLC_BC_CONST,  0);
    entry_ops[3] = uplc_bc_mk(UPLC_BC_APPLY,  0);
    entry_ops[4] = uplc_bc_mk(UPLC_BC_CONST,  1);
    entry_ops[5] = uplc_bc_mk(UPLC_BC_APPLY,  0);
    entry_ops[6] = uplc_bc_mk(UPLC_BC_RETURN, 0);

    uplc_bc_fn fns[3]{};
    fns[0].n_opcodes = 7; fns[0].opcodes = entry_ops; fns[0].max_stack = 8;
    fns[1].n_opcodes = 4; fns[1].opcodes = fn1_ops;   fns[1].n_args = 1; fns[1].max_stack = 4;
    fns[2].n_opcodes = 2; fns[2].opcodes = fn2_ops;   fns[2].n_args = 1; fns[2].n_upvals = 1; fns[2].max_stack = 1;

    uplc_bc_program prog{};
    prog.functions     = fns;
    prog.n_functions   = 3;
    prog.consts        = consts;
    prog.n_consts      = 2;
    prog.version_major = 1;
    prog.version_minor = 1;
    prog.version_patch = 0;

    uplc_budget budget;
    uplcrt_budget_init_with_arena(&budget, INT64_MAX, INT64_MAX, arena);

    uplc_bc_result r = uplc_bc_run(arena, &prog, &budget);

    CHECK(r.ok == 1);
    CHECK(uplc_value_is_int_inline(r.value));
    if (uplc_value_is_int_inline(r.value)) {
        CHECK_EQ_I64(uplc_value_int_inline(r.value), 3);
    }

    // Budget: startup + 2 Lambda + 2 Const + 2 Apply + 1 Var = 7 steps.
    int64_t spent_cpu = INT64_MAX - budget.cpu;
    int64_t spent_mem = INT64_MAX - budget.mem;
    CHECK_EQ_I64(spent_cpu, kStartCpu + 7 * kStepCpu);
    CHECK_EQ_I64(spent_mem, kStartMem + 7 * kStepMem);

    uplc_arena_destroy(arena);
}

}  // namespace

// -----------------------------------------------------------------------
// Test: build a 2-field VConstr then case-match it.
//   scrutinee   = (constr 0 (con integer 10) (con integer 20))
//   alts        = [ fn_0: lam a (lam b a), fn_1: error-stub ]
// Selecting alt 0 applies it to (10, 20) → returns 10.
// -----------------------------------------------------------------------
void test_constr_and_case() {
    uplc_arena* arena = uplc_arena_create();

    uplc_value consts[2];
    consts[0] = uplc_make_int_inline(10);
    consts[1] = uplc_make_int_inline(20);

    // fn 3 (inner: lam b a). n_upvals = 1 (captures a). env = [b, a].
    // Returns env[1] (= upval, the captured a).
    uplc_bc_word fn3_ops[2];
    fn3_ops[0] = uplc_bc_mk(UPLC_BC_VAR_UPVAL, 1);
    fn3_ops[1] = uplc_bc_mk(UPLC_BC_RETURN,    0);

    // fn 2 (outer alt-0 body: lam a (lam b a)). Takes a, returns MK_LAM wrapping fn 3 capturing a.
    uplc_bc_word fn2_ops[4];
    fn2_ops[0] = uplc_bc_mk(UPLC_BC_MK_LAM, 3);
    fn2_ops[1] = 1;   // n_upvals
    fn2_ops[2] = 0;   // capture env[0] = a
    fn2_ops[3] = uplc_bc_mk(UPLC_BC_RETURN, 0);

    // fn 1 (alt-0 "thunk" — evaluates the alt term to a VLam via MK_LAM fn 2).
    // Takes no arg; env is empty (no captures). Body: MK_LAM fn_2 nfree=0; RETURN.
    uplc_bc_word fn1_ops[3];
    fn1_ops[0] = uplc_bc_mk(UPLC_BC_MK_LAM, 2);
    fn1_ops[1] = 0;   // no upvals
    fn1_ops[2] = uplc_bc_mk(UPLC_BC_RETURN, 0);

    // fn 4 (alt-1 thunk — just an ERROR stub; never taken in this test).
    uplc_bc_word fn4_ops[1];
    fn4_ops[0] = uplc_bc_mk(UPLC_BC_ERROR, 0);

    // fn 0 (entry):
    //   CONST 0 ; CONST 1 ; CONSTR n_fields=2 tag=0
    //   CASE n_alts=2
    //     alt_0: fn_id=1, nfree=0
    //     alt_1: fn_id=4, nfree=0
    //   RETURN
    // CASE carries per-alt {fn_id, nfree, slots[nfree]} — both alts
    // here close over nothing, so no slot words follow their headers.
    uplc_bc_word entry_ops[11];
    entry_ops[0]  = uplc_bc_mk(UPLC_BC_CONST,  0);
    entry_ops[1]  = uplc_bc_mk(UPLC_BC_CONST,  1);
    entry_ops[2]  = uplc_bc_mk(UPLC_BC_CONSTR, 2);
    entry_ops[3]  = 0;                              // tag_lo
    entry_ops[4]  = 0;                              // tag_hi
    entry_ops[5]  = uplc_bc_mk(UPLC_BC_CASE,   2);
    entry_ops[6]  = 1;                              // alt 0 fn_id
    entry_ops[7]  = 0;                              // alt 0 nfree
    entry_ops[8]  = 4;                              // alt 1 fn_id
    entry_ops[9]  = 0;                              // alt 1 nfree
    entry_ops[10] = uplc_bc_mk(UPLC_BC_RETURN, 0);

    uplc_bc_fn fns[5]{};
    fns[0].n_opcodes = 11; fns[0].opcodes = entry_ops; fns[0].max_stack = 8;
    fns[1].n_opcodes = 3;  fns[1].opcodes = fn1_ops;   fns[1].n_args    = 0; fns[1].max_stack = 2;
    fns[2].n_opcodes = 4;  fns[2].opcodes = fn2_ops;   fns[2].n_args    = 1; fns[2].max_stack = 2;
    fns[3].n_opcodes = 2;  fns[3].opcodes = fn3_ops;   fns[3].n_args    = 1; fns[3].n_upvals  = 1; fns[3].max_stack = 1;
    fns[4].n_opcodes = 1;  fns[4].opcodes = fn4_ops;   fns[4].n_args    = 0; fns[4].max_stack = 0;

    uplc_bc_program prog{};
    prog.functions     = fns;
    prog.n_functions   = 5;
    prog.consts        = consts;
    prog.n_consts      = 2;
    prog.version_major = 1;
    prog.version_minor = 1;
    prog.version_patch = 0;

    uplc_budget budget;
    uplcrt_budget_init_with_arena(&budget, INT64_MAX, INT64_MAX, arena);

    uplc_bc_result r = uplc_bc_run(arena, &prog, &budget);

    CHECK(r.ok == 1);
    CHECK(uplc_value_is_int_inline(r.value));
    if (uplc_value_is_int_inline(r.value)) {
        CHECK_EQ_I64(uplc_value_int_inline(r.value), 10);
    }

    // Step accounting:
    //   entry: 2×CONST + CONSTR + CASE
    //   alt-thunk (fn1): MK_LAM (Lambda)
    //   field_0 apply (synthetic — no StepApply per TS CEK) enters fn2
    //     → runs MK_LAM (Lambda)
    //   field_1 apply (synthetic) enters fn3 → runs VAR_UPVAL (Var)
    // Total: 2 Const + 1 Constr + 1 Case + 2 Lambda + 1 Var = 7 steps.
    // (Case field applies do NOT charge StepApply; see ops_case.c.)
    int64_t spent_cpu = INT64_MAX - budget.cpu;
    int64_t spent_mem = INT64_MAX - budget.mem;
    CHECK_EQ_I64(spent_cpu, kStartCpu + 7 * kStepCpu);
    CHECK_EQ_I64(spent_mem, kStartMem + 7 * kStepMem);

    uplc_arena_destroy(arena);
}

int main() {
    test_const_integer();
    test_error_raises();
    test_identity_apply();
    test_force_delay();
    test_const_lambda_capture();
    test_constr_and_case();

    std::fprintf(stderr, "bc_vm_test: %d/%d checks passed\n",
                 g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
