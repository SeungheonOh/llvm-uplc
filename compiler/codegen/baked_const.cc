#include "compiler/codegen/baked_const.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include <gmp.h>

#include "compiler/ast/term.h"

namespace uplc {

namespace {

/* --- Runtime tag values (must stay in sync with runtime/cek/rterm.h) ----- */

enum RConstTag : std::uint8_t {
    RC_INTEGER              = 0,
    RC_BYTESTRING           = 1,
    RC_STRING               = 2,
    RC_BOOL                 = 3,
    RC_UNIT                 = 4,
    RC_DATA                 = 5,
    RC_BLS12_381_G1         = 6,
    RC_BLS12_381_G2         = 7,
    RC_BLS12_381_ML_RESULT  = 8,
    RC_VALUE                = 9,
    RC_LIST                 = 10,
    RC_PAIR                 = 11,
    RC_ARRAY                = 12,
};

enum RTypeTag : std::uint8_t {
    RT_INTEGER              = 0,
    RT_BYTESTRING           = 1,
    RT_STRING               = 2,
    RT_UNIT                 = 3,
    RT_BOOL                 = 4,
    RT_DATA                 = 5,
    RT_BLS12_381_G1         = 6,
    RT_BLS12_381_G2         = 7,
    RT_BLS12_381_ML_RESULT  = 8,
    RT_VALUE                = 9,
    RT_LIST                 = 10,
    RT_PAIR                 = 11,
    RT_ARRAY                = 12,
};

enum RDataTag : std::uint8_t {
    RD_CONSTR     = 0,
    RD_MAP        = 1,
    RD_LIST       = 2,
    RD_INTEGER    = 3,
    RD_BYTESTRING = 4,
};

/* --- Cursor helpers ------------------------------------------------------ */

class Writer {
public:
    void u8(std::uint8_t v) { buf_.push_back(v); }
    void u32(std::uint32_t v) {
        buf_.push_back(static_cast<std::uint8_t>(v & 0xff));
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
        buf_.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
        buf_.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    }
    void bytes(const std::uint8_t* p, std::uint32_t n) {
        buf_.insert(buf_.end(), p, p + n);
    }

    std::vector<std::uint8_t> take() && { return std::move(buf_); }

private:
    std::vector<std::uint8_t> buf_;
};

/* Write a sign+magnitude integer payload from a GMP mpz. */
void write_integer(Writer& w, const BigInt& bi) {
    int sign = mpz_sgn(bi.value);
    if (sign == 0) {
        w.u8(0);
        w.u32(0);
        return;
    }
    /* mpz_export gives MSB-first byte array of |bi|. */
    std::size_t count = (mpz_sizeinbase(bi.value, 2) + 7) / 8;
    std::vector<std::uint8_t> mag(count);
    std::size_t actual = 0;
    mpz_export(mag.data(), &actual, 1, 1, 0, 0, bi.value);
    /* Trim leading zero, if any (shouldn't happen, but be safe). */
    while (actual > 1 && mag[0] == 0) {
        mag.erase(mag.begin());
        --actual;
    }
    w.u8(sign < 0 ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(actual));
    w.bytes(mag.data(), static_cast<std::uint32_t>(actual));
}

/* --- Type encoder -------------------------------------------------------- */

void write_type(Writer& w, const ConstantType& t) {
    switch (t.tag) {
        case ConstantTypeTag::Integer:            w.u8(RT_INTEGER);            return;
        case ConstantTypeTag::ByteString:         w.u8(RT_BYTESTRING);         return;
        case ConstantTypeTag::String:             w.u8(RT_STRING);             return;
        case ConstantTypeTag::Unit:               w.u8(RT_UNIT);               return;
        case ConstantTypeTag::Bool:               w.u8(RT_BOOL);               return;
        case ConstantTypeTag::Data:               w.u8(RT_DATA);               return;
        case ConstantTypeTag::Bls12_381_G1:       w.u8(RT_BLS12_381_G1);       return;
        case ConstantTypeTag::Bls12_381_G2:       w.u8(RT_BLS12_381_G2);       return;
        case ConstantTypeTag::Bls12_381_MlResult: w.u8(RT_BLS12_381_ML_RESULT); return;
        case ConstantTypeTag::Value:              w.u8(RT_VALUE);              return;
        case ConstantTypeTag::List:
            w.u8(RT_LIST);
            write_type(w, *t.list.element);
            return;
        case ConstantTypeTag::Array:
            w.u8(RT_ARRAY);
            write_type(w, *t.array.element);
            return;
        case ConstantTypeTag::Pair:
            w.u8(RT_PAIR);
            write_type(w, *t.pair.first);
            write_type(w, *t.pair.second);
            return;
    }
    throw std::runtime_error("baked_const: unknown ConstantTypeTag");
}

/* --- PlutusData encoder -------------------------------------------------- */

void write_data(Writer& w, const PlutusData& d) {
    switch (d.tag) {
        case PlutusDataTag::Constr:
            w.u8(RD_CONSTR);
            write_integer(w, *d.constr.index);
            w.u32(d.constr.n_fields);
            for (std::uint32_t i = 0; i < d.constr.n_fields; ++i) {
                write_data(w, *d.constr.fields[i]);
            }
            return;
        case PlutusDataTag::Map:
            w.u8(RD_MAP);
            w.u32(d.map.n_entries);
            for (std::uint32_t i = 0; i < d.map.n_entries; ++i) {
                write_data(w, *d.map.entries[i].key);
                write_data(w, *d.map.entries[i].value);
            }
            return;
        case PlutusDataTag::List:
            w.u8(RD_LIST);
            w.u32(d.list.n_values);
            for (std::uint32_t i = 0; i < d.list.n_values; ++i) {
                write_data(w, *d.list.values[i]);
            }
            return;
        case PlutusDataTag::Integer:
            w.u8(RD_INTEGER);
            write_integer(w, *d.integer.value);
            return;
        case PlutusDataTag::ByteString:
            w.u8(RD_BYTESTRING);
            w.u32(d.bytestring.len);
            w.bytes(d.bytestring.bytes, d.bytestring.len);
            return;
    }
    throw std::runtime_error("baked_const: unknown PlutusDataTag");
}

/* --- Constant encoder ---------------------------------------------------- */

void write_constant(Writer& w, const Constant& c) {
    switch (c.tag) {
        case ConstTag::Integer:
            w.u8(RC_INTEGER);
            write_integer(w, *c.integer.value);
            return;
        case ConstTag::ByteString:
            w.u8(RC_BYTESTRING);
            w.u32(c.bytestring.len);
            w.bytes(c.bytestring.bytes, c.bytestring.len);
            return;
        case ConstTag::String:
            w.u8(RC_STRING);
            w.u32(c.string.len);
            w.bytes(reinterpret_cast<const std::uint8_t*>(c.string.utf8),
                    c.string.len);
            return;
        case ConstTag::Bool:
            w.u8(RC_BOOL);
            w.u8(c.boolean.value ? 1 : 0);
            return;
        case ConstTag::Unit:
            w.u8(RC_UNIT);
            return;
        case ConstTag::Data:
            w.u8(RC_DATA);
            write_data(w, *c.data.value);
            return;
        case ConstTag::Bls12_381_G1:
            w.u8(RC_BLS12_381_G1);
            w.bytes(c.bls_g1.bytes, 48);
            return;
        case ConstTag::Bls12_381_G2:
            w.u8(RC_BLS12_381_G2);
            w.bytes(c.bls_g2.bytes, 96);
            return;
        case ConstTag::Bls12_381_MlResult:
            w.u8(RC_BLS12_381_ML_RESULT);
            w.u32(c.bls_ml_result.len);
            w.bytes(c.bls_ml_result.bytes, c.bls_ml_result.len);
            return;
        case ConstTag::Value: {
            w.u8(RC_VALUE);
            const LedgerValue& lv = *c.value.value;
            w.u32(lv.n_entries);
            for (std::uint32_t i = 0; i < lv.n_entries; ++i) {
                const LedgerValueEntry& e = lv.entries[i];
                w.u32(e.currency_len);
                w.bytes(e.currency_bytes, e.currency_len);
                w.u32(e.n_tokens);
                for (std::uint32_t j = 0; j < e.n_tokens; ++j) {
                    const LedgerValueToken& t = e.tokens[j];
                    w.u32(t.name_len);
                    w.bytes(t.name_bytes, t.name_len);
                    write_integer(w, *t.quantity);
                }
            }
            return;
        }
        case ConstTag::List:
            w.u8(RC_LIST);
            write_type(w, *c.list.item_type);
            w.u32(c.list.n_values);
            for (std::uint32_t i = 0; i < c.list.n_values; ++i) {
                write_constant(w, *c.list.values[i]);
            }
            return;
        case ConstTag::Pair:
            w.u8(RC_PAIR);
            write_type(w, *c.pair.fst_type);
            write_type(w, *c.pair.snd_type);
            write_constant(w, *c.pair.first);
            write_constant(w, *c.pair.second);
            return;
        case ConstTag::Array:
            w.u8(RC_ARRAY);
            write_type(w, *c.array.item_type);
            w.u32(c.array.n_values);
            for (std::uint32_t i = 0; i < c.array.n_values; ++i) {
                write_constant(w, *c.array.values[i]);
            }
            return;
    }
    throw std::runtime_error("baked_const: unknown ConstTag");
}

}  // namespace

std::vector<std::uint8_t> serialize_baked_constant(const Constant& c) {
    Writer w;
    write_constant(w, c);
    return std::move(w).take();
}

}  // namespace uplc
