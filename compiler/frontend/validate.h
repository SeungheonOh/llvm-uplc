#pragma once

#include <stdexcept>
#include <string>

#include "compiler/ast/term.h"

namespace uplc {

class ValidationError : public std::runtime_error {
public:
    explicit ValidationError(const std::string& msg) : std::runtime_error(msg) {}
};

// Validate a Program:
//   * builtin tag values are in-range (defensive; parser already rejects bad names)
//   * Constr / Case appear only when program.version >= 1.1.0
//   * term is closed: every Var refers to an enclosing binder
//
// Accepts both named and de-Bruijn programs. Throws ValidationError on the
// first problem it finds.
void validate(const Program& program);

}  // namespace uplc
