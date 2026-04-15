// M7 codegen unit tests — verify that LlvmCodegen produces valid LLVM IR for
// small UPLC programs.  We do not execute the IR here (that is M8's job);
// we check:
//   • the IR passes LLVM's verifier (LlvmCodegen::emit throws if it fails)
//   • `program_entry` is present and has the right signature
//   • the expected closure functions are present
//   • key runtime call names appear in the IR text

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include <llvm/IR/LLVMContext.h>

#include "compiler/analysis/compile_plan.h"
#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"
#include "compiler/codegen/llvm_codegen.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"

namespace {

int g_failures = 0;
int g_total    = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_total;                                                           \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                        \
                         __FILE__, __LINE__, #cond);                         \
        }                                                                    \
    } while (0)

// Emit IR for a source program.  Throws on parse or codegen error.
std::string emit_ir(const std::string& src) {
    uplc::Arena ca;
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    llvm::LLVMContext ctx;
    uplc::LlvmCodegen cg(ctx, "test");
    cg.emit(db, plan);
    return cg.get_ir();
}

bool ir_contains(const std::string& ir, const std::string& needle) {
    return ir.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// (program 1.1.0 (con integer 0))
// Minimal: one Const step, no closures, program_entry present.
void test_constant_program() {
    std::string ir;
    try { ir = emit_ir("(program 1.1.0 (con integer 0))"); }
    catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL constant_program: %s\n", e.what());
        return;
    }
    CHECK(ir_contains(ir, "define"));
    CHECK(ir_contains(ir, "program_entry"));
    /* Budget step charges are emitted inline as raw IR now — look for
     * the scratch counter GEP label instead of the old symbol name. */
    CHECK(ir_contains(ir, "budget_scratch"));
    CHECK(ir_contains(ir, "uplcrt_const_int_bytes"));
    // No closures for a bare constant.
    CHECK(!ir_contains(ir, "uplc_closure_"));
}

// (program 1.1.0 (lam x x))
// One lambda closure (closure_0), no captures.
void test_identity_lambda() {
    std::string ir;
    try { ir = emit_ir("(program 1.1.0 (lam x x))"); }
    catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL identity_lambda: %s\n", e.what());
        return;
    }
    CHECK(ir_contains(ir, "program_entry"));
    CHECK(ir_contains(ir, "uplc_closure_0"));
    CHECK(ir_contains(ir, "uplcrt_make_lam"));
    // The identity body should not call make_lam again.
    // Var step (inline budget charge):
    CHECK(ir_contains(ir, "budget_scratch"));
}

// (program 1.1.0 (lam x (lam y x)))
// K-combinator: two closures, inner captures outer arg.
void test_k_combinator() {
    std::string ir;
    try { ir = emit_ir("(program 1.1.0 (lam x (lam y x)))"); }
    catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL k_combinator: %s\n", e.what());
        return;
    }
    CHECK(ir_contains(ir, "uplc_closure_0"));
    CHECK(ir_contains(ir, "uplc_closure_1"));
    CHECK(ir_contains(ir, "uplcrt_make_lam"));
    // Inner closure body loads from env (a GEP or load from env param).
    // Both closures should be internal linkage (not exported).
    CHECK(ir_contains(ir, "internal"));
}

// (program 1.1.0 [ (lam x x) (con integer 42) ])
// Apply: closure + constant, uses uplcrt_apply.
void test_apply() {
    std::string ir;
    try { ir = emit_ir("(program 1.1.0 [ (lam x x) (con integer 42) ])"); }
    catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL apply: %s\n", e.what());
        return;
    }
    CHECK(ir_contains(ir, "uplcrt_apply"));
    CHECK(ir_contains(ir, "uplcrt_const_int_bytes"));
    CHECK(ir_contains(ir, "uplcrt_make_lam"));
}

// (program 1.1.0 (force (delay (con bool True))))
// Force + Delay + bool constant.
void test_force_delay() {
    std::string ir;
    try { ir = emit_ir("(program 1.1.0 (force (delay (con bool True))))"); }
    catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL force_delay: %s\n", e.what());
        return;
    }
    CHECK(ir_contains(ir, "uplcrt_force"));
    CHECK(ir_contains(ir, "uplcrt_make_delay"));
    CHECK(ir_contains(ir, "uplcrt_const_bool"));
    CHECK(ir_contains(ir, "uplc_closure_0"));
}

// (program 1.1.0 (force (builtin addInteger)))
// Builtin: uplcrt_make_builtin.
void test_builtin() {
    std::string ir;
    try { ir = emit_ir("(program 1.1.0 (force (builtin addInteger)))"); }
    catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL builtin: %s\n", e.what());
        return;
    }
    CHECK(ir_contains(ir, "uplcrt_make_builtin"));
    CHECK(ir_contains(ir, "uplcrt_force"));
}

// (program 1.1.0 (con bytestring #))
// Empty bytestring constant.
void test_empty_bytestring() {
    std::string ir;
    try { ir = emit_ir("(program 1.1.0 (con bytestring #))"); }
    catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL empty_bs: %s\n", e.what());
        return;
    }
    CHECK(ir_contains(ir, "uplcrt_const_bs_ref"));
}

// (program 1.1.0 (con string "hello"))
// String constant.
void test_string_constant() {
    std::string ir;
    try { ir = emit_ir("(program 1.1.0 (con string \"hello\"))"); }
    catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL string_const: %s\n", e.what());
        return;
    }
    CHECK(ir_contains(ir, "uplcrt_const_string_ref"));
}

// (program 1.1.0 (con unit ()))
// Unit constant.
void test_unit_constant() {
    std::string ir;
    try { ir = emit_ir("(program 1.1.0 (con unit ()))"); }
    catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL unit_const: %s\n", e.what());
        return;
    }
    CHECK(ir_contains(ir, "uplcrt_const_unit"));
}

// (program 1.1.0 error)
// Error term: uplcrt_fail + unreachable.
void test_error_term() {
    std::string ir;
    try { ir = emit_ir("(program 1.1.0 (error))"); }
    catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL error_term: %s\n", e.what());
        return;
    }
    CHECK(ir_contains(ir, "uplcrt_fail"));
    CHECK(ir_contains(ir, "unreachable"));
}

// Verify IR is verifier-clean for the S-combinator (three nested lambdas
// with multiple captures — the most complex case in M6 tests).
void test_s_combinator_ir_valid() {
    std::string ir;
    try {
        ir = emit_ir("(program 1.1.0 (lam f (lam g (lam x [ [ f x ] [ g x ] ]))))");
    } catch (const std::exception& e) {
        ++g_failures; ++g_total;
        std::fprintf(stderr, "FAIL s_combinator_ir: %s\n", e.what());
        return;
    }
    // Three closures expected.
    CHECK(ir_contains(ir, "uplc_closure_0"));
    CHECK(ir_contains(ir, "uplc_closure_1"));
    CHECK(ir_contains(ir, "uplc_closure_2"));
    // At least one load from env (the captured vars inside the innermost fn).
    CHECK(ir_contains(ir, "uplcrt_apply"));
}

}  // namespace

int main() {
    test_constant_program();
    test_identity_lambda();
    test_k_combinator();
    test_apply();
    test_force_delay();
    test_builtin();
    test_empty_bytestring();
    test_string_constant();
    test_unit_constant();
    test_error_term();
    test_s_combinator_ir_valid();

    std::fprintf(stderr, "codegen_test: %d/%d passed\n",
                 g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
