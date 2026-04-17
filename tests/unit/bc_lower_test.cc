// End-to-end lowerer + bytecode VM test (M-bc-5).
//
// Parse a text UPLC program, convert to deBruijn + lower to rprogram,
// then lower rprogram → uplc_bc_program and run the bytecode VM.
// Compare against a parallel run through the tree-walking CEK
// interpreter: value and budget must match bit-exactly.

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/lowering.h"
#include "compiler_bc/lower_to_bc.h"

#include <gmp.h>

#include "runtime/cek/cek.h"
#include "runtime/core/arena.h"
#include "runtime/core/rterm.h"
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

struct RunResult {
    uplc_bc_result bc;
    uplc_cek_result cek;
    int64_t bc_cpu, bc_mem;
    int64_t cek_cpu, cek_mem;
    uplc_arena* bc_arena;   // kept alive so bc.value pointers stay valid
    uplc_arena* cek_arena;
};

// Extract an integer out of a V_CON value, handling both inline and
// boxed representations. Returns true on success.
bool v_as_int64(uplc_value v, int64_t* out) {
    if (v.tag != UPLC_V_CON) return false;
    if (uplc_value_is_int_inline(v)) {
        *out = uplc_value_int_inline(v);
        return true;
    }
    uplc_rconstant* rc = (uplc_rconstant*)uplc_value_payload(v);
    if (!rc || rc->tag != UPLC_RCONST_INTEGER) return false;
    *out = mpz_get_si(rc->integer.value);
    return true;
}

RunResult run_parallel(const char* src) {
    uplc::Arena compiler_arena;
    uplc::Program named = uplc::parse_program(compiler_arena, src);
    uplc::Program db    = uplc::name_to_debruijn(compiler_arena, named);

    RunResult out{};
    // -- Bytecode VM path --
    out.bc_arena = uplc_arena_create();
    {
        uplc_rprogram rp = uplc::lower_to_runtime(out.bc_arena, db);
        auto owner = uplc_bc::lower_rprogram(out.bc_arena, rp);
        uplc_budget b;
        uplcrt_budget_init_with_arena(&b, INT64_MAX, INT64_MAX, out.bc_arena);
        out.bc = uplc_bc_run(out.bc_arena, &owner->prog, &b);
        out.bc_cpu = INT64_MAX - b.cpu;
        out.bc_mem = INT64_MAX - b.mem;
    }
    // -- CEK reference --
    out.cek_arena = uplc_arena_create();
    {
        uplc_rprogram rp = uplc::lower_to_runtime(out.cek_arena, db);
        uplc_budget b;
        uplcrt_budget_init_with_arena(&b, INT64_MAX, INT64_MAX, out.cek_arena);
        out.cek = uplc_cek_run(out.cek_arena, rp.term, &b);
        out.cek_cpu = INT64_MAX - b.cpu;
        out.cek_mem = INT64_MAX - b.mem;
    }
    return out;
}

void free_result(RunResult& r) {
    if (r.bc_arena)  { uplc_arena_destroy(r.bc_arena);  r.bc_arena = nullptr; }
    if (r.cek_arena) { uplc_arena_destroy(r.cek_arena); r.cek_arena = nullptr; }
}

void test_constant_parity() {
    const char* src = "(program 1.1.0 (con integer 42))";
    RunResult r = run_parallel(src);
    CHECK(r.bc.ok == 1 && r.cek.ok == 1);
    int64_t iv = 0;
    CHECK(v_as_int64(r.bc.value, &iv));
    CHECK_EQ_I64(iv, 42);
    CHECK_EQ_I64(r.bc_cpu, r.cek_cpu);
    CHECK_EQ_I64(r.bc_mem, r.cek_mem);
    free_result(r);
}

void test_identity_parity() {
    const char* src = "(program 1.1.0 [(lam x x) (con integer 17)])";
    RunResult r = run_parallel(src);
    CHECK(r.bc.ok == 1 && r.cek.ok == 1);
    int64_t iv = 0;
    CHECK(v_as_int64(r.bc.value, &iv));
    CHECK_EQ_I64(iv, 17);
    CHECK_EQ_I64(r.bc_cpu, r.cek_cpu);
    CHECK_EQ_I64(r.bc_mem, r.cek_mem);
    free_result(r);
}

void test_force_delay_parity() {
    const char* src = "(program 1.1.0 (force (delay (con integer 99))))";
    RunResult r = run_parallel(src);
    CHECK(r.bc.ok == 1 && r.cek.ok == 1);
    int64_t iv = 0;
    CHECK(v_as_int64(r.bc.value, &iv));
    CHECK_EQ_I64(iv, 99);
    CHECK_EQ_I64(r.bc_cpu, r.cek_cpu);
    CHECK_EQ_I64(r.bc_mem, r.cek_mem);
    free_result(r);
}

void test_const_function_parity() {
    // (lam x (lam y x)) 3 4 → 3
    const char* src =
        "(program 1.1.0 [[(lam x (lam y x)) (con integer 3)] (con integer 4)])";
    RunResult r = run_parallel(src);
    CHECK(r.bc.ok == 1 && r.cek.ok == 1);
    int64_t iv = 0;
    CHECK(v_as_int64(r.bc.value, &iv));
    CHECK_EQ_I64(iv, 3);
    CHECK_EQ_I64(r.bc_cpu, r.cek_cpu);
    CHECK_EQ_I64(r.bc_mem, r.cek_mem);
    free_result(r);
}

void test_error_parity() {
    const char* src = "(program 1.1.0 (error))";
    RunResult r = run_parallel(src);
    CHECK(r.bc.ok == 0 && r.cek.ok == 0);
    CHECK_EQ_I64(r.bc.fail_kind, r.cek.fail_kind);
    CHECK_EQ_I64(r.bc_cpu, r.cek_cpu);
    CHECK_EQ_I64(r.bc_mem, r.cek_mem);
    free_result(r);
}

// Case alt captures an enclosing binder. Before the ISA extension for
// per-alt capture plans this program was rejected by the lowerer; after
// the extension it must produce the outer's value and match CEK budget.
//
// Using a 1-field scrutinee so the alt lambda is actually applied and
// the captured `outer` surfaces as the result:
//
//   (lam outer
//     (case (constr 0 (con integer 0)) ; tag = 0, one (ignored) field
//       (lam dummy outer)))            ; alt 0 references `outer`
//   applied to (con integer 77)  =>  77
void test_case_alt_captures_enclosing_binder() {
    const char* src =
        "(program 1.1.0 "
        "  [(lam outer "
        "     (case (constr 0 (con integer 0)) (lam dummy outer))) "
        "   (con integer 77)])";
    RunResult r = run_parallel(src);
    CHECK(r.bc.ok == 1 && r.cek.ok == 1);
    int64_t iv = 0;
    CHECK(v_as_int64(r.bc.value, &iv));
    CHECK_EQ_I64(iv, 77);
    CHECK_EQ_I64(r.bc_cpu, r.cek_cpu);
    CHECK_EQ_I64(r.bc_mem, r.cek_mem);
    free_result(r);
}

// Two alts, different captures. Only the selected alt's env is built;
// the other's capture plan is skipped over. Covers the header-walk in
// op_case.
//
//   (lam a (lam b
//     (case (constr 1 (con integer 9))       ; tag 1, one field
//       (lam f a)                            ; alt 0 captures a
//       (lam f [[(builtin addInteger) f] b])))) ; alt 1 captures b
//   applied to 2, 3  =>  3 + 9 = 12
void test_case_two_alts_different_captures() {
    const char* src =
        "(program 1.1.0 "
        "  [[(lam a (lam b "
        "      (case (constr 1 (con integer 9)) "
        "            (lam f a) "
        "            (lam f [[(builtin addInteger) f] b])))) "
        "    (con integer 2)] "
        "   (con integer 3)])";
    RunResult r = run_parallel(src);
    CHECK(r.bc.ok == 1 && r.cek.ok == 1);
    int64_t iv = 0;
    CHECK(v_as_int64(r.bc.value, &iv));
    CHECK_EQ_I64(iv, 12);
    CHECK_EQ_I64(r.bc_cpu, r.cek_cpu);
    CHECK_EQ_I64(r.bc_mem, r.cek_mem);
    free_result(r);
}

}  // namespace

int main() {
    test_constant_parity();
    test_identity_parity();
    test_force_delay_parity();
    test_const_function_parity();
    test_error_parity();
    test_case_alt_captures_enclosing_binder();
    test_case_two_alts_different_captures();

    std::fprintf(stderr, "bc_lower_test: %d/%d checks passed\n",
                 g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
