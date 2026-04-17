#pragma once

// Disassembler for uplc_bc_program: produces a human-readable dump of
// the bytecode VM's internal representation. Used by `uplci --dump`
// and available as a library for tests that need to inspect emitter
// output.

#include <string>

#include "uplc/bytecode.h"

namespace uplc_bc {

// Pretty-print a whole bytecode program to a string. The output is
// self-contained — it prints the version header, the constant pool,
// and every function with its capture plan and decoded opcode stream.
//
// Operand decoding handles variable-length instructions (MK_LAM,
// MK_DELAY, CONSTR, CASE) correctly: it walks the instruction stream
// in opcode-first order so data words for those ops are consumed as
// their operands rather than being mis-read as opcodes.
std::string disassemble(const uplc_bc_program& prog);

}  // namespace uplc_bc
