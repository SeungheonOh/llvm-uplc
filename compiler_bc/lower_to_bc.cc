// Lower a runtime rterm tree into a uplc_bc_program.
//
// Single-pass recursive descent: we walk the rterm tree, maintaining a
// scope stack. Each Lambda/Delay opens a new scope (a new bc function
// builder); each Var reference is resolved to either env[0] (arg) or
// env[binders + upval_slot] (captured free var). On scope close we
// emit MK_LAM / MK_DELAY in the parent scope with the finalised upval
// plan — each entry is the parent's slot for that captured deBruijn.
// Captures cascade up the scope stack: when an inner scope captures
// deBruijn d, we ensure every intermediate scope also captures d.
//
// Case alts in M-bc-5 MUST NOT reference any free var from the
// enclosing scope. The emitter errors out if they do. This keeps the
// CASE ISA simple (no per-alt capture plans); the constraint is
// relaxed when the ISA is extended in a later milestone.
//
// The fused flat->bytecode decoder described in plan-bc.md §0 Q6 is
// deferred to M-bc-8's perf pass; today we reuse the existing
// flat/text frontends to produce an rprogram, then run this lowerer.

#include "compiler_bc/lower_to_bc.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/core/arena.h"
#include "runtime/core/rterm.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/bytecode.h"

namespace uplc_bc {

namespace {

struct Scope {
    // 1 for Lambda (arg at env[0]), 0 for Delay / Case-alt (no arg).
    uint32_t binders = 0;
    // Map: deBruijn index in ENCLOSING scope (after subtracting our binders)
    //      → our upval slot.
    std::unordered_map<uint32_t, uint32_t> db_to_upval;
    // Ordered list of captured outer deBruijns (== upval positions).
    std::vector<uint32_t> upval_outer_db;
    // Emitted opcode words for this function's body.
    std::vector<uplc_bc_word> opcodes;
    // fn_id assigned at scope creation.
    uint32_t fn_id = 0;
    // Original rterm body — stored for readback reconstruction of
    // VLam / VDelay values. NULL for case-alt sub-fns (their closures
    // never escape op_case, so readback never sees them).
    const uplc_rterm* body_rterm = nullptr;
    // Running value-stack depth + high-water mark. Emitter adjusts
    // stack_depth on every push/pop it emits; max_stack is the peak.
    // Deep expressions (e.g. constr with many fields) need this to
    // size the runtime value stack correctly.
    uint32_t stack_depth = 0;
    uint32_t max_stack   = 0;
};

class Lowerer {
public:
    Lowerer(uplc_arena* rt_arena) : rt_arena_(rt_arena) {}

    std::unique_ptr<ProgramOwner> run(const uplc_rprogram& rp) {
        // Create the top-level entry function as scope 0. It has no
        // arg and no upvals (true for all closed programs).
        Scope entry;
        entry.binders = 0;
        entry.fn_id   = next_fn_id_++;
        scopes_.push_back(std::move(entry));
        functions_.emplace_back();  // placeholder, filled on scope_close

        emit(rp.term);
        emit_return();

        // Close the entry function.
        close_scope();

        // Build the final program.
        auto owner = std::make_unique<ProgramOwner>();
        owner->fns_storage    = std::make_unique<uplc_bc_fn[]>(functions_.size());
        owner->consts_storage = std::make_unique<uplc_value[]>(consts_.size());
        for (size_t i = 0; i < consts_.size(); ++i) {
            owner->consts_storage[i] = consts_[i];
        }
        for (size_t i = 0; i < functions_.size(); ++i) {
            owner->fns_storage[i] = functions_[i];
        }
        owner->prog.functions     = owner->fns_storage.get();
        owner->prog.n_functions   = (uint32_t)functions_.size();
        owner->prog.consts        = owner->consts_storage.get();
        owner->prog.n_consts      = (uint32_t)consts_.size();
        owner->prog.version_major = rp.version.major;
        owner->prog.version_minor = rp.version.minor;
        owner->prog.version_patch = rp.version.patch;
        return owner;
    }

private:
    // ---------- emission helpers ----------
    //
    // Tail-merge note: APPLY and FORCE are single-word opcodes with no
    // operand tail. After emit_op(APPLY) or emit_op(FORCE) with nothing
    // subsequently emitted, a following emit_return can safely rewrite
    // opcodes.back() from APPLY/FORCE → TAIL_APPLY/TAIL_FORCE.
    //
    // Variable-length ops (MK_LAM, MK_DELAY, CONSTR, CASE) end with
    // DATA words (slot indices, tag bytes, nfree counts). Looking at
    // the last word's low byte and matching 0x08 against UPLC_BC_FORCE
    // would false-positive on a slot-index value that happens to be 8,
    // overwriting it. Track the trailing-opcode explicitly instead.

    void emit_word(uplc_bc_word w) {
        scopes_.back().opcodes.push_back(w);
        last_emitted_tail_eligible_ = false;  // data word, not an op
    }
    void emit_op(uint8_t op, uint32_t imm24) {
        scopes_.back().opcodes.push_back(uplc_bc_mk(op, imm24));
        last_emitted_tail_eligible_ =
            (op == (uint8_t)UPLC_BC_APPLY) ||
            (op == (uint8_t)UPLC_BC_FORCE);
    }

    bool last_emitted_tail_eligible_ = false;
    /* emit_return: if the previous opcode is APPLY or FORCE in tail
     * position, fold it into TAIL_APPLY / TAIL_FORCE and skip the
     * RETURN entirely. Saves a frame push+pop at runtime per tail
     * call. Budget semantics are unchanged — both forms charge a
     * single StepApply / StepForce and RETURN is free.
     *
     * Only applied in *function-body* tail position, NOT in case-alt
     * sub-dispatch. The alt sub-dispatch boundary frame has
     * ret_pc==NULL (see ops_case.c::run_bytecode_subfn) — if we
     * tail-eliminate it, the callee's RETURN would decrement past
     * that boundary and return to the wrong caller. Alts are small
     * and the optimisation mostly helps regular function bodies, so
     * leaving alts to use APPLY+RETURN is fine.
     */
    bool tail_optimize_next_return_ = true;

    void emit_return() {
        auto& s = scopes_.back();
        if (tail_optimize_next_return_ &&
            last_emitted_tail_eligible_ &&
            !s.opcodes.empty()) {
            /* The previous emission was emit_op(APPLY|FORCE) and
             * nothing has been written since — safe to rewrite the
             * last word in place and skip the RETURN. */
            uplc_bc_word last = s.opcodes.back();
            uint8_t last_op = uplc_bc_op_of(last);
            if (last_op == (uint8_t)UPLC_BC_APPLY) {
                s.opcodes.back() = uplc_bc_mk(UPLC_BC_TAIL_APPLY, 0);
                last_emitted_tail_eligible_ = false;
                return;
            }
            if (last_op == (uint8_t)UPLC_BC_FORCE) {
                s.opcodes.back() = uplc_bc_mk(UPLC_BC_TAIL_FORCE, 0);
                last_emitted_tail_eligible_ = false;
                return;
            }
        }
        emit_op(UPLC_BC_RETURN, 0);
    }

    // Stack-depth bookkeeping. Every push/pop an opcode will do at
    // runtime is tracked here so close_scope can record an accurate
    // max_stack. Underflow (popping more than we pushed) is treated
    // as 0 — it would indicate a lowerer bug, not a runtime issue.
    void push_stack(uint32_t n) {
        auto& s = scopes_.back();
        s.stack_depth += n;
        if (s.stack_depth > s.max_stack) s.max_stack = s.stack_depth;
    }
    void pop_stack(uint32_t n) {
        auto& s = scopes_.back();
        s.stack_depth = (s.stack_depth > n) ? (s.stack_depth - n) : 0;
    }

    uint32_t intern_const(uplc_value v) {
        // For M-bc-5 we don't dedup — each const site gets its own slot.
        // Dedup-by-structural-equality requires a hasher that matches the
        // runtime's value semantics; deferred.
        consts_.push_back(v);
        return (uint32_t)(consts_.size() - 1);
    }

    // ---------- scope management ----------

    // Push a new scope for a Lambda / Delay / Case-alt.
    // Returns the new scope's fn_id.
    uint32_t open_scope(uint32_t binders) {
        Scope s;
        s.binders = binders;
        s.fn_id   = next_fn_id_++;
        functions_.emplace_back();  // placeholder
        scopes_.push_back(std::move(s));
        return scopes_.back().fn_id;
    }

    // Finalise the innermost scope: write into functions_[fn_id] and pop.
    void close_scope() {
        Scope s = std::move(scopes_.back());
        scopes_.pop_back();

        // Copy opcodes into rt_arena so the resulting uplc_bc_fn stays
        // valid as long as the arena is alive.
        uplc_bc_word* ops_rt = nullptr;
        if (!s.opcodes.empty()) {
            ops_rt = (uplc_bc_word*)uplc_arena_alloc(
                rt_arena_, sizeof(uplc_bc_word) * s.opcodes.size(),
                alignof(uplc_bc_word));
            std::memcpy(ops_rt, s.opcodes.data(),
                        sizeof(uplc_bc_word) * s.opcodes.size());
        }

        // Stash the upval deBruijn plan into rt_arena for readback.
        uint32_t* upvals_db_rt = nullptr;
        if (!s.upval_outer_db.empty()) {
            upvals_db_rt = (uint32_t*)uplc_arena_alloc(
                rt_arena_, sizeof(uint32_t) * s.upval_outer_db.size(),
                alignof(uint32_t));
            for (size_t i = 0; i < s.upval_outer_db.size(); ++i) {
                upvals_db_rt[i] = s.upval_outer_db[i];
            }
        }

        /* max_stack is the per-function high-water mark computed during
         * emission via push_stack/pop_stack. Add a small safety
         * margin for the return-value slot and any runtime helpers
         * that briefly push an intermediate. Cap at the 16-bit field. */
        uint32_t stack_slots = s.max_stack + 4;
        if (stack_slots < 8)    stack_slots = 8;
        if (stack_slots > 0xFFFF) stack_slots = 0xFFFF;

        uplc_bc_fn& f = functions_[s.fn_id];
        f.n_upvals       = (uint32_t)s.upval_outer_db.size();
        f.n_opcodes      = (uint32_t)s.opcodes.size();
        f.opcodes        = ops_rt;
        f.n_args         = (uint16_t)s.binders;
        f.max_stack      = (uint16_t)stack_slots;
        f.body_rterm     = s.body_rterm;
        f.upval_outer_db = upvals_db_rt;
    }

    // Resolve a variable reference in the innermost scope to its env slot,
    // bubbling captures up the scope stack if needed. Returns the env slot
    // (absolute index into the current scope's env). Throws if it's free
    // beyond the program's outermost scope.
    uint32_t resolve_var(uint32_t db_relative, int scope_idx) {
        Scope& s = scopes_[scope_idx];
        if (db_relative < s.binders) {
            return db_relative;  // local (only slot 0 for Lambda)
        }
        uint32_t in_outer = db_relative - s.binders;
        auto it = s.db_to_upval.find(in_outer);
        if (it != s.db_to_upval.end()) {
            return s.binders + it->second;
        }
        if (scope_idx == 0) {
            throw std::runtime_error(
                "uplci: unbound variable at deBruijn index " +
                std::to_string(db_relative));
        }
        // Ensure the parent resolves this too — recursive capture.
        // We don't use the returned slot directly here; it's validated
        // by the recursion and will be re-resolved at MK_LAM emission.
        (void)resolve_var(in_outer, scope_idx - 1);

        uint32_t new_upval = (uint32_t)s.upval_outer_db.size();
        s.db_to_upval[in_outer] = new_upval;
        s.upval_outer_db.push_back(in_outer);
        return s.binders + new_upval;
    }

    // ---------- rterm dispatch ----------

    void emit(const uplc_rterm* t) {
        switch ((uplc_rterm_tag)t->tag) {
            case UPLC_RTERM_VAR: {
                // rterm uses 1-based deBruijn? Let's assume 0-based same as
                // the rterm_var constructor, which takes `index`. The CEK
                // interpreter reads env with `env[index]`, implying 1-based
                // (index 1 = head of a cons-env). Check by looking at cek.
                // For the emitter, we adapt: convert to 0-based by -1.
                uint32_t db = t->var.index;
                if (db == 0) {
                    throw std::runtime_error(
                        "uplci: deBruijn index 0 is invalid (expected >=1)");
                }
                uint32_t slot = resolve_var(db - 1, (int)scopes_.size() - 1);
                uint8_t op = (slot < scopes_.back().binders)
                             ? (uint8_t)UPLC_BC_VAR_LOCAL
                             : (uint8_t)UPLC_BC_VAR_UPVAL;
                emit_op(op, slot);
                push_stack(1);
                break;
            }
            case UPLC_RTERM_LAMBDA: {
                uint32_t inner_fn_id = open_scope(/*binders=*/1);
                scopes_.back().body_rterm = t->lambda.body;
                emit(t->lambda.body);
                emit_return();
                // Close and remember capture plan (outer dbs).
                std::vector<uint32_t> upvals_outer = scopes_.back().upval_outer_db;
                close_scope();
                // Emit MK_LAM in outer.
                emit_op(UPLC_BC_MK_LAM, inner_fn_id);
                emit_word((uplc_bc_word)upvals_outer.size());
                for (uint32_t outer_db : upvals_outer) {
                    uint32_t slot = resolve_var(outer_db,
                                                (int)scopes_.size() - 1);
                    emit_word(slot);
                }
                push_stack(1);
                break;
            }
            case UPLC_RTERM_DELAY: {
                uint32_t inner_fn_id = open_scope(/*binders=*/0);
                scopes_.back().body_rterm = t->delay.term;
                emit(t->delay.term);
                emit_return();
                std::vector<uint32_t> upvals_outer = scopes_.back().upval_outer_db;
                close_scope();
                emit_op(UPLC_BC_MK_DELAY, inner_fn_id);
                emit_word((uplc_bc_word)upvals_outer.size());
                for (uint32_t outer_db : upvals_outer) {
                    uint32_t slot = resolve_var(outer_db,
                                                (int)scopes_.size() - 1);
                    emit_word(slot);
                }
                push_stack(1);
                break;
            }
            case UPLC_RTERM_APPLY: {
                emit(t->apply.fn);
                emit(t->apply.arg);
                emit_op(UPLC_BC_APPLY, 0);
                pop_stack(2); push_stack(1);
                break;
            }
            case UPLC_RTERM_FORCE: {
                emit(t->force.term);
                emit_op(UPLC_BC_FORCE, 0);
                /* FORCE pops one and pushes one — net zero. */
                break;
            }
            case UPLC_RTERM_CONSTANT: {
                uplc_value v = uplc_make_rcon(t->constant.value);
                uint32_t idx = intern_const(v);
                emit_op(UPLC_BC_CONST, idx);
                push_stack(1);
                break;
            }
            case UPLC_RTERM_BUILTIN: {
                emit_op(UPLC_BC_BUILTIN, t->builtin.tag);
                push_stack(1);
                break;
            }
            case UPLC_RTERM_ERROR: {
                emit_op(UPLC_BC_ERROR, 0);
                /* ERROR raises and doesn't actually push, but we pretend
                 * it pushes so parent emitters see the expected depth
                 * invariant. Dead-code after this path would never
                 * execute at runtime. */
                push_stack(1);
                break;
            }
            case UPLC_RTERM_CONSTR: {
                for (uint32_t i = 0; i < t->constr.n_fields; ++i) {
                    emit(t->constr.fields[i]);
                }
                emit_op(UPLC_BC_CONSTR, t->constr.n_fields);
                emit_word((uplc_bc_word)(t->constr.tag_index & 0xFFFFFFFFu));
                emit_word((uplc_bc_word)(t->constr.tag_index >> 32));
                pop_stack(t->constr.n_fields); push_stack(1);
                break;
            }
            case UPLC_RTERM_CASE: {
                // Each alt is emitted as its own sub-function (Delay-
                // shaped: no arg). After closing, we remember the alt's
                // capture plan (outer deBruijn indices) so we can
                // resolve them into slots in the enclosing env when we
                // emit the CASE instruction below.
                struct AltPlan {
                    uint32_t              fn_id;
                    std::vector<uint32_t> upvals_outer_db;
                };
                std::vector<AltPlan> alt_plans;
                alt_plans.reserve(t->case_.n_branches);

                for (uint32_t i = 0; i < t->case_.n_branches; ++i) {
                    uint32_t alt_id = open_scope(/*binders=*/0);
                    /* Alts run under a sub-dispatch boundary frame
                     * whose ret_pc == NULL; tail-eliminating their
                     * RETURN would skip that boundary check and
                     * dispatch to some other frame's ret_pc instead
                     * of handing the value back to op_case. Disable
                     * the tail-merge for alt bodies. */
                    bool saved = tail_optimize_next_return_;
                    tail_optimize_next_return_ = false;
                    emit(t->case_.branches[i]);
                    emit_return();
                    tail_optimize_next_return_ = saved;
                    std::vector<uint32_t> upvals = scopes_.back().upval_outer_db;
                    close_scope();
                    alt_plans.push_back({alt_id, std::move(upvals)});
                }

                // Scrutinee evaluates last — it leaves a VConstr (or a
                // constant-as-constr) on the stack that CASE pops.
                emit(t->case_.scrutinee);

                // CASE instruction: n_alts in imm24, then per-alt
                // {fn_id, nfree, slots[nfree]}.
                emit_op(UPLC_BC_CASE, t->case_.n_branches);
                for (auto& a : alt_plans) {
                    emit_word(a.fn_id);
                    emit_word((uplc_bc_word)a.upvals_outer_db.size());
                    for (uint32_t outer_db : a.upvals_outer_db) {
                        uint32_t slot = resolve_var(
                            outer_db, (int)scopes_.size() - 1);
                        emit_word(slot);
                    }
                }
                /* CASE pops the scrutinee, pushes the alt result —
                 * net zero on top of whatever the scrutinee pushed. */
                pop_stack(1); push_stack(1);
                break;
            }
            default:
                throw std::runtime_error("uplci: unknown rterm tag");
        }
    }

    uplc_arena*                rt_arena_;
    std::vector<Scope>         scopes_;
    std::vector<uplc_bc_fn>    functions_;
    std::vector<uplc_value>    consts_;
    uint32_t                   next_fn_id_ = 0;
};

}  // namespace

std::unique_ptr<ProgramOwner> lower_rprogram(uplc_arena* rt_arena,
                                             const uplc_rprogram& rp) {
    Lowerer l(rt_arena);
    return l.run(rp);
}

}  // namespace uplc_bc
