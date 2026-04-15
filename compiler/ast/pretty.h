#pragma once

#include <string>

#include "compiler/ast/term.h"

namespace uplc {

// Pretty-print a Program back to UPLC text. Mirrors TS pretty.ts for each
// printed form so output is byte-for-byte comparable.
std::string pretty_print_program(const Program& program);

// Pretty-print a term in de-Bruijn form (`i<index>` variables).
std::string pretty_print_debruijn(Term* term);

// Pretty-print a term in named form (uses the binder.text on each node).
std::string pretty_print_named(Term* term);

// Pretty-print an individual constant / type / PlutusData (exposed for tests
// and error messages).
std::string print_constant(const Constant& c);
std::string print_constant_type(const ConstantType& t);
std::string print_plutus_data(const PlutusData& d);

}  // namespace uplc
