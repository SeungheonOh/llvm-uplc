#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "compiler/ast/arena.h"
#include "compiler/ast/term.h"

namespace uplc {

class CborError : public std::runtime_error {
public:
    explicit CborError(const std::string& msg) : std::runtime_error(msg) {}
};

// Encode a PlutusData tree as CBOR bytes. Matches the byte-level output of
// TS cbor.ts so runtime `serialiseData` results are bit-compatible.
std::vector<std::uint8_t> encode_plutus_data(const PlutusData& data);

// Decode a CBOR byte sequence into a PlutusData tree allocated from `arena`.
// Throws CborError on malformed input.
PlutusData* decode_plutus_data(Arena& arena,
                               const std::uint8_t* bytes,
                               std::size_t len);

}  // namespace uplc
