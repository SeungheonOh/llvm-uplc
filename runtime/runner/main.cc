// Static-runner stub.
//
// libuplcrunner.a contains nothing but main() — `uplcc emit-exe` links it
// alongside the user's compiled program object, libuplcrt.a, the C++
// frontend (for result pretty-printing), and the static crypto deps. The
// resulting executable runs with no LLJIT, no dlopen, and no .uplcx
// indirection: just exec → program_entry → done.
//
// The program's Plutus version is embedded by codegen as three i32
// globals; the runner reads them when formatting output.

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "runtime/core/arena.h"
#include "runtime/compiled/entry.h"
#include "runtime/core/errors.h"
#include "runtime/cek/readback.h"
#include "uplc/abi.h"
#include "uplc/budget.h"
#include "uplc/version.h"

#include "compiler/ast/arena.h"
#include "compiler/ast/pretty.h"
#include "compiler/ast/term.h"
#include "compiler/lowering.h"

extern "C" {
// Provided by the linked-in compiled program object file.
uplc_value program_entry(uplc_budget* b);

// Plutus version metadata, also emitted by codegen. Defined as `extern`
// here; the program object provides the definitions.
extern const uint32_t uplc_program_version_major;
extern const uint32_t uplc_program_version_minor;
extern const uint32_t uplc_program_version_patch;
}

namespace {

constexpr const char* kUsage =
    "usage: %s [options]\n"
    "\n"
    "Options:\n"
    "  --version          print runtime version and exit\n"
    "  --help, -h         print this usage and exit\n"
    "  --budget cpu,mem   cap execution budget (default: unlimited)\n"
    "  --json             emit result + budget as JSON\n";

bool parse_budget(const char* s, int64_t& cpu, int64_t& mem) {
    char* end = nullptr;
    cpu = std::strtoll(s, &end, 10);
    if (!end || *end != ',') return false;
    mem = std::strtoll(end + 1, &end, 10);
    return end && *end == '\0';
}

std::string value_to_text(uplc_arena* arena, uplc_value v) {
    uplc_rterm* rt = uplcrt_readback(arena, v);
    uplc::Arena ca;
    uplc_rprogram rp;
    rp.version.major = uplc_program_version_major;
    rp.version.minor = uplc_program_version_minor;
    rp.version.patch = uplc_program_version_patch;
    rp.term          = rt;
    uplc::Program lifted = uplc::lift_program_from_runtime(ca, rp);
    return uplc::pretty_print_program(lifted);
}

}  // namespace

int main(int argc, char** argv) {
    int64_t budget_cpu = INT64_MAX;
    int64_t budget_mem = INT64_MAX;
    bool    json       = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--version") {
            std::printf("uplc-static-runner %s\n", UPLC_VERSION_STRING);
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::printf(kUsage, argv[0]);
            return 0;
        }
        if (a == "--json") { json = true; continue; }
        if (a == "--budget") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s: --budget requires cpu,mem\n", argv[0]);
                return 1;
            }
            if (!parse_budget(argv[++i], budget_cpu, budget_mem)) {
                std::fprintf(stderr, "%s: invalid --budget (expected cpu,mem)\n", argv[0]);
                return 1;
            }
            continue;
        }
        std::fprintf(stderr, "%s: unknown argument: %s\n", argv[0], argv[i]);
        return 1;
    }

    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init(&budget, budget_cpu, budget_mem);

    uplc_compiled_result result = uplcrt_run_compiled(program_entry, arena, &budget);

    int64_t cpu_used = uplcrt_sat_add_i64(budget_cpu, -budget.cpu);
    int64_t mem_used = uplcrt_sat_add_i64(budget_mem, -budget.mem);

    int rc = 0;
    if (!result.ok) {
        const char* msg = result.fail_message ? result.fail_message : "(no message)";
        if (json) {
            std::printf("{\"error\":\"%s\",\"kind\":%d,"
                        "\"budget\":{\"cpu\":%lld,\"mem\":%lld}}\n",
                        msg, (int)result.fail_kind,
                        (long long)cpu_used, (long long)mem_used);
        } else {
            std::fprintf(stderr, "%s: %s: %s\n", argv[0],
                         result.fail_kind == UPLC_FAIL_OUT_OF_BUDGET
                             ? "out of budget" : "evaluation failure",
                         msg);
        }
        rc = (result.fail_kind == UPLC_FAIL_OUT_OF_BUDGET) ? 3 : 2;
    } else {
        std::string term_str = value_to_text(arena, result.value);
        if (json) {
            std::printf("{\"result\":\"%s\","
                        "\"budget\":{\"cpu\":%lld,\"mem\":%lld}}\n",
                        term_str.c_str(),
                        (long long)cpu_used, (long long)mem_used);
        } else {
            std::printf("%s\n", term_str.c_str());
        }
    }

    if (!json) {
        std::fprintf(stderr, "ExBudget { cpu = %lld, mem = %lld }\n",
                     (long long)cpu_used, (long long)mem_used);
    }

    uplc_arena_destroy(arena);
    return rc;
}
