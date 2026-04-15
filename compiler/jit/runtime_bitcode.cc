#include "compiler/jit/runtime_bitcode.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <unordered_set>

extern "C" {
/* Provided by libuplcrt_bitcode.a, generated at build time from the
 * embedded uplcrt_runtime.bc. */
extern unsigned char uplcrt_runtime_bc[];
extern unsigned int  uplcrt_runtime_bc_len;
}

namespace uplc {

namespace {

std::string llvm_err_str(llvm::Error e) {
    std::string s;
    llvm::raw_string_ostream os(s);
    llvm::logAllUnhandledErrors(std::move(e), os, "");
    return s;
}

/*
 * The "hot path" runtime functions that we genuinely need inlined for
 * perf — anything called repeatedly from generated code. Every other
 * runtime function is left at its natural inliner cost (the optimizer
 * still gets to see the body, but only inlines if cheap enough).
 */
const std::unordered_set<std::string>& force_inline_targets() {
    static const std::unordered_set<std::string> s = {
        // Closure / value dispatch
        "uplcrt_apply",
        "uplcrt_force",
        "uplcrt_make_lam",
        "uplcrt_make_delay",
        "uplcrt_make_builtin",
        "uplcrt_make_constr",
        "uplcrt_closure_of",
        // Case + constr decomposition
        "uplcrt_case_decompose_out",
        "uplcrt_apply_fields",
        // Constant builders
        "uplcrt_const_int_si",
        "uplcrt_const_int_bytes",
        "uplcrt_const_bs_ref",
        "uplcrt_const_string_ref",
        "uplcrt_const_bool",
        "uplcrt_const_unit",
        // Step / budget bookkeeping
        "uplcrt_budget_step",
        "uplcrt_budget_startup",
        // Arena hot path
        "uplc_arena_alloc",
        "uplcrt_budget_arena",
        // Tag plumbing
        "uplc_make_con_raw",
        "uplc_value_payload",
        // Builtin state machine — small enough to inline
        "uplcrt_builtin_fresh",
        "uplcrt_builtin_state_of",
        "uplcrt_builtin_consume_arg",
        "uplcrt_builtin_consume_force",
        // Builtin dispatch — exposing this lets the inliner see the
        // const META[tag] table lookup, devirtualize the impl call, and
        // (if the impl is also in bitcode) inline the body itself.
        "uplcrt_run_builtin",
        "uplcrt_builtin_meta",
        "uplcrt_builtin_arg_sizes",
        "uplcrt_costfn_eval",
    };
    return s;
}

/*
 * Re-mark every function definition in the runtime bitcode as
 * `available_externally`. This tells the optimizer "you may inline this
 * but the real symbol lives elsewhere", so:
 *   - inlining sees the IR and aggressively inlines hot paths
 *   - any non-inlined call falls through to libuplcrt.a / the JIT
 *     symbol map at link/load time
 *   - the linked-in IR is dropped after the inliner runs (no code
 *     duplication in the final object)
 */
void mark_runtime_functions(llvm::Module& m) {
    const auto& force = force_inline_targets();
    for (llvm::Function& f : m) {
        if (f.isDeclaration()) continue;

        /* Only externally-visible runtime functions get retargeted to
         * available_externally. Internal (static) helpers must KEEP their
         * internal linkage — promoting them would tell the optimizer the
         * real definition lives elsewhere, but static helpers have no
         * "elsewhere", so any non-inlined call would become unresolved. */
        if (f.hasExternalLinkage()) {
            f.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
            f.setVisibility(llvm::GlobalValue::DefaultVisibility);
        }

        /* Force the hot-path helpers to inline. Without this the inliner
         * cost model rejects them at default thresholds (uplcrt_apply
         * has multiple dispatch branches, uplcrt_make_lam allocates,
         * etc), so the call stays as an external call to libuplcrt.a
         * and we lose the wire_ty marshaling collapse + closure
         * devirtualization that this whole exercise is for. */
        if (force.count(f.getName().str())) {
            f.removeFnAttr(llvm::Attribute::NoInline);
            f.removeFnAttr(llvm::Attribute::OptimizeNone);
            f.addFnAttr(llvm::Attribute::AlwaysInline);
        }
    }
    /* All globals — including const tables like UPLC_BUILTIN_META — go
     * to available_externally. The optimizer's constant-folding passes
     * (IPSCCP, GVN load-store-forwarding) can still see the full
     * initializer and propagate values through indexed loads, but the
     * global itself is NOT emitted into the resulting object. This is
     * critical for UPLC_BUILTIN_META: its initializer references all
     * 101 per-builtin function pointers, and if we materialised the
     * global we'd need every uplcrt_builtin_* function resolvable at
     * JIT load time even when the program only calls one of them.
     *
     * available_externally is the only LLVM linkage that says "use the
     * definition for inlining + folding, but never emit it". */
    for (llvm::GlobalVariable& g : m.globals()) {
        if (g.isDeclaration()) continue;
        if (!g.hasExternalLinkage()) continue;
        g.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
        g.setVisibility(llvm::GlobalValue::DefaultVisibility);
    }
}

/*
 * Shared helper: parse the embedded bitcode into `ctx` and rewrite
 * linkage / attributes so the result is ready to link into user code.
 * Used by both the eager and cached link paths.
 */
std::unique_ptr<llvm::Module>
parse_and_mark(llvm::LLVMContext& ctx) {
    if (uplcrt_runtime_bc_len == 0) {
        throw std::runtime_error(
            "parse_runtime_bitcode: embedded bitcode is empty");
    }
    if (std::getenv("UPLC_DEBUG_BITCODE_LINK")) {
        std::fprintf(stderr, "[uplc] parsing %u bytes of runtime bitcode\n",
                     uplcrt_runtime_bc_len);
    }

    /* Wrap the embedded byte array in a MemoryBuffer (no copy). */
    llvm::StringRef bc_ref(reinterpret_cast<const char*>(uplcrt_runtime_bc),
                           uplcrt_runtime_bc_len);
    auto buf = llvm::MemoryBuffer::getMemBuffer(bc_ref, "uplcrt_runtime.bc",
                                                /*RequiresNullTerminator=*/false);

    auto rt_or_err = llvm::parseBitcodeFile(buf->getMemBufferRef(), ctx);
    if (!rt_or_err) {
        throw std::runtime_error("parse_runtime_bitcode: parseBitcodeFile: " +
                                 llvm_err_str(rt_or_err.takeError()));
    }
    std::unique_ptr<llvm::Module> rt_mod = std::move(*rt_or_err);

    mark_runtime_functions(*rt_mod);
    return rt_mod;
}

/*
 * Shared helper: link `rt_mod` into `user_mod` and verify. Takes
 * ownership because llvm::Linker::linkInModule consumes the source.
 * Inherits data layout / triple from user_mod so cross-builds with a
 * different host triple don't fail the link.
 */
void link_and_verify(llvm::Module& user_mod,
                     std::unique_ptr<llvm::Module> rt_mod) {
    rt_mod->setTargetTriple(user_mod.getTargetTriple());
    rt_mod->setDataLayout(user_mod.getDataLayout());

    /* OverrideFromSrc=false so any symbol the user module already
     * defines (program_entry, per-program version globals) stays put. */
    llvm::Linker linker(user_mod);
    if (linker.linkInModule(std::move(rt_mod), llvm::Linker::Flags::None)) {
        throw std::runtime_error(
            "link_runtime_bitcode: linkInModule failed");
    }

    /* Sanity-check that the merged module still verifies. Catches any
     * type / linkage mismatch up front instead of crashing inside the
     * optimizer. */
    std::string err;
    llvm::raw_string_ostream es(err);
    if (llvm::verifyModule(user_mod, &es)) {
        throw std::runtime_error(
            "link_runtime_bitcode: post-link verifyModule failed: " + err);
    }
}

}  // namespace

std::unique_ptr<llvm::Module>
parse_runtime_bitcode(llvm::LLVMContext& ctx) {
    return parse_and_mark(ctx);
}

void link_runtime_bitcode(llvm::LLVMContext& ctx, llvm::Module& user_mod) {
    link_and_verify(user_mod, parse_and_mark(ctx));
}

void link_runtime_bitcode(llvm::Module& user_mod,
                          const llvm::Module& cached_runtime) {
    assert(&user_mod.getContext() == &cached_runtime.getContext() &&
           "cached_runtime must live in the same LLVMContext as user_mod");

    /* CloneModule produces a fresh module in the same LLVMContext as
     * the source, so it's safe to hand to the linker which mutates
     * user_mod in place. This is the hot path for batch JIT consumers:
     * the source was parsed + marked once at JitRunner construction and
     * every subsequent fixture reuses the structural clone. */
    link_and_verify(user_mod, llvm::CloneModule(cached_runtime));
}

}  // namespace uplc
