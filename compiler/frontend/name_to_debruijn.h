#pragma once

#include <stdexcept>
#include <string>

#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"

namespace uplc {

class ConvertError : public std::runtime_error {
public:
    explicit ConvertError(const std::string& msg) : std::runtime_error(msg) {}
};

// Rewrite a named Program in place to a de-Bruijn-indexed Program. Lookups
// use a unique-id -> lexical-level BiMap stack; the algorithm matches TS
// convert.ts exactly so indices stay bit-identical with the reference.
Program name_to_debruijn(Arena& arena, Program program);

// Reverse pass: turn a de-Bruijn program back into a named one, assigning
// each binder a fresh unique name (`v0`, `v1`, ...). Used by the round-trip
// tests to go through the parser twice.
Program debruijn_to_name(Arena& arena, Program program);

}  // namespace uplc
