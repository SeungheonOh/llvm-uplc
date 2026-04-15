#pragma once

#include <string_view>

#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/lexer.h"

namespace uplc {

// Parse a UPLC text program into a named AST allocated out of `arena`.
// Throws ParseError on failure. The returned Program is valid as long as
// the arena is alive.
Program parse_program(Arena& arena, std::string_view source);

}  // namespace uplc
