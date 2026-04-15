#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compiler/ast/term.h"

namespace uplc {

// ---------------------------------------------------------------------------
// Compile-time analysis that feeds M7's LLVM lowering.
//
// A UPLC program in closure-converted form looks like:
//   - One top-level "program entry" function that evaluates the root term
//     and returns the resulting value.
//   - One LLVM function per LamAbs node (signature:
//       uplc_value fn(uplc_value* env, uplc_value arg, uplc_budget* b))
//   - One LLVM function per Delay node (signature:
//       uplc_value fn(uplc_value* env, uplc_budget* b))
//   - Captured free variables are packed into a uplc_closure (fn pointer +
//     flat free[] array) at the node's creation site.
//
// Before codegen emits any IR it needs three things per term node:
//   1. For every Var: does this de Bruijn reference resolve to the
//      enclosing fn's own arg, or to a slot in that fn's env[]?
//   2. For every LamAbs / Delay: an ordered list of captures — each one
//      described in the PARENT fn's scope (Arg or Capture+slot).
//   3. For every step-chargeable node: which UPLC step kind to charge so
//      generated code preserves the CEK machine's bit-exact budget.
//
// M6 is pure analysis — no LLVM APIs, no IR. It produces a CompilePlan
// that M7 walks to emit concrete IR.
// ---------------------------------------------------------------------------

enum class VarOrigin : std::uint8_t {
    Arg,      // the enclosing lambda's own argument
    Capture,  // slot N in the enclosing fn's env[]
};

enum class StepKind : std::uint8_t {
    Var,
    Lambda,
    Apply,
    Delay,
    Force,
    Const,
    Builtin,
    Constr,
    Case,
};

struct VarResolution {
    VarOrigin     origin;
    std::uint32_t slot;  // only meaningful when origin == Capture
};

// How a specific capture slot gets populated when the parent fn executes
// the corresponding LamAbs / Delay creation site.
struct CaptureSource {
    VarOrigin     origin;  // source is the parent's Arg or one of its captures
    std::uint32_t slot;    // only meaningful when origin == Capture
};

struct ClosurePlan {
    // One entry per capture slot in the emitted uplc_closure::free[] array.
    // Order is "outer de Bruijn index ascending" so two passes can agree on
    // slot assignment without communicating.
    std::vector<CaptureSource> captures;
};

// Top-level result consumed by M7 codegen. Keyed by the original C++ AST
// node pointer — safe because the Program + AST live in a compiler Arena
// that outlives the codegen pass.
struct CompilePlan {
    std::unordered_map<const Term*, VarResolution>  var_resolutions;
    std::unordered_map<const Term*, ClosurePlan>    closure_plans;
    std::unordered_map<const Term*, std::uint32_t>  closure_ids;
    std::unordered_map<const Term*, std::uint32_t>  const_ids;

    // Document-order enumeration of step-chargeable nodes. Used by tests
    // and (optionally) by codegen if it wants a pre-built traversal order.
    std::vector<std::pair<const Term*, StepKind>>   site_table;

    // Running counts at analysis end. `n_closures` includes every LamAbs
    // and Delay node (each becomes its own LLVM function).
    std::uint32_t n_closures = 0;
    std::uint32_t n_consts   = 0;
};

// Analyse a de-Bruijn Program and produce a CompilePlan ready for M7.
// Throws std::runtime_error if `program` isn't in de-Bruijn form or has
// free variables (unclosed terms are rejected by name_to_debruijn anyway,
// so this only fires on internal errors).
CompilePlan analyse_program(const Program& program);

}  // namespace uplc
