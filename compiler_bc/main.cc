// uplci — bytecode-VM UPLC interpreter (plan-bc.md M-bc-6).
//
// Takes a UPLC script (text, flat, or CBOR-wrapped flat) and runs it
// through the bytecode VM, printing the result term and budget.
//
// No LLVM dependency and no JIT step: flat/text → AST → rprogram →
// bytecode → dispatch-loop evaluation. Parallel to uplcr but targets
// the interpreted path.
//
// v1 scope: no --arg (comes in M-bc-7 alongside conformance), no --json.

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "compiler/ast/arena.h"
#include "compiler/ast/pretty.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/cbor_unwrap.h"
#include "compiler/frontend/flat.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/lowering.h"
#include "compiler_bc/disasm.h"
#include "compiler_bc/lower_to_bc.h"

#include "runtime/cek/readback.h"
#include "runtime/core/arena.h"
#include "runtime/core/errors.h"
#include "runtime/core/rterm.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"
#include "uplc/bytecode.h"
#include "uplc/version.h"

namespace {

constexpr const char* kUsage =
    "usage: uplci [options] <script.{uplc,flat,cbor}>\n"
    "\n"
    "Options:\n"
    "  --version          print runtime version and exit\n"
    "  --help, -h         print this message and exit\n"
    "  --budget cpu,mem   cap execution budget; default is unlimited\n"
    "  --dump             lower to bytecode and print disassembly\n"
    "                     instead of running — shows constant pool,\n"
    "                     per-function capture plans, decoded opcodes\n";

void print_version() {
    std::printf("uplci %s\n", UPLC_VERSION_STRING);
}

bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool read_bytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    f.seekg(0);
    out.resize(static_cast<std::size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
    return true;
}

enum class Encoding { Text, Flat, Cbor };

Encoding detect_encoding(const std::string& path) {
    auto ends_with = [&](const char* sfx) {
        size_t n = std::strlen(sfx);
        return path.size() >= n &&
               path.compare(path.size() - n, n, sfx) == 0;
    };
    if (ends_with(".flat")) return Encoding::Flat;
    if (ends_with(".cbor")) return Encoding::Cbor;
    return Encoding::Text;
}

uplc::Program load_program(uplc::Arena& arena, const std::string& path) {
    Encoding enc = detect_encoding(path);
    if (enc == Encoding::Text) {
        std::string src;
        if (!read_text(path, src))
            throw std::runtime_error("cannot read: " + path);
        uplc::Program named = uplc::parse_program(arena, src);
        return uplc::name_to_debruijn(arena, named);
    }
    std::vector<uint8_t> bytes;
    if (!read_bytes(path, bytes))
        throw std::runtime_error("cannot read: " + path);
    if (enc == Encoding::Cbor)
        bytes = uplc::cbor_unwrap(bytes.data(), bytes.size());
    return uplc::decode_flat(arena, bytes.data(), bytes.size());
}

bool parse_budget(const char* s, int64_t& cpu, int64_t& mem) {
    char* end = nullptr;
    cpu = std::strtoll(s, &end, 10);
    if (!end || *end != ',') return false;
    mem = std::strtoll(end + 1, &end, 10);
    return end && *end == '\0';
}

}  // namespace

int main(int argc, char** argv) {
    int64_t budget_cpu = INT64_MAX;
    int64_t budget_mem = INT64_MAX;
    bool dump_only = false;
    std::string script_path;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--version") {
            print_version();
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::printf("%s\n", kUsage);
            return 0;
        }
        if (a == "--budget") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "uplci: --budget requires cpu,mem\n");
                return 1;
            }
            if (!parse_budget(argv[++i], budget_cpu, budget_mem)) {
                std::fprintf(stderr, "uplci: bad budget: %s\n", argv[i]);
                return 1;
            }
            continue;
        }
        if (a == "--dump") {
            dump_only = true;
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "uplci: unknown option: %s\n", argv[i]);
            return 1;
        }
        if (!script_path.empty()) {
            std::fprintf(stderr, "uplci: multiple scripts not supported\n");
            return 1;
        }
        script_path = argv[i];
    }

    if (script_path.empty()) {
        std::fprintf(stderr, "%s\n", kUsage);
        return 1;
    }

    uplc::Arena compiler_arena;
    uplc::Program db;
    try {
        db = load_program(compiler_arena, script_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uplci: %s\n", e.what());
        return 1;
    }

    uplc_arena* rt_arena = uplc_arena_create();
    uplc_rprogram rp;
    try {
        rp = uplc::lower_to_runtime(rt_arena, db);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uplci: lower_to_runtime: %s\n", e.what());
        uplc_arena_destroy(rt_arena);
        return 1;
    }

    std::unique_ptr<uplc_bc::ProgramOwner> bc_prog;
    try {
        bc_prog = uplc_bc::lower_rprogram(rt_arena, rp);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uplci: bytecode lowering: %s\n", e.what());
        uplc_arena_destroy(rt_arena);
        return 1;
    }

    if (dump_only) {
        std::string dump = uplc_bc::disassemble(bc_prog->prog);
        std::fputs(dump.c_str(), stdout);
        uplc_arena_destroy(rt_arena);
        return 0;
    }

    uplc_budget budget;
    uplcrt_budget_init_with_arena(&budget, budget_cpu, budget_mem, rt_arena);

    uplc_bc_result r = uplc_bc_run(rt_arena, &bc_prog->prog, &budget);
    int64_t spent_cpu = budget_cpu == INT64_MAX
                        ? (INT64_MAX - budget.cpu)
                        : (budget_cpu - budget.cpu);
    int64_t spent_mem = budget_mem == INT64_MAX
                        ? (INT64_MAX - budget.mem)
                        : (budget_mem - budget.mem);

    if (r.ok) {
        uplc_rterm* rt = uplcrt_readback(rt_arena, r.value);
        uplc::Arena ca;
        uplc_rprogram out_rp;
        out_rp.version = rp.version;
        out_rp.term    = rt;
        uplc::Program lifted = uplc::lift_program_from_runtime(ca, out_rp);
        std::string text = uplc::pretty_print_program(lifted);
        std::printf("Result: %s\n", text.c_str());
        std::printf("Budget: cpu=%lld mem=%lld\n",
                    (long long)spent_cpu, (long long)spent_mem);
        uplc_arena_destroy(rt_arena);
        return 0;
    }

    std::fprintf(stderr, "evaluation failure");
    if (r.fail_message) std::fprintf(stderr, ": %s", r.fail_message);
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "Budget: cpu=%lld mem=%lld\n",
                 (long long)spent_cpu, (long long)spent_mem);
    uplc_arena_destroy(rt_arena);
    if (r.fail_kind == UPLC_FAIL_OUT_OF_BUDGET) return 3;
    return 2;
}
