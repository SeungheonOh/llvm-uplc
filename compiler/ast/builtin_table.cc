#include "compiler/ast/builtin_tag.h"

#include <array>
#include <cstring>
#include <string_view>

namespace uplc {

namespace {

struct BuiltinInfo {
    std::string_view name;
    std::uint8_t     arity;
    std::uint8_t     force_count;
};

// Indexed by BuiltinTag ordinal. Rows match TS types.ts DEFAULT_FUNCTION_ARITIES
// and DEFAULT_FUNCTION_FORCE_COUNTS.
constexpr std::array<BuiltinInfo, kBuiltinCount> kBuiltinTable = {{
    // 0-9  integer ops
    {"addInteger",                      2, 0},
    {"subtractInteger",                 2, 0},
    {"multiplyInteger",                 2, 0},
    {"divideInteger",                   2, 0},
    {"quotientInteger",                 2, 0},
    {"remainderInteger",                2, 0},
    {"modInteger",                      2, 0},
    {"equalsInteger",                   2, 0},
    {"lessThanInteger",                 2, 0},
    {"lessThanEqualsInteger",           2, 0},
    // 10-17  bytestring ops
    {"appendByteString",                2, 0},
    {"consByteString",                  2, 0},
    {"sliceByteString",                 3, 0},
    {"lengthOfByteString",              1, 0},
    {"indexByteString",                 2, 0},
    {"equalsByteString",                2, 0},
    {"lessThanByteString",              2, 0},
    {"lessThanEqualsByteString",        2, 0},
    // 18-21  hashes + first signature
    {"sha2_256",                        1, 0},
    {"sha3_256",                        1, 0},
    {"blake2b_256",                     1, 0},
    {"verifyEd25519Signature",          3, 0},
    // 22-25  string ops
    {"appendString",                    2, 0},
    {"equalsString",                    2, 0},
    {"encodeUtf8",                      1, 0},
    {"decodeUtf8",                      1, 0},
    // 26-28  control
    {"ifThenElse",                      3, 1},
    {"chooseUnit",                      2, 1},
    {"trace",                           2, 1},
    // 29-30  pair
    {"fstPair",                         1, 2},
    {"sndPair",                         1, 2},
    // 31-35  list
    {"chooseList",                      3, 2},
    {"mkCons",                          2, 1},
    {"headList",                        1, 1},
    {"tailList",                        1, 1},
    {"nullList",                        1, 1},
    // 36-51  data
    {"chooseData",                      6, 1},
    {"constrData",                      2, 0},
    {"mapData",                         1, 0},
    {"listData",                        1, 0},
    {"iData",                           1, 0},
    {"bData",                           1, 0},
    {"unConstrData",                    1, 0},
    {"unMapData",                       1, 0},
    {"unListData",                      1, 0},
    {"unIData",                         1, 0},
    {"unBData",                         1, 0},
    {"equalsData",                      2, 0},
    {"mkPairData",                      2, 0},
    {"mkNilData",                       1, 0},
    {"mkNilPairData",                   1, 0},
    {"serialiseData",                   1, 0},
    // 52-53  ECDSA / Schnorr
    {"verifyEcdsaSecp256k1Signature",   3, 0},
    {"verifySchnorrSecp256k1Signature", 3, 0},
    // 54-70  BLS12-381
    {"bls12_381_G1_add",                2, 0},
    {"bls12_381_G1_neg",                1, 0},
    {"bls12_381_G1_scalarMul",          2, 0},
    {"bls12_381_G1_equal",              2, 0},
    {"bls12_381_G1_compress",           1, 0},
    {"bls12_381_G1_uncompress",         1, 0},
    {"bls12_381_G1_hashToGroup",        2, 0},
    {"bls12_381_G2_add",                2, 0},
    {"bls12_381_G2_neg",                1, 0},
    {"bls12_381_G2_scalarMul",          2, 0},
    {"bls12_381_G2_equal",              2, 0},
    {"bls12_381_G2_compress",           1, 0},
    {"bls12_381_G2_uncompress",         1, 0},
    {"bls12_381_G2_hashToGroup",        2, 0},
    {"bls12_381_millerLoop",            2, 0},
    {"bls12_381_mulMlResult",           2, 0},
    {"bls12_381_finalVerify",           2, 0},
    // 71-72  more hashes
    {"keccak_256",                      1, 0},
    {"blake2b_224",                     1, 0},
    // 73-74  integer/bytestring conversion
    {"integerToByteString",             3, 0},
    {"byteStringToInteger",             2, 0},
    // 75-85  bitwise
    {"andByteString",                   3, 0},
    {"orByteString",                    3, 0},
    {"xorByteString",                   3, 0},
    {"complementByteString",            1, 0},
    {"readBit",                         2, 0},
    {"writeBits",                       3, 0},
    {"replicateByte",                   2, 0},
    {"shiftByteString",                 2, 0},
    {"rotateByteString",                2, 0},
    {"countSetBits",                    1, 0},
    {"findFirstSetBit",                 1, 0},
    // 86  ripemd
    {"ripemd_160",                      1, 0},
    // 87  expMod
    {"expModInteger",                   3, 0},
    // 88-91  list/array
    {"dropList",                        2, 1},
    {"lengthOfArray",                   1, 1},
    {"listToArray",                     1, 1},
    {"indexArray",                      2, 1},
    // 92-93  BLS msm
    {"bls12_381_G1_multiScalarMul",     2, 0},
    {"bls12_381_G2_multiScalarMul",     2, 0},
    // 94-100  value
    {"insertCoin",                      4, 0},
    {"lookupCoin",                      3, 0},
    {"unionValue",                      2, 0},
    {"valueContains",                   2, 0},
    {"valueData",                       1, 0},
    {"unValueData",                     1, 0},
    {"scaleValue",                      2, 0},
}};

}  // namespace

std::string_view builtin_name(BuiltinTag tag) {
    auto idx = static_cast<std::size_t>(tag);
    return kBuiltinTable[idx].name;
}

std::uint8_t builtin_arity(BuiltinTag tag) {
    return kBuiltinTable[static_cast<std::size_t>(tag)].arity;
}

std::uint8_t builtin_force_count(BuiltinTag tag) {
    return kBuiltinTable[static_cast<std::size_t>(tag)].force_count;
}

std::optional<BuiltinTag> builtin_from_name(std::string_view name) {
    // Linear scan; 101 short strings is fine. Callers already know the set is
    // small. If this ever shows up in a profile we'll swap for a hash table.
    for (std::size_t i = 0; i < kBuiltinTable.size(); ++i) {
        if (kBuiltinTable[i].name == name) {
            return static_cast<BuiltinTag>(i);
        }
    }
    return std::nullopt;
}

std::optional<BuiltinTag> builtin_from_tag(std::uint8_t tag) {
    if (tag >= kBuiltinCount) return std::nullopt;
    return static_cast<BuiltinTag>(tag);
}

}  // namespace uplc
