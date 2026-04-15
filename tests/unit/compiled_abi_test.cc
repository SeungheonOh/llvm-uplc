// M2.5 compiled-mode ABI tests.
//
// Each test hand-writes a C function matching the uplc_lam_fn / uplc_delay_fn
// signatures that LLVM-generated code will emit in M7, then drives it through
// the runtime's compiled-mode ABI (uplcrt_apply, uplcrt_force, make_lam,
// make_constr, case_dispatch, run_compiled, ...). This validates the ABI in
// isolation, so when real codegen lands we only need to check that the IR
// matches the shapes tested here.

#include <cstdint>
#include <cstdio>

#include <gmp.h>

#include "runtime/arena.h"
#include "runtime/compiled/entry.h"
#include "runtime/value.h"
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
// Compiled constants share the uplc_rconstant byte layout (see
// runtime/compiled/consts.c). Mirror the first-level fields we need to
// inspect integer results without including rterm.h.
// -----------------------------------------------------------------------
struct const_layout_int {
    uint8_t tag;
    uint8_t _pad[7];
    mpz_ptr value;
};

int64_t read_int_const(uplc_value v) {
    /* Inline-int fast path: payload is the raw int64, not a pointer. */
    if (uplc_value_is_int_inline(v)) {
        return uplc_value_int_inline(v);
    }
    const_layout_int* c = reinterpret_cast<const_layout_int*>(v.payload);
    return mpz_get_si(c->value);
}

// -----------------------------------------------------------------------
// Test: a program that just returns a constant integer.
//
//   program 1.0.0 (con integer 42)
//
// In closure-converted form the top-level entry is a parameterless
// function that calls uplcrt_const_int_si and returns its value.
// -----------------------------------------------------------------------
uplc_value prog_const_42(uplc_budget* b) {
    return uplcrt_const_int_si(b, 42);
}

void test_compiled_const_int() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    auto r = uplcrt_run_compiled(prog_const_42, arena, &budget);
    CHECK(r.ok);
    CHECK_EQ(r.value.tag, UPLC_V_CON);
    CHECK_EQ(read_int_const(r.value), 42);
    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: the identity lambda (lam x x), unapplied.
//
// LLVM-generated code would emit:
//   uplc_value id_body(uplc_value* env, uplc_value arg, uplc_budget* b) {
//       return arg;
//   }
//   uplc_value entry(uplc_budget* b) {
//       return uplcrt_make_lam(b, (void*)id_body, nullptr, 0);
//   }
// -----------------------------------------------------------------------
uplc_value id_body(uplc_value* env, uplc_value arg, uplc_budget* b) {
    (void)env;
    (void)b;
    return arg;
}

uplc_value prog_identity(uplc_budget* b) {
    return uplcrt_make_lam(b, reinterpret_cast<void*>(id_body), nullptr, 0);
}

void test_compiled_identity_unapplied() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    auto r = uplcrt_run_compiled(prog_identity, arena, &budget);
    CHECK(r.ok);
    CHECK_EQ(r.value.tag, UPLC_V_LAM);
    CHECK_EQ(r.value.subtag, UPLC_VLAM_COMPILED);
    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: identity applied — [(lam x x) (con integer 7)].
// -----------------------------------------------------------------------
uplc_value prog_identity_applied(uplc_budget* b) {
    uplc_value id = uplcrt_make_lam(b, reinterpret_cast<void*>(id_body), nullptr, 0);
    uplc_value seven = uplcrt_const_int_si(b, 7);
    return uplcrt_apply(id, seven, b);
}

void test_compiled_identity_applied() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    auto r = uplcrt_run_compiled(prog_identity_applied, arena, &budget);
    CHECK(r.ok);
    CHECK_EQ(r.value.tag, UPLC_V_CON);
    CHECK_EQ(read_int_const(r.value), 7);
    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: K combinator applied to two arguments — [[(lam x (lam y x)) 1] 2].
//
// The outer lambda captures x as its single free var and returns the
// inner lambda closure. Inner body reads env[0] (= x).
// -----------------------------------------------------------------------
uplc_value k_inner_body(uplc_value* env, uplc_value arg, uplc_budget* b) {
    (void)arg;
    (void)b;
    return env[0];  // captured x
}

uplc_value k_outer_body(uplc_value* env, uplc_value arg, uplc_budget* b) {
    (void)env;
    uplc_value free_vars[1] = {arg};  // capture x
    return uplcrt_make_lam(b, reinterpret_cast<void*>(k_inner_body), free_vars, 1);
}

uplc_value prog_k_applied(uplc_budget* b) {
    uplc_value k = uplcrt_make_lam(b, reinterpret_cast<void*>(k_outer_body), nullptr, 0);
    uplc_value one = uplcrt_const_int_si(b, 1);
    uplc_value two = uplcrt_const_int_si(b, 2);
    uplc_value partial = uplcrt_apply(k, one, b);
    return uplcrt_apply(partial, two, b);
}

void test_compiled_k_combinator() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    auto r = uplcrt_run_compiled(prog_k_applied, arena, &budget);
    CHECK(r.ok);
    CHECK_EQ(read_int_const(r.value), 1);
    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: delay / force of a constant — (force (delay (con integer 99))).
//
// Delay bodies take no argument: their signature is
//   uplc_value delay_body(uplc_value* env, uplc_budget* b);
// The closure carries any captured free vars in env[] just like a lambda.
// -----------------------------------------------------------------------
uplc_value delay99_body(uplc_value* env, uplc_budget* b) {
    (void)env;
    return uplcrt_const_int_si(b, 99);
}

uplc_value prog_delay_force(uplc_budget* b) {
    uplc_value d = uplcrt_make_delay(b, reinterpret_cast<void*>(delay99_body),
                                     nullptr, 0);
    return uplcrt_force(d, b);
}

void test_compiled_delay_force() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    auto r = uplcrt_run_compiled(prog_delay_force, arena, &budget);
    CHECK(r.ok);
    CHECK_EQ(read_int_const(r.value), 99);
    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: constr + case — case (constr 1 42) of { _ -> 100 } { x -> x }
//
// alts[0] = const lambda returning 100
// alts[1] = identity lambda (reuses id_body)
// -----------------------------------------------------------------------
uplc_value const100_body(uplc_value* env, uplc_value arg, uplc_budget* b) {
    (void)env;
    (void)arg;
    return uplcrt_const_int_si(b, 100);
}

uplc_value prog_case_select(uplc_budget* b) {
    uplc_value forty_two = uplcrt_const_int_si(b, 42);
    uplc_value fields[] = {forty_two};
    uplc_value c = uplcrt_make_constr(b, /*tag=*/1, fields, 1);

    uplc_value alts[] = {
        uplcrt_make_lam(b, reinterpret_cast<void*>(const100_body), nullptr, 0),
        uplcrt_make_lam(b, reinterpret_cast<void*>(id_body), nullptr, 0),
    };
    return uplcrt_case_dispatch(c, alts, 2, b);
}

void test_compiled_case_dispatch() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    auto r = uplcrt_run_compiled(prog_case_select, arena, &budget);
    CHECK(r.ok);
    CHECK_EQ(read_int_const(r.value), 42);
    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: partial builtin application stays a VBuiltin (no dispatch yet).
// -----------------------------------------------------------------------
uplc_value prog_partial_builtin(uplc_budget* b) {
    uplc_value builtin = uplcrt_make_builtin(b, /*addInteger=*/0);
    return uplcrt_apply(builtin, uplcrt_const_int_si(b, 5), b);
}

void test_compiled_partial_builtin() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    auto r = uplcrt_run_compiled(prog_partial_builtin, arena, &budget);
    CHECK(r.ok);
    CHECK_EQ(r.value.tag, UPLC_V_BUILTIN);
    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: uplcrt_fail from inside a program is caught by run_compiled.
// -----------------------------------------------------------------------
uplc_value prog_fails(uplc_budget* b) {
    uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    return uplcrt_const_int_si(b, 0);  // unreachable
}

void test_compiled_error_trampoline() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    auto r = uplcrt_run_compiled(prog_fails, arena, &budget);
    CHECK(!r.ok);
    CHECK_EQ((int)r.fail_kind, (int)UPLC_FAIL_EVALUATION);
    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: applying a non-lambda value raises EVALUATION.
// -----------------------------------------------------------------------
uplc_value prog_apply_const(uplc_budget* b) {
    uplc_value c = uplcrt_const_int_si(b, 1);
    uplc_value a = uplcrt_const_int_si(b, 2);
    return uplcrt_apply(c, a, b);  // should fail
}

void test_compiled_apply_non_function() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    auto r = uplcrt_run_compiled(prog_apply_const, arena, &budget);
    CHECK(!r.ok);
    CHECK_EQ((int)r.fail_kind, (int)UPLC_FAIL_EVALUATION);
    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: case with an out-of-range tag fails.
// -----------------------------------------------------------------------
uplc_value prog_case_oob(uplc_budget* b) {
    uplc_value c = uplcrt_make_constr(b, /*tag=*/5, nullptr, 0);
    uplc_value alts[] = {
        uplcrt_make_lam(b, reinterpret_cast<void*>(id_body), nullptr, 0),
    };
    return uplcrt_case_dispatch(c, alts, 1, b);
}

void test_compiled_case_tag_oob() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    auto r = uplcrt_run_compiled(prog_case_oob, arena, &budget);
    CHECK(!r.ok);
    CHECK_EQ((int)r.fail_kind, (int)UPLC_FAIL_EVALUATION);
    uplc_arena_destroy(arena);
}

// -----------------------------------------------------------------------
// Test: budget runs out mid-program → OUT_OF_BUDGET via terminal flush.
// -----------------------------------------------------------------------
uplc_value prog_heavy(uplc_budget* b) {
    // Charge a bunch of steps so the cpu counter goes negative.
    for (int i = 0; i < 10; ++i) {
        uplcrt_budget_step(b, UPLC_STEP_CONST);
    }
    return uplcrt_const_int_si(b, 0);
}

void test_compiled_out_of_budget() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, /*cpu=*/1000, /*mem=*/1000);
    auto r = uplcrt_run_compiled(prog_heavy, arena, &budget);
    CHECK(!r.ok);
    CHECK_EQ((int)r.fail_kind, (int)UPLC_FAIL_OUT_OF_BUDGET);
    uplc_arena_destroy(arena);
}

}  // namespace

int main() {
    test_compiled_const_int();
    test_compiled_identity_unapplied();
    test_compiled_identity_applied();
    test_compiled_k_combinator();
    test_compiled_delay_force();
    test_compiled_case_dispatch();
    test_compiled_partial_builtin();
    test_compiled_error_trampoline();
    test_compiled_apply_non_function();
    test_compiled_case_tag_oob();
    test_compiled_out_of_budget();

    std::fprintf(stderr, "%d/%d passed\n", g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
