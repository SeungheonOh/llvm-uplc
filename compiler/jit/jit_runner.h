#ifndef UPLC_JIT_JIT_RUNNER_H
#define UPLC_JIT_JIT_RUNNER_H

#include <cstdint>
#include <memory>
#include <string>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include "runtime/compiled/entry.h"
#include "uplc/abi.h"

namespace uplc {

/*
 * Single-source-of-truth host for the LLJIT pipeline.
 *
 * Construction:
 *   - initializes the native LLVM target
 *   - creates an LLJIT instance
 *   - registers every uplcrt_* runtime symbol against the main JITDylib
 *   - builds the host TargetMachine once (reused for every add_pipeline)
 *   - parses + marks the runtime bitcode into the shared LLVMContext
 *     (clone-per-fixture instead of re-parse-per-fixture)
 *
 * Usage:
 *   JitRunner runner;
 *   auto prog = runner.add_pipeline(p, "auction");
 *   uplc_value v = prog.entry(&budget);
 *   runner.remove(*prog.dylib);  // when done, frees JIT-allocated memory
 *
 * Each `add_pipeline` call creates its own JITDylib so `program_entry`
 * symbols don't collide across loaded modules, but every module lives
 * in the runner's single shared ThreadSafeContext. The returned function
 * pointer and dylib remain valid until `remove()` is called on the dylib,
 * or until the JitRunner is destroyed.
 */
struct Pipeline;  // defined in codegen_pipeline.h

/* Handle returned from add_pipeline: the entry point the caller uses to
 * execute the compiled program, plus the owning JITDylib so the caller
 * can dispose of it later via JitRunner::remove(). The dylib pointer is
 * owned by the JitRunner's LLJIT instance — do NOT delete it. */
struct JitProgram {
    uplc_program_entry    entry = nullptr;
    llvm::orc::JITDylib*  dylib = nullptr;
};

class JitRunner {
public:
    JitRunner();
    ~JitRunner();

    JitRunner(const JitRunner&)            = delete;
    JitRunner& operator=(const JitRunner&) = delete;

    /* Compile a frontend Pipeline (de-Bruijn Program + CompilePlan)
     * through codegen + runtime-bitcode link + O3 + LLJIT add. Uses the
     * runner's shared LLVMContext, cached host TargetMachine, and
     * pre-parsed runtime bitcode module, so the per-program cost is
     * just codegen + CloneModule + optimize. */
    JitProgram add_pipeline(const Pipeline& p, const std::string& unique_id);

    /* Tear down a previously-added program. Removes the JITDylib from the
     * ExecutionSession, which releases its materialized code + linker state
     * (including JIT-allocated executable pages). After this call the
     * corresponding `entry` pointer is dangling and must not be invoked.
     * Throws std::runtime_error on any LLJIT failure. */
    void remove(llvm::orc::JITDylib& dy);

private:
    void register_runtime_symbols();

    std::unique_ptr<llvm::orc::LLJIT>    jit_;
    llvm::orc::ThreadSafeContext         ts_ctx_;
    std::unique_ptr<llvm::TargetMachine> tm_;
    std::unique_ptr<llvm::Module>        runtime_cache_;
    std::size_t next_dylib_id_ = 0;
};

}  // namespace uplc

#endif
