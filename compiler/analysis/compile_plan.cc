#include "compiler/analysis/compile_plan.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace uplc {

namespace {

// ---------------------------------------------------------------------------
// Pass 1: free-variable computation
//
// For each term, compute the set of de Bruijn indices that are FREE —
// i.e. not bound by a lambda inside the term. Result indices are expressed
// relative to the term's own outer context:
//
//   fv(Var k)            = {k}
//   fv(Lambda body)      = { k-1 : k ∈ fv(body), k >= 2 }  (own arg drops out, rest shift down)
//   fv(Delay body)       = fv(body)                        (no binder)
//   fv(Apply f a)        = fv(f) ∪ fv(a)
//   fv(Force e)          = fv(e)
//   fv(Constr _ fs)      = ∪ fv(fs)
//   fv(Case sc alts)     = fv(sc) ∪ ∪ fv(alts)
//   fv(Constant)         = fv(Builtin) = fv(Error) = ∅
//
// We memoise on the node pointer — repeated walks across the tree only pay
// for each subtree once. A closed program's fv(root) is empty.
// ---------------------------------------------------------------------------

using FvSet = std::set<std::uint32_t>;
using FvMap = std::unordered_map<const Term*, FvSet>;

const FvSet& compute_fv(const Term* t, FvMap& memo) {
    auto it = memo.find(t);
    if (it != memo.end()) return it->second;

    FvSet out;
    switch (t->tag) {
        case TermTag::Var:
            out.insert(t->var.binder.id);
            break;
        case TermTag::Lambda: {
            const FvSet& bf = compute_fv(t->lambda.body, memo);
            for (std::uint32_t k : bf) {
                if (k >= 2) out.insert(k - 1);
            }
            break;
        }
        case TermTag::Delay: {
            const FvSet& bf = compute_fv(t->delay.term, memo);
            out.insert(bf.begin(), bf.end());
            break;
        }
        case TermTag::Apply: {
            const FvSet& f = compute_fv(t->apply.function, memo);
            const FvSet& a = compute_fv(t->apply.argument, memo);
            out.insert(f.begin(), f.end());
            out.insert(a.begin(), a.end());
            break;
        }
        case TermTag::Force: {
            const FvSet& f = compute_fv(t->force.term, memo);
            out.insert(f.begin(), f.end());
            break;
        }
        case TermTag::Constr:
            for (std::uint32_t i = 0; i < t->constr.n_fields; ++i) {
                const FvSet& f = compute_fv(t->constr.fields[i], memo);
                out.insert(f.begin(), f.end());
            }
            break;
        case TermTag::Case: {
            const FvSet& s = compute_fv(t->case_.scrutinee, memo);
            out.insert(s.begin(), s.end());
            for (std::uint32_t i = 0; i < t->case_.n_branches; ++i) {
                const FvSet& f = compute_fv(t->case_.branches[i], memo);
                out.insert(f.begin(), f.end());
            }
            break;
        }
        case TermTag::Constant:
        case TermTag::Builtin:
        case TermTag::Error:
            break;
    }
    auto [ins, _] = memo.emplace(t, std::move(out));
    return ins->second;
}

// ---------------------------------------------------------------------------
// Pass 2: scope resolution + closure planning
//
// Walk top-down. Each LamAbs or Delay starts a new fn scope, with the
// captures determined by fv(lam) / fv(delay). Vars inside that scope
// resolve against (Arg | captures[]).
//
// `scope` describes the CURRENT FN's scope:
//   arg_count = 1 for a lambda fn, 0 for a delay fn, 0 for the top-level.
//   captures  = ordered list of outer-context de Bruijn indices this fn
//               captured. Slot 0 is the first capture, slot n-1 the last.
// ---------------------------------------------------------------------------

struct FnScope {
    std::uint32_t                    arg_count;  // 0 (delay / top) or 1 (lambda)
    std::vector<std::uint32_t>       captures;   // outer indices; sorted ascending
};

// Return the slot index for `outer_index` in the current fn's captures,
// or throw if not present (indicates the program is not closed).
std::uint32_t find_slot(const FnScope& scope, std::uint32_t outer_index) {
    for (std::uint32_t i = 0; i < scope.captures.size(); ++i) {
        if (scope.captures[i] == outer_index) return i;
    }
    throw std::runtime_error(
        "compile_plan: de Bruijn index " + std::to_string(outer_index) +
        " escapes its closure — program is not closed");
}

// Translate a de Bruijn index in the enclosing fn's scope (the "outer
// context" of a nested lambda/delay) into the capture source that should
// be emitted at the nested closure's creation site.
CaptureSource resolve_outer(const FnScope& scope, std::uint32_t outer_idx) {
    CaptureSource src{};
    if (scope.arg_count > 0 && outer_idx <= scope.arg_count) {
        src.origin = VarOrigin::Arg;
        src.slot = 0;
        return src;
    }
    src.origin = VarOrigin::Capture;
    src.slot = find_slot(scope, outer_idx - scope.arg_count);
    return src;
}

// Build the ordered capture list for a lambda/delay given its body's fv
// set (expressed in body-local de Bruijn indices) and its own arg_count.
std::vector<std::uint32_t> build_capture_list(const FvSet& body_fv,
                                              std::uint32_t arg_count) {
    // Shift body-fv down by arg_count (the lambda's own arg drops out).
    std::vector<std::uint32_t> out;
    out.reserve(body_fv.size());
    for (std::uint32_t k : body_fv) {
        if (k > arg_count) out.push_back(k - arg_count);
    }
    std::sort(out.begin(), out.end());
    // Dedup — a body_fv is already a set, but after shifting we could in
    // principle collapse; defensive.
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void analyse_in_scope(const Term* term, const FnScope& scope,
                      FvMap& fv_memo, CompilePlan& plan) {
    switch (term->tag) {
        case TermTag::Var: {
            plan.site_table.emplace_back(term, StepKind::Var);
            std::uint32_t k = term->var.binder.id;
            VarResolution r{};
            if (k == 0) {
                // de Bruijn 0 is invalid — the parser & converter should
                // never produce it, but guard anyway.
                throw std::runtime_error(
                    "compile_plan: invalid de Bruijn index 0");
            }
            if (scope.arg_count > 0 && k <= scope.arg_count) {
                r.origin = VarOrigin::Arg;
                r.slot = 0;
            } else {
                std::uint32_t outer_idx = k - scope.arg_count;
                r.origin = VarOrigin::Capture;
                r.slot = find_slot(scope, outer_idx);
            }
            plan.var_resolutions[term] = r;
            break;
        }

        case TermTag::Lambda: {
            plan.site_table.emplace_back(term, StepKind::Lambda);
            plan.closure_ids[term] = plan.n_closures++;

            const FvSet& body_fv = compute_fv(term->lambda.body, fv_memo);
            FnScope inner{};
            inner.arg_count = 1;
            inner.captures = build_capture_list(body_fv, /*arg_count=*/1);

            // Build the closure plan: resolve each capture in the PARENT
            // scope.
            ClosurePlan cp;
            cp.captures.reserve(inner.captures.size());
            for (std::uint32_t outer_idx : inner.captures) {
                cp.captures.push_back(resolve_outer(scope, outer_idx));
            }
            plan.closure_plans[term] = std::move(cp);

            analyse_in_scope(term->lambda.body, inner, fv_memo, plan);
            break;
        }

        case TermTag::Delay: {
            plan.site_table.emplace_back(term, StepKind::Delay);
            plan.closure_ids[term] = plan.n_closures++;

            const FvSet& body_fv = compute_fv(term->delay.term, fv_memo);
            FnScope inner{};
            inner.arg_count = 0;
            inner.captures = build_capture_list(body_fv, /*arg_count=*/0);

            ClosurePlan cp;
            cp.captures.reserve(inner.captures.size());
            for (std::uint32_t outer_idx : inner.captures) {
                cp.captures.push_back(resolve_outer(scope, outer_idx));
            }
            plan.closure_plans[term] = std::move(cp);

            analyse_in_scope(term->delay.term, inner, fv_memo, plan);
            break;
        }

        case TermTag::Apply:
            plan.site_table.emplace_back(term, StepKind::Apply);
            analyse_in_scope(term->apply.function, scope, fv_memo, plan);
            analyse_in_scope(term->apply.argument, scope, fv_memo, plan);
            break;

        case TermTag::Force:
            plan.site_table.emplace_back(term, StepKind::Force);
            analyse_in_scope(term->force.term, scope, fv_memo, plan);
            break;

        case TermTag::Constant:
            plan.site_table.emplace_back(term, StepKind::Const);
            plan.const_ids[term] = plan.n_consts++;
            break;

        case TermTag::Builtin:
            plan.site_table.emplace_back(term, StepKind::Builtin);
            break;

        case TermTag::Constr:
            plan.site_table.emplace_back(term, StepKind::Constr);
            for (std::uint32_t i = 0; i < term->constr.n_fields; ++i) {
                analyse_in_scope(term->constr.fields[i], scope, fv_memo, plan);
            }
            break;

        case TermTag::Case:
            plan.site_table.emplace_back(term, StepKind::Case);
            analyse_in_scope(term->case_.scrutinee, scope, fv_memo, plan);
            for (std::uint32_t i = 0; i < term->case_.n_branches; ++i) {
                analyse_in_scope(term->case_.branches[i], scope, fv_memo, plan);
            }
            break;

        case TermTag::Error:
            // Error raises; no step charge, no sub-nodes.
            break;
    }
}

}  // namespace

CompilePlan analyse_program(const Program& program) {
    if (!program.is_debruijn) {
        throw std::runtime_error(
            "analyse_program: input must already be in de-Bruijn form");
    }

    FvMap fv_memo;
    CompilePlan plan;

    // Top-level scope: zero args, zero captures. A closed program's root
    // term must not reference any outer index; compute_fv will catch any
    // escape via find_slot at the first Var.
    FnScope top{};
    top.arg_count = 0;
    // top.captures is empty.

    analyse_in_scope(program.term, top, fv_memo, plan);

    return plan;
}

}  // namespace uplc
