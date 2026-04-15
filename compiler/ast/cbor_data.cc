#include "compiler/ast/cbor_data.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include <gmp.h>

namespace uplc {

namespace {

constexpr std::size_t kChunkSize = 64;

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

struct Encoder {
    std::vector<std::uint8_t> out;

    void push(std::uint8_t b) { out.push_back(b); }

    // Write (major | additional-info) then the big-endian 64-bit argument.
    void write_typed_int(std::uint8_t major, std::uint64_t val) {
        if (val < 24) {
            push(static_cast<std::uint8_t>(major | val));
        } else if (val <= 0xffu) {
            push(static_cast<std::uint8_t>(major | 24u));
            push(static_cast<std::uint8_t>(val));
        } else if (val <= 0xffffu) {
            push(static_cast<std::uint8_t>(major | 25u));
            push(static_cast<std::uint8_t>((val >> 8) & 0xffu));
            push(static_cast<std::uint8_t>(val & 0xffu));
        } else if (val <= 0xffffffffu) {
            push(static_cast<std::uint8_t>(major | 26u));
            push(static_cast<std::uint8_t>((val >> 24) & 0xffu));
            push(static_cast<std::uint8_t>((val >> 16) & 0xffu));
            push(static_cast<std::uint8_t>((val >> 8) & 0xffu));
            push(static_cast<std::uint8_t>(val & 0xffu));
        } else {
            push(static_cast<std::uint8_t>(major | 27u));
            for (int i = 7; i >= 0; --i) {
                push(static_cast<std::uint8_t>((val >> (i * 8)) & 0xffu));
            }
        }
    }

    void write_uint(std::uint64_t v)   { write_typed_int(0x00, v); }
    void write_nint(std::uint64_t v)   { write_typed_int(0x20, v); }
    void write_bytes_header(std::uint64_t n) { write_typed_int(0x40, n); }
    void write_array_header(std::uint64_t n) { write_typed_int(0x80, n); }
    void write_map_header(std::uint64_t n)   { write_typed_int(0xa0, n); }
    void write_tag(std::uint64_t v)    { write_typed_int(0xc0, v); }

    void write_bytes(const std::uint8_t* bytes, std::size_t len) {
        if (len <= kChunkSize) {
            write_bytes_header(len);
            out.insert(out.end(), bytes, bytes + len);
        } else {
            push(0x5f);  // indefinite byte string
            std::size_t off = 0;
            while (off < len) {
                std::size_t end = std::min(off + kChunkSize, len);
                std::size_t chunk = end - off;
                write_bytes_header(chunk);
                out.insert(out.end(), bytes + off, bytes + end);
                off = end;
            }
            push(0xff);  // break
        }
    }
};

// Convert a non-negative mpz to big-endian bytes (unsigned, minimal length;
// returns a single 0 byte for zero).
std::vector<std::uint8_t> mpz_to_be_bytes(mpz_srcptr v) {
    if (mpz_sgn(v) == 0) return {0};
    std::size_t count = (mpz_sizeinbase(v, 2) + 7) / 8;
    std::vector<std::uint8_t> out(count);
    std::size_t written = 0;
    mpz_export(out.data(), &written, 1 /*order: MSB first*/,
               1 /*size*/, 1 /*endian: big*/, 0 /*nails*/, v);
    if (written < count) {
        // mpz_export wrote fewer bytes than expected: happens for small values
        // where sizeinbase overcounts by a byte. Shrink the buffer.
        out.resize(written == 0 ? 1 : written);
        if (written == 0) out[0] = 0;
    }
    return out;
}

// True iff value fits in an unsigned 64-bit integer.
bool mpz_fits_u64(mpz_srcptr v) {
    return mpz_sgn(v) >= 0 && mpz_sizeinbase(v, 2) <= 64;
}

// Value must satisfy mpz_fits_u64.
std::uint64_t mpz_to_u64(mpz_srcptr v) {
    // GMP has no direct mpz→uint64 on all platforms, so go via mpz_export.
    std::uint64_t out = 0;
    if (mpz_sgn(v) == 0) return 0;
    std::size_t count = 0;
    mpz_export(&out, &count, -1 /*least significant word first*/,
               sizeof(out), 0 /*native endian*/, 0, v);
    (void)count;
    return out;
}

void encode_data(Encoder& e, const PlutusData& data);

void encode_integer(Encoder& e, mpz_srcptr value) {
    if (mpz_sgn(value) == 0) {
        e.push(0x00);
        return;
    }
    if (mpz_sgn(value) > 0) {
        // Positive: uint if < 2^64, else tag 2 + big-endian bytes.
        if (mpz_sizeinbase(value, 2) <= 64) {
            e.write_uint(mpz_to_u64(value));
        } else {
            e.write_tag(2);
            auto bs = mpz_to_be_bytes(value);
            e.write_bytes(bs.data(), bs.size());
        }
    } else {
        // Negative: let m = -v - 1. Nint if m < 2^64, else tag 3 + bytes.
        mpz_t tmp;
        mpz_init(tmp);
        mpz_neg(tmp, value);
        mpz_sub_ui(tmp, tmp, 1);  // tmp = |v| - 1
        if (mpz_sizeinbase(tmp, 2) <= 64) {
            e.write_nint(mpz_to_u64(tmp));
        } else {
            e.write_tag(3);
            auto bs = mpz_to_be_bytes(tmp);
            e.write_bytes(bs.data(), bs.size());
        }
        mpz_clear(tmp);
    }
}

void encode_constr(Encoder& e, mpz_srcptr index, PlutusData* const* fields,
                   std::uint32_t n_fields) {
    // Tag encoding matches TS cbor.ts encodeConstr:
    //   0..6    -> tag 121..127
    //   7..127  -> tag 1280..1400
    //   otherwise -> tag 102 + [discriminator, fields...]
    if (mpz_cmp_ui(index, 6) <= 0 && mpz_sgn(index) >= 0) {
        std::uint64_t i = mpz_to_u64(index);
        e.write_tag(121 + i);
    } else if (mpz_cmp_ui(index, 127) <= 0 && mpz_sgn(index) >= 0) {
        std::uint64_t i = mpz_to_u64(index);
        e.write_tag(1280 + (i - 7));
    } else {
        e.write_tag(102);
        e.write_array_header(2);
        if (!mpz_fits_u64(index)) {
            throw CborError("constr index does not fit in uint64");
        }
        e.write_uint(mpz_to_u64(index));
    }

    if (n_fields == 0) {
        e.write_array_header(0);
    } else {
        e.push(0x9f);  // indefinite-length array
        for (std::uint32_t i = 0; i < n_fields; ++i) {
            encode_data(e, *fields[i]);
        }
        e.push(0xff);
    }
}

void encode_data(Encoder& e, const PlutusData& data) {
    switch (data.tag) {
        case PlutusDataTag::Constr:
            encode_constr(e, data.constr.index->value, data.constr.fields,
                          data.constr.n_fields);
            break;
        case PlutusDataTag::Map: {
            e.write_map_header(data.map.n_entries);
            for (std::uint32_t i = 0; i < data.map.n_entries; ++i) {
                encode_data(e, *data.map.entries[i].key);
                encode_data(e, *data.map.entries[i].value);
            }
            break;
        }
        case PlutusDataTag::List: {
            if (data.list.n_values == 0) {
                e.write_array_header(0);
            } else {
                e.push(0x9f);
                for (std::uint32_t i = 0; i < data.list.n_values; ++i) {
                    encode_data(e, *data.list.values[i]);
                }
                e.push(0xff);
            }
            break;
        }
        case PlutusDataTag::Integer:
            encode_integer(e, data.integer.value->value);
            break;
        case PlutusDataTag::ByteString:
            e.write_bytes(data.bytestring.bytes, data.bytestring.len);
            break;
    }
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

struct Decoder {
    const std::uint8_t* data;
    std::size_t         len;
    std::size_t         pos;

    std::uint8_t read_byte() {
        if (pos >= len) throw CborError("end of input");
        return data[pos++];
    }

    void read_bytes_into(std::vector<std::uint8_t>& dst, std::size_t n) {
        if (pos + n > len) throw CborError("end of input");
        dst.insert(dst.end(), data + pos, data + pos + n);
        pos += n;
    }

    const std::uint8_t* read_bytes_view(std::size_t n) {
        if (pos + n > len) throw CborError("end of input");
        const std::uint8_t* p = data + pos;
        pos += n;
        return p;
    }

    // Read the argument word of a CBOR head. For additional < 24 returns the
    // value directly; otherwise reads 1/2/4/8 big-endian bytes.
    std::uint64_t read_argument(std::uint8_t additional) {
        if (additional < 24) return additional;
        switch (additional) {
            case 24: return read_byte();
            case 25: {
                auto* p = read_bytes_view(2);
                return (static_cast<std::uint64_t>(p[0]) << 8) | p[1];
            }
            case 26: {
                auto* p = read_bytes_view(4);
                return (static_cast<std::uint64_t>(p[0]) << 24) |
                       (static_cast<std::uint64_t>(p[1]) << 16) |
                       (static_cast<std::uint64_t>(p[2]) << 8) |
                       p[3];
            }
            case 27: {
                auto* p = read_bytes_view(8);
                std::uint64_t v = 0;
                for (int i = 0; i < 8; ++i) v = (v << 8) | p[static_cast<std::size_t>(i)];
                return v;
            }
            default:
                throw CborError("invalid additional info " +
                                std::to_string(additional));
        }
    }
};

// Set `out` to the unsigned big-endian bytes interpreted as an integer.
void mpz_from_be_bytes(mpz_ptr out, const std::uint8_t* bytes, std::size_t n) {
    mpz_import(out, n, 1, 1, 1, 0, bytes);
}

PlutusData* decode_data_item(Arena& arena, Decoder& d);

// Decode a CBOR array (major type 4), returning the items as an arena-allocated
// PlutusData** + count. Handles both indefinite- and definite-length arrays.
std::pair<PlutusData**, std::uint32_t>
decode_array_fields(Arena& arena, Decoder& d) {
    std::uint8_t initial = d.read_byte();
    std::uint8_t major = initial >> 5;
    if (major != 4) throw CborError("expected array");
    std::uint8_t additional = initial & 0x1f;

    std::vector<PlutusData*> items;
    if (additional == 31) {
        for (;;) {
            if (d.pos >= d.len) throw CborError("end of input");
            if (d.data[d.pos] == 0xff) { ++d.pos; break; }
            items.push_back(decode_data_item(arena, d));
        }
    } else {
        std::uint64_t len = d.read_argument(additional);
        items.reserve(static_cast<std::size_t>(len));
        for (std::uint64_t i = 0; i < len; ++i) {
            items.push_back(decode_data_item(arena, d));
        }
    }

    auto n = static_cast<std::uint32_t>(items.size());
    PlutusData** arr = arena.alloc_array_uninit<PlutusData*>(n);
    for (std::uint32_t i = 0; i < n; ++i) arr[i] = items[i];
    return {arr, n};
}

PlutusData* decode_tagged(Arena& arena, Decoder& d, std::uint64_t tag) {
    if (tag >= 121 && tag <= 127) {
        auto [arr, n] = decode_array_fields(arena, d);
        BigInt* idx = arena.make_bigint();
        mpz_set_ui(idx->value, static_cast<unsigned long>(tag - 121));
        return make_data_constr(arena, idx, arr, n);
    }
    if (tag >= 1280 && tag <= 1400) {
        auto [arr, n] = decode_array_fields(arena, d);
        BigInt* idx = arena.make_bigint();
        mpz_set_ui(idx->value, static_cast<unsigned long>(7 + (tag - 1280)));
        return make_data_constr(arena, idx, arr, n);
    }
    if (tag == 102) {
        std::uint8_t initial = d.read_byte();
        std::uint8_t major = initial >> 5;
        if (major != 4) throw CborError("expected array for tag 102");
        std::uint8_t add = initial & 0x1f;
        std::uint64_t arr_len = d.read_argument(add);
        if (arr_len != 2) throw CborError("expected 2-element array for tag 102");

        std::uint8_t disc_byte = d.read_byte();
        std::uint8_t disc_major = disc_byte >> 5;
        if (disc_major != 0) throw CborError("expected unsigned int discriminator");
        std::uint8_t disc_add = disc_byte & 0x1f;
        std::uint64_t disc = d.read_argument(disc_add);

        auto [arr, n] = decode_array_fields(arena, d);
        BigInt* idx = arena.make_bigint();
        mpz_set_ui(idx->value, static_cast<unsigned long>(disc));
        return make_data_constr(arena, idx, arr, n);
    }
    if (tag == 2) {
        PlutusData* bs = decode_data_item(arena, d);
        if (bs->tag != PlutusDataTag::ByteString) {
            throw CborError("expected byte string for big int");
        }
        BigInt* out = arena.make_bigint();
        mpz_from_be_bytes(out->value, bs->bytestring.bytes, bs->bytestring.len);
        return make_data_integer(arena, out);
    }
    if (tag == 3) {
        PlutusData* bs = decode_data_item(arena, d);
        if (bs->tag != PlutusDataTag::ByteString) {
            throw CborError("expected byte string for big nint");
        }
        BigInt* out = arena.make_bigint();
        mpz_from_be_bytes(out->value, bs->bytestring.bytes, bs->bytestring.len);
        // represent -(val+1)
        mpz_add_ui(out->value, out->value, 1);
        mpz_neg(out->value, out->value);
        return make_data_integer(arena, out);
    }
    throw CborError("unsupported tag " + std::to_string(tag));
}

PlutusData* decode_data_item(Arena& arena, Decoder& d) {
    std::uint8_t initial = d.read_byte();
    std::uint8_t major = initial >> 5;
    std::uint8_t additional = initial & 0x1f;

    switch (major) {
        case 0: {  // unsigned int
            std::uint64_t v = d.read_argument(additional);
            BigInt* bi = arena.make_bigint();
            mpz_set_ui(bi->value, static_cast<unsigned long>(v));
            // If sizeof(unsigned long) < 8 this loses high bits; GMP's
            // mpz_set_ui takes unsigned long. On aarch64/linux/darwin it's
            // 64-bit so we're fine; guard anyway.
            if (v > static_cast<unsigned long>(-1L)) {
                // Use mpz_import for the high bits.
                std::uint8_t be[8];
                for (int i = 0; i < 8; ++i) be[i] = (v >> ((7 - i) * 8)) & 0xff;
                mpz_import(bi->value, 8, 1, 1, 1, 0, be);
            }
            return make_data_integer(arena, bi);
        }
        case 1: {  // negative int: -1 - v
            std::uint64_t v = d.read_argument(additional);
            BigInt* bi = arena.make_bigint();
            mpz_set_ui(bi->value, static_cast<unsigned long>(v));
            if (v > static_cast<unsigned long>(-1L)) {
                std::uint8_t be[8];
                for (int i = 0; i < 8; ++i) be[i] = (v >> ((7 - i) * 8)) & 0xff;
                mpz_import(bi->value, 8, 1, 1, 1, 0, be);
            }
            mpz_add_ui(bi->value, bi->value, 1);
            mpz_neg(bi->value, bi->value);
            return make_data_integer(arena, bi);
        }
        case 2: {  // byte string
            if (additional == 31) {
                std::vector<std::uint8_t> collected;
                for (;;) {
                    std::uint8_t next = d.read_byte();
                    if (next == 0xff) break;
                    std::uint8_t chunk_major = next >> 5;
                    if (chunk_major != 2) {
                        throw CborError("invalid byte string chunk");
                    }
                    std::uint8_t chunk_add = next & 0x1f;
                    std::uint64_t chunk_len = d.read_argument(chunk_add);
                    d.read_bytes_into(collected, static_cast<std::size_t>(chunk_len));
                }
                auto n = static_cast<std::uint32_t>(collected.size());
                const std::uint8_t* interned =
                    arena.intern_bytes(collected.data(), collected.size());
                return make_data_bytestring(arena, interned, n);
            } else {
                std::uint64_t len = d.read_argument(additional);
                const std::uint8_t* view = d.read_bytes_view(static_cast<std::size_t>(len));
                const std::uint8_t* owned =
                    arena.intern_bytes(view, static_cast<std::size_t>(len));
                return make_data_bytestring(arena, owned, static_cast<std::uint32_t>(len));
            }
        }
        case 4: {  // array => list
            std::vector<PlutusData*> items;
            if (additional == 31) {
                for (;;) {
                    if (d.pos >= d.len) throw CborError("end of input");
                    if (d.data[d.pos] == 0xff) { ++d.pos; break; }
                    items.push_back(decode_data_item(arena, d));
                }
            } else {
                std::uint64_t len = d.read_argument(additional);
                items.reserve(static_cast<std::size_t>(len));
                for (std::uint64_t i = 0; i < len; ++i) {
                    items.push_back(decode_data_item(arena, d));
                }
            }
            auto n = static_cast<std::uint32_t>(items.size());
            PlutusData** arr = arena.alloc_array_uninit<PlutusData*>(n);
            for (std::uint32_t i = 0; i < n; ++i) arr[i] = items[i];
            return make_data_list(arena, arr, n);
        }
        case 5: {  // map
            std::uint64_t len = d.read_argument(additional);
            auto n = static_cast<std::uint32_t>(len);
            PlutusDataPair* entries = arena.alloc_array_uninit<PlutusDataPair>(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                PlutusData* k = decode_data_item(arena, d);
                PlutusData* v = decode_data_item(arena, d);
                entries[i] = PlutusDataPair{k, v};
            }
            return make_data_map(arena, entries, n);
        }
        case 6: {  // tagged
            std::uint64_t tag_val = d.read_argument(additional);
            return decode_tagged(arena, d, tag_val);
        }
        default:
            throw CborError("unsupported major type " + std::to_string(major));
    }
}

}  // namespace

std::vector<std::uint8_t> encode_plutus_data(const PlutusData& data) {
    Encoder e;
    encode_data(e, data);
    return e.out;
}

PlutusData* decode_plutus_data(Arena& arena,
                               const std::uint8_t* bytes,
                               std::size_t len) {
    Decoder d{bytes, len, 0};
    return decode_data_item(arena, d);
}

}  // namespace uplc
