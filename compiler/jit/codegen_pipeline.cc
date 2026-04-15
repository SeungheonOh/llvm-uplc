#include "compiler/jit/codegen_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

/*
 * Phase-time instrumentation. Compiled out unless UPLC_TRACE_PIPELINE_TIME
 * is defined at build time (e.g. -DUPLC_TRACE_PIPELINE_TIME=1). When
 * enabled, set the env var UPLC_TIME_PIPELINE=1 at runtime to print
 * per-phase wall-clock numbers to stderr. Zero overhead when off.
 */
#ifdef UPLC_TRACE_PIPELINE_TIME
#  include <chrono>
#  define UPLC_TIME_BEGIN()                                                 \
        const bool _uplc_trace_pipeline_time =                              \
            std::getenv("UPLC_TIME_PIPELINE") != nullptr;                   \
        auto _uplc_trace_pipeline_t = std::chrono::steady_clock::now();
#  define UPLC_TIME_STAMP(phase)                                            \
        do {                                                                \
            if (_uplc_trace_pipeline_time) {                                \
                auto _now = std::chrono::steady_clock::now();               \
                auto _ms  = std::chrono::duration<double, std::milli>(      \
                                _now - _uplc_trace_pipeline_t).count();     \
                std::fprintf(stderr,                                        \
                    "[uplc-time] %-22s %8.2f ms\n", (phase), _ms);          \
                _uplc_trace_pipeline_t = _now;                              \
            }                                                               \
        } while (0)
#else
#  define UPLC_TIME_BEGIN()        ((void)0)
#  define UPLC_TIME_STAMP(phase)   ((void)0)
#endif

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cstring>

#include "compiler/codegen/llvm_codegen.h"
#include "compiler/frontend/cbor_unwrap.h"
#include "compiler/frontend/flat.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/jit/runtime_bitcode.h"

#include <cstdlib>

namespace uplc {

// ---------------------------------------------------------------------------
// Frontend
// ---------------------------------------------------------------------------

namespace {

bool ends_with(const std::string& s, const char* suffix) {
    std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool read_bytes(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
    return true;
}

}  // namespace

void build_pipeline_from_path(const std::string& path, Pipeline& out) {
    bool is_flat = ends_with(path, ".flat");
    bool is_cbor = ends_with(path, ".cbor");

    if (!is_flat && !is_cbor) {
        std::string source;
        if (!read_text(path, source)) {
            throw std::runtime_error("cannot read " + path);
        }
        Program named = parse_program(out.arena, source);
        out.db        = name_to_debruijn(out.arena, named);
    } else {
        std::vector<std::uint8_t> bytes;
        if (!read_bytes(path, bytes)) {
            throw std::runtime_error("cannot read " + path);
        }
        if (is_cbor) bytes = cbor_unwrap(bytes.data(), bytes.size());
        out.db = decode_flat(out.arena, bytes.data(), bytes.size());
    }
    out.plan = analyse_program(out.db);
}

void build_pipeline_from_flat(const std::uint8_t* bytes, std::size_t len,
                               Pipeline& out) {
    out.db   = decode_flat(out.arena, bytes, len);
    out.plan = analyse_program(out.db);
}

// ---------------------------------------------------------------------------
// Optimizer
// ---------------------------------------------------------------------------

void optimize_module(llvm::Module& mod, llvm::TargetMachine* tm,
                     OptPreset preset) {
    llvm::LoopAnalysisManager     lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager    cgam;
    llvm::ModuleAnalysisManager   mam;

    /* O3 turns on loop opts + vectorization, which dominate compile
     * time on the linked user+runtime module (~14 s on the auction).
     * For the JIT path we use O2: same inliner + scalar opts, no loop
     * vectorization or interleaving. ~3-4x faster compile, near
     * identical runtime perf for UPLC programs (which don't have hot
     * vectorizable loops anyway). */
    const bool aggressive = (preset == OptPreset::Aggressive);

    llvm::PipelineTuningOptions pto;
    pto.LoopUnrolling      = aggressive;
    pto.LoopInterleaving   = aggressive;
    pto.LoopVectorization  = aggressive;
    pto.SLPVectorization   = aggressive;
    pto.MergeFunctions     = aggressive;

    llvm::PassBuilder pb(tm, pto);
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    auto level = aggressive ? llvm::OptimizationLevel::O3
                             : llvm::OptimizationLevel::O1;
    llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(level);
    mpm.run(mod, mam);
}

// ---------------------------------------------------------------------------
// TargetMachine factory
// ---------------------------------------------------------------------------

std::unique_ptr<llvm::TargetMachine> make_host_target_machine(const char* who) {
    llvm::Triple triple(llvm::sys::getDefaultTargetTriple());

    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        std::fprintf(stderr, "uplc-jit: %s: no target for %s: %s\n",
                     who, triple.str().c_str(), err.c_str());
        return nullptr;
    }

    std::string features;
    {
        llvm::StringMap<bool> feats = llvm::sys::getHostCPUFeatures();
        bool first = true;
        for (auto& kv : feats) {
            if (!kv.second) continue;
            if (!first) features += ',';
            features += '+';
            features += kv.first().str();
            first = false;
        }
    }

    llvm::TargetOptions opts;
    auto rm = std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);
    auto cm = std::optional<llvm::CodeModel::Model>();
    std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
        triple,
        llvm::sys::getHostCPUName(),
        features,
        opts,
        rm,
        cm,
        llvm::CodeGenOptLevel::Aggressive));
    if (!tm) {
        std::fprintf(stderr, "uplc-jit: %s: failed to create TargetMachine\n", who);
        return nullptr;
    }
    return tm;
}

// ---------------------------------------------------------------------------
// Codegen
// ---------------------------------------------------------------------------

std::unique_ptr<llvm::Module>
codegen_to_module(llvm::LLVMContext& ctx,
                  const std::string& module_name,
                  const Pipeline&    p) {
    LlvmCodegen cg(ctx, module_name);
    cg.emit(p.db, p.plan);
    return cg.take_module();
}

std::unique_ptr<llvm::Module>
compile_pipeline_to_optimized_module(llvm::LLVMContext&   ctx,
                                      const std::string&  module_name,
                                      const Pipeline&     p,
                                      llvm::TargetMachine* tm,
                                      const llvm::Module* cached_runtime,
                                      OptPreset           preset) {
    UPLC_TIME_BEGIN();

    auto mod = codegen_to_module(ctx, module_name, p);
    UPLC_TIME_STAMP("codegen");

    if (tm) {
        mod->setTargetTriple(tm->getTargetTriple());
        mod->setDataLayout(tm->createDataLayout());
    }

    /*
     * Runtime bitcode inlining is on by default for every preset. The
     * runtime perf win — cross-module call boundary elimination so the
     * mid-end can inline, CSE, and register-allocate through the runtime
     * ABI — is worth the extra compile time even on JIT one-shot
     * evaluation, given that jit_runner pre-parses the runtime bitcode
     * once per process and every pipeline clones it instead of re-parsing.
     *
     * UPLC_NO_RUNTIME_INLINE=1 forces "off" (A/B switch + conformance
     * test escape hatch — see tests/conformance/jit_conformance_test.cc).
     */
    bool inline_runtime = true;
    if (std::getenv("UPLC_NO_RUNTIME_INLINE")) inline_runtime = false;

    if (inline_runtime) {
        if (cached_runtime) {
            link_runtime_bitcode(*mod, *cached_runtime);
        } else {
            link_runtime_bitcode(ctx, *mod);
        }
        UPLC_TIME_STAMP("link runtime bc");
    }

    optimize_module(*mod, tm, preset);
    UPLC_TIME_STAMP("optimize");
    return mod;
}

}  // namespace uplc
