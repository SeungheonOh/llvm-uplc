#pragma once

#include <cstddef>
#include <cstdint>

#include "compiler/ast/arena.h"
#include "compiler/ast/builtin_tag.h"

namespace uplc {

// ---------------------------------------------------------------------------
// Binder — dual-use: holds either a unique id (named AST) or a de Bruijn
// index (converted AST). `text` is non-null for named AST, null otherwise.
// ---------------------------------------------------------------------------
struct Binder {
    std::uint32_t id;    // unique id OR de Bruijn index, depending on Program::is_debruijn
    const char*   text;  // interned in the arena, or nullptr for de Bruijn
};

// ---------------------------------------------------------------------------
// ConstantType — recursive type annotation for `con` values.
// ---------------------------------------------------------------------------
enum class ConstantTypeTag : std::uint8_t {
    Integer,
    ByteString,
    String,
    Bool,
    Unit,
    Data,
    Bls12_381_G1,
    Bls12_381_G2,
    Bls12_381_MlResult,
    Value,
    List,
    Array,
    Pair,
};

struct ConstantType {
    ConstantTypeTag tag;
    union {
        // list / array
        struct {
            ConstantType* element;
        } list;
        struct {
            ConstantType* element;
        } array;
        struct {
            ConstantType* first;
            ConstantType* second;
        } pair;
    };
};

// ---------------------------------------------------------------------------
// PlutusData — the Data universe: constr, map, list, integer, bytestring.
// ---------------------------------------------------------------------------
enum class PlutusDataTag : std::uint8_t {
    Constr,
    Map,
    List,
    Integer,
    ByteString,
};

struct PlutusData;
struct PlutusDataPair {
    PlutusData* key;
    PlutusData* value;
};

struct PlutusData {
    PlutusDataTag tag;
    union {
        struct {
            BigInt*      index;         // non-negative integer (Haskell Natural)
            PlutusData** fields;
            std::uint32_t n_fields;
        } constr;
        struct {
            PlutusDataPair* entries;
            std::uint32_t   n_entries;
        } map;
        struct {
            PlutusData**  values;
            std::uint32_t n_values;
        } list;
        struct {
            BigInt* value;
        } integer;
        struct {
            const std::uint8_t* bytes;
            std::uint32_t       len;
        } bytestring;
    };
};

// ---------------------------------------------------------------------------
// LedgerValue — for the experimental `value` constant universe. Kept simple;
// not yet parsed in M1.
// ---------------------------------------------------------------------------
struct LedgerValueToken {
    const std::uint8_t* name_bytes;
    std::uint32_t       name_len;
    BigInt*             quantity;
};

struct LedgerValueEntry {
    const std::uint8_t* currency_bytes;
    std::uint32_t       currency_len;
    LedgerValueToken*   tokens;
    std::uint32_t       n_tokens;
};

struct LedgerValue {
    LedgerValueEntry* entries;
    std::uint32_t     n_entries;
};

// ---------------------------------------------------------------------------
// Constant — tagged union matching TS Constant.
// ---------------------------------------------------------------------------
enum class ConstTag : std::uint8_t {
    Integer,
    ByteString,
    String,
    Bool,
    Unit,
    Data,
    Bls12_381_G1,
    Bls12_381_G2,
    Bls12_381_MlResult,
    Value,
    List,
    Pair,
    Array,
};

struct Constant;  // forward decl for recursion in list/pair

struct Constant {
    ConstTag tag;
    union {
        struct {
            BigInt* value;
        } integer;
        struct {
            const std::uint8_t* bytes;
            std::uint32_t       len;
        } bytestring;
        struct {
            const char*   utf8;   // NOT NUL-terminated; use `len`
            std::uint32_t len;
        } string;
        struct {
            bool value;
        } boolean;
        struct {
            PlutusData* value;
        } data;
        struct {
            const std::uint8_t* bytes;  // 48 bytes
        } bls_g1;
        struct {
            const std::uint8_t* bytes;  // 96 bytes
        } bls_g2;
        struct {
            const std::uint8_t* bytes;
            std::uint32_t       len;
        } bls_ml_result;
        struct {
            LedgerValue* value;
        } value;
        struct {
            ConstantType* item_type;
            Constant**    values;
            std::uint32_t n_values;
        } list;
        struct {
            ConstantType* fst_type;
            ConstantType* snd_type;
            Constant*     first;
            Constant*     second;
        } pair;
        struct {
            ConstantType* item_type;
            Constant**    values;
            std::uint32_t n_values;
        } array;
    };
};

// ---------------------------------------------------------------------------
// Term — tagged union over the 10 UPLC term constructors.
// ---------------------------------------------------------------------------
enum class TermTag : std::uint8_t {
    Var      = 0,
    Delay    = 1,
    Lambda   = 2,
    Apply    = 3,
    Constant = 4,
    Force    = 5,
    Error    = 6,
    Builtin  = 7,
    Constr   = 8,
    Case     = 9,
};

struct Term;  // forward

struct Term {
    TermTag tag;
    union {
        struct {
            Binder binder;  // id + optional text
        } var;
        struct {
            Binder parameter;
            Term*  body;
        } lambda;
        struct {
            Term* function;
            Term* argument;
        } apply;
        struct {
            Constant* value;
        } constant;
        struct {
            BuiltinTag function;
        } builtin;
        struct {
            Term* term;
        } delay;
        struct {
            Term* term;
        } force;
        struct {
            std::uint64_t tag_index;   // Constr tag (from spec: 0 .. 2^64 - 1)
            Term**        fields;
            std::uint32_t n_fields;
        } constr;
        struct {
            Term*         scrutinee;
            Term**        branches;
            std::uint32_t n_branches;
        } case_;
        // Error has no payload.
    };
};

struct Version {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;

    bool before_1_1_0() const {
        return major < 1 || (major == 1 && minor < 1);
    }
};

struct Program {
    Version version;
    Term*   term;
    bool    is_debruijn;  // false after parse, true after name_to_debruijn
};

// ---------------------------------------------------------------------------
// Constructors — all allocate into the arena. Convenience wrappers so the
// parser and converter don't have to touch the union directly.
// ---------------------------------------------------------------------------
Term* make_var(Arena& a, Binder binder);
Term* make_lambda(Arena& a, Binder parameter, Term* body);
Term* make_apply(Arena& a, Term* function, Term* argument);
Term* make_delay(Arena& a, Term* inner);
Term* make_force(Arena& a, Term* inner);
Term* make_error(Arena& a);
Term* make_builtin(Arena& a, BuiltinTag fn);
Term* make_constant(Arena& a, Constant* c);
Term* make_constr(Arena& a, std::uint64_t index, Term** fields, std::uint32_t n_fields);
Term* make_case(Arena& a, Term* scrutinee, Term** branches, std::uint32_t n_branches);

Constant* make_const_integer(Arena& a, BigInt* value);
Constant* make_const_bytestring(Arena& a, const std::uint8_t* bytes, std::uint32_t len);
Constant* make_const_string(Arena& a, const char* utf8, std::uint32_t len);
Constant* make_const_bool(Arena& a, bool value);
Constant* make_const_unit(Arena& a);
Constant* make_const_data(Arena& a, PlutusData* value);
Constant* make_const_list(Arena& a, ConstantType* item_type, Constant** values, std::uint32_t n);
Constant* make_const_pair(Arena& a, ConstantType* fst_type, ConstantType* snd_type,
                          Constant* first, Constant* second);

ConstantType* make_type_simple(Arena& a, ConstantTypeTag tag);
ConstantType* make_type_list(Arena& a, ConstantType* element);
ConstantType* make_type_array(Arena& a, ConstantType* element);
ConstantType* make_type_pair(Arena& a, ConstantType* first, ConstantType* second);

PlutusData* make_data_integer(Arena& a, BigInt* value);
PlutusData* make_data_bytestring(Arena& a, const std::uint8_t* bytes, std::uint32_t len);
PlutusData* make_data_list(Arena& a, PlutusData** values, std::uint32_t n);
PlutusData* make_data_map(Arena& a, PlutusDataPair* entries, std::uint32_t n);
PlutusData* make_data_constr(Arena& a, BigInt* index, PlutusData** fields, std::uint32_t n);

}  // namespace uplc
