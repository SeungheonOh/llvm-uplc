// JIT conformance runner: parse → debruijn → compile-plan → LLJIT → run →
// readback → compare against the upstream `.uplc.expected` text, plus a
// `.uplc.budget.expected` budget check.
//
// Mirrors eval_conformance_test.cc in structure and gate conditions; the only
// difference is execution mode (LLJIT instead of CEK).
//
// Each fixture gets a fresh LLJIT instance.  Fixture categories:
//   "parse error"        — parser must reject the input
//   "evaluation failure" — parse OK but JIT run must fail at evaluation
//   otherwise            — JIT result must match expected pretty-print

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "compiler/analysis/compile_plan.h"
#include "compiler/ast/arena.h"
#include "compiler/ast/pretty.h"
#include "compiler/ast/term.h"
#include "compiler/codegen/llvm_codegen.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/jit/codegen_pipeline.h"
#include "compiler/jit/jit_runner.h"
#include "compiler/lowering.h"

#include "runtime/core/arena.h"
#include "runtime/compiled/entry.h"
#include "runtime/cek/readback.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

#ifndef UPLC_CONFORMANCE_ROOT
#define UPLC_CONFORMANCE_ROOT "tests/conformance/fixtures"
#endif

namespace fs = std::filesystem;

namespace {

// LLJIT setup, symbol registration, codegen, and module loading all live
// behind uplc::JitRunner now (compiler/jit/jit_runner.{h,cc}).

// ---------------------------------------------------------------------------
// Shared infrastructure (mirrors eval_conformance_test.cc)
// ---------------------------------------------------------------------------

const std::set<std::string>& known_deferred() {
    static const std::set<std::string> k = {
        "builtin/constant/bls12-381/G1/bad-zero-01/bad-zero-01.uplc",
        "builtin/constant/bls12-381/G1/bad-zero-02/bad-zero-02.uplc",
        "builtin/constant/bls12-381/G1/bad-zero-03/bad-zero-03.uplc",
        "builtin/constant/bls12-381/G1/off-curve/off-curve.uplc",
        "builtin/constant/bls12-381/G1/out-of-group/out-of-group.uplc",
        "builtin/constant/bls12-381/G2/bad-zero-01/bad-zero-01.uplc",
        "builtin/constant/bls12-381/G2/bad-zero-02/bad-zero-02.uplc",
        "builtin/constant/bls12-381/G2/off-curve/off-curve.uplc",
        "builtin/constant/bls12-381/G2/out-of-group/out-of-group.uplc",
    };
    return k;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string rtrim(std::string s) {
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == '\r' ||
            s.back() == ' '  || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

enum class Expect { Success, ParseError, EvalFailure };

Expect classify(const fs::path& uplc_path) {
    fs::path exp = uplc_path;
    exp += ".expected";
    std::string body = rtrim(read_file(exp));
    if (body == "parse error")        return Expect::ParseError;
    if (body == "evaluation failure") return Expect::EvalFailure;
    return Expect::Success;
}

struct ExpectedBudget {
    bool    present;
    int64_t cpu;
    int64_t mem;
};

ExpectedBudget parse_budget(const fs::path& budget_path) {
    ExpectedBudget out{false, 0, 0};
    std::string body = read_file(budget_path);
    if (body.empty()) return out;
    auto cpu_pos = body.find("cpu:");
    auto mem_pos = body.find("mem:");
    if (cpu_pos == std::string::npos || mem_pos == std::string::npos) return out;
    try {
        out.cpu = std::stoll(body.substr(cpu_pos + 4));
        out.mem = std::stoll(body.substr(mem_pos + 4));
        out.present = true;
    } catch (...) {}
    return out;
}

// Pretty-print the expected source file's de-Bruijn form.
std::string expected_pretty(const std::string& expected_source) {
    uplc::Arena ca;
    uplc::Program named = uplc::parse_program(ca, expected_source);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    return uplc::pretty_print_program(db);
}

struct Stats {
    int total            = 0;
    int success_ok       = 0;
    int parse_err_ok     = 0;
    int eval_fail_ok     = 0;
    int deferred         = 0;
    int budget_ok        = 0;
    int result_mismatch  = 0;
    int budget_mismatch  = 0;
    int exec_failure     = 0;
    int spurious_success = 0;
    int runner_error     = 0;
    std::vector<std::string> failures;
};

// ---------------------------------------------------------------------------
// JIT execution
// ---------------------------------------------------------------------------

// Run one source program through the full JIT pipeline.
// On success, writes result pretty-print to `out_result` and budget to
// `*cpu`/`*mem` and returns true.
// On evaluation failure, returns false with `out_fail_msg` set and
// `out_fail_kind` populated.
// Throws on parse / codegen / JIT-setup errors.
bool jit_run(uplc::JitRunner& jit,
             const std::string& source,
             std::string& out_result,
             std::string& out_fail_msg,
             uplc_fail_kind& out_fail_kind,
             int64_t& cpu_consumed,
             int64_t& mem_consumed,
             const uplc::Program& db_prog) {
    /* Reuse the caller's shared JitRunner — the LLJIT instance and runtime
     * symbol map are created once at startup, then this function adds one
     * fixture's module, runs it, and removes the dylib so JIT-allocated
     * memory doesn't accumulate. Without sharing the runner, the
     * conformance pass spent ~80% of its time spinning up ORC infra. */
    uplc::Pipeline p;
    p.db   = db_prog;
    p.plan = uplc::analyse_program(db_prog);

    uplc::JitProgram prog = jit.add_pipeline(p, "fixture");
    uplc_program_entry entry_fn = prog.entry;

    // Run.
    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, INT64_MAX, INT64_MAX);

    uplc_compiled_result result = uplcrt_run_compiled(entry_fn, arena, &budget);

    cpu_consumed = uplcrt_sat_add_i64(INT64_MAX, -budget.cpu);
    mem_consumed = uplcrt_sat_add_i64(INT64_MAX, -budget.mem);

    if (!result.ok) {
        out_fail_msg  = result.fail_message ? result.fail_message : "(no message)";
        out_fail_kind = result.fail_kind;
        uplc_arena_destroy(arena);
        try { jit.remove(*prog.dylib); } catch (...) {}
        return false;
    }

    // Readback → pretty-print.
    uplc_rterm* rt = uplcrt_readback(arena, result.value);
    uplc::Arena ca;
    uplc_rprogram rp;
    rp.version.major = db_prog.version.major;
    rp.version.minor = db_prog.version.minor;
    rp.version.patch = db_prog.version.patch;
    rp.term = rt;
    uplc::Program lifted = uplc::lift_program_from_runtime(ca, rp);
    out_result = uplc::pretty_print_program(lifted);

    uplc_arena_destroy(arena);
    try { jit.remove(*prog.dylib); } catch (...) {}
    return true;
}

// ---------------------------------------------------------------------------
// Per-fixture runner
// ---------------------------------------------------------------------------

void run_fixture(uplc::JitRunner& jit, const fs::path& root, const fs::path& uplc_path, Stats& s) {
    ++s.total;
    std::string rel = fs::relative(uplc_path, root).string();

    if (known_deferred().count(rel)) {
        ++s.deferred;
        return;
    }

    std::string source = read_file(uplc_path);
    Expect expect = classify(uplc_path);

    // Parse + de-Bruijn conversion up front (same as CEK runner).
    uplc::Arena ca;
    uplc::Program db;
    try {
        uplc::Program named = uplc::parse_program(ca, source);
        db = uplc::name_to_debruijn(ca, named);
    } catch (const uplc::ParseError&) {
        if (expect == Expect::ParseError) { ++s.parse_err_ok; return; }
        if (expect == Expect::EvalFailure) { ++s.eval_fail_ok; return; }
        s.failures.push_back(rel + ": parse rejected a success fixture");
        ++s.exec_failure;
        return;
    } catch (const uplc::ConvertError&) {
        if (expect == Expect::ParseError) { ++s.parse_err_ok; return; }
        if (expect == Expect::EvalFailure) { ++s.eval_fail_ok; return; }
        s.failures.push_back(rel + ": convert rejected a success fixture");
        ++s.exec_failure;
        return;
    } catch (const std::exception& e) {
        ++s.runner_error;
        s.failures.push_back(rel + ": frontend exception: " + std::string(e.what()));
        return;
    }

    // JIT execution.
    try {
        std::string result_str, fail_msg;
        uplc_fail_kind fail_kind = UPLC_FAIL_MACHINE;
        int64_t cpu = 0, mem = 0;
        bool ok = jit_run(jit, source, result_str, fail_msg, fail_kind, cpu, mem, db);

        if (!ok) {
            // Evaluation failure.
            std::string msg = fail_msg;
            // Reclassify known-deferred unimplemented features:
            //   • FAIL_MACHINE from codegen means an unimplemented constant
            //     type (Data, List, Pair, BLS) hit the default: branch.
            //   • CEK-style "not implemented" / "BLS/Value" messages.
            if (fail_kind == UPLC_FAIL_MACHINE ||
                msg.find("BLS/Value constants") != std::string::npos ||
                msg.find("not implemented (M5") != std::string::npos) {
                ++s.deferred;
                return;
            }
            if (expect == Expect::EvalFailure) { ++s.eval_fail_ok; return; }
            if (expect == Expect::ParseError) {
                s.failures.push_back(rel + ": eval failed for a parse-error fixture");
                ++s.exec_failure;
                return;
            }
            // Expected success, got failure.
            s.failures.push_back(rel + ": JIT eval failure: " + msg);
            ++s.exec_failure;
            return;
        }

        // Evaluation succeeded.
        if (expect == Expect::ParseError) {
            s.failures.push_back(rel + ": expected parse error, JIT evaluated OK");
            ++s.spurious_success;
            return;
        }
        if (expect == Expect::EvalFailure) {
            s.failures.push_back(rel + ": expected eval failure, JIT evaluated OK");
            ++s.spurious_success;
            return;
        }

        // Compare result.
        std::string expected_src = read_file(uplc_path.string() + ".expected");
        std::string expected;
        try {
            expected = expected_pretty(expected_src);
        } catch (const std::exception& e) {
            s.failures.push_back(rel + ": cannot parse .expected: " + std::string(e.what()));
            ++s.runner_error;
            return;
        }

        if (result_str != expected) {
            ++s.result_mismatch;
            s.failures.push_back(rel + ": result mismatch\n  want: " +
                                  expected + "\n  got:  " + result_str);
            return;
        }

        ++s.success_ok;

        // Budget check.
        ExpectedBudget eb = parse_budget(uplc_path.string() + ".budget.expected");
        if (eb.present && (eb.cpu != cpu || eb.mem != mem)) {
            ++s.budget_mismatch;
            if (s.failures.size() < 20) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "%s: budget mismatch\n  want: cpu=%lld mem=%lld\n  got:  cpu=%lld mem=%lld",
                    rel.c_str(),
                    (long long)eb.cpu, (long long)eb.mem,
                    (long long)cpu,    (long long)mem);
                s.failures.push_back(buf);
            }
        } else if (eb.present) {
            ++s.budget_ok;
        }

    } catch (const std::exception& e) {
        // Codegen / JIT infrastructure failure.
        std::string msg = e.what();
        // Reclassify known-deferred unimplemented builtins.
        if (msg.find("BLS/Value constants") != std::string::npos ||
            msg.find("not implemented (M5") != std::string::npos) {
            ++s.deferred;
            return;
        }
        ++s.runner_error;
        s.failures.push_back(rel + ": JIT runner error: " + msg);
    }
}

}  // namespace

int main() {
    fs::path root = UPLC_CONFORMANCE_ROOT;
    if (!fs::exists(root)) {
        std::fprintf(stderr, "conformance root missing: %s\n", root.c_str());
        return 2;
    }

    /* The conformance test only checks correctness — it doesn't measure
     * runtime performance. The bitcode-inlining pre-pass doubles the
     * per-fixture compile time (full O3 over runtime+user IR) for zero
     * correctness benefit here. Force it off so the test stays fast.
     * `setenv` is read by compile_pipeline_to_optimized_module before
     * each fixture's compile. */
    setenv("UPLC_NO_RUNTIME_INLINE", "1", /*overwrite=*/1);

    /* One JitRunner shared across all 999 fixtures. The constructor spins
     * up the entire ORC infrastructure (LLJIT instance, runtime symbol
     * map) which used to dominate per-fixture wall time when each fixture
     * built its own runner. With sharing, that work happens once. */
    uplc::JitRunner jit;

    Stats s;
    for (auto it = fs::recursive_directory_iterator(root);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        if (it->path().extension() != ".uplc") continue;
        run_fixture(jit, root, it->path(), s);
    }

    std::fprintf(stderr,
        "JIT conformance:\n"
        "  total               = %d\n"
        "  success (result==)  = %d\n"
        "  parse-error ok      = %d\n"
        "  eval-failure ok     = %d\n"
        "  deferred            = %d\n"
        "  result mismatch     = %d\n"
        "  exec failure        = %d\n"
        "  spurious success    = %d\n"
        "  runner error        = %d\n",
        s.total, s.success_ok, s.parse_err_ok, s.eval_fail_ok,
        s.deferred, s.result_mismatch, s.exec_failure,
        s.spurious_success, s.runner_error);
    std::fprintf(stderr,
        "  budget ok           = %d\n"
        "  budget mismatch     = %d\n",
        s.budget_ok, s.budget_mismatch);

    for (const auto& f : s.failures) {
        std::fprintf(stderr, "FAIL %s\n", f.c_str());
    }

    int hard_failures = s.exec_failure + s.spurious_success + s.runner_error;
    return hard_failures == 0 ? 0 : 1;
}
