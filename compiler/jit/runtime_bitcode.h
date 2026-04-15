#ifndef UPLC_JIT_RUNTIME_BITCODE_H
#define UPLC_JIT_RUNTIME_BITCODE_H

#include <memory>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace uplc {

/*
 * Link the runtime bitcode bundle (uplcrt_runtime.bc, embedded into the
 * compiler binary at build time) into `user_mod`. After this call:
 *
 *   - uplcrt_apply, uplcrt_force, uplcrt_make_lam, uplcrt_make_delay,
 *     uplcrt_const_baked, the consts.c helpers, the budget step, the
 *     arena bump-allocator, and friends are all *visible* to the LLVM
 *     optimizer as IR functions.
 *
 *   - The module-level optimizer can now inline them at every call site,
 *     which collapses the wire_ty marshalling, devirtualizes the closure
 *     dispatch path, and exposes the alloca for SROA / mem2reg.
 *
 * The bitcode functions are re-marked as `available_externally` linkage
 * before linking, so the linked-in copies serve only as inline candidates
 * — the actual definitions still come from libuplcrt.a (or the JIT's
 * registered symbol map) at runtime. This avoids duplicating the runtime
 * code in every compiled .uplcx and keeps the symbol resolution path
 * unchanged.
 *
 * Throws std::runtime_error on parse / link failure (which would
 * indicate a build-system bug, not a user-facing failure).
 */
void link_runtime_bitcode(llvm::LLVMContext& ctx, llvm::Module& user_mod);

/*
 * Cache-friendly split of the above. `parse_runtime_bitcode` performs
 * the expensive half (parseBitcodeFile + linkage rewrite + AlwaysInline
 * reapply) and returns the resulting module. The cached-link overload
 * then clones that module into each user module per program.
 *
 * This matters for batch JIT consumers (uplcbench, jit_conformance_test)
 * that compile hundreds of scripts through one JitRunner: calling the
 * eager overload above re-parses the embedded 73 KB blob and re-walks
 * every function+global on every fixture. Parsing + marking once and
 * cloning drops the per-fixture cost to a structural clone.
 *
 * Contract: `cached_runtime` and `user_mod` MUST share an LLVMContext —
 * llvm::CloneModule is context-local and cannot bridge contexts.
 *
 * Throws std::runtime_error on parse / clone / link failure.
 */
std::unique_ptr<llvm::Module>
parse_runtime_bitcode(llvm::LLVMContext& ctx);

void link_runtime_bitcode(llvm::Module& user_mod,
                          const llvm::Module& cached_runtime);

}  // namespace uplc

#endif
