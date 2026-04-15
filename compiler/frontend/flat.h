#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"

namespace uplc {

// Decode a UPLC flat-encoded program into a de-Bruijn Program allocated out
// of `arena`. Mirrors TS flat.ts decodeFlatDeBruijn. Throws ParseError on
// malformed input.
Program decode_flat(Arena& arena, const std::uint8_t* bytes, std::size_t len);

// Encode a (de-Bruijn) Program back to flat bytes. Iterative walk matches TS
// flat.ts encodeFlatDeBruijn. Used by the round-trip test and (later) by the
// compiler when embedding the original term blob in a .uplcx artifact.
std::vector<std::uint8_t> encode_flat(const Program& program);

}  // namespace uplc
