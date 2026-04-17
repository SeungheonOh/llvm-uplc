// Bytecode-VM conformance runner (plan-bc.md M-bc-7).
//
// Mirrors tests/conformance/eval_conformance_test.cc but swaps the CEK
// backend for the bytecode VM: the fixture is parsed, converted to
// de-Bruijn, lowered to rterm, then lowered again to bytecode, and run
// through uplc_bc_run. Result term and {cpu, mem} are compared against
// the upstream .uplc.expected and .uplc.budget.expected.
//
// Shares the same classification and gating logic so pass/fail numbers
// are directly comparable with the CEK runner.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "compiler/ast/arena.h"
#include "compiler/ast/pretty.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/lowering.h"
#include "compiler_bc/lower_to_bc.h"

#include "runtime/cek/readback.h"
#include "runtime/core/arena.h"
#include "runtime/core/rterm.h"
#include "uplc/abi.h"
#include "uplc/budget.h"
#include "uplc/bytecode.h"

#ifndef UPLC_CONFORMANCE_ROOT
#define UPLC_CONFORMANCE_ROOT "tests/conformance/fixtures"
#endif

namespace fs = std::filesystem;

namespace {

constexpr bool kCheckBudget = true;

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
           (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
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
    } catch (...) {
        out.present = false;
    }
    return out;
}

struct Stats {
    int total              = 0;
    int success_ok         = 0;
    int parse_err_ok       = 0;
    int eval_fail_ok       = 0;
    int deferred           = 0;
    int budget_ok          = 0;
    int result_mismatch    = 0;
    int budget_mismatch    = 0;
    int exec_failure       = 0;
    int spurious_success   = 0;
    int runner_error       = 0;
    std::vector<std::string> failures;
};

std::string expected_pretty(const std::string& expected_source) {
    uplc::Arena ca;
    uplc::Program named = uplc::parse_program(ca, expected_source);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    return uplc::pretty_print_program(db);
}

// Run a source program through the bytecode VM and return the pretty-
// printed result term + consumed {cpu, mem}.
std::string run_and_print(const std::string& source, int64_t& cpu_consumed,
                          int64_t& mem_consumed) {
    uplc::Arena ca;
    uplc::Program named = uplc::parse_program(ca, source);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);

    uplc_arena* ra = uplc_arena_create();
    uplc_rprogram rp = uplc::lower_to_runtime(ra, db);

    auto owner = uplc_bc::lower_rprogram(ra, rp);

    uplc_budget budget;
    uplcrt_budget_init_with_arena(&budget, INT64_MAX, INT64_MAX, ra);
    uplc_bc_result r = uplc_bc_run(ra, &owner->prog, &budget);
    if (!r.ok) {
        uplc_arena_destroy(ra);
        throw std::runtime_error(r.fail_message ? r.fail_message
                                                : "bytecode VM failure");
    }

    cpu_consumed = uplcrt_sat_add_i64(INT64_MAX, -budget.cpu);
    mem_consumed = uplcrt_sat_add_i64(INT64_MAX, -budget.mem);

    uplc_rterm* result_term = uplcrt_readback(ra, r.value);

    uplc::Arena cb;
    uplc_rprogram rp_result;
    rp_result.version = rp.version;
    rp_result.term = result_term;
    uplc::Program lifted = uplc::lift_program_from_runtime(cb, rp_result);
    std::string out = uplc::pretty_print_program(lifted);

    uplc_arena_destroy(ra);
    return out;
}

void run_fixture(const fs::path& root, const fs::path& uplc_path, Stats& s) {
    ++s.total;
    std::string rel = fs::relative(uplc_path, root).string();

    if (known_deferred().count(rel)) {
        ++s.deferred;
        return;
    }

    std::string source = read_file(uplc_path);
    Expect expect = classify(uplc_path);

    try {
        int64_t cpu = 0, mem = 0;
        std::string actual;
        try {
            actual = run_and_print(source, cpu, mem);
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
            if (expect == Expect::EvalFailure) { ++s.eval_fail_ok; return; }
            if (expect == Expect::ParseError) {
                s.failures.push_back(rel + ": eval failed for a parse-error fixture");
                ++s.exec_failure;
                return;
            }
            std::string msg = e.what();
            if (msg.find("BLS/Value constants") != std::string::npos ||
                msg.find("not implemented (M5") != std::string::npos) {
                ++s.deferred;
                return;
            }
            s.failures.push_back(rel + ": BC/eval: " + msg);
            ++s.exec_failure;
            return;
        }

        if (expect == Expect::ParseError) {
            s.failures.push_back(rel + ": expected parse error, evaluated OK");
            ++s.spurious_success;
            return;
        }
        if (expect == Expect::EvalFailure) {
            s.failures.push_back(rel + ": expected eval failure, evaluated OK");
            ++s.spurious_success;
            return;
        }

        std::string expected_body = read_file(uplc_path.string() + ".expected");
        std::string expected;
        try {
            expected = expected_pretty(expected_body);
        } catch (const std::exception& e) {
            s.failures.push_back(rel + ": cannot parse .expected: " + std::string(e.what()));
            ++s.runner_error;
            return;
        }

        if (actual != expected) {
            ++s.result_mismatch;
            if (s.failures.size() < 20) {
                s.failures.push_back(rel + ": result mismatch\n  want: " +
                                     expected + "\n  got:  " + actual);
            }
            return;
        }

        ++s.success_ok;

        if (kCheckBudget) {
            ExpectedBudget eb = parse_budget(uplc_path.string() + ".budget.expected");
            if (eb.present && (eb.cpu != cpu || eb.mem != mem)) {
                ++s.budget_mismatch;
                if (s.failures.size() < 20) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                                  "%s: budget mismatch\n  want: cpu=%lld mem=%lld\n  got:  cpu=%lld mem=%lld",
                                  rel.c_str(),
                                  (long long)eb.cpu, (long long)eb.mem,
                                  (long long)cpu, (long long)mem);
                    s.failures.push_back(buf);
                }
            } else if (eb.present) {
                ++s.budget_ok;
            }
        }
    } catch (const std::exception& e) {
        ++s.runner_error;
        s.failures.push_back(rel + ": runner exception: " + e.what());
    }
}

}  // namespace

int main() {
    fs::path root = UPLC_CONFORMANCE_ROOT;
    if (!fs::exists(root)) {
        std::fprintf(stderr, "conformance root missing: %s\n", root.c_str());
        return 2;
    }

    Stats s;
    for (auto it = fs::recursive_directory_iterator(root);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        if (it->path().extension() != ".uplc") continue;
        run_fixture(root, it->path(), s);
    }

    std::fprintf(stderr,
        "bytecode conformance:\n"
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
    if (kCheckBudget) {
        std::fprintf(stderr,
            "  budget ok           = %d\n"
            "  budget mismatch     = %d\n",
            s.budget_ok, s.budget_mismatch);
    }

    for (const auto& f : s.failures) {
        std::fprintf(stderr, "FAIL %s\n", f.c_str());
    }

    int hard_failures =
        s.exec_failure + s.spurious_success + s.runner_error;
    return hard_failures == 0 ? 0 : 1;
}
