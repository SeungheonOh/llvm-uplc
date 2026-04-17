#pragma once

// The runtime headers already declare their symbols with extern "C" via
// their own __cplusplus guards, so we include them unwrapped — wrapping
// them in a second extern "C" block would drag gmp.h's libstdc++ bits
// into C linkage and break libc++.
#include "runtime/core/arena.h"
#include "runtime/core/rterm.h"

#include "compiler/ast/term.h"

namespace uplc {

// Walk a C++ Program (de Bruijn form) and allocate the equivalent runtime
// rterm tree out of `rt_arena`. The returned pointer is valid until the
// runtime arena is destroyed.
//
// Throws std::runtime_error if the program is not in de Bruijn form or
// contains constant types the runtime does not yet model.
uplc_rprogram lower_to_runtime(uplc_arena* rt_arena, const Program& program);

// Inverse: walk a runtime rterm tree and allocate the equivalent compiler
// Term / Constant / PlutusData out of `compiler_arena`. Used by the M4
// conformance runner to pretty-print post-readback CEK values via the
// compiler's existing Term<DeBruijn> pretty-printer.
Term*   lift_term_from_runtime(Arena& compiler_arena, const uplc_rterm& term);
Program lift_program_from_runtime(Arena& compiler_arena, const uplc_rprogram& program);

}  // namespace uplc
