#ifndef UPLC_JIT_CODEGEN_PIPELINE_H
#define UPLC_JIT_CODEGEN_PIPELINE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include "compiler/analysis/compile_plan.h"
#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"

namespace uplc {

/*
 * Shared compilation pipeline used by every component that needs to turn a
 * UPLC source / flat blob into an in-memory LLVM module:
 *
 *   uplcc emit-ir   uplcc emit-obj   uplcc emit-exe   uplcc run
 *   tools/bench     tests/conformance/jit_conformance_test
 *
 * Each phase is a free function so callers can mix-and-match (the bench
 * tool only needs frontend + codegen, the static-exe builder only needs
 * frontend + codegen + object-writing, etc).
 */

// ---------------------------------------------------------------------------
// Frontend pipeline
// ---------------------------------------------------------------------------

/* The output of the frontend: arena-owned de-Bruijn Program + its plan. */
struct Pipeline {
    Arena       arena;
    Program     db;
    CompilePlan plan;
};

/* Decode `path` (text, .flat, or .cbor — auto-detected by extension), lower
 * to de-Bruijn, run the closure analysis. Throws std::runtime_error on any
 * frontend failure (invalid syntax, malformed flat, etc.). */
void build_pipeline_from_path(const std::string& path, Pipeline& out);

/* Decode raw flat bytes (no extension dispatch). */
void build_pipeline_from_flat(const std::uint8_t* bytes, std::size_t len,
                               Pipeline& out);

// ---------------------------------------------------------------------------
// LLVM codegen + optimization
// ---------------------------------------------------------------------------

/* Build a host-tuned TargetMachine at -O3 (Aggressive). Returns nullptr and
 * writes an error to stderr on failure. `who` is a label used in error
 * messages so the caller (e.g. "emit-obj", "run") shows up. */
std::unique_ptr<llvm::TargetMachine> make_host_target_machine(const char* who);

/*
 * Optimization aggressiveness preset.
 *
 *   Aggressive (default for emit-obj / emit-exe):
 *       Full LLVM O3 pipeline. Slow to compile (~10+ s on the auction)
 *       but produces the fastest code. Use for shipping artifacts.
 *
 *   FastJIT (default for `uplcc run`):
 *       Stripped-down pipeline targeted at JIT cold-start. Inlines the
 *       runtime hot path, runs basic mem2reg / SCCP / GVN / DCE, then
 *       stops. Skips loop opts, vectorization, GlobalOpt, etc. The
 *       resulting code is ~2x slower than O3 but compiles ~10x faster,
 *       which is the right trade for a one-shot evaluation.
 */
enum class OptPreset {
    Aggressive,
    FastJIT,
};

/* Run the chosen optimization pipeline on `mod`. */
void optimize_module(llvm::Module& mod, llvm::TargetMachine* tm,
                     OptPreset preset = OptPreset::Aggressive);

/* Codegen a Pipeline into a freshly-created Module owned by the supplied
 * LLVMContext. The caller is responsible for moving the context+module into
 * the next stage (LLJIT, file writer, object dump, ...).
 *
 * This is the raw codegen step — no optimization, no bitcode link. Most
 * callers should use compile_pipeline_to_optimized_module instead. */
std::unique_ptr<llvm::Module>
codegen_to_module(llvm::LLVMContext& ctx,
                  const std::string& module_name,
                  const Pipeline&    p);

/*
 * Full per-program compile pipeline. This is the single entry point that
 * every JIT consumer (uplcc run / emit-obj / emit-exe, the static-exe
 * runner, the bench tool, the conformance test) should funnel through:
 *
 *   1. codegen_to_module (uplc Pipeline → llvm::Module)
 *   2. set the host triple + data layout from `tm`
 *   3. link the runtime bitcode (compiler/jit/runtime_bitcode.h) so the
 *      mid-end optimizer can inline through the runtime ABI
 *   4. run the full O3 module pipeline
 *
 * The returned module is ready to be:
 *   - dropped into LLJIT::addIRModule (JIT path), or
 *   - handed to TargetMachine::addPassesToEmitFile (object path)
 *
 * No callers need to remember to invoke link_runtime_bitcode or
 * optimize_module manually any more — getting it wrong would silently
 * regress runtime performance.
 *
 * Set the env var UPLC_NO_RUNTIME_INLINE=1 to skip the bitcode link
 * step (useful for A/B comparing the inlined vs opaque-call codegen).
 *
 * `cached_runtime` (optional) lets batch consumers reuse a pre-parsed
 * runtime bitcode module across many programs. When non-null, the
 * cached module is cloned into `user_mod`'s context instead of
 * re-parsing the embedded blob — the structural clone is ~an order of
 * magnitude cheaper than parseBitcodeFile + walking every function to
 * re-mark linkage. It MUST live in the same LLVMContext as `ctx`
 * (asserted inside the link step).
 */
std::unique_ptr<llvm::Module>
compile_pipeline_to_optimized_module(llvm::LLVMContext&   ctx,
                                      const std::string&  module_name,
                                      const Pipeline&     p,
                                      llvm::TargetMachine* tm,
                                      const llvm::Module* cached_runtime = nullptr,
                                      OptPreset           preset = OptPreset::Aggressive);

}  // namespace uplc

#endif
