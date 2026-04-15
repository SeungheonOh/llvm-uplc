// Conformance round-trip sweep for the M1 frontend.
//
// Walks every .uplc fixture under tests/conformance/fixtures and asserts one
// of the following:
//   * `.expected` says "parse error"       -> our parser must fail
//   * `.expected` says "evaluation failure" -> our parser may succeed OR the
//     converter may reject it (both outcomes match TS)
//   * otherwise                             -> parse must succeed AND the
//     round-trip
//       parse -> convert -> dename -> pretty -> parse -> convert -> pretty
//     must produce the same de-Bruijn text as the direct path.
//
// Fixtures that hit known unimplemented surfaces (BLS curve validation) are
// listed in kKnownDeferred and skipped with a WARN, not a FAIL.

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

#include "compiler/ast/arena.h"
#include "compiler/ast/pretty.h"
#include "compiler/ast/term.h"
#include "compiler/driver.h"
#include "compiler/frontend/flat.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/name_to_debruijn.h"

#ifndef UPLC_CONFORMANCE_ROOT
#define UPLC_CONFORMANCE_ROOT "tests/conformance/fixtures"
#endif

namespace fs = std::filesystem;

namespace {

// Fixtures whose parse-rejection we cannot yet reproduce. All nine are
// BLS12-381 curve-membership failures; M5 will enable real validation via BLST.
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

// Trim trailing whitespace so `.expected` comparisons aren't thrown off by
// a stray newline in the fixture.
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

struct Stats {
    int total         = 0;
    int success_ok    = 0;
    int parse_err_ok  = 0;
    int eval_fail_ok  = 0;
    int deferred      = 0;
    int flat_rt_ok    = 0;   // fixtures that round-tripped through flat
    int flat_rt_skip  = 0;   // fixtures with constant types flat cannot encode
    std::vector<std::string> failures;
};

void run_fixture(const fs::path& root, const fs::path& uplc_path, Stats& s) {
    ++s.total;
    std::string rel = fs::relative(uplc_path, root).string();
    std::string source = read_file(uplc_path);

    Expect expect = classify(uplc_path);

    try {
        std::string direct = uplc::frontend_parse_to_debruijn_text(source);

        if (expect == Expect::ParseError) {
            if (known_deferred().count(rel)) {
                ++s.deferred;
                return;
            }
            s.failures.push_back(rel + ": expected parse error, parser accepted");
            return;
        }

        // Named round-trip stability:
        //   direct == parse(roundtrip_named(source))
        std::string renamed = uplc::frontend_roundtrip_named(source);
        std::string stable  = uplc::frontend_parse_to_debruijn_text(renamed);
        if (stable != direct) {
            s.failures.push_back(
                rel + ": round-trip mismatch\n  direct: " + direct +
                "\n  stable: " + stable);
            return;
        }

        // Flat encode/decode round-trip. Constant types that flat cannot
        // represent (array, value, BLS) throw from encode_flat; we count
        // those as skipped rather than failed.
        try {
            std::string flat_rt = uplc::frontend_flat_round_trip(source);
            if (flat_rt != direct) {
                s.failures.push_back(
                    rel + ": flat round-trip mismatch\n  direct:  " + direct +
                    "\n  flat_rt: " + flat_rt);
                return;
            }
            ++s.flat_rt_ok;
        } catch (const uplc::ParseError& e) {
            const std::string msg = e.what();
            if (msg.find("flat: constant type not encodable") != std::string::npos ||
                msg.find("flat: unsupported type for encoding") != std::string::npos ||
                msg.find("flat: unsupported constant value") != std::string::npos) {
                ++s.flat_rt_skip;
            } else {
                s.failures.push_back(rel + ": flat round-trip: " + msg);
                return;
            }
        } catch (const std::exception& e) {
            s.failures.push_back(rel + ": flat round-trip: " + e.what());
            return;
        }

        if (expect == Expect::Success) ++s.success_ok;
        else                            ++s.eval_fail_ok;  // accepted, will eval-fail later
    } catch (const uplc::ParseError&) {
        if (expect == Expect::ParseError) {
            ++s.parse_err_ok;
            return;
        }
        if (expect == Expect::EvalFailure) {
            // ParseError on an eval-failure fixture is a real mismatch (we
            // should accept it, since TS does).
            s.failures.push_back(rel + ": parse rejected an eval-failure fixture");
            return;
        }
        s.failures.push_back(rel + ": parse rejected a success fixture");
    } catch (const uplc::ConvertError&) {
        // Converter rejection is the named-AST analogue of an evaluation
        // failure: TS convert.ts also throws on a free unique. We allow this
        // for eval-failure fixtures and for success fixtures (rare but
        // semantically equivalent to "would eval-fail").
        if (expect == Expect::EvalFailure) {
            ++s.eval_fail_ok;
            return;
        }
        if (expect == Expect::ParseError) {
            ++s.parse_err_ok;
            return;
        }
        s.failures.push_back(rel + ": converter rejected a success fixture");
    } catch (const std::exception& e) {
        s.failures.push_back(rel + ": " + e.what());
    }
}

}  // namespace

int main() {
    fs::path root = UPLC_CONFORMANCE_ROOT;
    if (!fs::exists(root) || !fs::is_directory(root)) {
        std::fprintf(stderr, "conformance root not found: %s\n", root.c_str());
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
        "conformance round-trip:\n"
        "  total          = %d\n"
        "  success ok     = %d\n"
        "  parse-err ok   = %d\n"
        "  eval-fail ok   = %d\n"
        "  deferred       = %d\n"
        "  flat rt ok     = %d\n"
        "  flat rt skip   = %d  (array/value/BLS not representable in flat)\n"
        "  failures       = %zu\n",
        s.total, s.success_ok, s.parse_err_ok, s.eval_fail_ok,
        s.deferred, s.flat_rt_ok, s.flat_rt_skip, s.failures.size());

    for (const auto& f : s.failures) {
        std::fprintf(stderr, "FAIL %s\n", f.c_str());
    }

    return s.failures.empty() ? 0 : 1;
}
