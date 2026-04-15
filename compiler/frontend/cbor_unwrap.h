#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace uplc {

// On-chain Plutus scripts are distributed wrapped in a CBOR byte string —
// sometimes double-wrapped (the inner CBOR byte string contains another CBOR
// byte string whose payload is the flat program).
//
// This helper peels the wrapping, returning the innermost bytes. It only
// looks at CBOR major type 2 (byte string) headers; anything else is returned
// as-is. Passing already-raw flat bytes is therefore safe — they just come
// back unchanged.
//
// Accepts both definite-length byte strings (0x40..0x5b) and the single
// indefinite-length form (0x5f).
std::vector<std::uint8_t> cbor_unwrap(const std::uint8_t* bytes, std::size_t len);

}  // namespace uplc
