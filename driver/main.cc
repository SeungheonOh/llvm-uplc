// uplcr — loads a compiled .uplcx object file and runs it.
//
// Usage:
//   uplcr [options] <script.uplcx> [--arg FILE]...
//
// <script.uplcx> is a native object file produced by `uplcc emit-obj`.
// LLJIT links it in-process and calls its `program_entry` symbol.
//
// --arg FILE is applied left-to-right via uplcrt_apply after calling the
// compiled entry.  FILE encoding is auto-detected: .flat → flat binary,
// .cbor → CBOR-wrapped flat, anything else → UPLC text.  Each arg is
// evaluated through the CEK interpreter to normal form before application.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <fstream>
#include <setjmp.h>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// LLVM JIT infrastructure for loading the compiled object file.
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

// Runtime.
#include "runtime/core/arena.h"
#include "runtime/cek/cek.h"
#include "runtime/compiled/entry.h"
#include "runtime/core/errors.h"
#include "runtime/cek/readback.h"
#include "uplc/abi.h"
#include "uplc/budget.h"
#include "uplc/version.h"

// Compiler frontend — arg file parsing and result pretty-printing.
#include "compiler/ast/arena.h"
#include "compiler/ast/pretty.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/cbor_unwrap.h"
#include "compiler/frontend/flat.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/lowering.h"

namespace {

// ---------------------------------------------------------------------------
// Usage / version
// ---------------------------------------------------------------------------

constexpr const char* kUsage =
    "usage: uplcr [options] <script.uplcx> [--arg FILE]...\n"
    "\n"
    "Options:\n"
    "  --version          print runtime version and exit\n"
    "  --help, -h         print this message and exit\n"
    "  --arg FILE         apply FILE (text/flat/cbor UPLC term) to the script\n"
    "  --budget cpu,mem   cap execution budget; default is unlimited\n"
    "  --json             print result + budget as JSON instead of text\n";

void print_version() {
    std::printf("uplcr %s\n", UPLC_VERSION_STRING);
}

// ---------------------------------------------------------------------------
// LLVM error helper
// ---------------------------------------------------------------------------

static std::string err_str(llvm::Error e) {
    std::string s;
    llvm::raw_string_ostream os(s);
    llvm::logAllUnhandledErrors(std::move(e), os, "");
    return s;
}

// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------

static bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool read_bytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    f.seekg(0);
    out.resize(static_cast<std::size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
    return true;
}

// ---------------------------------------------------------------------------
// Arg file parsing
// ---------------------------------------------------------------------------

enum class ArgEncoding { Text, Flat, Cbor };

static ArgEncoding detect_encoding(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".flat")
        return ArgEncoding::Flat;
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".cbor")
        return ArgEncoding::Cbor;
    return ArgEncoding::Text;
}

// Parse a UPLC arg file into a de-Bruijn Program.  Throws on parse error.
static uplc::Program parse_arg(uplc::Arena& arena, const std::string& path) {
    ArgEncoding enc = detect_encoding(path);
    if (enc == ArgEncoding::Text) {
        std::string src;
        if (!read_text(path, src))
            throw std::runtime_error("cannot read: " + path);
        uplc::Program named = uplc::parse_program(arena, src);
        return uplc::name_to_debruijn(arena, named);
    }
    std::vector<uint8_t> bytes;
    if (!read_bytes(path, bytes))
        throw std::runtime_error("cannot read: " + path);
    if (enc == ArgEncoding::Cbor)
        bytes = uplc::cbor_unwrap(bytes.data(), bytes.size());
    return uplc::decode_flat(arena, bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------
// Budget argument parsing  ("cpu,mem")
// ---------------------------------------------------------------------------

static bool parse_budget(const char* s, int64_t& cpu, int64_t& mem) {
    char* end = nullptr;
    cpu = std::strtoll(s, &end, 10);
    if (!end || *end != ',') return false;
    mem = std::strtoll(end + 1, &end, 10);
    return end && *end == '\0';
}

// ---------------------------------------------------------------------------
// Result pretty-printing
// ---------------------------------------------------------------------------

static std::string value_to_text(uplc_arena* arena, uplc_value v) {
    uplc_rterm* rt = uplcrt_readback(arena, v);
    uplc::Arena ca;
    // Plutus version not yet embedded in artifacts — default to 1.1.0.
    uplc_rprogram rp;
    rp.version = {1, 1, 0};
    rp.term = rt;
    uplc::Program lifted = uplc::lift_program_from_runtime(ca, rp);
    return uplc::pretty_print_program(lifted);
}

// ---------------------------------------------------------------------------
// Compiled entry runner with post-entry arg application
//
// Mirrors uplcrt_run_compiled but applies arg_vals via uplcrt_apply after
// calling the compiled entry, so all apply calls share the same setjmp
// trampoline and budget.
// ---------------------------------------------------------------------------

struct RunResult {
    int            ok;
    uplc_fail_kind fail_kind;
    const char*    fail_message;
    uplc_value     value;
};

static RunResult run_entry_apply_args(uplc_program_entry entry,
                                      const std::vector<uplc_value>& arg_vals,
                                      uplc_arena* arena,
                                      uplc_budget* budget) {
    RunResult res{};
    res.fail_kind = UPLC_FAIL_MACHINE;

    budget->arena = arena;

    uplc_fail_ctx ctx;
    if (setjmp(ctx.env) != 0) {
        uplcrt_fail_install(nullptr);
        res.ok           = 0;
        res.fail_kind    = ctx.kind;
        res.fail_message = ctx.message;
        uplcrt_budget_flush(budget);
        return res;
    }
    uplcrt_fail_install(&ctx);

    uplc_value v = entry(budget);
    for (const auto& arg : arg_vals)
        v = uplcrt_apply(v, arg, budget);

    uplcrt_fail_install(nullptr);
    uplcrt_budget_flush(budget);
    if (!uplcrt_budget_ok(budget)) {
        res.ok           = 0;
        res.fail_kind    = UPLC_FAIL_OUT_OF_BUDGET;
        res.fail_message = "out of budget";
        return res;
    }
    res.ok    = 1;
    res.value = v;
    return res;
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::string              script_path;
    std::vector<std::string> arg_files;
    int64_t                  budget_cpu  = INT64_MAX;
    int64_t                  budget_mem  = INT64_MAX;
    bool                     json_output = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--version")            { print_version(); return 0; }
        if (a == "--help" || a == "-h")  { std::fputs(kUsage, stdout); return 0; }
        if (a == "--json")               { json_output = true; continue; }
        if (a == "--arg") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "uplcr: --arg requires a FILE argument\n");
                return 1;
            }
            arg_files.emplace_back(argv[++i]);
            continue;
        }
        if (a == "--budget") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "uplcr: --budget requires cpu,mem\n");
                return 1;
            }
            if (!parse_budget(argv[++i], budget_cpu, budget_mem)) {
                std::fprintf(stderr, "uplcr: invalid --budget (expected cpu,mem integers)\n");
                return 1;
            }
            continue;
        }
        if (!a.empty() && a[0] != '-' && script_path.empty()) {
            script_path = std::string(a);
            continue;
        }
        std::fprintf(stderr, "uplcr: unknown argument: %s\n", argv[i]);
        return 1;
    }

    if (script_path.empty()) {
        std::fputs(kUsage, stderr);
        return 1;
    }

    // ---- JIT setup ---------------------------------------------------------

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto jit_or_err = llvm::orc::LLJITBuilder().create();
    if (!jit_or_err) {
        std::fprintf(stderr, "uplcr: JIT init failed: %s\n",
                     err_str(jit_or_err.takeError()).c_str());
        return 1;
    }
    auto& jit = *jit_or_err;

    // Register all uplcrt ABI symbols by explicit address so the JIT linker
    // can resolve them against this process regardless of visibility settings.
    {
        auto fl = llvm::JITSymbolFlags::Exported;
        llvm::orc::SymbolMap syms;
#define REG(fn) \
        syms[jit->mangleAndIntern(#fn)] = llvm::orc::ExecutorSymbolDef( \
            llvm::orc::ExecutorAddr::fromPtr(fn), fl)

        /* uplcrt_budget_step is `static inline` in uplc/budget.h and
         * emitted inline into the IR — no JIT registration needed. */
        REG(uplcrt_budget_startup);
        REG(uplcrt_budget_flush);
        REG(uplcrt_make_lam);
        REG(uplcrt_make_delay);
        REG(uplcrt_make_builtin);
        REG(uplcrt_make_constr);
        REG(uplcrt_apply);
        REG(uplcrt_apply_slow);
        REG(uplcrt_force);
        REG(uplcrt_force_slow);
        REG(uplcrt_case_decompose_out);
        REG(uplcrt_apply_fields);
        REG(uplcrt_fail);
        REG(uplcrt_const_int_bytes);
        REG(uplcrt_const_bs_ref);
        REG(uplcrt_const_string_ref);
        REG(uplcrt_const_bool);
        REG(uplcrt_const_unit);
        REG(uplcrt_const_baked);
#undef REG

        if (auto err = jit->getMainJITDylib().define(
                llvm::orc::absoluteSymbols(std::move(syms)))) {
            std::fprintf(stderr, "uplcr: symbol registration failed: %s\n",
                         err_str(std::move(err)).c_str());
            return 1;
        }
    }

    // Load the compiled object file.
    auto buf = llvm::MemoryBuffer::getFile(script_path);
    if (!buf) {
        std::fprintf(stderr, "uplcr: cannot open %s: %s\n",
                     script_path.c_str(), buf.getError().message().c_str());
        return 1;
    }
    if (auto err = jit->addObjectFile(std::move(*buf))) {
        std::fprintf(stderr, "uplcr: loading %s failed: %s\n",
                     script_path.c_str(), err_str(std::move(err)).c_str());
        return 1;
    }

    // Locate the program entry point.
    auto sym = jit->lookup("program_entry");
    if (!sym) {
        std::fprintf(stderr, "uplcr: 'program_entry' not found in %s: %s\n",
                     script_path.c_str(), err_str(sym.takeError()).c_str());
        return 1;
    }
    auto entry_fn = sym->toPtr<uplc_program_entry>();

    // ---- Shared evaluation arena + budget ----------------------------------

    uplc_arena* arena = uplc_arena_create();
    uplc_budget budget;
    uplcrt_budget_init_with_arena(&budget, budget_cpu, budget_mem, arena);

    // ---- Parse and evaluate each --arg file --------------------------------
    //
    // Each arg is lowered to a runtime rterm and evaluated via the CEK
    // interpreter so the driver doesn't need to JIT-compile argument scripts.
    // CEK-produced values are V_CON / V_CONSTR for typical Data arguments;
    // V_LAM / V_DELAY args would hit the INTERP-vs-COMPILED guard in
    // uplcrt_apply and fail — this matches the M2-level interop contract.

    std::vector<uplc_value> arg_vals;
    for (const auto& path : arg_files) {
        uplc::Arena comp_arena;
        uplc::Program db;
        try {
            db = parse_arg(comp_arena, path);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "uplcr: --arg %s: %s\n", path.c_str(), e.what());
            uplc_arena_destroy(arena);
            return 1;
        }

        uplc_rprogram rp  = uplc::lower_to_runtime(arena, db);
        uplc_cek_result cr = uplc_cek_run(arena, rp.term, &budget);
        if (!cr.ok) {
            const char* msg = cr.fail_message ? cr.fail_message : "(no message)";
            if (cr.fail_kind == UPLC_FAIL_OUT_OF_BUDGET) {
                std::fprintf(stderr, "uplcr: --arg %s: out of budget\n", path.c_str());
                uplc_arena_destroy(arena);
                return 3;
            }
            std::fprintf(stderr, "uplcr: --arg %s: evaluation failure: %s\n",
                         path.c_str(), msg);
            uplc_arena_destroy(arena);
            return 2;
        }
        arg_vals.push_back(cr.value);
    }

    // ---- Run the compiled entry + apply args -------------------------------

    RunResult result = run_entry_apply_args(entry_fn, arg_vals, arena, &budget);

    // Budget consumed = initial − remaining (saturating).
    int64_t cpu_used = uplcrt_sat_add_i64(budget_cpu,  -budget.cpu);
    int64_t mem_used = uplcrt_sat_add_i64(budget_mem,  -budget.mem);

    int rc = 0;
    if (!result.ok) {
        const char* msg = result.fail_message ? result.fail_message : "(no message)";
        if (json_output) {
            std::printf("{\"error\":\"%s\",\"kind\":%d,"
                        "\"budget\":{\"cpu\":%lld,\"mem\":%lld}}\n",
                        msg, (int)result.fail_kind,
                        (long long)cpu_used, (long long)mem_used);
        } else {
            std::fprintf(stderr, "uplcr: %s: %s\n",
                         result.fail_kind == UPLC_FAIL_OUT_OF_BUDGET
                             ? "out of budget" : "evaluation failure",
                         msg);
        }
        rc = (result.fail_kind == UPLC_FAIL_OUT_OF_BUDGET) ? 3 : 2;
    } else {
        std::string term_str = value_to_text(arena, result.value);
        if (json_output) {
            std::printf("{\"result\":\"%s\","
                        "\"budget\":{\"cpu\":%lld,\"mem\":%lld}}\n",
                        term_str.c_str(),
                        (long long)cpu_used, (long long)mem_used);
        } else {
            std::printf("%s\n", term_str.c_str());
        }
    }

    if (!json_output) {
        std::fprintf(stderr, "ExBudget { cpu = %lld, mem = %lld }\n",
                     (long long)cpu_used, (long long)mem_used);
    }

    uplc_arena_destroy(arena);
    return rc;
}
