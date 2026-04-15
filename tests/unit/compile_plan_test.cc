// Unit tests for M6 compile-plan analysis (compiler/analysis/compile_plan.{h,cc}).
//
// Hand-rolled harness — no external test framework dependency.
//
// Each fixture is a small UPLC program in textual form, converted to de-Bruijn,
// and then analysed.  We verify:
//   • var_resolutions : Arg vs Capture + slot
//   • closure_plans   : ordered capture sources at each LamAbs / Delay
//   • closure_ids     : unique sequential IDs
//   • const_ids       : unique sequential IDs
//   • site_table      : document-order step kinds
//
// Named-form variables are one-indexed de Bruijn after name_to_debruijn; the
// comments in each fixture show the resulting indices explicitly.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "compiler/analysis/compile_plan.h"
#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"

namespace {

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

int g_failures = 0;
int g_total    = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_total;                                                         \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        ++g_total;                                                         \
        auto a_ = (a);                                                     \
        auto b_ = (b);                                                     \
        if (a_ != b_) {                                                    \
            ++g_failures;                                                  \
            std::fprintf(stderr, "FAIL %s:%d: expected %lld, got %lld\n", \
                         __FILE__, __LINE__,                               \
                         (long long)(b_), (long long)(a_));               \
        }                                                                  \
    } while (0)

// ---------------------------------------------------------------------------
// collect_terms: walk the AST in document order and collect Term* by tag.
// Mirrors the walk order used by analyse_in_scope so we can cross-reference.
// ---------------------------------------------------------------------------

void collect_terms_rec(const uplc::Term* t,
                       std::vector<const uplc::Term*>& out) {
    using uplc::TermTag;
    out.push_back(t);
    switch (t->tag) {
        case TermTag::Lambda:
            collect_terms_rec(t->lambda.body, out);
            break;
        case TermTag::Delay:
            collect_terms_rec(t->delay.term, out);
            break;
        case TermTag::Apply:
            collect_terms_rec(t->apply.function, out);
            collect_terms_rec(t->apply.argument, out);
            break;
        case TermTag::Force:
            collect_terms_rec(t->force.term, out);
            break;
        case TermTag::Constr:
            for (std::uint32_t i = 0; i < t->constr.n_fields; ++i)
                collect_terms_rec(t->constr.fields[i], out);
            break;
        case TermTag::Case:
            collect_terms_rec(t->case_.scrutinee, out);
            for (std::uint32_t i = 0; i < t->case_.n_branches; ++i)
                collect_terms_rec(t->case_.branches[i], out);
            break;
        default:
            break;
    }
}

std::vector<const uplc::Term*> collect_terms(const uplc::Program& db) {
    std::vector<const uplc::Term*> out;
    collect_terms_rec(db.term, out);
    return out;
}

// ---------------------------------------------------------------------------
// Fixture 1: identity lambda — (program 1.1.0 (lam x x))
//
//   Named: (lam x x)
//   DB:    (lam Var(1))
//
//   Lambda (top scope, arg_count=1, captures=[]):
//     fv(Var 1) = {1};  build_capture_list({1}, 1) → {} (k=1 not > 1)
//     → closure_plans[lam] = {}
//   Var 1 inside lam:  k=1 <= 1 → Arg, slot=0
// ---------------------------------------------------------------------------

void test_identity_lambda() {
    uplc::Arena ca;
    const std::string src = "(program 1.1.0 (lam x x))";
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    auto terms = collect_terms(db);
    // terms[0] = Lambda, terms[1] = Var(1)
    const uplc::Term* lam = terms[0];
    const uplc::Term* var = terms[1];

    CHECK(lam->tag == uplc::TermTag::Lambda);
    CHECK(var->tag == uplc::TermTag::Var);

    // Lambda: closure_id=0, empty captures
    CHECK(plan.closure_ids.count(lam) == 1);
    CHECK_EQ(plan.closure_ids.at(lam), 0u);
    CHECK(plan.closure_plans.count(lam) == 1);
    CHECK(plan.closure_plans.at(lam).captures.empty());

    // Var: resolves to Arg, slot=0
    CHECK(plan.var_resolutions.count(var) == 1);
    CHECK(plan.var_resolutions.at(var).origin == uplc::VarOrigin::Arg);
    CHECK_EQ(plan.var_resolutions.at(var).slot, 0u);

    // Global counts
    CHECK_EQ(plan.n_closures, 1u);
    CHECK_EQ(plan.n_consts,   0u);

    // site_table: Lambda then Var
    CHECK_EQ((int)plan.site_table.size(), 2);
    CHECK(plan.site_table[0].second == uplc::StepKind::Lambda);
    CHECK(plan.site_table[1].second == uplc::StepKind::Var);
}

// ---------------------------------------------------------------------------
// Fixture 2: K-combinator — (program 1.1.0 (lam x (lam y x)))
//
//   DB: (lam (lam Var(2)))
//
//   Outer lambda (top, arg_count=1, captures=[]):
//     body = (lam Var(2))
//     fv((lam Var(2))) = fv_body_shifted = {2-1}={1}  →  k=1 not > 1 → captures=[]
//   Inner lambda (arg_count=1, captures=[]):
//     body = Var(2)
//     fv(Var 2) = {2}; build_capture_list({2}, 1) → {2-1}={1}; captures=[1]
//     resolve capture 1 in outer scope (arg_count=1, caps=[]): 1 <= 1 → {Arg, 0}
//     → closure_plans[inner] = [{Arg, 0}]
//   Var 2 inside inner:  k=2 > 1 → Capture; outer_idx=2-1=1; find_slot([1], 1)=0
//     → {Capture, slot=0}
// ---------------------------------------------------------------------------

void test_k_combinator() {
    uplc::Arena ca;
    const std::string src = "(program 1.1.0 (lam x (lam y x)))";
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    auto terms = collect_terms(db);
    // terms: outer_lam, inner_lam, var(2)
    const uplc::Term* outer_lam = terms[0];
    const uplc::Term* inner_lam = terms[1];
    const uplc::Term* var       = terms[2];

    CHECK(outer_lam->tag == uplc::TermTag::Lambda);
    CHECK(inner_lam->tag == uplc::TermTag::Lambda);
    CHECK(var->tag == uplc::TermTag::Var);

    // Outer lambda: no captures
    CHECK(plan.closure_plans.at(outer_lam).captures.empty());

    // Inner lambda: one capture sourced from outer Arg
    const auto& inner_caps = plan.closure_plans.at(inner_lam).captures;
    CHECK_EQ((int)inner_caps.size(), 1);
    CHECK(inner_caps[0].origin == uplc::VarOrigin::Arg);
    CHECK_EQ(inner_caps[0].slot, 0u);

    // Var inside inner: Capture slot 0
    const auto& vr = plan.var_resolutions.at(var);
    CHECK(vr.origin == uplc::VarOrigin::Capture);
    CHECK_EQ(vr.slot, 0u);

    CHECK_EQ(plan.n_closures, 2u);
    CHECK_EQ(plan.n_consts,   0u);

    // Closure IDs are unique and in document order: outer=0, inner=1
    CHECK_EQ(plan.closure_ids.at(outer_lam), 0u);
    CHECK_EQ(plan.closure_ids.at(inner_lam), 1u);
}

// ---------------------------------------------------------------------------
// Fixture 3: 3-nested lambdas — innermost captures the outermost arg.
//   (program 1.1.0 (lam x (lam y (lam z x))))
//
//   DB: (lam (lam (lam Var(3))))
//
//   Outermost (top, arg_count=1, caps=[]):
//     fv(body=(lam(lam Var(3)))) → {1}; build_capture_list({1},1)={} → caps=[]
//   Middle (arg_count=1, caps=[]):
//     fv(body=(lam Var(3))) → {2};  wait let me redo this carefully.
//
//     fv(Var 3) = {3}
//     fv(lam Var(3)) = {k-1 : k ∈ {3}, k>=2} = {2}
//     fv(lam(lam Var(3))) = {k-1 : k ∈ {2}, k>=2} = {1}
//
//   Outermost body_fv = {1} (body=(lam(lam Var(3)))); build({1},1) → {} → caps=[]
//   Middle: its body = (lam Var(3)). body_fv = fv(lam Var(3)) = {2}.
//     build_capture_list({2}, 1) → {2-1}={1} → caps=[1].
//     resolve 1 in outermost scope (arg_count=1, caps=[]): 1<=1 → {Arg,0}
//     closure_plans[middle] = [{Arg,0}]
//   Innermost: its body = Var(3). body_fv = {3}.
//     build_capture_list({3}, 1) → {3-1}={2} → caps=[2].
//     resolve 2 in middle scope (arg_count=1, caps=[1]):
//       2 > 1 → Capture; find_slot([1], 2-1=1) = 0 → {Capture, 0}
//     closure_plans[innermost] = [{Capture,0}]
//   Var 3 inside innermost: k=3, arg_count=1. 3>1 → Capture.
//     outer_idx=3-1=2; find_slot(innermost.caps=[2], 2) = slot 0.
//     VarResolution = {Capture, 0}
// ---------------------------------------------------------------------------

void test_three_nested_lambdas() {
    uplc::Arena ca;
    const std::string src = "(program 1.1.0 (lam x (lam y (lam z x))))";
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    auto terms = collect_terms(db);
    // terms: outer_lam, middle_lam, inner_lam, var(3)
    const uplc::Term* outer_lam  = terms[0];
    const uplc::Term* middle_lam = terms[1];
    const uplc::Term* inner_lam  = terms[2];
    const uplc::Term* var        = terms[3];

    CHECK(outer_lam->tag  == uplc::TermTag::Lambda);
    CHECK(middle_lam->tag == uplc::TermTag::Lambda);
    CHECK(inner_lam->tag  == uplc::TermTag::Lambda);
    CHECK(var->tag        == uplc::TermTag::Var);

    // Outer: no captures
    CHECK(plan.closure_plans.at(outer_lam).captures.empty());

    // Middle: captures outer Arg
    const auto& mid_caps = plan.closure_plans.at(middle_lam).captures;
    CHECK_EQ((int)mid_caps.size(), 1);
    CHECK(mid_caps[0].origin == uplc::VarOrigin::Arg);
    CHECK_EQ(mid_caps[0].slot, 0u);

    // Innermost: captures middle's capture slot 0 (which itself is the outer Arg)
    const auto& inn_caps = plan.closure_plans.at(inner_lam).captures;
    CHECK_EQ((int)inn_caps.size(), 1);
    CHECK(inn_caps[0].origin == uplc::VarOrigin::Capture);
    CHECK_EQ(inn_caps[0].slot, 0u);

    // Var 3: resolves to Capture slot 0 within the innermost fn
    const auto& vr = plan.var_resolutions.at(var);
    CHECK(vr.origin == uplc::VarOrigin::Capture);
    CHECK_EQ(vr.slot, 0u);

    // IDs in document order: 0,1,2
    CHECK_EQ(plan.closure_ids.at(outer_lam),  0u);
    CHECK_EQ(plan.closure_ids.at(middle_lam), 1u);
    CHECK_EQ(plan.closure_ids.at(inner_lam),  2u);
    CHECK_EQ(plan.n_closures, 3u);
}

// ---------------------------------------------------------------------------
// Fixture 4: delay with no captures — (program 1.1.0 (delay (con integer 42)))
//
//   Delay: arg_count=0, body_fv={}. caps=[].
//   Constant: const_id=0.
//   site_table: Delay, Const
// ---------------------------------------------------------------------------

void test_delay_no_captures() {
    uplc::Arena ca;
    const std::string src = "(program 1.1.0 (delay (con integer 42)))";
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    auto terms = collect_terms(db);
    const uplc::Term* delay = terms[0];
    const uplc::Term* con   = terms[1];

    CHECK(delay->tag == uplc::TermTag::Delay);
    CHECK(con->tag   == uplc::TermTag::Constant);

    CHECK(plan.closure_plans.at(delay).captures.empty());
    CHECK_EQ(plan.closure_ids.at(delay), 0u);
    CHECK_EQ(plan.const_ids.at(con), 0u);
    CHECK_EQ(plan.n_closures, 1u);
    CHECK_EQ(plan.n_consts,   1u);

    CHECK_EQ((int)plan.site_table.size(), 2);
    CHECK(plan.site_table[0].second == uplc::StepKind::Delay);
    CHECK(plan.site_table[1].second == uplc::StepKind::Const);
}

// ---------------------------------------------------------------------------
// Fixture 5: delay capturing a lambda arg.
//   (program 1.1.0 (lam x (delay x)))
//
//   DB: (lam (delay Var(1)))
//
//   Lambda (top, arg_count=1, caps=[]):
//     fv(body=(delay Var(1))) = fv(delay Var(1)) = fv(Var 1) = {1}
//     build_capture_list({1}, 1) → {} → outer lam caps=[]
//   Delay (inside lam, arg_count=0):
//     body_fv = fv(Var 1) = {1}
//     build_capture_list({1}, 0) → {1-0}={1} → caps=[1]
//     resolve 1 in lam scope (arg_count=1, caps=[]): 1 <= 1 → {Arg, 0}
//     closure_plans[delay] = [{Arg, 0}]
//   Var 1 inside delay: k=1, delay scope has arg_count=0.
//     0 means we never have an arg — everything is a capture.
//     outer_idx = 1-0 = 1; find_slot(delay.caps=[1], 1) = slot 0.
//     VarResolution = {Capture, 0}
// ---------------------------------------------------------------------------

void test_delay_captures_arg() {
    uplc::Arena ca;
    const std::string src = "(program 1.1.0 (lam x (delay x)))";
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    auto terms = collect_terms(db);
    // terms: lam, delay, var(1)
    const uplc::Term* lam   = terms[0];
    const uplc::Term* delay = terms[1];
    const uplc::Term* var   = terms[2];

    CHECK(lam->tag   == uplc::TermTag::Lambda);
    CHECK(delay->tag == uplc::TermTag::Delay);
    CHECK(var->tag   == uplc::TermTag::Var);

    // Lambda: no captures
    CHECK(plan.closure_plans.at(lam).captures.empty());

    // Delay: captures the lam's Arg
    const auto& delay_caps = plan.closure_plans.at(delay).captures;
    CHECK_EQ((int)delay_caps.size(), 1);
    CHECK(delay_caps[0].origin == uplc::VarOrigin::Arg);
    CHECK_EQ(delay_caps[0].slot, 0u);

    // Var inside delay: Capture slot 0
    const auto& vr = plan.var_resolutions.at(var);
    CHECK(vr.origin == uplc::VarOrigin::Capture);
    CHECK_EQ(vr.slot, 0u);

    CHECK_EQ(plan.n_closures, 2u);  // lam + delay
    CHECK_EQ(plan.closure_ids.at(lam),   0u);
    CHECK_EQ(plan.closure_ids.at(delay), 1u);
}

// ---------------------------------------------------------------------------
// Fixture 6: apply of identity to a constant.
//   (program 1.1.0 [ (lam x x) (con integer 1) ])
//
//   site_table document order: Apply, Lambda, Var, Const
//   const_ids: con → 0
//   No captures on the lambda.
// ---------------------------------------------------------------------------

void test_apply_identity_to_const() {
    uplc::Arena ca;
    const std::string src = "(program 1.1.0 [ (lam x x) (con integer 1) ])";
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    // Verify site_table order
    CHECK_EQ((int)plan.site_table.size(), 4);
    CHECK(plan.site_table[0].second == uplc::StepKind::Apply);
    CHECK(plan.site_table[1].second == uplc::StepKind::Lambda);
    CHECK(plan.site_table[2].second == uplc::StepKind::Var);
    CHECK(plan.site_table[3].second == uplc::StepKind::Const);

    CHECK_EQ(plan.n_closures, 1u);
    CHECK_EQ(plan.n_consts,   1u);
}

// ---------------------------------------------------------------------------
// Fixture 7: two constants — const IDs are sequential.
//   (program 1.1.0 [ (lam x x) (con integer 1) ]) is just one const.
//   Use a more explicit fixture:
//   (program 1.1.0 [ [ (lam x (lam y x)) (con integer 10) ] (con integer 20) ])
//
//   K applied to 10 then 20.  site_table:
//     Apply(outer), Apply(inner), Lambda(x), Lambda(y), Var(2), Const(10), Const(20)
//   const_ids: con(10)=0, con(20)=1
// ---------------------------------------------------------------------------

void test_two_constants_sequential_ids() {
    uplc::Arena ca;
    const std::string src =
        "(program 1.1.0 [ [ (lam x (lam y x)) (con integer 10) ] (con integer 20) ])";
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    CHECK_EQ(plan.n_consts,   2u);
    CHECK_EQ(plan.n_closures, 2u);

    // site_table: Apply Apply Lambda Lambda Var Const Const
    CHECK_EQ((int)plan.site_table.size(), 7);
    CHECK(plan.site_table[0].second == uplc::StepKind::Apply);
    CHECK(plan.site_table[1].second == uplc::StepKind::Apply);
    CHECK(plan.site_table[2].second == uplc::StepKind::Lambda);
    CHECK(plan.site_table[3].second == uplc::StepKind::Lambda);
    CHECK(plan.site_table[4].second == uplc::StepKind::Var);
    CHECK(plan.site_table[5].second == uplc::StepKind::Const);
    CHECK(plan.site_table[6].second == uplc::StepKind::Const);

    // The two const nodes get sequential IDs
    const uplc::Term* c0 = plan.site_table[5].first;
    const uplc::Term* c1 = plan.site_table[6].first;
    CHECK_EQ(plan.const_ids.at(c0), 0u);
    CHECK_EQ(plan.const_ids.at(c1), 1u);
}

// ---------------------------------------------------------------------------
// Fixture 8: force / builtin site_table entries.
//   (program 1.1.0 (force (builtin addInteger)))
//
//   site_table: Force, Builtin   (Error not step-charged → not in table)
// ---------------------------------------------------------------------------

void test_force_builtin() {
    uplc::Arena ca;
    const std::string src = "(program 1.1.0 (force (builtin addInteger)))";
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    CHECK_EQ((int)plan.site_table.size(), 2);
    CHECK(plan.site_table[0].second == uplc::StepKind::Force);
    CHECK(plan.site_table[1].second == uplc::StepKind::Builtin);
    CHECK_EQ(plan.n_closures, 0u);
    CHECK_EQ(plan.n_consts,   0u);
}

// ---------------------------------------------------------------------------
// Fixture 9: lambda with multiple captures (S-combinator body).
//   (program 1.1.0 (lam f (lam g (lam x [ [ f x ] [ g x ] ]))))
//
//   DB: (lam (lam (lam [ [ Var(3) Var(1) ] [ Var(2) Var(1) ] ])))
//
//   Outermost lam (top, arg=f at index 1):
//     body = (lam (lam [...]))
//     fv_body = set of outer refs that escape = let's compute:
//       fv(Var 3) = {3}, fv(Var 1) = {1}, fv(Var 2) = {2}
//       fv(inner apply left)  = {3,1}
//       fv(inner apply right) = {2,1}
//       fv(outer apply) = {1,2,3}
//       fv(lam x [...]) = {k-1 : k ∈ {1,2,3}, k>=2} = {1,2}
//       fv(lam g (lam x [...])) = {k-1 : k ∈ {1,2}, k>=2} = {1}
//     build_capture_list({1}, 1) → {} → outermost caps=[]
//
//   Middle lam g (arg_count=1, caps=[]):
//     body = (lam x [...])
//     fv_body = fv(lam x [...]) = {1,2}  (computed above)
//     build_capture_list({1,2}, 1) → {1-1?, no: k>1 → k=2 → {2-1}={1}} → caps=[1]
//     resolve 1 in outermost scope (arg=1, caps=[]): 1<=1 → {Arg,0}
//     → closure_plans[middle] = [{Arg,0}]
//
//   Innermost lam x (arg_count=1, caps=[1]):  (inner scope: arg_count=1, captures=[1])
//     body = [ [ Var(3) Var(1) ] [ Var(2) Var(1) ] ]
//     fv_body = {1,2,3}
//     build_capture_list({1,2,3}, 1) → {k>1 → 2→1, 3→2} = [1,2]
//     resolve 1 in middle scope (arg_count=1, caps=[1]):  1<=1 → {Arg,0}
//     resolve 2 in middle scope: 2>1 → Capture; find_slot([1], 2-1=1) = 0 → {Capture,0}
//     → closure_plans[innermost] = [{Arg,0}, {Capture,0}]
//
//   Vars inside innermost (arg_count=1, captures=[1,2]):
//     Var(3): k=3>1 → Capture; outer=3-1=2; find_slot([1,2], 2)=slot 1. → {Capture,1}
//     Var(1): k=1<=1 → {Arg,0}
//     Var(2): k=2>1 → Capture; outer=2-1=1; find_slot([1,2],1)=slot 0. → {Capture,0}
//     Var(1): k=1<=1 → {Arg,0}
// ---------------------------------------------------------------------------

void test_s_combinator() {
    uplc::Arena ca;
    const std::string src =
        "(program 1.1.0 (lam f (lam g (lam x [ [ f x ] [ g x ] ]))))";
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    auto terms = collect_terms(db);
    // terms[0]=outer_lam, [1]=middle_lam, [2]=inner_lam, then applies+vars
    const uplc::Term* outer_lam  = terms[0];
    const uplc::Term* middle_lam = terms[1];
    const uplc::Term* inner_lam  = terms[2];

    // Outer: no captures
    CHECK(plan.closure_plans.at(outer_lam).captures.empty());

    // Middle: 1 capture: {Arg,0} (outer f)
    const auto& mid_caps = plan.closure_plans.at(middle_lam).captures;
    CHECK_EQ((int)mid_caps.size(), 1);
    CHECK(mid_caps[0].origin == uplc::VarOrigin::Arg);
    CHECK_EQ(mid_caps[0].slot, 0u);

    // Innermost: 2 captures: [{Arg,0}, {Capture,0}]
    const auto& inn_caps = plan.closure_plans.at(inner_lam).captures;
    CHECK_EQ((int)inn_caps.size(), 2);
    CHECK(inn_caps[0].origin == uplc::VarOrigin::Arg);
    CHECK_EQ(inn_caps[0].slot, 0u);
    CHECK(inn_caps[1].origin == uplc::VarOrigin::Capture);
    CHECK_EQ(inn_caps[1].slot, 0u);

    CHECK_EQ(plan.n_closures, 3u);

    // Collect the 4 Var nodes from site_table (in document order)
    std::vector<const uplc::Term*> var_nodes;
    for (const auto& [term, kind] : plan.site_table) {
        if (kind == uplc::StepKind::Var) var_nodes.push_back(term);
    }
    CHECK_EQ((int)var_nodes.size(), 4);

    // Var f (index 3): Capture slot 1
    const auto& vr_f = plan.var_resolutions.at(var_nodes[0]);
    CHECK(vr_f.origin == uplc::VarOrigin::Capture);
    CHECK_EQ(vr_f.slot, 1u);

    // Var x (index 1): Arg slot 0
    const auto& vr_x1 = plan.var_resolutions.at(var_nodes[1]);
    CHECK(vr_x1.origin == uplc::VarOrigin::Arg);
    CHECK_EQ(vr_x1.slot, 0u);

    // Var g (index 2): Capture slot 0
    const auto& vr_g = plan.var_resolutions.at(var_nodes[2]);
    CHECK(vr_g.origin == uplc::VarOrigin::Capture);
    CHECK_EQ(vr_g.slot, 0u);

    // Var x again (index 1): Arg slot 0
    const auto& vr_x2 = plan.var_resolutions.at(var_nodes[3]);
    CHECK(vr_x2.origin == uplc::VarOrigin::Arg);
    CHECK_EQ(vr_x2.slot, 0u);
}

// ---------------------------------------------------------------------------
// Fixture 10: closed constant program — no lambdas, no vars.
//   (program 1.1.0 (con integer 0))
//   site_table: [Const]; n_closures=0, n_consts=1.
// ---------------------------------------------------------------------------

void test_constant_program() {
    uplc::Arena ca;
    const std::string src = "(program 1.1.0 (con integer 0))";
    uplc::Program named = uplc::parse_program(ca, src);
    uplc::Program db    = uplc::name_to_debruijn(ca, named);
    uplc::CompilePlan plan = uplc::analyse_program(db);

    CHECK_EQ((int)plan.site_table.size(), 1);
    CHECK(plan.site_table[0].second == uplc::StepKind::Const);
    CHECK_EQ(plan.n_closures, 0u);
    CHECK_EQ(plan.n_consts,   1u);
}

}  // namespace

int main() {
    test_identity_lambda();
    test_k_combinator();
    test_three_nested_lambdas();
    test_delay_no_captures();
    test_delay_captures_arg();
    test_apply_identity_to_const();
    test_two_constants_sequential_ids();
    test_force_builtin();
    test_s_combinator();
    test_constant_program();

    std::fprintf(stderr, "compile_plan_test: %d/%d passed\n",
                 g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
