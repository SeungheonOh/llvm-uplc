#pragma once

#include <memory>
#include <string>

// Forward-declare LLVM types to avoid pulling llvm headers into user code.
namespace llvm {
class LLVMContext;
class Module;
}  // namespace llvm

namespace uplc {

struct Program;
struct CompilePlan;

// ---------------------------------------------------------------------------
// M7 LLVM IR emitter.
//
// Accepts a de-Bruijn Program and its CompilePlan (from M6) and emits LLVM IR
// in closure-converted form:
//   • One LLVM function per LamAbs / Delay node (identified by closure_id)
//   • One top-level `program_entry(ptr %b) -> %uplc_value` function
//   • .rodata globals for baked integer / bytestring / string constants
//
// The emitted IR calls back into `libuplcrt` for all runtime services
// (closure allocation, apply, force, builtins, budget).  No LLVM IR is
// executed here — that is left to the M8 driver / JIT wrapper.
//
// Usage:
//   llvm::LLVMContext ctx;
//   LlvmCodegen cg(ctx, "my_module");
//   cg.emit(db_program, compile_plan);
//   std::string ir = cg.get_ir();       // human-readable .ll text
// ---------------------------------------------------------------------------

class LlvmCodegen {
public:
    LlvmCodegen(llvm::LLVMContext& ctx, const std::string& module_name);
    ~LlvmCodegen();

    LlvmCodegen(const LlvmCodegen&)            = delete;
    LlvmCodegen& operator=(const LlvmCodegen&) = delete;

    // Emit all IR for `program` using the precomputed `plan`.
    // `program` must be in de-Bruijn form (program.is_debruijn == true).
    // Throws std::runtime_error on internal errors.
    void emit(const Program& program, const CompilePlan& plan);

    // Return the emitted IR as human-readable LLVM assembly.
    // May only be called after emit().
    std::string get_ir() const;

    // Return a reference to the underlying Module.
    llvm::Module& module() { return *mod_; }
    const llvm::Module& module() const { return *mod_; }

    // Transfer ownership of the Module to the caller (e.g. for LLJIT).
    // After this call the LlvmCodegen object must not be used further.
    std::unique_ptr<llvm::Module> take_module() { return std::move(mod_); }

private:
    llvm::LLVMContext& ctx_;
    std::unique_ptr<llvm::Module> mod_;
};

}  // namespace uplc
