// M2 CEK interpreter unit tests.
//
// Each test parses a small UPLC text program with the compiler frontend,
// converts to de Bruijn, lowers to runtime rterm, runs cek_run, and
// inspects the result's tag/payload. No builtin dispatch is required at
// M2 — programs either avoid builtins or only mention them without
// saturating.
//
// The harness is hand-rolled (no gtest yet), matching frontend_test.cc.

#include <cstdint>
#include <cstdio>
#include <string>

#include <gmp.h>

#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/lowering.h"

#include "runtime/core/arena.h"
#include "runtime/cek/cek.h"
#include "runtime/cek/closure.h"
#include "runtime/cek/env.h"
#include "runtime/core/rterm.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

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

#define CHECK_EQ(actual, expected)                                       \
    do {                                                                 \
        ++g_total;                                                       \
        auto a_ = (actual);                                               \
        auto e_ = (expected);                                             \
        if (a_ != e_) {                                                  \
            ++g_failures;                                                \
            std::fprintf(stderr,                                         \
                         "FAIL %s:%d: got %lld, want %lld\n",            \
                         __FILE__, __LINE__,                             \
                         (long long)a_, (long long)e_);                  \
        }                                                                \
    } while (0)

// -----------------------------------------------------------------------
// Harness: parse a text program and run it through the CEK interpreter.
// -----------------------------------------------------------------------
struct RunResult {
    uplc_cek_result result;
    uplc_arena*     arena;  // kept alive so the value pointers stay valid
};

RunResult run_text(const char* src, int64_t cpu = INT64_MAX, int64_t mem = INT64_MAX) {
    uplc::Arena compiler_arena;
    uplc::Program named = uplc::parse_program(compiler_arena, src);
    uplc::Program db    = uplc::name_to_debruijn(compiler_arena, named);

    uplc_arena* a = uplc_arena_create();
    uplc_rprogram rprog = uplc::lower_to_runtime(a, db);

    uplc_budget budget;
    uplcrt_budget_init(&budget, cpu, mem);

    uplc_cek_result r = uplc_cek_run(a, rprog.term, &budget);
    RunResult out{r, a};
    return out;
}

// Materialize a V_CON into an rconstant regardless of whether the value
// happens to be an inline-int fast-path value. Inline ints get promoted
// into a fresh rconstant in the test's arena.
uplc_rconstant* rcon_of(uplc_arena* arena, uplc_value v) {
    return uplc_rcon_of_materialize(arena, v);
}

// -----------------------------------------------------------------------
// Individual tests
// -----------------------------------------------------------------------

void test_const_integer() {
    auto r = run_text("(program 1.0.0 (con integer 42))");
    CHECK(r.result.ok);
    CHECK_EQ(r.result.value.tag, UPLC_V_CON);
    auto* c = rcon_of(r.arena, r.result.value);
    CHECK_EQ(c->tag, UPLC_RCONST_INTEGER);
    CHECK_EQ(mpz_get_si(c->integer.value), 42);
    uplc_arena_destroy(r.arena);
}

void test_const_negative_integer() {
    auto r = run_text("(program 1.0.0 (con integer -7))");
    CHECK(r.result.ok);
    auto* c = rcon_of(r.arena, r.result.value);
    CHECK_EQ(mpz_get_si(c->integer.value), -7);
    uplc_arena_destroy(r.arena);
}

void test_const_bool_and_unit() {
    {
        auto r = run_text("(program 1.0.0 (con bool True))");
        CHECK(r.result.ok);
        auto* c = rcon_of(r.arena, r.result.value);
        CHECK_EQ((int)c->tag, (int)UPLC_RCONST_BOOL);
        CHECK(c->boolean.value == true);
        uplc_arena_destroy(r.arena);
    }
    {
        auto r = run_text("(program 1.0.0 (con unit ()))");
        CHECK(r.result.ok);
        auto* c = rcon_of(r.arena, r.result.value);
        CHECK_EQ((int)c->tag, (int)UPLC_RCONST_UNIT);
        uplc_arena_destroy(r.arena);
    }
}

void test_lambda_value() {
    // An unapplied lambda evaluates to a VLam.
    auto r = run_text("(program 1.0.0 (lam x x))");
    CHECK(r.result.ok);
    CHECK_EQ(r.result.value.tag, UPLC_V_LAM);
    CHECK_EQ(r.result.value.subtag, UPLC_VLAM_INTERP);
    uplc_arena_destroy(r.arena);
}

void test_identity_application() {
    // [(lam x x) (con integer 42)] -> 42
    auto r = run_text("(program 1.0.0 [ (lam x x) (con integer 42) ])");
    CHECK(r.result.ok);
    CHECK_EQ(r.result.value.tag, UPLC_V_CON);
    auto* c = rcon_of(r.arena, r.result.value);
    CHECK_EQ(mpz_get_si(c->integer.value), 42);
    uplc_arena_destroy(r.arena);
}

void test_const_under_lambda() {
    // (lam x (con integer 7)) (con integer 100) -> 7 (argument discarded)
    auto r = run_text(
        "(program 1.0.0 [ (lam x (con integer 7)) (con integer 100) ])");
    CHECK(r.result.ok);
    auto* c = rcon_of(r.arena, r.result.value);
    CHECK_EQ(mpz_get_si(c->integer.value), 7);
    uplc_arena_destroy(r.arena);
}

void test_two_arg_function() {
    // [ [ (lam x (lam y x)) 1 ] 2 ] -> 1   (K combinator)
    auto r = run_text(
        "(program 1.0.0 [ [ (lam x (lam y x)) (con integer 1) ] (con integer 2) ])");
    CHECK(r.result.ok);
    auto* c = rcon_of(r.arena, r.result.value);
    CHECK_EQ(mpz_get_si(c->integer.value), 1);
    uplc_arena_destroy(r.arena);
}

void test_two_arg_function_second() {
    // [ [ (lam x (lam y y)) 1 ] 2 ] -> 2
    auto r = run_text(
        "(program 1.0.0 [ [ (lam x (lam y y)) (con integer 1) ] (con integer 2) ])");
    CHECK(r.result.ok);
    auto* c = rcon_of(r.arena, r.result.value);
    CHECK_EQ(mpz_get_si(c->integer.value), 2);
    uplc_arena_destroy(r.arena);
}

void test_delay_force() {
    auto r = run_text(
        "(program 1.0.0 (force (delay (con integer 99))))");
    CHECK(r.result.ok);
    auto* c = rcon_of(r.arena, r.result.value);
    CHECK_EQ(mpz_get_si(c->integer.value), 99);
    uplc_arena_destroy(r.arena);
}

void test_unapplied_delay() {
    // Unforced delay stays as a VDelay.
    auto r = run_text("(program 1.0.0 (delay (con integer 5)))");
    CHECK(r.result.ok);
    CHECK_EQ(r.result.value.tag, UPLC_V_DELAY);
    uplc_arena_destroy(r.arena);
}

void test_error_propagates() {
    auto r = run_text("(program 1.0.0 (error))");
    CHECK(!r.result.ok);
    CHECK_EQ((int)r.result.fail_kind, (int)UPLC_FAIL_EVALUATION);
    uplc_arena_destroy(r.arena);
}

void test_error_under_apply() {
    // Applying a function whose body is (error) should fail.
    auto r = run_text(
        "(program 1.0.0 [ (lam x (error)) (con integer 1) ])");
    CHECK(!r.result.ok);
    CHECK_EQ((int)r.result.fail_kind, (int)UPLC_FAIL_EVALUATION);
    uplc_arena_destroy(r.arena);
}

void test_builtin_unapplied_is_vbuiltin() {
    auto r = run_text("(program 1.0.0 (builtin addInteger))");
    CHECK(r.result.ok);
    CHECK_EQ(r.result.value.tag, UPLC_V_BUILTIN);
    uplc_arena_destroy(r.arena);
}

void test_builtin_partial_app_does_not_saturate() {
    // Partially applying addInteger: the argument's evaluation itself
    // succeeds, and dispatch_apply returns an updated VBuiltin without
    // saturating (M2 has no builtin impls). We only reach saturation on
    // the *second* argument.
    auto r = run_text(
        "(program 1.0.0 [ (builtin addInteger) (con integer 1) ])");
    CHECK(r.result.ok);
    CHECK_EQ(r.result.value.tag, UPLC_V_BUILTIN);
    uplc_arena_destroy(r.arena);
}

void test_constr_zero_fields() {
    auto r = run_text("(program 1.1.0 (constr 3))");
    CHECK(r.result.ok);
    CHECK_EQ(r.result.value.tag, UPLC_V_CONSTR);
    auto* cp = uplc_constr_of(r.result.value);
    CHECK_EQ(uplc_constr_tag(cp), (uint64_t)3);
    CHECK_EQ(uplc_constr_arity(cp), (uint32_t)0);
    uplc_arena_destroy(r.arena);
}

void test_constr_multi_field() {
    auto r = run_text(
        "(program 1.1.0 (constr 7 (con integer 1) (con integer 2) (con integer 3)))");
    CHECK(r.result.ok);
    auto* cp = uplc_constr_of(r.result.value);
    CHECK_EQ(uplc_constr_tag(cp), (uint64_t)7);
    CHECK_EQ(uplc_constr_arity(cp), (uint32_t)3);
    for (uint32_t i = 0; i < 3; ++i) {
        uplc_value f = uplc_constr_field(cp, i);
        CHECK_EQ(f.tag, UPLC_V_CON);
        auto* c = rcon_of(r.arena, f);
        CHECK_EQ(mpz_get_si(c->integer.value), (long)(i + 1));
    }
    uplc_arena_destroy(r.arena);
}

void test_case_selects_branch() {
    // case (constr 1 (con integer 42))
    //   (lam x (con integer 100))      -- branch 0: returns 100
    //   (lam x x)                       -- branch 1: returns the field (42)
    auto r = run_text(
        "(program 1.1.0 "
        "(case (constr 1 (con integer 42)) "
        "  (lam x (con integer 100)) "
        "  (lam x x)))");
    CHECK(r.result.ok);
    CHECK_EQ(r.result.value.tag, UPLC_V_CON);
    auto* c = rcon_of(r.arena, r.result.value);
    CHECK_EQ(mpz_get_si(c->integer.value), 42);
    uplc_arena_destroy(r.arena);
}

void test_case_multi_field_apply() {
    // case (constr 0 (con integer 1) (con integer 2) (con integer 3))
    //   (lam a (lam b (lam c a)))   -- returns the first field
    auto r = run_text(
        "(program 1.1.0 "
        "(case (constr 0 (con integer 1) (con integer 2) (con integer 3)) "
        "  (lam a (lam b (lam c a)))))");
    CHECK(r.result.ok);
    auto* c = rcon_of(r.arena, r.result.value);
    CHECK_EQ(mpz_get_si(c->integer.value), 1);
    uplc_arena_destroy(r.arena);
}

void test_out_of_budget() {
    // Tiny budget — even a single step overflows, triggering OutOfBudget.
    auto r = run_text("(program 1.0.0 (con integer 42))", /*cpu=*/100, /*mem=*/100);
    CHECK(!r.result.ok);
    CHECK_EQ((int)r.result.fail_kind, (int)UPLC_FAIL_OUT_OF_BUDGET);
    uplc_arena_destroy(r.arena);
}

void test_budget_counts_exact() {
    // Program is a single `(con integer 42)`. Budget charges:
    //   startup  (100 cpu,  100 mem)   -- one-off
    //   Const    (16000 cpu, 100 mem)  -- from the compute step
    // Total: 16100 cpu / 200 mem. Matches TS CekMachine.
    uplc::Arena compiler_arena;
    uplc::Program named = uplc::parse_program(compiler_arena,
                                              "(program 1.0.0 (con integer 42))");
    uplc::Program db = uplc::name_to_debruijn(compiler_arena, named);
    uplc_arena* a = uplc_arena_create();
    uplc_rprogram rprog = uplc::lower_to_runtime(a, db);

    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    uplc_cek_result r = uplc_cek_run(a, rprog.term, &budget);
    CHECK(r.ok);
    CHECK_EQ(INT64_MAX - budget.cpu, 16100);
    CHECK_EQ(INT64_MAX - budget.mem, 200);
    uplc_arena_destroy(a);
}

void test_budget_counts_apply_chain() {
    // [ (lam x x) (con integer 42) ]
    // startup (100, 100) + 4 compute steps (Apply, Lambda, Const, Var),
    // each 16000/100 = (64000, 400). Grand total: 64100 cpu / 500 mem.
    uplc::Arena compiler_arena;
    uplc::Program named = uplc::parse_program(
        compiler_arena,
        "(program 1.0.0 [ (lam x x) (con integer 42) ])");
    uplc::Program db = uplc::name_to_debruijn(compiler_arena, named);
    uplc_arena* a = uplc_arena_create();
    uplc_rprogram rprog = uplc::lower_to_runtime(a, db);

    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    uplc_cek_result r = uplc_cek_run(a, rprog.term, &budget);
    CHECK(r.ok);
    CHECK_EQ(INT64_MAX - budget.cpu, 64100);
    CHECK_EQ(INT64_MAX - budget.mem, 500);
    uplc_arena_destroy(a);
}

void test_saturating_arith() {
    CHECK_EQ(uplcrt_sat_add_i64(1, 2), 3);
    CHECK_EQ(uplcrt_sat_add_i64(INT64_MAX, 1), INT64_MAX);
    CHECK_EQ(uplcrt_sat_add_i64(INT64_MIN, -1), INT64_MIN);
    CHECK_EQ(uplcrt_sat_mul_i64(3, 4), 12);
    CHECK_EQ(uplcrt_sat_mul_i64(INT64_MAX, 2), INT64_MAX);
    CHECK_EQ(uplcrt_sat_mul_i64(INT64_MIN, 2), INT64_MIN);
    CHECK_EQ(uplcrt_sat_mul_i64(INT64_MAX, -2), INT64_MIN);
}

}  // namespace

int main() {
    test_const_integer();
    test_const_negative_integer();
    test_const_bool_and_unit();
    test_lambda_value();
    test_identity_application();
    test_const_under_lambda();
    test_two_arg_function();
    test_two_arg_function_second();
    test_delay_force();
    test_unapplied_delay();
    test_error_propagates();
    test_error_under_apply();
    test_builtin_unapplied_is_vbuiltin();
    test_builtin_partial_app_does_not_saturate();
    test_constr_zero_fields();
    test_constr_multi_field();
    test_case_selects_branch();
    test_case_multi_field_apply();
    test_out_of_budget();
    test_budget_counts_exact();
    test_budget_counts_apply_chain();
    test_saturating_arith();

    std::fprintf(stderr, "%d/%d passed\n", g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
