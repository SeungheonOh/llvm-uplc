#include "compiler/codegen/llvm_codegen.h"
#include "compiler/codegen/baked_const.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <gmp.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "compiler/analysis/compile_plan.h"
#include "compiler/ast/arena.h"
#include "compiler/ast/builtin_tag.h"
#include "compiler/ast/term.h"

namespace uplc {

// ---------------------------------------------------------------------------
// Internal implementation
// ---------------------------------------------------------------------------

namespace {

// UPLC_STEP_* ordinals from include/uplc/budget.h
enum {
    STEP_CONST   = 0,
    STEP_VAR     = 1,
    STEP_LAMBDA  = 2,
    STEP_APPLY   = 3,
    STEP_DELAY   = 4,
    STEP_FORCE   = 5,
    STEP_BUILTIN = 6,
    STEP_CONSTR  = 7,
    STEP_CASE    = 8,
};

// UPLC_FAIL_* from include/uplc/abi.h
enum {
    FAIL_EVALUATION = 1,
    FAIL_MACHINE    = 3,
};

std::string closure_fn_name(std::uint32_t id) {
    return "uplc_closure_" + std::to_string(id);
}

// ---------------------------------------------------------------------------
// Emitter state — one instance per LlvmCodegen::emit() call.
// ---------------------------------------------------------------------------

struct Emitter {
    llvm::LLVMContext& ctx;
    llvm::Module&      mod;

    // Cached types
    llvm::StructType* value_ty;   // { i8, i8, [6 x i8], i64 }  — internal layout
    llvm::ArrayType*  wire_ty;    // [2 x i64]  — ABI-compatible wire type for value
    llvm::StructType* budget_ty;  // uplc_budget layout for inline budget steps
    //
    // IMPORTANT: On ARM64 (Apple Silicon), LLVM's calling-convention lowering
    // for the raw struct type `{ i8, i8, [6 x i8], i64 }` (value_ty) disagrees
    // with Clang's C ABI:
    //   • Clang C:   passes/returns the 16-byte struct in x0:x1 (register pair)
    //   • LLVM IR:   uses x8-indirect for return, stack slots for args
    //
    // Using `[2 x i64]` (wire_ty) for all function-signature positions forces
    // the same x0:x1 / register-pair convention that Clang generates for C
    // struct { uint8[8]; uint64 } returns and arguments.
    //
    // Internal SSA values remain typed as value_ty; to_wire() / from_wire()
    // convert at ABI boundaries via a stack-slot bitcast (same pattern Clang
    // generates: `load [2 x i64], ptr %local_alloca`).
    llvm::Type* ptr_ty;
    llvm::Type* void_ty;
    llvm::Type* i8_ty;
    llvm::Type* i32_ty;
    llvm::Type* i64_ty;

    // Runtime function callees (declared extern in the module)
    llvm::FunctionCallee fn_budget_startup;
    llvm::FunctionCallee fn_budget_flush;
    llvm::FunctionCallee fn_make_lam;
    llvm::FunctionCallee fn_make_delay;
    llvm::FunctionCallee fn_make_builtin;
    llvm::FunctionCallee fn_make_constr;
    llvm::FunctionCallee fn_apply;       // legacy generic apply
    llvm::FunctionCallee fn_force;       // legacy generic force
    llvm::FunctionCallee fn_apply_slow;  // cold path for inline apply
    llvm::FunctionCallee fn_force_slow;  // cold path for inline force

    /* Cached LLVM function types for direct closure body calls.
     *   uplc_lam_fn:   uplc_value(*)(uplc_value* env, uplc_value arg, uplc_budget* b)
     *   uplc_delay_fn: uplc_value(*)(uplc_value* env, uplc_budget* b)
     *
     * Both use wire_ty for value positions, matching the closure body
     * functions emitted by fill_closure_fn. */
    llvm::FunctionType* lam_fn_ty   = nullptr;
    llvm::FunctionType* delay_fn_ty = nullptr;
    llvm::FunctionCallee fn_case_decompose_out;
    llvm::FunctionCallee fn_apply_fields;
    llvm::FunctionCallee fn_fail;

    // Constant constructors
    llvm::FunctionCallee fn_const_int_bytes;
    llvm::FunctionCallee fn_const_bs_ref;
    llvm::FunctionCallee fn_const_string_ref;
    llvm::FunctionCallee fn_const_bool;
    llvm::FunctionCallee fn_const_unit;
    llvm::FunctionCallee fn_const_baked;

    // Direct saturated-builtin dispatcher (skips the state machine).
    llvm::FunctionCallee fn_run_builtin;

    // Analysis
    const CompilePlan* plan;
    const Program*     prog;

    // closure_id -> LLVM Function* (populated by forward_declare_closures)
    std::unordered_map<std::uint32_t, llvm::Function*> closure_fns;
    // term pointer -> LLVM GlobalVariable* for baked constant data
    std::unordered_map<const Term*, llvm::GlobalVariable*> const_data_globals;

    // Eager-built constant cache. Every CONST term that needs runtime
    // construction (Integer, Data, List, Pair, Array, BLS, Value) gets a
    // mutable global uplc_value slot. The program_entry prologue fills
    // every slot exactly once per program execution; emit_constant then
    // reads the slot instead of re-decoding on every reach.
    //
    // CEK builds the rconstant tree once at program-load time and re-uses
    // the same pointer on every CONST step. This brings the JIT to parity.
    std::unordered_map<const Term*, llvm::GlobalVariable*> const_value_slots;


    // -----------------------------------------------------------------------
    // Setup
    // -----------------------------------------------------------------------

    void setup_types() {
        ptr_ty  = llvm::PointerType::getUnqual(ctx);
        void_ty = llvm::Type::getVoidTy(ctx);
        i8_ty   = llvm::Type::getInt8Ty(ctx);
        i32_ty  = llvm::Type::getInt32Ty(ctx);
        i64_ty  = llvm::Type::getInt64Ty(ctx);

        // struct uplc_value { uint8 tag; uint8 subtag; uint8 _pad[6]; uint64 payload; }
        llvm::Type* pad_ty = llvm::ArrayType::get(i8_ty, 6);
        value_ty = llvm::StructType::create(ctx, {i8_ty, i8_ty, pad_ty, i64_ty}, "uplc_value");

        // [2 x i64]: ARM64-ABI-compatible wire type for uplc_value.
        wire_ty = llvm::ArrayType::get(i64_ty, 2);

        /* struct uplc_budget — layout mirrored from include/uplc/budget.h.
         * Used so emit_budget_step can emit inline field accesses via
         * typed GEPs instead of a cross-module call to uplcrt_budget_step. */
        auto* scratch_arr_ty = llvm::ArrayType::get(i32_ty,
                                                     /*UPLC_STEP__COUNT+1*/ 10);
        budget_ty = llvm::StructType::create(ctx,
            /* { i64 cpu; i64 mem; i32 scratch[10]; ptr arena; i64 initial_cpu; i64 initial_mem } */
            {i64_ty, i64_ty, scratch_arr_ty, ptr_ty, i64_ty, i64_ty},
            "uplc_budget");
    }

    // -----------------------------------------------------------------------
    // ABI conversion helpers (value_ty <-> wire_ty via stack slot)
    // -----------------------------------------------------------------------

    // Convert an internal value_ty SSA value to wire_ty for a call argument or
    // return instruction.  Stores to a stack slot then reloads as [2 x i64].
    llvm::Value* to_wire(llvm::IRBuilder<>& B, llvm::Value* val) {
        llvm::AllocaInst* slot = B.CreateAlloca(value_ty, nullptr, "to_wire");
        B.CreateStore(val, slot);
        return B.CreateLoad(wire_ty, slot, "wire");
    }

    // Convert a wire_ty call result back to internal value_ty.
    // Stores the [2 x i64] to a stack slot then reloads as %uplc_value.
    llvm::Value* from_wire(llvm::IRBuilder<>& B, llvm::Value* wire,
                            const char* name = "val") {
        llvm::AllocaInst* slot = B.CreateAlloca(wire_ty, nullptr, "from_wire");
        B.CreateStore(wire, slot);
        return B.CreateLoad(value_ty, slot, name);
    }

    // -----------------------------------------------------------------------
    // Inline Apply / Force dispatch helpers.
    //
    // These emit the V_LAM/V_DELAY + COMPILED hot path directly as IR and
    // fall through to uplcrt_apply_slow / uplcrt_force_slow only for the
    // V_BUILTIN partial-application and evaluation-failure branches.
    //
    // The hot path does no wire_ty marshalling — it works on the
    // already-in-register value_ty struct via extractvalue, GEPs into
    // the closure layout, and a direct indirect call to the closure
    // body function pointer. LLVM can fold/CSE/register-allocate
    // across the dispatch because it's all SSA in the parent function.
    //
    // The slow branch keeps the existing wire_ty-marshalling signature;
    // its to_wire stack slots are only emitted on that branch and stay
    // off the hot path.
    //
    // The closure layout (uplc_closure in include/uplc/abi.h):
    //   offset 0:   void* fn
    //   offset 8:   uint32_t nfree
    //   offset 12:  uint32_t _pad
    //   offset 16:  uplc_value free[]   ← passed to body as `env`
    //
    // The 16-byte fixed prefix means free array starts at byte offset
    // 16 from the closure pointer.
    // -----------------------------------------------------------------------

    /* Helper: build the branch_weights metadata for a likely-true branch. */
    llvm::MDNode* hot_branch_weights() {
        llvm::MDBuilder mdb(ctx);
        return mdb.createBranchWeights(/*True=*/2000, /*False=*/1);
    }

    /*
     * Inline a STEP_X charge against `budget`. Replaces a cross-module
     * call to uplcrt_budget_step with two load-add-store pairs — one
     * for the per-kind scratch counter, one for the running total.
     *
     * The slippage branch that the runtime helper runs every step
     * (flush when total >= 200) is SKIPPED here: emitting it at every
     * step site would nearly double the IR basic-block count and
     * destroy JIT compile time. Instead, program_entry terminates with
     * a final uplcrt_budget_flush that settles the accumulated scratch
     * before the outer uplcrt_budget_ok check — see fill_entry_fn.
     *
     * Correctness: scratch counters are u32 and overflow at ~4B steps.
     * Real UPLC programs max out well below that (the auction runs
     * ~1000 steps total), and early-OOB detection for pathological
     * runs is preserved at builtin-call boundaries where the bulk of
     * the cost gets charged against cpu/mem directly.
     */
    void emit_budget_step(llvm::IRBuilder<>& B,
                          llvm::Value* budget,
                          std::uint32_t kind) {
        static constexpr std::uint32_t UPLC_STEP_COUNT_IDX = 9;

        /* scratch[] is field 2 of uplc_budget. Typed GEPs let LLVM see
         * field boundaries for CSE across sequential charges. */
        llvm::Value* slot_ptr = B.CreateConstInBoundsGEP2_32(
            budget_ty, budget, 0, /*scratch field*/ 2);
        llvm::Value* per_kind_ptr = B.CreateConstInBoundsGEP2_32(
            llvm::ArrayType::get(i32_ty, 10), slot_ptr, 0, kind,
            "budget_scratch_k");
        llvm::Value* total_ptr = B.CreateConstInBoundsGEP2_32(
            llvm::ArrayType::get(i32_ty, 10), slot_ptr, 0,
            UPLC_STEP_COUNT_IDX, "budget_scratch_total");

        /* scratch[kind]++ */
        llvm::Value* per_kind = B.CreateLoad(i32_ty, per_kind_ptr, "sk");
        llvm::Value* per_kind_new = B.CreateAdd(per_kind,
            llvm::ConstantInt::get(i32_ty, 1), "sk_new");
        B.CreateStore(per_kind_new, per_kind_ptr);

        /* ++scratch[COUNT] */
        llvm::Value* total = B.CreateLoad(i32_ty, total_ptr, "stot");
        llvm::Value* total_new = B.CreateAdd(total,
            llvm::ConstantInt::get(i32_ty, 1), "stot_new");
        B.CreateStore(total_new, total_ptr);
    }

    /* Convenience: charge several sequential steps in one helper call,
     * letting LLVM fuse the GEP + scratch[kind] ops when possible. */
    void emit_budget_steps(llvm::IRBuilder<>& B,
                           llvm::Value* budget,
                           std::initializer_list<std::uint32_t> kinds) {
        for (auto k : kinds) emit_budget_step(B, budget, k);
    }

    /* Inline Apply dispatch. fn_v and arg_v are value_ty SSA values. */
    llvm::Value* emit_apply_inline(llvm::IRBuilder<>& B,
                                    llvm::Value* fn_v,
                                    llvm::Value* arg_v,
                                    llvm::Value* budget) {
        llvm::Function* parent = B.GetInsertBlock()->getParent();
        auto* bb_compiled = llvm::BasicBlock::Create(ctx, "apply.compiled", parent);
        auto* bb_slow     = llvm::BasicBlock::Create(ctx, "apply.slow",     parent);
        auto* bb_join     = llvm::BasicBlock::Create(ctx, "apply.join",     parent);

        /* Fast-path predicate: tag == V_LAM AND subtag == VLAM_COMPILED.
         * Build it as a single i16 compare (tag in low byte, subtag in
         * second byte) so LLVM emits one cmp+br instead of two. */
        llvm::Value* tag    = B.CreateExtractValue(fn_v, {0}, "fn_tag");
        llvm::Value* subtag = B.CreateExtractValue(fn_v, {1}, "fn_subtag");
        llvm::Value* is_lam = B.CreateICmpEQ(tag,
            llvm::ConstantInt::get(i8_ty, /*UPLC_V_LAM*/ 2), "is_lam");
        llvm::Value* is_compiled = B.CreateICmpEQ(subtag,
            llvm::ConstantInt::get(i8_ty, /*UPLC_VLAM_COMPILED*/ 1), "is_compiled");
        llvm::Value* is_lam_compiled = B.CreateAnd(is_lam, is_compiled,
                                                    "is_lam_compiled");
        auto* br = B.CreateCondBr(is_lam_compiled, bb_compiled, bb_slow);
        br->setMetadata(llvm::LLVMContext::MD_prof, hot_branch_weights());

        /* ---- compiled fast path ---- */
        B.SetInsertPoint(bb_compiled);

        /* Closure pointer = (uplc_closure*)(uintptr_t)payload */
        llvm::Value* payload = B.CreateExtractValue(fn_v, {3}, "fn_payload");
        llvm::Value* closure_ptr = B.CreateIntToPtr(payload, ptr_ty, "closure_ptr");

        /* fn pointer at offset 0. Mark the load as invariant — the
         * closure's body fn never changes after construction, so LLVM
         * can hoist this out of loops and CSE across repeated applies. */
        llvm::Value* fn_addr  = closure_ptr;  /* offset 0 */
        llvm::LoadInst* fn_ptr = B.CreateLoad(ptr_ty, fn_addr, "closure_fn");
        fn_ptr->setMetadata(llvm::LLVMContext::MD_invariant_load,
                            llvm::MDNode::get(ctx, {}));

        /* free array starts at offset 16 (sizeof(void*) + 2*sizeof(uint32)). */
        llvm::Value* free_ptr = B.CreateConstInBoundsGEP1_64(
            i8_ty, closure_ptr, /*offset=*/16, "closure_free");

        /* Direct call: body(free, arg_wire, b).
         *
         * We CANNOT mark this as a tail call: `free_ptr` aims into the
         * parent's alloca'd capture array, and `tail call` semantically
         * promises the caller's stack is gone before the callee runs.
         * Marking it tail produces use-after-free in the callee. Tail
         * recursion for UPLC needs either heap-allocated envs or a
         * dedicated calling convention — left as a follow-up. */
        llvm::Value* arg_wire = to_wire(B, arg_v);
        llvm::Value* fast_wire = B.CreateCall(
            lam_fn_ty, fn_ptr, {free_ptr, arg_wire, budget}, "apply_fast_wire");
        B.CreateBr(bb_join);
        llvm::BasicBlock* fast_pred = B.GetInsertBlock();

        /* ---- slow path ---- */
        B.SetInsertPoint(bb_slow);
        llvm::Value* fn_wire_cold  = to_wire(B, fn_v);
        llvm::Value* arg_wire_cold = to_wire(B, arg_v);
        llvm::Value* slow_wire = B.CreateCall(fn_apply_slow,
            {fn_wire_cold, arg_wire_cold, budget}, "apply_slow_wire");
        B.CreateBr(bb_join);
        llvm::BasicBlock* slow_pred = B.GetInsertBlock();

        /* ---- join ---- */
        B.SetInsertPoint(bb_join);
        llvm::PHINode* phi = B.CreatePHI(wire_ty, 2, "apply_wire");
        phi->addIncoming(fast_wire, fast_pred);
        phi->addIncoming(slow_wire, slow_pred);
        return from_wire(B, phi, "apply_inline");
    }

    /* Inline Force dispatch. thunk_v is a value_ty SSA value. */
    llvm::Value* emit_force_inline(llvm::IRBuilder<>& B,
                                    llvm::Value* thunk_v,
                                    llvm::Value* budget) {
        llvm::Function* parent = B.GetInsertBlock()->getParent();
        auto* bb_compiled = llvm::BasicBlock::Create(ctx, "force.compiled", parent);
        auto* bb_slow     = llvm::BasicBlock::Create(ctx, "force.slow",     parent);
        auto* bb_join     = llvm::BasicBlock::Create(ctx, "force.join",     parent);

        llvm::Value* tag    = B.CreateExtractValue(thunk_v, {0}, "thunk_tag");
        llvm::Value* subtag = B.CreateExtractValue(thunk_v, {1}, "thunk_subtag");
        llvm::Value* is_delay = B.CreateICmpEQ(tag,
            llvm::ConstantInt::get(i8_ty, /*UPLC_V_DELAY*/ 1), "is_delay");
        llvm::Value* is_compiled = B.CreateICmpEQ(subtag,
            llvm::ConstantInt::get(i8_ty, /*UPLC_VLAM_COMPILED*/ 1), "is_compiled");
        llvm::Value* is_delay_compiled = B.CreateAnd(is_delay, is_compiled,
                                                      "is_delay_compiled");
        auto* br = B.CreateCondBr(is_delay_compiled, bb_compiled, bb_slow);
        br->setMetadata(llvm::LLVMContext::MD_prof, hot_branch_weights());

        /* ---- compiled fast path ---- */
        B.SetInsertPoint(bb_compiled);
        llvm::Value* payload = B.CreateExtractValue(thunk_v, {3}, "thunk_payload");
        llvm::Value* closure_ptr = B.CreateIntToPtr(payload, ptr_ty, "closure_ptr");

        llvm::LoadInst* fn_ptr = B.CreateLoad(ptr_ty, closure_ptr, "closure_fn");
        fn_ptr->setMetadata(llvm::LLVMContext::MD_invariant_load,
                            llvm::MDNode::get(ctx, {}));

        llvm::Value* free_ptr = B.CreateConstInBoundsGEP1_64(
            i8_ty, closure_ptr, /*offset=*/16, "closure_free");

        /* Same stack-liveness concern as emit_apply_inline — free_ptr
         * points at the caller's alloca so we can't tail-call. */
        llvm::Value* fast_wire = B.CreateCall(
            delay_fn_ty, fn_ptr, {free_ptr, budget}, "force_fast_wire");
        B.CreateBr(bb_join);
        llvm::BasicBlock* fast_pred = B.GetInsertBlock();

        /* ---- slow path ---- */
        B.SetInsertPoint(bb_slow);
        llvm::Value* thunk_wire_cold = to_wire(B, thunk_v);
        llvm::Value* slow_wire = B.CreateCall(fn_force_slow,
            {thunk_wire_cold, budget}, "force_slow_wire");
        B.CreateBr(bb_join);
        llvm::BasicBlock* slow_pred = B.GetInsertBlock();

        /* ---- join ---- */
        B.SetInsertPoint(bb_join);
        llvm::PHINode* phi = B.CreatePHI(wire_ty, 2, "force_wire");
        phi->addIncoming(fast_wire, fast_pred);
        phi->addIncoming(slow_wire, slow_pred);
        return from_wire(B, phi, "force_inline");
    }

    // -----------------------------------------------------------------------

    llvm::FunctionCallee decl(const char* name, llvm::Type* ret,
                               std::initializer_list<llvm::Type*> params,
                               bool is_var_arg = false) {
        auto* fty = llvm::FunctionType::get(ret, llvm::ArrayRef<llvm::Type*>(params), is_var_arg);
        return mod.getOrInsertFunction(name, fty);
    }

    void declare_runtime() {
        /* void uplcrt_budget_flush(ptr b) — slippage flush path.
         * uplcrt_budget_step is no longer called from generated code; the
         * step charge is emitted inline by emit_budget_step. */
        fn_budget_flush = decl("uplcrt_budget_flush", void_ty, {ptr_ty});
        // void uplcrt_budget_startup(ptr b)  — one-time startup charge
        fn_budget_startup = decl("uplcrt_budget_startup", void_ty, {ptr_ty});

        // uplc_value uplcrt_make_lam(ptr b, ptr fn, ptr free, uint32_t nfree)
        fn_make_lam   = decl("uplcrt_make_lam",   wire_ty, {ptr_ty, ptr_ty, ptr_ty, i32_ty});
        fn_make_delay = decl("uplcrt_make_delay",  wire_ty, {ptr_ty, ptr_ty, ptr_ty, i32_ty});

        // uplc_value uplcrt_make_builtin(ptr b, uint8_t tag)
        // The runtime declaration uses uint8_t; we MUST match the bitcode
        // signature exactly (i8 zeroext) so the inliner can splice the
        // body into the call site. Any type widening here would block
        // inlining and silently regress perf.
        fn_make_builtin = decl("uplcrt_make_builtin", wire_ty, {ptr_ty, i8_ty});

        // uplc_value uplcrt_make_constr(ptr b, uint64_t tag, ptr fields, uint32_t n)
        fn_make_constr = decl("uplcrt_make_constr", wire_ty, {ptr_ty, i64_ty, ptr_ty, i32_ty});

        // uplc_value uplcrt_apply(uplc_value fn, uplc_value arg, ptr b)
        // Both uplc_value args and the return use wire_ty.
        fn_apply = decl("uplcrt_apply", wire_ty, {wire_ty, wire_ty, ptr_ty});

        // uplc_value uplcrt_force(uplc_value thunk, ptr b)
        fn_force = decl("uplcrt_force", wire_ty, {wire_ty, ptr_ty});

        // Cold-path variants used by emit_apply_inline / emit_force_inline.
        fn_apply_slow = decl("uplcrt_apply_slow", wire_ty, {wire_ty, wire_ty, ptr_ty});
        fn_force_slow = decl("uplcrt_force_slow", wire_ty, {wire_ty, ptr_ty});

        // Function-pointer types for the direct closure-body call inside
        // the inline fast path. Match the signatures emitted by
        // fill_closure_fn — wire_ty for value positions.
        lam_fn_ty = llvm::FunctionType::get(wire_ty,
                        {ptr_ty, wire_ty, ptr_ty}, false);
        delay_fn_ty = llvm::FunctionType::get(wire_ty,
                        {ptr_ty, ptr_ty}, false);

        // void uplcrt_case_decompose_out(ptr b, [2xi64] sc, i32 n,
        //                               ptr out_tag, ptr out_nf, ptr out_fields)
        fn_case_decompose_out = decl("uplcrt_case_decompose_out", void_ty,
                                     {ptr_ty, wire_ty, i32_ty,
                                      ptr_ty, ptr_ty, ptr_ty});

        // uplc_value uplcrt_apply_fields([2xi64] branch, ptr fields, i32 n, ptr b)
        fn_apply_fields = decl("uplcrt_apply_fields", wire_ty,
                               {wire_ty, ptr_ty, i32_ty, ptr_ty});

        // void __attribute__((noreturn)) uplcrt_fail(ptr b, i32 kind)
        fn_fail = decl("uplcrt_fail", void_ty, {ptr_ty, i32_ty});
        {
            auto* f = llvm::cast<llvm::Function>(fn_fail.getCallee());
            f->addFnAttr(llvm::Attribute::NoReturn);
        }

        // Constant constructors — all return uplc_value (wire_ty).
        fn_const_int_bytes = decl("uplcrt_const_int_bytes", wire_ty,
                                  {ptr_ty, i32_ty, ptr_ty, i32_ty});
        fn_const_bs_ref    = decl("uplcrt_const_bs_ref",    wire_ty, {ptr_ty, ptr_ty, i32_ty});
        fn_const_string_ref = decl("uplcrt_const_string_ref", wire_ty, {ptr_ty, ptr_ty, i32_ty});
        fn_const_bool = decl("uplcrt_const_bool", wire_ty, {ptr_ty, i32_ty});
        fn_const_unit = decl("uplcrt_const_unit", wire_ty, {ptr_ty});

        // uplc_value uplcrt_const_baked(ptr b, ptr blob, i32 len) — decodes
        // a self-describing baked-constant blob into a fully-formed VCon.
        fn_const_baked = decl("uplcrt_const_baked", wire_ty,
                              {ptr_ty, ptr_ty, i32_ty});

        // uplc_value uplcrt_run_builtin(ptr b, uint8_t tag, ptr argv, uint32_t argc)
        // Direct dispatch that bypasses the VBuiltin state machine. Called
        // from the saturated-builtin fast path in emit_term. The signature
        // matches runtime/builtin_dispatch.h exactly so the bitcode inliner
        // can splice it in.
        fn_run_builtin = decl("uplcrt_run_builtin", wire_ty,
                              {ptr_ty, i8_ty, ptr_ty, i32_ty});
    }

    // -----------------------------------------------------------------------
    // Constant baking helpers
    // -----------------------------------------------------------------------

    // Produce a private .rodata global whose contents are `data`.
    llvm::GlobalVariable* bake_bytes(const std::string& name,
                                      const std::uint8_t* data, std::size_t len) {
        auto* arr_ty = llvm::ArrayType::get(i8_ty, len);
        std::vector<llvm::Constant*> elems;
        elems.reserve(len);
        for (std::size_t i = 0; i < len; ++i) {
            elems.push_back(llvm::ConstantInt::get(i8_ty, data[i]));
        }
        auto* init = llvm::ConstantArray::get(arr_ty, elems);
        auto* gv   = new llvm::GlobalVariable(
            mod, arr_ty, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, init, name);
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        return gv;
    }

    llvm::GlobalVariable* bake_bytes_empty(const std::string& name) {
        // Zero-length byte array; some constants (e.g. empty bytestring) need this.
        auto* arr_ty = llvm::ArrayType::get(i8_ty, 1);
        auto* init   = llvm::ConstantAggregateZero::get(arr_ty);
        return new llvm::GlobalVariable(
            mod, arr_ty, true, llvm::GlobalValue::PrivateLinkage, init, name);
    }

    // Pre-scan all Constant terms in the site_table and bake their data into
    // module globals so we have stable pointers for the emit_term phase.
    void bake_constant_globals() {
        for (auto& [term, kind] : plan->site_table) {
            if (kind != StepKind::Const) continue;
            const Constant* c = term->constant.value;
            std::uint32_t cid = plan->const_ids.at(term);
            std::string prefix = "uplc_const_" + std::to_string(cid);

            switch (c->tag) {
            case ConstTag::Integer: {
                // Small ints (fit in long): emitted as LLVM struct
                // constants at every use site — no rodata global. Skip.
                if (int_fits_inline(*c)) break;

                // Big int: export magnitude as big-endian bytes.
                mpz_srcptr z = c->integer.value->value;
                if (mpz_sgn(z) == 0) {
                    // Zero: no bytes needed; runtime handles empty.
                    auto* gv = bake_bytes_empty(prefix + "_mag");
                    const_data_globals[term] = gv;
                } else {
                    std::size_t count = 0;
                    // Size in bytes: (bits + 7) / 8
                    std::size_t nbytes =
                        (mpz_sizeinbase(z, 2) + 7) / 8;
                    std::vector<std::uint8_t> buf(nbytes, 0);
                    mpz_export(buf.data(), &count, 1 /*MSB first*/, 1 /*byte*/,
                               0, 0, z);
                    // count may be smaller if leading zeros; pad from front.
                    if (count < nbytes) {
                        std::vector<std::uint8_t> padded(nbytes, 0);
                        std::memcpy(padded.data() + (nbytes - count),
                                    buf.data(), count);
                        buf = std::move(padded);
                        count = nbytes;
                    }
                    auto* gv = bake_bytes(prefix + "_mag", buf.data(), count);
                    const_data_globals[term] = gv;
                }
                break;
            }
            case ConstTag::ByteString: {
                std::size_t len = c->bytestring.len;
                if (len == 0) {
                    const_data_globals[term] = bake_bytes_empty(prefix + "_bs");
                } else {
                    auto* gv = bake_bytes(prefix + "_bs",
                                          c->bytestring.bytes, len);
                    const_data_globals[term] = gv;
                }
                break;
            }
            case ConstTag::String: {
                std::size_t len = c->string.len;
                if (len == 0) {
                    const_data_globals[term] = bake_bytes_empty(prefix + "_str");
                } else {
                    auto* gv = bake_bytes(prefix + "_str",
                        reinterpret_cast<const std::uint8_t*>(c->string.utf8), len);
                    const_data_globals[term] = gv;
                }
                break;
            }
            // Complex constants (Data, List, Pair, Array, BLS, Value) are
            // baked as a self-describing binary blob and decoded at runtime
            // by uplcrt_const_baked.
            case ConstTag::Data:
            case ConstTag::List:
            case ConstTag::Pair:
            case ConstTag::Array:
            case ConstTag::Bls12_381_G1:
            case ConstTag::Bls12_381_G2:
            case ConstTag::Bls12_381_MlResult:
            case ConstTag::Value: {
                std::vector<std::uint8_t> blob = serialize_baked_constant(*c);
                if (blob.empty()) {
                    /* defensive — Unit baking would land here too, but we
                     * already short-circuit Unit/Bool above. */
                    const_data_globals[term] = bake_bytes_empty(prefix + "_blob");
                } else {
                    auto* gv = bake_bytes(prefix + "_blob",
                                          blob.data(), blob.size());
                    const_data_globals[term] = gv;
                }
                break;
            }
            // Bool / Unit need no rodata.
            default:
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Allocate a per-execution mutable slot for every constant that requires
    // runtime construction. The slot is filled by the program_entry prologue
    // (see emit_const_prologue) and read by emit_constant. CEK gets the same
    // amortization for free because its rconstant trees live in the program
    // arena and persist across CONST steps.
    // -----------------------------------------------------------------------

    // Small-int constants are materialized as LLVM struct-constants at
    // every use site — no slot, no runtime call, no load. LLVM's
    // constant folder treats them as plain i64 immediates, so an
    // `addInteger (con integer 3) (con integer 5)` can constant-fold
    // to `(con integer 8)` after the runtime builtin is inlined.
    static bool int_fits_inline(const Constant& c) {
        return c.tag == ConstTag::Integer
            && mpz_fits_slong_p(c.integer.value->value)
            /* A GMP `long` is 64-bit on tier-1 targets, so this is the
             * right fit check. */;
    }

    bool needs_eager_slot(const Constant& c) const {
        switch (c.tag) {
            case ConstTag::Integer:
                // Inline ints are emitted as IR struct constants — no
                // per-execution slot, no prologue fill.
                return !int_fits_inline(c);
            case ConstTag::ByteString:
            case ConstTag::String:
            case ConstTag::Data:
            case ConstTag::List:
            case ConstTag::Pair:
            case ConstTag::Array:
            case ConstTag::Bls12_381_G1:
            case ConstTag::Bls12_381_G2:
            case ConstTag::Bls12_381_MlResult:
            case ConstTag::Value:
                return true;
            // Bool/Unit are immediate-payload values; no point caching.
            case ConstTag::Bool:
            case ConstTag::Unit:
                return false;
        }
        return false;
    }

    void allocate_const_slots() {
        for (auto& [term, kind] : plan->site_table) {
            if (kind != StepKind::Const) continue;
            const Constant* c = term->constant.value;
            if (!needs_eager_slot(*c)) continue;

            std::uint32_t cid = plan->const_ids.at(term);
            std::string name = "uplc_const_" + std::to_string(cid) + "_slot";

            // Initialised to all-zeros (tag = UPLC_V_CON, payload = NULL).
            // The prologue overwrites it with the real decoded value before
            // any user code runs, so the zero value is never observed.
            auto* zero = llvm::ConstantAggregateZero::get(value_ty);
            auto* gv = new llvm::GlobalVariable(
                mod, value_ty, /*isConstant=*/false,
                llvm::GlobalValue::PrivateLinkage,
                zero, name);
            // Slots are mutated on every program_entry call, so they
            // must NOT be in a constant section. Default linkage is fine.
            const_value_slots[term] = gv;
        }
    }

    // -----------------------------------------------------------------------
    // Forward-declare all closure body functions (so creation sites can
    // reference them before the body is emitted).
    //
    // All functions use wire_ty ([2 x i64]) for uplc_value positions so the
    // generated code matches Clang's C calling convention on ARM64.
    // -----------------------------------------------------------------------

    void forward_declare_closures() {
        for (auto& [term, kind] : plan->site_table) {
            bool is_lam   = (kind == StepKind::Lambda);
            bool is_delay = (kind == StepKind::Delay);
            if (!is_lam && !is_delay) continue;

            std::uint32_t cid = plan->closure_ids.at(term);
            std::string name  = closure_fn_name(cid);

            llvm::FunctionType* fty;
            if (is_lam) {
                // [2 x i64] fn(ptr env, [2 x i64] arg, ptr b)
                fty = llvm::FunctionType::get(wire_ty,
                          {ptr_ty, wire_ty, ptr_ty}, false);
            } else {
                // [2 x i64] fn(ptr env, ptr b)
                fty = llvm::FunctionType::get(wire_ty,
                          {ptr_ty, ptr_ty}, false);
            }
            auto* fn = llvm::Function::Create(
                fty, llvm::Function::InternalLinkage, name, mod);
            closure_fns[cid] = fn;
        }
    }

    // -----------------------------------------------------------------------
    // Saturated-builtin recognizer.
    //
    // Matches terms of the shape
    //     [ [ ... [ (force^N (builtin TAG)) arg_1 ] arg_2 ] ... arg_arity ]
    // where N == force_count(TAG) and arity == arity(TAG). These are the
    // vast majority of builtin call sites in real UPLC — everything that
    // the CEK machine would saturate on the spot, which is basically
    // everything except a bare "(builtin X)" value passed around.
    //
    // Returns the recognized builtin tag and writes the outer-to-inner
    // argument list to `out_args` in normal left-to-right order. The list
    // is empty for nullary builtins. Returns std::nullopt when the shape
    // doesn't match.
    // -----------------------------------------------------------------------

    struct SaturatedBuiltin {
        BuiltinTag             tag;
        std::vector<const Term*> args;  // in call order, left to right
    };

    std::optional<SaturatedBuiltin>
    match_saturated_builtin(const Term* term) const {
        // Walk Apply chain outermost-first, collecting arguments right-to-left.
        std::vector<const Term*> args_rev;
        const Term* cur = term;
        while (cur->tag == TermTag::Apply) {
            args_rev.push_back(cur->apply.argument);
            cur = cur->apply.function;
        }
        // Peel Force wrappers.
        std::uint32_t forces = 0;
        while (cur->tag == TermTag::Force) {
            ++forces;
            cur = cur->force.term;
        }
        if (cur->tag != TermTag::Builtin) return std::nullopt;

        BuiltinTag tag = cur->builtin.function;
        if (forces != builtin_force_count(tag)) return std::nullopt;
        if (args_rev.size() != builtin_arity(tag)) return std::nullopt;

        SaturatedBuiltin out;
        out.tag = tag;
        out.args.reserve(args_rev.size());
        for (auto it = args_rev.rbegin(); it != args_rev.rend(); ++it) {
            out.args.push_back(*it);
        }
        return out;
    }

    // Emit a direct call to uplcrt_run_builtin, skipping the VBuiltin
    // state machine. The caller must have already verified saturation.
    //
    // Preserves budget parity with the unspecialized path:
    //     STEP_BUILTIN  (one per call)
    //     STEP_FORCE    (one per force in the source)
    //     STEP_APPLY    (one per value argument)
    //     builtin cost  (charged inside uplcrt_run_builtin)
    llvm::Value* emit_saturated_builtin(llvm::IRBuilder<>& B,
                                        const SaturatedBuiltin& sb,
                                        llvm::Value* env,
                                        llvm::Value* arg,
                                        llvm::Value* budget) {
        std::uint32_t n_forces = builtin_force_count(sb.tag);
        std::uint32_t n_args   = static_cast<std::uint32_t>(sb.args.size());

        // STEP_BUILTIN (for the builtin itself).
        emit_budget_step(B, budget, STEP_BUILTIN);

        // One STEP_FORCE per force in the source.
        for (std::uint32_t i = 0; i < n_forces; ++i) {
            emit_budget_step(B, budget, STEP_FORCE);
        }

        // Allocate an argv array on the stack and evaluate each argument
        // into it. STEP_APPLY is charged per argument to match the
        // unspecialized path (which goes through uplcrt_apply once per
        // value arg).
        llvm::Value* argv = nullptr;
        if (n_args > 0) {
            argv = emit_value_array(B, "satb_argv", n_args);
            for (std::uint32_t i = 0; i < n_args; ++i) {
                emit_budget_step(B, budget, STEP_APPLY);
                llvm::Value* v = emit_term(B, sb.args[i], env, arg, budget);
                llvm::Value* slot = B.CreateConstGEP1_32(value_ty, argv, i);
                B.CreateStore(v, slot);
            }
        } else {
            argv = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptr_ty));
        }

        auto tag_val = llvm::ConstantInt::get(i8_ty,
            static_cast<std::uint8_t>(sb.tag));
        auto argc_val = llvm::ConstantInt::get(i32_ty, n_args);

        /* argv is a local alloca — tail-calling would leave the
         * runtime reading freed stack memory. Keep this as a normal
         * call. */
        llvm::Value* wire = B.CreateCall(fn_run_builtin,
            {budget, tag_val, argv, argc_val}, "satb_wire");
        return from_wire(B, wire, "satb");
    }

    // -----------------------------------------------------------------------
    // N-arity beta-redex recognizer.
    //
    // Matches an apply chain whose head is a chain of nested Lambdas:
    //
    //     [[[ (lam v0 (lam v1 (lam v2 body))) arg0 ] arg1 ] arg2 ]
    //
    // Every Plutus program built from curried let-bindings looks like
    // this; the textbook compiled path bounces off uplcrt_apply between
    // each level (call lam_v0_body_fn → returns lam_v1 closure → unwrap →
    // call lam_v1_body_fn → returns lam_v2 closure → unwrap → call
    // lam_v2_body_fn). With this recognizer we collapse the whole chain
    // into one direct emission.
    //
    // Returns the matched lambdas (outermost first), the matched args
    // (outermost first), and the innermost body. `n_levels` is
    // min(|lams|, |args|) — the chain depth that actually got matched.
    // -----------------------------------------------------------------------
    struct NArityBeta {
        std::vector<const Term*> lams;          // outermost lambda first
        std::vector<const Term*> args;          // outermost arg first; size == n_levels
        std::vector<const Term*> leftover_args; // applies BEYOND the matched chain, applied to the body's result
        std::uint32_t            n_levels;
        const Term*              tail_body;     // body of the innermost matched lambda
    };

    std::optional<NArityBeta> match_narity_beta(const Term* term) const {
        /* Walk the apply chain outermost-first to collect arguments.
         * args_walk[0] is the OUTERMOST arg. */
        std::vector<const Term*> args_walk;
        const Term* cur = term;
        while (cur->tag == TermTag::Apply) {
            args_walk.push_back(cur->apply.argument);
            cur = cur->apply.function;
        }
        if (cur->tag != TermTag::Lambda) return std::nullopt;
        if (args_walk.empty()) return std::nullopt;

        /* args_walk is currently outermost-first: [arg_outermost, ..., arg_innermost].
         * The applies are LEFT-LEANING, so the INNERMOST arg is applied
         * FIRST (to the lambda chain), and the outermost arg is applied
         * LAST (to the result of all the inner reductions).
         * Reverse to evaluation order. */
        std::vector<const Term*> args_eval(args_walk.rbegin(), args_walk.rend());

        /* Walk the lambda chain matching one arg per lambda. */
        std::vector<const Term*> lams_outer_first;
        const Term* l = cur;
        while (l->tag == TermTag::Lambda
               && lams_outer_first.size() < args_eval.size()) {
            lams_outer_first.push_back(l);
            l = l->lambda.body;
        }
        if (lams_outer_first.empty()) return std::nullopt;

        NArityBeta out;
        out.n_levels  = static_cast<std::uint32_t>(lams_outer_first.size());
        out.tail_body = l;  // body of the innermost matched lambda
        out.lams      = std::move(lams_outer_first);
        out.args.assign(args_eval.begin(),
                        args_eval.begin() + out.n_levels);
        out.leftover_args.assign(args_eval.begin() + out.n_levels,
                                  args_eval.end());
        return out;
    }

    /*
     * Emit an n-arity beta reduction.
     *
     * Walks the matched lambda+arg pairs from outermost to innermost. At
     * each level we:
     *   1. Charge STEP_APPLY + STEP_LAMBDA (matches the unspecialized
     *      path which charges these via emit_term recursion).
     *   2. Compute the level's `env` array using the current env / arg
     *      and the level's closure plan.
     *   3. Evaluate the level's argument expression in the OUTER scope
     *      (i.e. before we install the new env).
     *   4. Slide env/arg forward for the next iteration.
     *
     * After all levels are reduced we have an env corresponding to the
     * innermost matched lambda's captures and an arg corresponding to its
     * bound variable. We then emit `tail_body` directly into the current
     * frame using those — exactly mirroring what fill_closure_fn would
     * have done for the innermost lambda's body_fn, but inlined.
     *
     * If there are MORE applies in the source than lambdas we matched,
     * the leftover applies are folded back in as normal Apply terms over
     * the result.
     */
    llvm::Value* emit_narity_beta(llvm::IRBuilder<>& B,
                                  const NArityBeta& nb,
                                  llvm::Value* env,
                                  llvm::Value* arg,
                                  llvm::Value* budget) {
        /*
         * Step 1: evaluate every matched argument in the OUTER scope.
         * The args live in the syntax tree as direct children of the
         * apply chain, so their free-var indices resolve against (env,
         * arg) — NOT against any of the lambda body scopes we're about
         * to descend into.
         *
         * Charge STEP_APPLY per arg up front, matching the recursive
         * walk the unspecialized path would do.
         */
        std::vector<llvm::Value*> arg_vals;
        arg_vals.reserve(nb.n_levels);
        for (std::uint32_t i = 0; i < nb.n_levels; ++i) {
            emit_budget_step(B, budget, STEP_APPLY);
            arg_vals.push_back(emit_term(B, nb.args[i], env, arg, budget));
        }

        /*
         * Step 2: walk the lambda chain, sliding (cur_env, cur_arg)
         * through each lambda's body scope. At each level:
         *   - charge STEP_LAMBDA
         *   - build that lambda's captures from the CURRENT body scope
         *   - bind cur_arg = the corresponding pre-evaluated arg
         *   - cur_env = the captures we just built (the lambda's body's env)
         *
         * Crucially the arg is taken from arg_vals (outer scope), NOT
         * re-evaluated in the descending scope.
         */
        llvm::Value* cur_env = env;
        llvm::Value* cur_arg = arg;
        for (std::uint32_t i = 0; i < nb.n_levels; ++i) {
            emit_budget_step(B, budget, STEP_LAMBDA);

            const auto& cp = plan->closure_plans.at(nb.lams[i]);
            std::uint32_t n = static_cast<std::uint32_t>(cp.captures.size());
            llvm::Value* lam_env = emit_capture_array(B, cp, cur_env, cur_arg, n);

            cur_env = lam_env;
            cur_arg = arg_vals[i];
        }

        /* Step 3: emit the innermost matched lambda's body inline using
         * the innermost (cur_env, cur_arg). */
        llvm::Value* result = emit_term(B, nb.tail_body, cur_env, cur_arg, budget);

        /* Step 4: leftover applies on top of the body result. These run
         * in the OUTER scope. */
        for (const Term* leftover_arg : nb.leftover_args) {
            emit_budget_step(B, budget, STEP_APPLY);
            llvm::Value* arg_val = emit_term(B, leftover_arg, env, arg, budget);
            result = emit_apply_inline(B, result, arg_val, budget);
        }
        return result;
    }

    // -----------------------------------------------------------------------
    // Core term emitter — emits instructions into the current BB of B.
    // Returns an llvm::Value* of type value_ty.
    //
    //   env    — ptr to uplc_value[] (the closure's free-var array), or
    //            llvm::ConstantPointerNull for the top-level entry scope.
    //   arg    — uplc_value by value (only valid inside a lambda body), or
    //            llvm::UndefValue for delay / entry bodies.
    //   budget — ptr to uplc_budget.
    // -----------------------------------------------------------------------

    llvm::Value* emit_term(llvm::IRBuilder<>& B,
                           const Term* term,
                           llvm::Value* env,
                           llvm::Value* arg,
                           llvm::Value* budget) {

        switch (term->tag) {

        // ------------------------------------------------------------------ Var
        case TermTag::Var: {
            emit_budget_step(B, budget, STEP_VAR);

            const auto& vr = plan->var_resolutions.at(term);
            if (vr.origin == VarOrigin::Arg) {
                return arg;
            }
            // Capture: env[slot]
            llvm::Value* gep = B.CreateConstGEP1_32(value_ty, env, vr.slot);
            return B.CreateLoad(value_ty, gep, "capture");
        }

        // ------------------------------------------------------------------ Lambda
        case TermTag::Lambda: {
            emit_budget_step(B, budget, STEP_LAMBDA);

            const auto& cp  = plan->closure_plans.at(term);
            std::uint32_t n = static_cast<std::uint32_t>(cp.captures.size());
            std::uint32_t cid = plan->closure_ids.at(term);
            llvm::Function* body_fn = closure_fns.at(cid);

            llvm::Value* free_arr = emit_capture_array(B, cp, env, arg, n);
            llvm::Value* wire = B.CreateCall(fn_make_lam,
                {budget, body_fn, free_arr,
                 llvm::ConstantInt::get(i32_ty, n)}, "lam_wire");
            return from_wire(B, wire, "lam");
        }

        // ------------------------------------------------------------------ Delay
        case TermTag::Delay: {
            emit_budget_step(B, budget, STEP_DELAY);

            const auto& cp  = plan->closure_plans.at(term);
            std::uint32_t n = static_cast<std::uint32_t>(cp.captures.size());
            std::uint32_t cid = plan->closure_ids.at(term);
            llvm::Function* body_fn = closure_fns.at(cid);

            llvm::Value* free_arr = emit_capture_array(B, cp, env, arg, n);
            llvm::Value* wire = B.CreateCall(fn_make_delay,
                {budget, body_fn, free_arr,
                 llvm::ConstantInt::get(i32_ty, n)}, "delay_wire");
            return from_wire(B, wire, "delay");
        }

        // ------------------------------------------------------------------ Apply
        case TermTag::Apply: {
            // Fast path 1: saturated builtin call. Bypass make_builtin +
            // the state machine and go straight to uplcrt_run_builtin.
            if (auto sb = match_saturated_builtin(term)) {
                return emit_saturated_builtin(B, *sb, env, arg, budget);
            }

            // Fast path 2: n-arity beta-redex — see emit_narity_beta.
            // Catches curried let-binding chains like
            //   [[[(lam v0 (lam v1 (lam v2 body))) arg0] arg1] arg2]
            // and beta-reduces all matching levels in one pass, instead
            // of bouncing off uplcrt_apply between each level.
            if (auto narity = match_narity_beta(term)) {
                if (narity->n_levels >= 2) {
                    return emit_narity_beta(B, *narity, env, arg, budget);
                }
            }

            // Fast path 3: beta-redex — the function subterm is a Lambda
            // literal. Skip make_lam + apply and call the closure body
            // function directly with a stack-allocated capture array.
            //
            // Budget parity is preserved: STEP_APPLY and STEP_LAMBDA are
            // both charged up front, the capture array is built on the
            // stack (just like the unspecialized path), the argument is
            // evaluated, and the body is invoked directly.
            if (term->apply.function->tag == TermTag::Lambda) {
                const Term* lam = term->apply.function;

                emit_budget_step(B, budget, STEP_APPLY);
                emit_budget_step(B, budget, STEP_LAMBDA);

                const auto& cp  = plan->closure_plans.at(lam);
                std::uint32_t n = static_cast<std::uint32_t>(cp.captures.size());
                std::uint32_t cid = plan->closure_ids.at(lam);
                llvm::Function* body_fn = closure_fns.at(cid);

                llvm::Value* free_arr = emit_capture_array(B, cp, env, arg, n);

                llvm::Value* arg_val = emit_term(B, term->apply.argument,
                                                 env, arg, budget);

                // body_fn: [2 x i64] fn(ptr env, [2 x i64] arg, ptr b)
                // free_arr points into our caller's alloca — can't tail.
                llvm::Value* wire = B.CreateCall(body_fn,
                    {free_arr, to_wire(B, arg_val), budget}, "beta_wire");
                return from_wire(B, wire, "beta");
            }

            emit_budget_step(B, budget, STEP_APPLY);

            llvm::Value* fn_val  = emit_term(B, term->apply.function, env, arg, budget);
            llvm::Value* arg_val = emit_term(B, term->apply.argument, env, arg, budget);
            return emit_apply_inline(B, fn_val, arg_val, budget);
        }

        // ------------------------------------------------------------------ Force
        case TermTag::Force: {
            // Fast path: force of delay. Bypass make_delay + uplcrt_force
            // and call the delay body directly with a stack-allocated
            // capture array.
            if (term->force.term->tag == TermTag::Delay) {
                const Term* del = term->force.term;

                emit_budget_step(B, budget, STEP_FORCE);
                emit_budget_step(B, budget, STEP_DELAY);

                const auto& cp  = plan->closure_plans.at(del);
                std::uint32_t n = static_cast<std::uint32_t>(cp.captures.size());
                std::uint32_t cid = plan->closure_ids.at(del);
                llvm::Function* body_fn = closure_fns.at(cid);

                llvm::Value* free_arr = emit_capture_array(B, cp, env, arg, n);

                // body_fn: [2 x i64] fn(ptr env, ptr b)
                // free_arr points into our caller's alloca — can't tail.
                llvm::Value* wire = B.CreateCall(body_fn,
                    {free_arr, budget}, "forcedelay_wire");
                return from_wire(B, wire, "forcedelay");
            }

            emit_budget_step(B, budget, STEP_FORCE);

            llvm::Value* thunk = emit_term(B, term->force.term, env, arg, budget);
            return emit_force_inline(B, thunk, budget);
        }

        // ------------------------------------------------------------------ Constant
        case TermTag::Constant: {
            emit_budget_step(B, budget, STEP_CONST);

            return emit_constant(B, term, budget);
        }

        // ------------------------------------------------------------------ Builtin
        case TermTag::Builtin: {
            emit_budget_step(B, budget, STEP_BUILTIN);

            // Pass the tag as i8 so the call signature exactly matches the
            // bitcode's `uplc_builtin_tag` (uint8_t). A type mismatch here
            // blocks the inliner and the runtime call stays opaque.
            auto tag = static_cast<std::uint8_t>(term->builtin.function);
            llvm::Value* wire = B.CreateCall(fn_make_builtin,
                {budget, llvm::ConstantInt::get(i8_ty, tag)}, "builtin_wire");
            return from_wire(B, wire, "builtin");
        }

        // ------------------------------------------------------------------ Constr
        case TermTag::Constr: {
            emit_budget_step(B, budget, STEP_CONSTR);

            std::uint32_t nf = term->constr.n_fields;
            llvm::Value* fields_arr = emit_value_array(B, "constr_fields", nf);
            for (std::uint32_t i = 0; i < nf; ++i) {
                llvm::Value* fv = emit_term(B, term->constr.fields[i], env, arg, budget);
                llvm::Value* slot = B.CreateConstGEP1_32(value_ty, fields_arr, i);
                B.CreateStore(fv, slot);
            }
            llvm::Value* wire = B.CreateCall(fn_make_constr,
                {budget,
                 llvm::ConstantInt::get(i64_ty, term->constr.tag_index),
                 fields_arr,
                 llvm::ConstantInt::get(i32_ty, nf)}, "constr_wire");
            return from_wire(B, wire, "constr");
        }

        // ------------------------------------------------------------------ Case
        case TermTag::Case: {
            emit_budget_step(B, budget, STEP_CASE);

            llvm::Value* sc = emit_term(B, term->case_.scrutinee, env, arg, budget);

            std::uint32_t nb = term->case_.n_branches;

            // Decompose the scrutinee — raises on invalid input so no branch
            // is ever reached with an out-of-range tag.
            llvm::Value* out_tag    = B.CreateAlloca(i64_ty, nullptr, "case_tag");
            llvm::Value* out_nf     = B.CreateAlloca(i32_ty, nullptr, "case_nf");
            llvm::Value* out_fields = B.CreateAlloca(ptr_ty, nullptr, "case_flds");
            B.CreateCall(fn_case_decompose_out,
                {budget, to_wire(B, sc),
                 llvm::ConstantInt::get(i32_ty, nb),
                 out_tag, out_nf, out_fields});
            llvm::Value* tag    = B.CreateLoad(i64_ty, out_tag,    "tag");
            llvm::Value* n_flds = B.CreateLoad(i32_ty, out_nf,     "n_flds");
            llvm::Value* flds   = B.CreateLoad(ptr_ty,  out_fields, "flds");

            // One alloca for the wire result; whichever branch runs writes it.
            llvm::Value* result_slot = B.CreateAlloca(wire_ty, nullptr, "case_result");

            llvm::Function* cur_fn   = B.GetInsertBlock()->getParent();
            auto* bb_default = llvm::BasicBlock::Create(ctx, "case_default", cur_fn);
            auto* bb_merge   = llvm::BasicBlock::Create(ctx, "case_merge",   cur_fn);

            llvm::SwitchInst* sw = B.CreateSwitch(tag, bb_default,
                                                   static_cast<unsigned>(nb));

            // Emit one BB per branch; only the selected branch is evaluated
            // (preserving the lazy semantics of CEK case).
            for (std::uint32_t i = 0; i < nb; ++i) {
                auto* bb_i = llvm::BasicBlock::Create(
                    ctx, "case_br_" + std::to_string(i), cur_fn);
                sw->addCase(
                    llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(i64_ty),
                                           static_cast<std::uint64_t>(i)),
                    bb_i);

                B.SetInsertPoint(bb_i);
                llvm::Value* bv = emit_term(B, term->case_.branches[i],
                                            env, arg, budget);
                llvm::Value* res = B.CreateCall(fn_apply_fields,
                    {to_wire(B, bv), flds, n_flds, budget},
                    "br_res_" + std::to_string(i));
                B.CreateStore(res, result_slot);
                B.CreateBr(bb_merge);
            }

            // Unreachable: decompose already verified tag < n_branches.
            B.SetInsertPoint(bb_default);
            B.CreateUnreachable();

            B.SetInsertPoint(bb_merge);
            llvm::Value* wire = B.CreateLoad(wire_ty, result_slot, "case_wire");
            return from_wire(B, wire, "case");
        }

        // ------------------------------------------------------------------ Error
        case TermTag::Error: {
            B.CreateCall(fn_fail,
                {budget, llvm::ConstantInt::get(i32_ty, FAIL_EVALUATION)});
            B.CreateUnreachable();
            // Move to a dead successor so callers (which always emit `ret`)
            // don't append instructions after the terminator.  Dead BBs are
            // valid LLVM IR and are removed by the first DCE pass.
            auto* fn      = B.GetInsertBlock()->getParent();
            auto* dead_bb = llvm::BasicBlock::Create(ctx, "dead", fn);
            B.SetInsertPoint(dead_bb);
            return llvm::UndefValue::get(value_ty);
        }
        }

        throw std::runtime_error("llvm_codegen: unknown TermTag");
    }

    // -----------------------------------------------------------------------
    // Helpers used by emit_term
    // -----------------------------------------------------------------------

    // Allocate an N-element uplc_value array on the stack.
    llvm::Value* emit_value_array(llvm::IRBuilder<>& B,
                                  const char* name,
                                  std::uint32_t n) {
        if (n == 0) {
            // Return a null pointer — the callee checks nfree/n.
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr_ty));
        }
        auto* arr_ty = llvm::ArrayType::get(value_ty, n);
        llvm::Value* arr = B.CreateAlloca(arr_ty, nullptr, name);
        // Bitcast not needed in opaque-pointer IR; GEP from arr directly.
        return arr;
    }

    // Emit capture array: evaluate each capture source and store into an
    // N-element stack array.  Returns ptr to the array (null if N==0).
    llvm::Value* emit_capture_array(llvm::IRBuilder<>& B,
                                     const ClosurePlan& cp,
                                     llvm::Value* env,
                                     llvm::Value* arg,
                                     std::uint32_t n) {
        llvm::Value* arr = emit_value_array(B, "caps", n);
        for (std::uint32_t i = 0; i < n; ++i) {
            const CaptureSource& src = cp.captures[i];
            llvm::Value* val;
            if (src.origin == VarOrigin::Arg) {
                val = arg;
            } else {
                llvm::Value* gep = B.CreateConstGEP1_32(value_ty, env, src.slot);
                val = B.CreateLoad(value_ty, gep, "capt_src");
            }
            // Store into caps[i]
            llvm::Value* dst = B.CreateConstGEP1_32(value_ty, arr, i);
            B.CreateStore(val, dst);
        }
        return arr;
    }

    // Build a constant value via the appropriate runtime helper. This is
    // the "expensive" path — runs the actual decode / GMP import / arena
    // allocation, etc. Used by:
    //   - emit_const_prologue, to fill the eager-build cache slot once
    //   - emit_constant for Bool / Unit (which are immediate and don't
    //     bother with a slot)
    llvm::Value* build_constant_value(llvm::IRBuilder<>& B,
                                       const Term* term,
                                       llvm::Value* budget) {
        const Constant* c = term->constant.value;

        switch (c->tag) {

        case ConstTag::Integer: {
            // Small-int fast path: build the uplc_value struct as a pure
            // LLVM constant. No runtime call, no memory, no cache slot.
            // LLVM's optimizer can constant-propagate it through subsequent
            // builtin calls (after the runtime bitcode is inlined).
            if (int_fits_inline(*c)) {
                long iv = mpz_get_si(c->integer.value->value);
                auto* tag_k    = llvm::ConstantInt::get(i8_ty,
                                    /*UPLC_V_CON*/ 0);
                auto* subtag_k = llvm::ConstantInt::get(i8_ty,
                                    /*UPLC_VCON_INT_INLINE*/ 0x80);
                auto* pad_ty   = llvm::ArrayType::get(i8_ty, 6);
                auto* pad_k    = llvm::ConstantAggregateZero::get(pad_ty);
                auto* payload_k = llvm::ConstantInt::getSigned(i64_ty,
                                    static_cast<int64_t>(iv));
                return llvm::ConstantStruct::get(
                    llvm::cast<llvm::StructType>(value_ty),
                    {tag_k, subtag_k, pad_k, payload_k});
            }

            auto* gv = const_data_globals.at(term);
            mpz_srcptr z = c->integer.value->value;
            int negative  = (mpz_sgn(z) < 0) ? 1 : 0;
            std::size_t nbytes =
                llvm::cast<llvm::ArrayType>(gv->getValueType())->getNumElements();
            if (mpz_sgn(z) == 0) nbytes = 0;
            llvm::Value* wire = B.CreateCall(fn_const_int_bytes,
                {budget,
                 llvm::ConstantInt::get(i32_ty, negative),
                 gv,
                 llvm::ConstantInt::get(i32_ty, static_cast<uint32_t>(nbytes))},
                "cint_wire");
            return from_wire(B, wire, "const_int");
        }

        case ConstTag::ByteString: {
            auto* gv = const_data_globals.at(term);
            std::uint32_t len = c->bytestring.len;
            llvm::Value* wire = B.CreateCall(fn_const_bs_ref,
                {budget, gv, llvm::ConstantInt::get(i32_ty, len)}, "cbs_wire");
            return from_wire(B, wire, "const_bs");
        }

        case ConstTag::String: {
            auto* gv = const_data_globals.at(term);
            std::uint32_t len = c->string.len;
            llvm::Value* wire = B.CreateCall(fn_const_string_ref,
                {budget, gv, llvm::ConstantInt::get(i32_ty, len)}, "cstr_wire");
            return from_wire(B, wire, "const_str");
        }

        case ConstTag::Bool: {
            llvm::Value* wire = B.CreateCall(fn_const_bool,
                {budget,
                 llvm::ConstantInt::get(i32_ty, c->boolean.value ? 1 : 0)},
                "cbool_wire");
            return from_wire(B, wire, "const_bool");
        }

        case ConstTag::Unit: {
            llvm::Value* wire = B.CreateCall(fn_const_unit, {budget}, "cunit_wire");
            return from_wire(B, wire, "const_unit");
        }

        case ConstTag::Data:
        case ConstTag::List:
        case ConstTag::Pair:
        case ConstTag::Array:
        case ConstTag::Bls12_381_G1:
        case ConstTag::Bls12_381_G2:
        case ConstTag::Bls12_381_MlResult:
        case ConstTag::Value: {
            auto* gv = const_data_globals.at(term);
            std::size_t nbytes =
                llvm::cast<llvm::ArrayType>(gv->getValueType())->getNumElements();
            llvm::Value* wire = B.CreateCall(fn_const_baked,
                {budget, gv,
                 llvm::ConstantInt::get(i32_ty, static_cast<uint32_t>(nbytes))},
                "cbaked_wire");
            return from_wire(B, wire, "const_baked");
        }
        }
        // Unreachable — every ConstTag is handled above.
        B.CreateCall(fn_fail,
            {budget, llvm::ConstantInt::get(i32_ty, FAIL_MACHINE)});
        B.CreateUnreachable();
        auto* fn_ptr = B.GetInsertBlock()->getParent();
        auto* dead   = llvm::BasicBlock::Create(ctx, "dead_build", fn_ptr);
        B.SetInsertPoint(dead);
        return llvm::UndefValue::get(value_ty);
    }

    // Hot-path constant emission: for cached constants (anything that has
    // a slot), this is just a `load slot`. For the cheap immediate-payload
    // constants (Bool/Unit), we skip the slot and inline the construction
    // directly.
    llvm::Value* emit_constant(llvm::IRBuilder<>& B,
                                const Term* term,
                                llvm::Value* budget) {
        auto it = const_value_slots.find(term);
        if (it != const_value_slots.end()) {
            return B.CreateLoad(value_ty, it->second, "const_cached");
        }
        // No slot: cheap inline path (Bool, Unit).
        return build_constant_value(B, term, budget);
    }

    // -----------------------------------------------------------------------
    // Fill in a closure body function.
    // -----------------------------------------------------------------------

    void fill_closure_fn(const Term* closure_term) {
        bool is_lam = (closure_term->tag == TermTag::Lambda);
        std::uint32_t cid = plan->closure_ids.at(closure_term);
        llvm::Function* fn = closure_fns.at(cid);

        auto* bb = llvm::BasicBlock::Create(ctx, "entry", fn);
        llvm::IRBuilder<> B(bb);

        // Parameter layout (all value positions use wire_ty in the signature):
        //   Lambda: (ptr env, [2 x i64] arg_wire, ptr budget)
        //   Delay:  (ptr env, ptr budget)
        auto it = fn->arg_begin();
        llvm::Value* env_param = &*it++;
        env_param->setName("env");

        llvm::Value* arg_param;   // value_ty (internal)
        llvm::Value* budget_param;
        if (is_lam) {
            llvm::Value* arg_wire = &*it++;
            arg_wire->setName("arg_wire");
            // Convert wire ABI type to internal value_ty for use in emit_term.
            arg_param    = from_wire(B, arg_wire, "arg");
            budget_param = &*it;
            budget_param->setName("b");
        } else {
            arg_param    = llvm::UndefValue::get(value_ty);
            budget_param = &*it;
            budget_param->setName("b");
        }

        const Term* body = is_lam ? closure_term->lambda.body
                                   : closure_term->delay.term;

        llvm::Value* result = emit_term(B, body, env_param, arg_param, budget_param);
        // Return via wire_ty to match the Clang C ABI.
        B.CreateRet(to_wire(B, result));
    }

    // -----------------------------------------------------------------------
    // Emit the top-level entry function:
    //   [2 x i64] program_entry(ptr b)
    //
    // The C caller (uplcrt_run_compiled) uses the Clang calling convention for
    // uplc_value (x0:x1 register pair), so we use wire_ty here.
    // -----------------------------------------------------------------------

    // Emit the Plutus version (major/minor/patch) as three i32 globals so
    // the static runner stub can format the result with the right
    // (program X.Y.Z ...) wrapper without re-parsing the source.
    void emit_version_globals() {
        auto def = [&](const char* name, std::uint32_t v) {
            auto* gv = new llvm::GlobalVariable(
                mod, i32_ty, /*isConstant=*/true,
                llvm::GlobalValue::ExternalLinkage,
                llvm::ConstantInt::get(i32_ty, v),
                name);
            gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
        };
        def("uplc_program_version_major", prog->version.major);
        def("uplc_program_version_minor", prog->version.minor);
        def("uplc_program_version_patch", prog->version.patch);
    }

    void emit_entry_fn() {
        auto* fty = llvm::FunctionType::get(wire_ty, {ptr_ty}, false);
        auto* fn  = llvm::Function::Create(
            fty, llvm::Function::ExternalLinkage, "program_entry", mod);

        auto* bb = llvm::BasicBlock::Create(ctx, "entry", fn);
        llvm::IRBuilder<> B(bb);

        llvm::Value* budget = &*fn->arg_begin();
        budget->setName("b");

        // One-time startup cost matching the CEK machine's initial charge.
        B.CreateCall(fn_budget_startup, {budget});

        // Eagerly build every cached constant into its slot. After this
        // block, every CONST step in the body becomes a single load from
        // the precomputed slot — no decode / GMP import / arena alloc on
        // the hot path. CEK gets this for free because its rconstant
        // trees are built once at lower time and never re-built.
        emit_const_prologue(B, budget);

        // Top-level scope: env = null (no captures), no arg.
        llvm::Value* null_env = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptr_ty));
        llvm::Value* undef_arg = llvm::UndefValue::get(value_ty);

        llvm::Value* result = emit_term(B, prog->term, null_env, undef_arg, budget);
        B.CreateRet(to_wire(B, result));
    }

    // Fill every cached-constant slot exactly once at program_entry start.
    // No STEP_CONST charge here — the prologue is bookkeeping, not user
    // evaluation. STEP_CONST is still charged per term reach by emit_term.
    void emit_const_prologue(llvm::IRBuilder<>& B, llvm::Value* budget) {
        for (auto& [term, kind] : plan->site_table) {
            if (kind != StepKind::Const) continue;
            auto it = const_value_slots.find(term);
            if (it == const_value_slots.end()) continue;
            llvm::Value* val = build_constant_value(B, term, budget);
            B.CreateStore(val, it->second);
        }
    }

    // -----------------------------------------------------------------------
    // Top-level driver
    // -----------------------------------------------------------------------

    void run() {
        setup_types();
        declare_runtime();
        emit_version_globals();
        bake_constant_globals();
        allocate_const_slots();
        forward_declare_closures();

        // Fill all closure bodies.
        for (auto& [term, kind] : plan->site_table) {
            if (kind == StepKind::Lambda || kind == StepKind::Delay) {
                fill_closure_fn(term);
            }
        }

        emit_entry_fn();

        // Verify the module; throw on internal IR errors.
        std::string err;
        llvm::raw_string_ostream es(err);
        if (llvm::verifyModule(mod, &es)) {
            throw std::runtime_error("llvm_codegen: IR verification failed:\n" + err);
        }
    }
};  // struct Emitter

}  // namespace

// ---------------------------------------------------------------------------
// LlvmCodegen public API
// ---------------------------------------------------------------------------

LlvmCodegen::LlvmCodegen(llvm::LLVMContext& ctx, const std::string& module_name)
    : ctx_(ctx),
      mod_(std::make_unique<llvm::Module>(module_name, ctx)) {}

LlvmCodegen::~LlvmCodegen() = default;

void LlvmCodegen::emit(const Program& program, const CompilePlan& plan) {
    if (!program.is_debruijn) {
        throw std::runtime_error(
            "LlvmCodegen::emit: program must be in de-Bruijn form");
    }

    Emitter em{ctx_, *mod_};
    em.plan = &plan;
    em.prog = &program;
    em.run();
}

std::string LlvmCodegen::get_ir() const {
    std::string out;
    llvm::raw_string_ostream os(out);
    mod_->print(os, nullptr);
    return out;
}

}  // namespace uplc
