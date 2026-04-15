#ifndef UPLC_CODEGEN_BAKED_CONST_H
#define UPLC_CODEGEN_BAKED_CONST_H

#include <cstdint>
#include <vector>

#include "compiler/ast/term.h"

namespace uplc {

/*
 * Serialize a complex Constant (Data, List, Pair, Array, BLS, Value) to
 * the wire format consumed by uplcrt_const_baked at runtime. Integers,
 * bytestrings, strings, bools, and units are also handled — useful for
 * tests, though the codegen prefers their dedicated runtime helpers.
 *
 * Integers serialize as: u8 sign, u32 nbytes, [nbytes BE magnitude].
 *
 * The wire format mirrors the runtime uplc_rconst_tag / uplc_rdata_tag
 * / uplc_rtype_tag enums byte-for-byte, so the decoder can dispatch on
 * the raw tag value without translation.
 *
 * Throws std::runtime_error on unsupported / malformed input (no MlResult
 * support yet — Plutus has no source-level syntax for it).
 */
std::vector<std::uint8_t> serialize_baked_constant(const Constant& c);

}  // namespace uplc

#endif
