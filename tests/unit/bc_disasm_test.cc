// Unit test for the bytecode disassembler (`uplci --dump` backend).

#include <cstdio>
#include <string>
#include <string_view>

#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/lowering.h"
#include "compiler_bc/disasm.h"
#include "compiler_bc/lower_to_bc.h"

#include "runtime/core/arena.h"
#include "runtime/core/rterm.h"

namespace {

int g_total    = 0;
int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_total;                                                       \
        if (!(cond)) {                                                   \
            ++g_failures;                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                         __FILE__, __LINE__, #cond);                     \
        }                                                                \
    } while (0)

#define CHECK_CONTAINS(haystack, needle)                                  \
    do {                                                                  \
        ++g_total;                                                        \
        std::string_view h{(haystack)};                                   \
        std::string_view n{(needle)};                                     \
        if (h.find(n) == std::string_view::npos) {                        \
            ++g_failures;                                                 \
            std::fprintf(stderr,                                          \
                         "FAIL %s:%d: disasm missing %s\nfull:\n%s\n",    \
                         __FILE__, __LINE__,                              \
                         #needle, std::string(h).c_str());                \
        }                                                                 \
    } while (0)

std::string disasm(const char* src) {
    uplc::Arena ca;
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc_arena* ra = uplc_arena_create();
    uplc_rprogram rp = uplc::lower_to_runtime(ra, db);
    auto owner = uplc_bc::lower_rprogram(ra, rp);
    std::string out = uplc_bc::disassemble(owner->prog);
    uplc_arena_destroy(ra);
    return out;
}

// Identity applied to 7 — should emit MK_LAM, CONST, TAIL_APPLY
// (tail-fold fires at the entry function's closing RETURN).
void test_identity_tail_apply() {
    std::string d = disasm("(program 1.1.0 [(lam x x) (con integer 7)])");
    CHECK_CONTAINS(d, "program 1.1.0");
    CHECK_CONTAINS(d, "consts:");
    CHECK_CONTAINS(d, "integer 7");
    CHECK_CONTAINS(d, "MK_LAM");
    CHECK_CONTAINS(d, "TAIL_APPLY");
    CHECK_CONTAINS(d, "VAR_LOCAL slot=0");
}

// Force-of-delay — entry fn should end with MK_DELAY + TAIL_FORCE.
void test_force_delay_tail_force() {
    std::string d = disasm("(program 1.1.0 (force (delay (con integer 42))))");
    CHECK_CONTAINS(d, "MK_DELAY");
    CHECK_CONTAINS(d, "TAIL_FORCE");
    CHECK_CONTAINS(d, "integer 42");
}

// Inner lambda captures the outer binder → upval_outer_db shows the
// deBruijn index, VAR_UPVAL reads the captured slot.
void test_captures_shown() {
    std::string d = disasm(
        "(program 1.1.0 "
        "  [(lam outer (lam inner outer)) (con integer 11)])");
    CHECK_CONTAINS(d, "upval_outer_db=[0]");
    CHECK_CONTAINS(d, "VAR_UPVAL");
    CHECK_CONTAINS(d, "integer 11");
}

// Constr + case — CASE disasm should surface n_alts + per-alt plans.
void test_constr_case_plans() {
    std::string d = disasm(
        "(program 1.1.0 "
        "  (case (constr 0 (con integer 1)) (lam x x)))");
    CHECK_CONTAINS(d, "CONSTR");
    CHECK_CONTAINS(d, "CASE");
    CHECK_CONTAINS(d, "n_alts=1");
    CHECK_CONTAINS(d, "fn=");
}

}  // namespace

int main() {
    test_identity_tail_apply();
    test_force_delay_tail_force();
    test_captures_shown();
    test_constr_case_plans();
    std::fprintf(stderr, "bc_disasm_test: %d/%d checks passed\n",
                 g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
