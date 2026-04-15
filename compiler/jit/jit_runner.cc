#include "compiler/jit/jit_runner.h"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <llvm/ADT/SmallVector.h>

#include "compiler/jit/codegen_pipeline.h"
#include "compiler/jit/runtime_bitcode.h"

/* Internal runtime headers — needed so REG() can take the address of
 * helpers that aren't part of the stable uplc/abi.h surface. */
#include "compiler/ast/builtin_tag.h"
#include "runtime/arena.h"
#include "runtime/builtin_dispatch.h"
#include "runtime/builtin_state.h"
#include "runtime/case_decompose.h"
#include "runtime/compiled/closure.h"
#include "runtime/errors.h"
#include "runtime/exmem.h"
#include "runtime/value.h"
#include "uplc/budget.h"

/* Runtime tables defined in C, declared here for C++ linkage so we can
 * register their addresses with the JIT. */
extern "C" {
extern const uplc_builtin_meta UPLC_BUILTIN_META[];
extern const std::uint32_t     UPLC_BUILTIN_COUNT;
}

namespace uplc {

namespace {

std::string llvm_err_str(llvm::Error e) {
    std::string s;
    llvm::raw_string_ostream os(s);
    llvm::logAllUnhandledErrors(std::move(e), os, "");
    return s;
}

}  // namespace

JitRunner::JitRunner()
    : ts_ctx_(std::make_unique<llvm::LLVMContext>()) {
    /* Idempotent — safe to call from multiple JitRunner instances. */
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        throw std::runtime_error("JITTargetMachineBuilder::detectHost: " +
                                 llvm_err_str(jtmb.takeError()));
    }

    /* On aarch64 the default code model emits ADRP + ADD / ADRP + GOT
     * LDR sequences for references to global symbols. Page21 fixups
     * have only ±4 GB of reach, but on Linux aarch64 with PIE the host
     * binary sits in the 0xaaaa_xxxx range while JITLink's slab
     * allocator hands out executable pages in the 0xffff_xxxx range —
     * a ~22 GB gap. Every reference from JIT-emitted code into a host-
     * resident symbol we registered via absoluteSymbols (UPLC_BUILTIN_META,
     * uplcrt_*, the per-builtin impls, …) then fails materialization
     * with "relocation target is out of range of Page21 fixup".
     *
     * Getting the aarch64 back end to stop emitting ADRP for externals
     * requires ALL THREE of the following conditions in combination —
     * relaxing any one of them puts the ADRP back:
     *
     *   1. Relocation model = Static (PIC forces a GOT indirection
     *      even when the symbol is dso_local).
     *   2. Code model = Large (the Small / Tiny models hard-code
     *      ADRP + ADD for direct references).
     *   3. Every external GlobalValue referenced from the module has
     *      its dso_local bit set (otherwise the back end goes through
     *      :got: regardless of relocation or code model).
     *
     * Conditions (1) and (2) are set here on a single TargetMachine
     * that we own and drive both the optimizer AND the object-file
     * codegen with. Condition (3) is enforced by mark_dso_local_for_jit
     * running both BEFORE and AFTER the optimizer in
     * compile_pipeline_to_optimized_module (the optimizer can
     * internalize or add new external decls and we want to catch both).
     *
     * We bypass LLJIT's built-in IRCompileLayer entirely and hand it
     * pre-compiled object buffers via addObjectFile — see
     * add_pipeline below. Going through addObjectFile keeps LLJIT's
     * internal JITTargetMachineBuilder out of the picture so there's
     * exactly one TargetMachine (ours) governing every relocation kind
     * the JIT linker ever sees. This also sidesteps LLVM-version-
     * specific variance in how LLJITBuilder::setJITTargetMachineBuilder
     * is propagated to the internal compile function.
     *
     * Slightly larger code per external reference, but that's the right
     * trade-off here — we're JIT'ing for correctness, not for code
     * density, and the hot path is inside user code / runtime bitcode
     * that's already resolved locally within the JIT slab. */
    const bool is_aarch64 = jtmb->getTargetTriple().isAArch64();
    if (is_aarch64) {
        jtmb->setCodeModel(llvm::CodeModel::Large);
        jtmb->setRelocationModel(llvm::Reloc::Static);
    }

    /* Build the single TargetMachine we'll use for everything. We
     * create it before handing the builder to LLJITBuilder (which
     * consumes it) so the optimizer and our own object-file codegen
     * share identical target settings — no divergence possible. */
    auto tm_or_err = jtmb->createTargetMachine();
    if (!tm_or_err) {
        throw std::runtime_error("createTargetMachine: " +
                                 llvm_err_str(tm_or_err.takeError()));
    }
    tm_ = std::move(*tm_or_err);

    auto j = llvm::orc::LLJITBuilder()
                 .setJITTargetMachineBuilder(std::move(*jtmb))
                 .create();
    if (!j) {
        throw std::runtime_error("LLJIT init failed: " +
                                 llvm_err_str(j.takeError()));
    }
    jit_ = std::move(*j);

    register_runtime_symbols();

    /* Parse + mark the embedded runtime bitcode ONCE into the shared
     * context. Every add_pipeline clones this instead of re-parsing the
     * 73 KB blob and re-walking every function / global to rewrite
     * linkage — which used to dominate the per-fixture compile time in
     * batch consumers (uplcbench, jit_conformance_test).
     *
     * Runtime bitcode inlining is the default policy (see
     * codegen_pipeline.cc), so we pre-parse the cache eagerly. The only
     * reason to skip is if the user has opted out via
     * UPLC_NO_RUNTIME_INLINE=1, in which case the cache would never be
     * consulted and parsing it would be pure waste. */
    if (!std::getenv("UPLC_NO_RUNTIME_INLINE")) {
        ts_ctx_.withContextDo([&](llvm::LLVMContext* ctx) {
            runtime_cache_ = parse_runtime_bitcode(*ctx);
        });
    }
}

JitRunner::~JitRunner() = default;

void JitRunner::register_runtime_symbols() {
    /* Resolve every uplcrt_* runtime function by direct address against
     * the main JITDylib. Per-program dylibs we create later inherit
     * MainJITDylib in their link order so they all see these symbols. */
    auto fl = llvm::JITSymbolFlags::Exported;
    llvm::orc::SymbolMap syms;
#define REG(fn) \
    syms[jit_->mangleAndIntern(#fn)] = llvm::orc::ExecutorSymbolDef( \
        llvm::orc::ExecutorAddr::fromPtr(fn), fl)

    /* Direct ABI surface — what generated IR calls. */
    REG(uplcrt_budget_step);
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

    /* Transitively reachable internal helpers exposed by the bitcode
     * inliner. The runtime is built with -fvisibility=hidden so these
     * are NOT in the dynamic symbol table; we hand the JIT explicit
     * function-pointer addresses instead. Any time an inlined runtime
     * function newly references an internal helper, add it here. */
    REG(uplcrt_run_builtin);
    REG(uplcrt_builtin_meta);
    REG(uplcrt_builtin_fresh);
    REG(uplcrt_builtin_state_of);
    REG(uplcrt_builtin_consume_arg);
    REG(uplcrt_builtin_consume_force);
    REG(uplcrt_builtin_arg_sizes);
    REG(uplcrt_costfn_eval);
    REG(uplcrt_exmem_integer);
    REG(uplcrt_exmem_bytestring);
    REG(uplcrt_exmem_string_utf8);
    REG(uplcrt_exmem_data);
    /* The const META table referenced by the inlined dispatch chain when
     * the optimizer can't fully fold the load through it. */
    syms[jit_->mangleAndIntern("UPLC_BUILTIN_META")] =
        llvm::orc::ExecutorSymbolDef(
            llvm::orc::ExecutorAddr::fromPtr(&::UPLC_BUILTIN_META[0]), fl);
    /* Per-builtin implementations referenced from META[].impl. We walk
     * the runtime table in lock-step with the compiler's name table —
     * both are indexed by BuiltinTag — and register every non-null
     * impl pointer under the canonical `uplcrt_builtin_<name>` symbol.
     * This is what frees us from `-fvisibility=hidden` headaches: the
     * JIT linker resolves every builtin reference up-front instead of
     * relying on dlsym walking the host process's dynamic symbol table. */
    for (std::uint32_t i = 0; i < ::UPLC_BUILTIN_COUNT; ++i) {
        if (!::UPLC_BUILTIN_META[i].impl) continue;
        std::string sym = "uplcrt_builtin_";
        sym += builtin_name(static_cast<BuiltinTag>(i));
        syms[jit_->mangleAndIntern(sym)] =
            llvm::orc::ExecutorSymbolDef(
                llvm::orc::ExecutorAddr::fromPtr(
                    reinterpret_cast<void*>(::UPLC_BUILTIN_META[i].impl)),
                fl);
    }
    REG(uplcrt_sat_add_i64);
    REG(uplcrt_sat_mul_i64);
    REG(uplcrt_budget_init);
    REG(uplcrt_budget_init_with_arena);
    REG(uplcrt_budget_arena);
    REG(uplcrt_budget_ok);
    REG(uplcrt_raise);
    REG(uplcrt_fail_install);
    REG(uplc_arena_create);
    REG(uplc_arena_destroy);
    REG(uplc_arena_alloc);
    REG(uplc_arena_alloc_mpz);
    REG(uplc_arena_dup);
    REG(uplc_arena_intern_str);
    REG(uplcrt_closure_of);
    REG(uplcrt_case_decompose);
    REG(uplc_make_con_raw);
    REG(uplc_make_constr_vals);
    REG(uplc_constr_of);
    REG(uplc_constr_tag);
    REG(uplc_constr_arity);
    REG(uplc_constr_field);
#undef REG

    if (auto err = jit_->getMainJITDylib().define(
            llvm::orc::absoluteSymbols(std::move(syms)))) {
        throw std::runtime_error("JIT symbol registration failed: " +
                                 llvm_err_str(std::move(err)));
    }

    /* Fallback: resolve any other symbol against the running process.
     *
     * The runtime bitcode link (compiler/jit/runtime_bitcode.cc) may
     * inline a runtime helper into user code, exposing previously-hidden
     * internal calls (uplcrt_run_builtin, uplc_arena_alloc_mpz, mpz_*,
     * memcpy, ...) at the JIT linker level. Maintaining an explicit
     * allowlist here is fragile — the inliner may pull in different
     * dependencies as the runtime evolves. So we hand the JIT a process-
     * wide search generator: anything dynamically reachable from the
     * uplcc / uplcr / uplcbench binary itself becomes resolvable.
     *
     * The explicit REG_SYM list above is still useful for symbols that
     * were compiled with -fvisibility=hidden and don't show up in the
     * dynamic symbol table; the absoluteSymbols definition wins on
     * lookup so they take precedence over the search generator. */
    auto dl_or_err = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit_->getDataLayout().getGlobalPrefix());
    if (!dl_or_err) {
        throw std::runtime_error("DynamicLibrarySearchGenerator: " +
                                 llvm_err_str(dl_or_err.takeError()));
    }
    jit_->getMainJITDylib().addGenerator(std::move(*dl_or_err));
}

/*
 * Compile `mod` through the backend codegen pipeline into an in-memory
 * ELF/Mach-O object buffer using our own TargetMachine. This is what
 * lets us keep LLJIT's internal IRCompileLayer out of the JIT path —
 * on aarch64 the whole fix depends on knowing exactly which code /
 * relocation model is used for the final machine-code emission, and
 * funnelling everything through one TargetMachine that we own is the
 * only reliable way to get that guarantee across LLVM versions.
 */
static std::unique_ptr<llvm::MemoryBuffer>
emit_object_to_buffer(llvm::Module& mod, llvm::TargetMachine& tm) {
    llvm::SmallVector<char, 0> buf;
    llvm::raw_svector_ostream os(buf);

    llvm::legacy::PassManager pm;
    if (tm.addPassesToEmitFile(pm, os, nullptr,
                                llvm::CodeGenFileType::ObjectFile)) {
        throw std::runtime_error(
            "JitRunner: target cannot emit object files");
    }
    pm.run(mod);

    return std::make_unique<llvm::SmallVectorMemoryBuffer>(
        std::move(buf), /*BufferName=*/"jit-module",
        /*RequiresNullTerminator=*/false);
}

JitProgram JitRunner::add_pipeline(const Pipeline& p,
                                   const std::string& unique_id) {
    /* Codegen + runtime-bitcode link + optimization happen inside the
     * shared context under ThreadSafeContext::withContextDo, which
     * holds the recursive mutex guarding the context. This is what
     * lets future multi-threaded JitRunner use be safe; in the current
     * single-threaded consumers (uplcbench, conformance, uplcc run)
     * the lock is uncontended. The object-file emission also runs
     * under the lock because legacy PassManager::run mutates the
     * module. */
    std::unique_ptr<llvm::MemoryBuffer> obj_buf;
    ts_ctx_.withContextDo([&](llvm::LLVMContext* ctx) {
        auto mod = compile_pipeline_to_optimized_module(
            *ctx, unique_id, p, tm_.get(), runtime_cache_.get(),
            OptPreset::FastJIT);
        obj_buf = emit_object_to_buffer(*mod, *tm_);
    });

    /* Each program gets its own JITDylib so `program_entry` doesn't
     * collide across multiple add_pipeline calls (used by uplcbench). */
    std::string dylib_name = "u" + std::to_string(next_dylib_id_++) + "_" + unique_id;
    auto dy = jit_->createJITDylib(dylib_name);
    if (!dy) {
        throw std::runtime_error("createJITDylib(" + dylib_name + ") failed: " +
                                 llvm_err_str(dy.takeError()));
    }
    dy->addToLinkOrder(jit_->getMainJITDylib(),
        llvm::orc::JITDylibLookupFlags::MatchExportedSymbolsOnly);

    if (auto err = jit_->addObjectFile(*dy, std::move(obj_buf))) {
        throw std::runtime_error("addObjectFile(" + dylib_name + ") failed: " +
                                 llvm_err_str(std::move(err)));
    }

    auto sym = jit_->lookup(*dy, "program_entry");
    if (!sym) {
        throw std::runtime_error("lookup program_entry in " + dylib_name + ": " +
                                 llvm_err_str(sym.takeError()));
    }
    return JitProgram{sym->toPtr<uplc_program_entry>(), &*dy};
}

void JitRunner::remove(llvm::orc::JITDylib& dy) {
    /* removeJITDylib drops all materialized code in the dylib and releases
     * the JITLink allocator's executable pages. Without this call, long
     * batch runs (e.g. uplcbench over the full corpus) accumulate every
     * previously-compiled script's code in the process image until the
     * macOS JIT allocator runs out of segment space. */
    if (auto err = jit_->getExecutionSession().removeJITDylib(dy)) {
        throw std::runtime_error("removeJITDylib failed: " +
                                 llvm_err_str(std::move(err)));
    }
}

}  // namespace uplc
