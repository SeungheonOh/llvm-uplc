#include "compiler/frontend/validate.h"

#include <cstdint>

#include "compiler/ast/builtin_tag.h"

namespace uplc {

namespace {

class Validator {
public:
    explicit Validator(const Program& program) : program_(program) {}

    void run() {
        walk(program_.term);
    }

private:
    void walk(Term* term) {
        switch (term->tag) {
            case TermTag::Var:
                // Closedness of named programs is enforced by name_to_debruijn
                // (TS convert.ts throws ConvertError on a free unique). This
                // matches the reference's split between "syntactically valid"
                // (parse succeeds) and "evaluationally valid" (convert + eval
                // succeed): a program like `(program 1.0.0 x)` must parse so
                // that downstream reports "evaluation failure", not "parse
                // error".
                if (program_.is_debruijn) {
                    std::uint32_t idx = term->var.binder.id;
                    if (idx == 0 || idx > depth_) {
                        throw ValidationError(
                            "free de Bruijn index " + std::to_string(idx));
                    }
                }
                return;
            case TermTag::Lambda: {
                if (program_.is_debruijn) {
                    ++depth_;
                    walk(term->lambda.body);
                    --depth_;
                } else {
                    walk(term->lambda.body);
                }
                return;
            }
            case TermTag::Apply:
                walk(term->apply.function);
                walk(term->apply.argument);
                return;
            case TermTag::Delay:
                walk(term->delay.term);
                return;
            case TermTag::Force:
                walk(term->force.term);
                return;
            case TermTag::Constr: {
                if (program_.version.before_1_1_0()) {
                    throw ValidationError("constr requires Plutus >= 1.1.0");
                }
                for (std::uint32_t i = 0; i < term->constr.n_fields; ++i) {
                    walk(term->constr.fields[i]);
                }
                return;
            }
            case TermTag::Case: {
                if (program_.version.before_1_1_0()) {
                    throw ValidationError("case requires Plutus >= 1.1.0");
                }
                walk(term->case_.scrutinee);
                for (std::uint32_t i = 0; i < term->case_.n_branches; ++i) {
                    walk(term->case_.branches[i]);
                }
                return;
            }
            case TermTag::Builtin: {
                auto tag = static_cast<std::uint8_t>(term->builtin.function);
                if (tag >= kBuiltinCount) {
                    throw ValidationError(
                        "invalid builtin tag " + std::to_string(tag));
                }
                return;
            }
            case TermTag::Constant:
            case TermTag::Error:
                return;
        }
    }

    const Program& program_;
    std::uint32_t  depth_ = 0;  // used when is_debruijn
};

}  // namespace

void validate(const Program& program) {
    Validator(program).run();
}

}  // namespace uplc
