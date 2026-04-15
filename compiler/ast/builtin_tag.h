#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace uplc {

// Numeric values match the UPLC flat-encoding builtin tag table
// (BUILTIN_TAG_TO_NAME in the TS reference flat.ts).
enum class BuiltinTag : std::uint8_t {
    AddInteger                     = 0,
    SubtractInteger                = 1,
    MultiplyInteger                = 2,
    DivideInteger                  = 3,
    QuotientInteger                = 4,
    RemainderInteger               = 5,
    ModInteger                     = 6,
    EqualsInteger                  = 7,
    LessThanInteger                = 8,
    LessThanEqualsInteger          = 9,
    AppendByteString               = 10,
    ConsByteString                 = 11,
    SliceByteString                = 12,
    LengthOfByteString             = 13,
    IndexByteString                = 14,
    EqualsByteString               = 15,
    LessThanByteString             = 16,
    LessThanEqualsByteString       = 17,
    Sha2_256                       = 18,
    Sha3_256                       = 19,
    Blake2b_256                    = 20,
    VerifyEd25519Signature         = 21,
    AppendString                   = 22,
    EqualsString                   = 23,
    EncodeUtf8                     = 24,
    DecodeUtf8                     = 25,
    IfThenElse                     = 26,
    ChooseUnit                     = 27,
    Trace                          = 28,
    FstPair                        = 29,
    SndPair                        = 30,
    ChooseList                     = 31,
    MkCons                         = 32,
    HeadList                       = 33,
    TailList                       = 34,
    NullList                       = 35,
    ChooseData                     = 36,
    ConstrData                     = 37,
    MapData                        = 38,
    ListData                       = 39,
    IData                          = 40,
    BData                          = 41,
    UnConstrData                   = 42,
    UnMapData                      = 43,
    UnListData                     = 44,
    UnIData                        = 45,
    UnBData                        = 46,
    EqualsData                     = 47,
    MkPairData                     = 48,
    MkNilData                      = 49,
    MkNilPairData                  = 50,
    SerialiseData                  = 51,
    VerifyEcdsaSecp256k1Signature  = 52,
    VerifySchnorrSecp256k1Signature = 53,
    Bls12_381_G1_Add               = 54,
    Bls12_381_G1_Neg               = 55,
    Bls12_381_G1_ScalarMul         = 56,
    Bls12_381_G1_Equal             = 57,
    Bls12_381_G1_Compress          = 58,
    Bls12_381_G1_Uncompress        = 59,
    Bls12_381_G1_HashToGroup       = 60,
    Bls12_381_G2_Add               = 61,
    Bls12_381_G2_Neg               = 62,
    Bls12_381_G2_ScalarMul         = 63,
    Bls12_381_G2_Equal             = 64,
    Bls12_381_G2_Compress          = 65,
    Bls12_381_G2_Uncompress        = 66,
    Bls12_381_G2_HashToGroup       = 67,
    Bls12_381_MillerLoop           = 68,
    Bls12_381_MulMlResult          = 69,
    Bls12_381_FinalVerify          = 70,
    Keccak_256                     = 71,
    Blake2b_224                    = 72,
    IntegerToByteString            = 73,
    ByteStringToInteger            = 74,
    AndByteString                  = 75,
    OrByteString                   = 76,
    XorByteString                  = 77,
    ComplementByteString           = 78,
    ReadBit                        = 79,
    WriteBits                      = 80,
    ReplicateByte                  = 81,
    ShiftByteString                = 82,
    RotateByteString               = 83,
    CountSetBits                   = 84,
    FindFirstSetBit                = 85,
    Ripemd_160                     = 86,
    ExpModInteger                  = 87,
    DropList                       = 88,
    LengthOfArray                  = 89,
    ListToArray                    = 90,
    IndexArray                     = 91,
    Bls12_381_G1_MultiScalarMul    = 92,
    Bls12_381_G2_MultiScalarMul    = 93,
    InsertCoin                     = 94,
    LookupCoin                     = 95,
    UnionValue                     = 96,
    ValueContains                  = 97,
    ValueData                      = 98,
    UnValueData                    = 99,
    ScaleValue                     = 100,
};

inline constexpr std::uint8_t kBuiltinCount = 101;

// Lowest-level metadata. Static tables defined in builtin_table.cc.
std::string_view builtin_name(BuiltinTag tag);
std::uint8_t     builtin_arity(BuiltinTag tag);
std::uint8_t     builtin_force_count(BuiltinTag tag);

// Returns std::nullopt for unknown names.
std::optional<BuiltinTag> builtin_from_name(std::string_view name);

// Returns std::nullopt if `tag` is outside [0, kBuiltinCount).
std::optional<BuiltinTag> builtin_from_tag(std::uint8_t tag);

}  // namespace uplc
