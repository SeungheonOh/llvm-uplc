#include "compiler/frontend/name_to_debruijn.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace uplc {

namespace {

// ---------------------------------------------------------------------------
// name -> deBruijn
// ---------------------------------------------------------------------------

class BiMap {
public:
    void insert(std::uint32_t unique, std::uint32_t level) {
        unique_to_level_[unique] = level;
    }
    void remove(std::uint32_t unique) { unique_to_level_.erase(unique); }
    bool find(std::uint32_t unique, std::uint32_t& out_level) const {
        auto it = unique_to_level_.find(unique);
        if (it == unique_to_level_.end()) return false;
        out_level = it->second;
        return true;
    }

private:
    std::unordered_map<std::uint32_t, std::uint32_t> unique_to_level_;
};

class Converter {
public:
    explicit Converter(Arena& arena) : arena_(arena) {
        levels_.emplace_back();
    }

    Term* convert_term(Term* term) {
        switch (term->tag) {
            case TermTag::Var: {
                Binder b;
                b.id = get_index(term->var.binder.id, term->var.binder.text);
                b.text = nullptr;
                return make_var(arena_, b);
            }
            case TermTag::Lambda: {
                std::uint32_t unique = term->lambda.parameter.id;
                declare_unique(unique);
                // In TS the converter records `paramIndex = getIndex(unique)`
                // *before* descending, then the flat encoder later always
                // emits 0 for it. We set the new binder's id to 0 directly to
                // match the flat reader's expectation.
                start_scope();
                Term* body = convert_term(term->lambda.body);
                end_scope();
                remove_unique(unique);
                return make_lambda(arena_, Binder{0u, nullptr}, body);
            }
            case TermTag::Apply: {
                Term* fn = convert_term(term->apply.function);
                Term* arg = convert_term(term->apply.argument);
                return make_apply(arena_, fn, arg);
            }
            case TermTag::Delay:
                return make_delay(arena_, convert_term(term->delay.term));
            case TermTag::Force:
                return make_force(arena_, convert_term(term->force.term));
            case TermTag::Constr: {
                auto n = term->constr.n_fields;
                Term** fields = arena_.alloc_array_uninit<Term*>(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    fields[i] = convert_term(term->constr.fields[i]);
                }
                return make_constr(arena_, term->constr.tag_index, fields, n);
            }
            case TermTag::Case: {
                Term* sc = convert_term(term->case_.scrutinee);
                auto n = term->case_.n_branches;
                Term** br = arena_.alloc_array_uninit<Term*>(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    br[i] = convert_term(term->case_.branches[i]);
                }
                return make_case(arena_, sc, br, n);
            }
            case TermTag::Constant:
            case TermTag::Builtin:
            case TermTag::Error:
                // These carry no binders; reuse the pointer as-is. Caller owns
                // the arena and won't mutate.
                return term;
        }
        throw ConvertError("internal: unknown term tag in convert");
    }

private:
    std::uint32_t get_index(std::uint32_t unique, const char* text) {
        // Search outer-to-inner for the rightmost binding of `unique`.
        for (std::size_t i = levels_.size(); i-- > 0; ) {
            std::uint32_t lvl;
            if (levels_[i].find(unique, lvl)) {
                return current_level_ - lvl;
            }
        }
        std::string name = text ? text : "<?>";
        throw ConvertError("free variable `" + name + "` (unique " +
                           std::to_string(unique) + ")");
    }

    void declare_unique(std::uint32_t unique) {
        levels_.back().insert(unique, current_level_);
    }
    void remove_unique(std::uint32_t unique) {
        levels_.back().remove(unique);
    }
    void start_scope() {
        ++current_level_;
        levels_.emplace_back();
    }
    void end_scope() {
        --current_level_;
        levels_.pop_back();
    }

    Arena&               arena_;
    std::uint32_t        current_level_ = 0;
    std::vector<BiMap>   levels_;
};

// ---------------------------------------------------------------------------
// deBruijn -> name
// ---------------------------------------------------------------------------

class Dename {
public:
    explicit Dename(Arena& arena) : arena_(arena) {}

    Term* run(Term* term) {
        switch (term->tag) {
            case TermTag::Var: {
                std::uint32_t idx = term->var.binder.id;
                if (idx > scope_.size()) {
                    throw ConvertError("free de Bruijn index " + std::to_string(idx));
                }
                // de Bruijn index 1 is the innermost binder.
                std::size_t position = scope_.size() - idx;
                return make_var(arena_, scope_[position]);
            }
            case TermTag::Lambda: {
                Binder name = fresh_name();
                scope_.push_back(name);
                Term* body = run(term->lambda.body);
                scope_.pop_back();
                return make_lambda(arena_, name, body);
            }
            case TermTag::Apply:
                return make_apply(arena_,
                                  run(term->apply.function),
                                  run(term->apply.argument));
            case TermTag::Delay:
                return make_delay(arena_, run(term->delay.term));
            case TermTag::Force:
                return make_force(arena_, run(term->force.term));
            case TermTag::Constr: {
                auto n = term->constr.n_fields;
                Term** fields = arena_.alloc_array_uninit<Term*>(n);
                for (std::uint32_t i = 0; i < n; ++i) fields[i] = run(term->constr.fields[i]);
                return make_constr(arena_, term->constr.tag_index, fields, n);
            }
            case TermTag::Case: {
                Term* sc = run(term->case_.scrutinee);
                auto n = term->case_.n_branches;
                Term** br = arena_.alloc_array_uninit<Term*>(n);
                for (std::uint32_t i = 0; i < n; ++i) br[i] = run(term->case_.branches[i]);
                return make_case(arena_, sc, br, n);
            }
            case TermTag::Constant:
            case TermTag::Builtin:
            case TermTag::Error:
                return term;
        }
        throw ConvertError("internal: unknown term tag in dename");
    }

private:
    Binder fresh_name() {
        std::uint32_t unique = counter_++;
        std::string text = "v" + std::to_string(unique);
        return Binder{unique, arena_.intern_str(text)};
    }

    Arena&              arena_;
    std::vector<Binder> scope_;
    std::uint32_t       counter_ = 0;
};

}  // namespace

Program name_to_debruijn(Arena& arena, Program program) {
    if (program.is_debruijn) return program;
    Converter conv(arena);
    Term* new_term = conv.convert_term(program.term);
    return Program{program.version, new_term, /*is_debruijn=*/true};
}

Program debruijn_to_name(Arena& arena, Program program) {
    if (!program.is_debruijn) return program;
    Dename de(arena);
    Term* new_term = de.run(program.term);
    return Program{program.version, new_term, /*is_debruijn=*/false};
}

}  // namespace uplc
