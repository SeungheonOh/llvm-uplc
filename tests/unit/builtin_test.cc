// M3a builtin end-to-end tests.
//
// Runs a selection of UPLC programs that exercise the implemented
// builtins (arith / control / list / pair / bytestring) through both
// execution paths:
//
//   CEK path     : parse → convert → lower → uplc_cek_run
//   compiled path: hand-written C "entry" that mimics what LLVM-generated
//                  code will emit, driven by uplcrt_run_compiled
//
// Both modes dispatch through the shared runtime/builtins/ implementations
// and runtime/builtin_dispatch.c so the results must be bit-identical.

#include <cstdint>
#include <cstdio>
#include <string>

#include <gmp.h>

#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/lowering.h"

#include "runtime/arena.h"
#include "runtime/cek/cek.h"
#include "runtime/cek/rterm.h"
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

// --- Shared helpers -------------------------------------------------------

struct CekResult {
    uplc_cek_result r;
    uplc_arena*     arena;
};

CekResult run_cek(const char* src) {
    uplc::Arena ca;
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc_arena* a = uplc_arena_create();
    uplc_rprogram p = uplc::lower_to_runtime(a, db);
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);
    uplc_cek_result r = uplc_cek_run(a, p.term, &budget);
    return {r, a};
}

int64_t cek_int(uplc_value v) {
    if (uplc_value_is_int_inline(v)) {
        return uplc_value_int_inline(v);
    }
    auto* c = (const uplc_rconstant*)uplc_value_payload(v);
    return mpz_get_si(c->integer.value);
}

bool cek_bool(uplc_value v) {
    auto* c = (const uplc_rconstant*)uplc_value_payload(v);
    return c->boolean.value;
}

// Common compiled-entry budget init used throughout the tests.
void init_budget(uplc_budget& b) {
    uplcrt_budget_init(&b, INT64_MAX, INT64_MAX);
}

// --- CEK-only tests -------------------------------------------------------

void test_cek_add_integer() {
    auto r = run_cek("(program 1.0.0 "
                     "[ [ (builtin addInteger) (con integer 2) ] (con integer 3) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 5);
    uplc_arena_destroy(r.arena);
}

void test_cek_subtract_integer() {
    auto r = run_cek("(program 1.0.0 "
                     "[ [ (builtin subtractInteger) (con integer 10) ] (con integer 3) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 7);
    uplc_arena_destroy(r.arena);
}

void test_cek_multiply_integer() {
    auto r = run_cek("(program 1.0.0 "
                     "[ [ (builtin multiplyInteger) (con integer 6) ] (con integer 7) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 42);
    uplc_arena_destroy(r.arena);
}

void test_cek_divide_integer_floor() {
    // divideInteger uses floor division: (-7) / 2 == -4
    auto r = run_cek("(program 1.0.0 "
                     "[ [ (builtin divideInteger) (con integer -7) ] (con integer 2) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), -4);
    uplc_arena_destroy(r.arena);
}

void test_cek_quotient_integer_trunc() {
    // quotientInteger uses truncated division: (-7) / 2 == -3
    auto r = run_cek("(program 1.0.0 "
                     "[ [ (builtin quotientInteger) (con integer -7) ] (con integer 2) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), -3);
    uplc_arena_destroy(r.arena);
}

void test_cek_mod_integer() {
    // modInteger uses Euclidean remainder: (-7) mod 3 == 2
    auto r = run_cek("(program 1.0.0 "
                     "[ [ (builtin modInteger) (con integer -7) ] (con integer 3) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 2);
    uplc_arena_destroy(r.arena);
}

void test_cek_remainder_integer() {
    // remainderInteger uses truncated remainder: (-7) rem 3 == -1
    auto r = run_cek("(program 1.0.0 "
                     "[ [ (builtin remainderInteger) (con integer -7) ] (con integer 3) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), -1);
    uplc_arena_destroy(r.arena);
}

void test_cek_equals_less_integer() {
    auto eq = run_cek("(program 1.0.0 "
                      "[ [ (builtin equalsInteger) (con integer 5) ] (con integer 5) ])");
    CHECK(eq.r.ok);
    CHECK(cek_bool(eq.r.value));
    uplc_arena_destroy(eq.arena);

    auto lt = run_cek("(program 1.0.0 "
                      "[ [ (builtin lessThanInteger) (con integer 2) ] (con integer 3) ])");
    CHECK(lt.r.ok);
    CHECK(cek_bool(lt.r.value));
    uplc_arena_destroy(lt.arena);

    auto le = run_cek("(program 1.0.0 "
                      "[ [ (builtin lessThanEqualsInteger) (con integer 3) ] (con integer 3) ])");
    CHECK(le.r.ok);
    CHECK(cek_bool(le.r.value));
    uplc_arena_destroy(le.arena);
}

void test_cek_divide_by_zero() {
    auto r = run_cek("(program 1.0.0 "
                     "[ [ (builtin divideInteger) (con integer 5) ] (con integer 0) ])");
    CHECK(!r.r.ok);
    CHECK_EQ((int)r.r.fail_kind, (int)UPLC_FAIL_EVALUATION);
    uplc_arena_destroy(r.arena);
}

void test_cek_if_then_else_true() {
    // (ifThenElse True 1 2)  ==  1
    // ifThenElse has one force (instantiating the result type).
    auto r = run_cek(
        "(program 1.0.0 "
        " [ [ [ (force (builtin ifThenElse)) (con bool True) ]"
        "     (con integer 1) ]"
        "   (con integer 2) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 1);
    uplc_arena_destroy(r.arena);
}

void test_cek_if_then_else_false() {
    auto r = run_cek(
        "(program 1.0.0 "
        " [ [ [ (force (builtin ifThenElse)) (con bool False) ]"
        "     (con integer 1) ]"
        "   (con integer 2) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 2);
    uplc_arena_destroy(r.arena);
}

void test_cek_bytestring_length_append() {
    auto len = run_cek(
        "(program 1.0.0 [ (builtin lengthOfByteString) (con bytestring #cafe) ])");
    CHECK(len.r.ok);
    CHECK_EQ(cek_int(len.r.value), 2);
    uplc_arena_destroy(len.arena);

    auto app = run_cek(
        "(program 1.0.0 "
        " [ (builtin lengthOfByteString)"
        "   [ [ (builtin appendByteString) (con bytestring #ab) ] (con bytestring #cdef) ] ])");
    CHECK(app.r.ok);
    CHECK_EQ(cek_int(app.r.value), 3);
    uplc_arena_destroy(app.arena);
}

void test_cek_bytestring_index_equals() {
    auto idx = run_cek(
        "(program 1.0.0 "
        " [ [ (builtin indexByteString) (con bytestring #0af1) ] (con integer 1) ])");
    CHECK(idx.r.ok);
    CHECK_EQ(cek_int(idx.r.value), 0xf1);
    uplc_arena_destroy(idx.arena);

    auto eq = run_cek(
        "(program 1.0.0 "
        " [ [ (builtin equalsByteString) (con bytestring #dead) ] (con bytestring #dead) ])");
    CHECK(eq.r.ok);
    CHECK(cek_bool(eq.r.value));
    uplc_arena_destroy(eq.arena);

    auto neq = run_cek(
        "(program 1.0.0 "
        " [ [ (builtin equalsByteString) (con bytestring #dead) ] (con bytestring #beef) ])");
    CHECK(neq.r.ok);
    CHECK(!cek_bool(neq.r.value));
    uplc_arena_destroy(neq.arena);
}

void test_cek_list_head_tail_null() {
    auto head = run_cek(
        "(program 1.0.0 "
        " [ (force (builtin headList)) (con (list integer) [10, 20, 30]) ])");
    CHECK(head.r.ok);
    CHECK_EQ(cek_int(head.r.value), 10);
    uplc_arena_destroy(head.arena);

    auto null_empty = run_cek(
        "(program 1.0.0 "
        " [ (force (builtin nullList)) (con (list integer) []) ])");
    CHECK(null_empty.r.ok);
    CHECK(cek_bool(null_empty.r.value));
    uplc_arena_destroy(null_empty.arena);

    auto null_nonempty = run_cek(
        "(program 1.0.0 "
        " [ (force (builtin nullList)) (con (list integer) [1]) ])");
    CHECK(null_nonempty.r.ok);
    CHECK(!cek_bool(null_nonempty.r.value));
    uplc_arena_destroy(null_nonempty.arena);

    // headList on (tailList [1,2,3]) == 2
    auto tail_head = run_cek(
        "(program 1.0.0 "
        " [ (force (builtin headList))"
        "   [ (force (builtin tailList)) (con (list integer) [1, 2, 3]) ] ])");
    CHECK(tail_head.r.ok);
    CHECK_EQ(cek_int(tail_head.r.value), 2);
    uplc_arena_destroy(tail_head.arena);
}

void test_cek_pair_projection() {
    // fstPair takes 2 forces (one per type variable) and one value argument.
    auto fst = run_cek(
        "(program 1.0.0 "
        " [ (force (force (builtin fstPair))) "
        "   (con (pair integer bool) (7, True)) ])");
    CHECK(fst.r.ok);
    CHECK_EQ(cek_int(fst.r.value), 7);
    uplc_arena_destroy(fst.arena);

    auto snd = run_cek(
        "(program 1.0.0 "
        " [ (force (force (builtin sndPair))) "
        "   (con (pair integer bool) (7, True)) ])");
    CHECK(snd.r.ok);
    CHECK(cek_bool(snd.r.value));
    uplc_arena_destroy(snd.arena);
}

void test_cek_nested_arith_under_if() {
    // if (1 < 2) then 10 + 20 else 0
    auto r = run_cek(
        "(program 1.0.0 "
        " [ [ [ (force (builtin ifThenElse))"
        "       [ [ (builtin lessThanInteger) (con integer 1) ] (con integer 2) ] ]"
        "     [ [ (builtin addInteger) (con integer 10) ] (con integer 20) ] ]"
        "   (con integer 0) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 30);
    uplc_arena_destroy(r.arena);
}

// --- M3b: data / string / array / bitwise / intbs tests -----------------

void test_cek_i_data_round_trip() {
    auto r = run_cek(
        "(program 1.0.0 "
        " [ (builtin unIData) [ (builtin iData) (con integer 42) ] ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 42);
    uplc_arena_destroy(r.arena);
}

void test_cek_b_data_round_trip() {
    auto r = run_cek(
        "(program 1.0.0 "
        " [ (builtin lengthOfByteString)"
        "   [ (builtin unBData) [ (builtin bData) (con bytestring #deadbeef) ] ] ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 4);
    uplc_arena_destroy(r.arena);
}

void test_cek_equals_data_true() {
    auto r = run_cek(
        "(program 1.0.0 "
        " [ [ (builtin equalsData) (con data (I 7)) ] (con data (I 7)) ])");
    CHECK(r.r.ok);
    CHECK(cek_bool(r.r.value));
    uplc_arena_destroy(r.arena);
}

void test_cek_equals_data_constr_deep() {
    auto r = run_cek(
        "(program 1.0.0 "
        " [ [ (builtin equalsData) (con data (Constr 3 [I 1, B #ab])) ]"
        "   (con data (Constr 3 [I 1, B #ab])) ])");
    CHECK(r.r.ok);
    CHECK(cek_bool(r.r.value));
    uplc_arena_destroy(r.arena);
}

void test_cek_choose_data_dispatch() {
    // chooseData on (I 5) -> returns the "integer" branch (args[4]).
    auto r = run_cek(
        "(program 1.0.0 "
        " [ [ [ [ [ [ (force (builtin chooseData)) (con data (I 5)) ]"
        "             (con integer 1) ]"
        "           (con integer 2) ]"
        "         (con integer 3) ]"
        "       (con integer 4) ]"
        "     (con integer 5) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 4);
    uplc_arena_destroy(r.arena);
}

void test_cek_string_append_equals() {
    auto r = run_cek(
        "(program 1.0.0 "
        " [ [ (builtin equalsString)"
        "     [ [ (builtin appendString) (con string \"hello \") ] (con string \"world\") ] ]"
        "   (con string \"hello world\") ])");
    CHECK(r.r.ok);
    CHECK(cek_bool(r.r.value));
    uplc_arena_destroy(r.arena);
}

void test_cek_encode_decode_utf8() {
    auto r = run_cek(
        "(program 1.0.0 "
        " [ [ (builtin equalsString)"
        "     [ (builtin decodeUtf8) [ (builtin encodeUtf8) (con string \"hi\") ] ] ]"
        "   (con string \"hi\") ])");
    CHECK(r.r.ok);
    CHECK(cek_bool(r.r.value));
    uplc_arena_destroy(r.arena);
}

void test_cek_array_length_index() {
    auto len = run_cek(
        "(program 1.0.0 "
        " [ (force (builtin lengthOfArray)) (con (array integer) [10, 20, 30]) ])");
    CHECK(len.r.ok);
    CHECK_EQ(cek_int(len.r.value), 3);
    uplc_arena_destroy(len.arena);

    auto idx = run_cek(
        "(program 1.0.0 "
        " [ [ (force (builtin indexArray)) (con (array integer) [10, 20, 30]) ]"
        "   (con integer 1) ])");
    CHECK(idx.r.ok);
    CHECK_EQ(cek_int(idx.r.value), 20);
    uplc_arena_destroy(idx.arena);
}

void test_cek_drop_list() {
    auto head = run_cek(
        "(program 1.0.0 "
        " [ (force (builtin headList))"
        "   [ [ (force (builtin dropList)) (con integer 2) ] (con (list integer) [1, 2, 3, 4]) ] ])");
    CHECK(head.r.ok);
    CHECK_EQ(cek_int(head.r.value), 3);
    uplc_arena_destroy(head.arena);
}

void test_cek_complement_and_count() {
    // countSetBits of complementByteString of 0x00 = 8
    auto r = run_cek(
        "(program 1.0.0 "
        " [ (builtin countSetBits)"
        "   [ (builtin complementByteString) (con bytestring #00) ] ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 8);
    uplc_arena_destroy(r.arena);
}

void test_cek_intbs_round_trip() {
    // byteStringToInteger True (integerToByteString True 4 0x12345678) == 0x12345678
    auto r = run_cek(
        "(program 1.0.0 "
        " [ [ (builtin byteStringToInteger) (con bool True) ]"
        "   [ [ [ (builtin integerToByteString) (con bool True) ]"
        "       (con integer 4) ]"
        "     (con integer 305419896) ] ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 305419896);
    uplc_arena_destroy(r.arena);
}

void test_cek_exp_mod_small() {
    // 2^10 mod 1000 == 24
    auto r = run_cek(
        "(program 1.0.0 "
        " [ [ [ (builtin expModInteger) (con integer 2) ]"
        "       (con integer 10) ]"
        "   (con integer 1000) ])");
    CHECK(r.r.ok);
    CHECK_EQ(cek_int(r.r.value), 24);
    uplc_arena_destroy(r.arena);
}

void test_cek_serialise_data_nonempty() {
    auto r = run_cek(
        "(program 1.0.0 "
        " [ (builtin lengthOfByteString)"
        "   [ (builtin serialiseData) (con data (I 42)) ] ])");
    CHECK(r.r.ok);
    CHECK(cek_int(r.r.value) > 0);
    uplc_arena_destroy(r.arena);
}

// --- Compiled-path test: hand-written add(2,3) -------------------------
//
// Simulates what LLVM IR would emit for:
//     (program 1.0.0 [[(builtin addInteger) 2] 3])

uplc_value prog_compiled_add_2_3(uplc_budget* b) {
    uplc_value add = uplcrt_make_builtin(b, /*addInteger=*/0);
    uplc_value two = uplcrt_const_int_si(b, 2);
    uplc_value three = uplcrt_const_int_si(b, 3);
    uplc_value partial = uplcrt_apply(add, two, b);
    return uplcrt_apply(partial, three, b);
}

void test_compiled_add_integer() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    init_budget(budget);
    auto r = uplcrt_run_compiled(prog_compiled_add_2_3, arena, &budget);
    CHECK(r.ok);
    CHECK_EQ(r.value.tag, UPLC_V_CON);
    CHECK_EQ(cek_int(r.value), (int64_t)5);
    uplc_arena_destroy(arena);
}

// Compiled ifThenElse with one Force + three value args.
// Simulates: [ [ [ (force (builtin ifThenElse)) True ] 10 ] 20 ]  == 10
uplc_value prog_compiled_ite_true(uplc_budget* b) {
    uplc_value ite = uplcrt_make_builtin(b, /*ifThenElse=*/26);
    ite = uplcrt_force(ite, b);                      // consume the single force
    uplc_value cond = uplcrt_const_bool(b, 1);
    uplc_value t = uplcrt_const_int_si(b, 10);
    uplc_value e = uplcrt_const_int_si(b, 20);
    uplc_value a1 = uplcrt_apply(ite, cond, b);
    uplc_value a2 = uplcrt_apply(a1, t, b);
    return uplcrt_apply(a2, e, b);
}

void test_compiled_if_then_else() {
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    init_budget(budget);
    auto r = uplcrt_run_compiled(prog_compiled_ite_true, arena, &budget);
    CHECK(r.ok);
    CHECK_EQ(cek_int(r.value), (int64_t)10);
    uplc_arena_destroy(arena);
}

}  // namespace

int main() {
    test_cek_add_integer();
    test_cek_subtract_integer();
    test_cek_multiply_integer();
    test_cek_divide_integer_floor();
    test_cek_quotient_integer_trunc();
    test_cek_mod_integer();
    test_cek_remainder_integer();
    test_cek_equals_less_integer();
    test_cek_divide_by_zero();
    test_cek_if_then_else_true();
    test_cek_if_then_else_false();
    test_cek_bytestring_length_append();
    test_cek_bytestring_index_equals();
    test_cek_list_head_tail_null();
    test_cek_pair_projection();
    test_cek_nested_arith_under_if();

    test_cek_i_data_round_trip();
    test_cek_b_data_round_trip();
    test_cek_equals_data_true();
    test_cek_equals_data_constr_deep();
    test_cek_choose_data_dispatch();
    test_cek_string_append_equals();
    test_cek_encode_decode_utf8();
    test_cek_array_length_index();
    test_cek_drop_list();
    test_cek_complement_and_count();
    test_cek_intbs_round_trip();
    test_cek_exp_mod_small();
    test_cek_serialise_data_nonempty();

    test_compiled_add_integer();
    test_compiled_if_then_else();

    std::fprintf(stderr, "%d/%d passed\n", g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
