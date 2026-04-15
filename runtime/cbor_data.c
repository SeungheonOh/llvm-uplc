#include "runtime/cbor_data.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gmp.h>

#include "runtime/errors.h"
#include "uplc/abi.h"

/*
 * CBOR Data encoder. Pure C port of compiler/ast/cbor_data.cc; the two
 * must stay in sync because the compiler bakes constant data via its C++
 * encoder and the runtime produces serialiseData output via this one.
 *
 * The output buffer grows geometrically inside the evaluation arena.
 * Every allocation returns an arena-owned pointer, so freeing happens
 * when the eval arena is destroyed.
 */

#define CBOR_CHUNK_SIZE 64

typedef struct {
    uplc_arena* arena;
    uint8_t*    data;
    size_t      len;
    size_t      cap;
} cbor_buf;

static void buf_grow(cbor_buf* b, size_t min_extra) {
    size_t need = b->len + min_extra;
    if (need <= b->cap) return;
    size_t new_cap = b->cap == 0 ? 64 : b->cap * 2;
    while (new_cap < need) new_cap *= 2;
    uint8_t* new_data = (uint8_t*)uplc_arena_alloc(b->arena, new_cap, alignof(uint8_t));
    if (b->len > 0) memcpy(new_data, b->data, b->len);
    b->data = new_data;
    b->cap = new_cap;
}

static void buf_push(cbor_buf* b, uint8_t byte) {
    buf_grow(b, 1);
    b->data[b->len++] = byte;
}

static void buf_push_bytes(cbor_buf* b, const uint8_t* bytes, size_t n) {
    if (n == 0) return;
    buf_grow(b, n);
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
}

/* write major-tagged unsigned argument per RFC 8949 §3 */
static void write_typed_int(cbor_buf* b, uint8_t major, uint64_t val) {
    if (val < 24) {
        buf_push(b, (uint8_t)(major | val));
    } else if (val <= 0xffu) {
        buf_push(b, (uint8_t)(major | 24));
        buf_push(b, (uint8_t)val);
    } else if (val <= 0xffffu) {
        buf_push(b, (uint8_t)(major | 25));
        buf_push(b, (uint8_t)((val >> 8) & 0xff));
        buf_push(b, (uint8_t)(val & 0xff));
    } else if (val <= 0xffffffffu) {
        buf_push(b, (uint8_t)(major | 26));
        buf_push(b, (uint8_t)((val >> 24) & 0xff));
        buf_push(b, (uint8_t)((val >> 16) & 0xff));
        buf_push(b, (uint8_t)((val >> 8) & 0xff));
        buf_push(b, (uint8_t)(val & 0xff));
    } else {
        buf_push(b, (uint8_t)(major | 27));
        for (int i = 7; i >= 0; --i) {
            buf_push(b, (uint8_t)((val >> (i * 8)) & 0xff));
        }
    }
}

static void write_uint(cbor_buf* b, uint64_t v)         { write_typed_int(b, 0x00, v); }
static void write_nint(cbor_buf* b, uint64_t v)         { write_typed_int(b, 0x20, v); }
static void write_bytes_header(cbor_buf* b, uint64_t n) { write_typed_int(b, 0x40, n); }
static void write_array_header(cbor_buf* b, uint64_t n) { write_typed_int(b, 0x80, n); }
static void write_map_header(cbor_buf* b, uint64_t n)   { write_typed_int(b, 0xa0, n); }
static void write_tag(cbor_buf* b, uint64_t v)          { write_typed_int(b, 0xc0, v); }

/* Write a byte string split into 64-byte chunks if it's large. Matches
 * the CBOR indefinite-length byte string shape used by the TS encoder. */
static void write_bytes(cbor_buf* b, const uint8_t* bytes, size_t len) {
    if (len <= CBOR_CHUNK_SIZE) {
        write_bytes_header(b, (uint64_t)len);
        buf_push_bytes(b, bytes, len);
        return;
    }
    buf_push(b, 0x5f);  /* indefinite byte string */
    size_t off = 0;
    while (off < len) {
        size_t remaining = len - off;
        size_t chunk = remaining < CBOR_CHUNK_SIZE ? remaining : CBOR_CHUNK_SIZE;
        write_bytes_header(b, (uint64_t)chunk);
        buf_push_bytes(b, bytes + off, chunk);
        off += chunk;
    }
    buf_push(b, 0xff);  /* break */
}

/* Serialise an mpz_t absolute value to big-endian bytes. Callers pass
 * a non-negative source; negative-sign handling lives in encode_integer.
 * Returns an arena-allocated pointer (borrowed by the caller). */
static const uint8_t* mpz_to_be_bytes(uplc_arena* a, mpz_srcptr v, uint32_t* out_len) {
    if (mpz_sgn(v) == 0) {
        uint8_t* p = (uint8_t*)uplc_arena_alloc(a, 1, alignof(uint8_t));
        p[0] = 0;
        *out_len = 1;
        return p;
    }
    size_t count = (mpz_sizeinbase(v, 2) + 7) / 8;
    uint8_t* p = (uint8_t*)uplc_arena_alloc(a, count, alignof(uint8_t));
    size_t written = 0;
    mpz_export(p, &written, 1, 1, 1, 0, v);
    *out_len = (uint32_t)(written == 0 ? 1 : written);
    if (written == 0) p[0] = 0;
    return p;
}

static bool mpz_fits_u64(mpz_srcptr v) {
    return mpz_sgn(v) >= 0 && mpz_sizeinbase(v, 2) <= 64;
}

/* mpz → uint64 (caller must ensure fits_u64). */
static uint64_t mpz_to_u64(mpz_srcptr v) {
    if (mpz_sgn(v) == 0) return 0;
    uint64_t out = 0;
    size_t count = 0;
    mpz_export(&out, &count, -1, sizeof(out), 0, 0, v);
    (void)count;
    return out;
}

static void encode_data(cbor_buf* b, const uplc_rdata* d);

static void encode_integer(cbor_buf* b, mpz_srcptr value) {
    if (mpz_sgn(value) == 0) {
        buf_push(b, 0x00);
        return;
    }
    if (mpz_sgn(value) > 0) {
        if (mpz_sizeinbase(value, 2) <= 64) {
            write_uint(b, mpz_to_u64(value));
        } else {
            write_tag(b, 2);
            uint32_t bslen = 0;
            const uint8_t* bs = mpz_to_be_bytes(b->arena, value, &bslen);
            write_bytes(b, bs, bslen);
        }
    } else {
        mpz_t tmp;
        mpz_init(tmp);
        mpz_neg(tmp, value);
        mpz_sub_ui(tmp, tmp, 1);        /* tmp = |v| - 1 */
        if (mpz_sizeinbase(tmp, 2) <= 64) {
            write_nint(b, mpz_to_u64(tmp));
        } else {
            write_tag(b, 3);
            uint32_t bslen = 0;
            const uint8_t* bs = mpz_to_be_bytes(b->arena, tmp, &bslen);
            write_bytes(b, bs, bslen);
        }
        mpz_clear(tmp);
    }
}

static void encode_constr(cbor_buf* b, mpz_srcptr index,
                          uplc_rdata* const* fields, uint32_t n_fields) {
    if (mpz_sgn(index) >= 0 && mpz_cmp_ui(index, 6) <= 0) {
        uint64_t i = mpz_to_u64(index);
        write_tag(b, 121 + i);
    } else if (mpz_sgn(index) >= 0 && mpz_cmp_ui(index, 127) <= 0) {
        uint64_t i = mpz_to_u64(index);
        write_tag(b, 1280 + (i - 7));
    } else {
        write_tag(b, 102);
        write_array_header(b, 2);
        if (!mpz_fits_u64(index)) {
            /* The constr index is a Haskell Natural; TS clamps to uint64
             * and fails below. We mirror that with an evaluation error. */
            uplcrt_raise(UPLC_FAIL_EVALUATION, "serialiseData: constr index out of range");
        }
        write_uint(b, mpz_to_u64(index));
    }

    if (n_fields == 0) {
        write_array_header(b, 0);
    } else {
        buf_push(b, 0x9f);  /* indefinite array */
        for (uint32_t i = 0; i < n_fields; ++i) encode_data(b, fields[i]);
        buf_push(b, 0xff);
    }
}

static void encode_data(cbor_buf* b, const uplc_rdata* d) {
    switch ((uplc_rdata_tag)d->tag) {
        case UPLC_RDATA_CONSTR:
            encode_constr(b, d->constr.index, d->constr.fields, d->constr.n_fields);
            break;
        case UPLC_RDATA_MAP: {
            write_map_header(b, d->map.n_entries);
            for (uint32_t i = 0; i < d->map.n_entries; ++i) {
                encode_data(b, d->map.entries[i].key);
                encode_data(b, d->map.entries[i].value);
            }
            break;
        }
        case UPLC_RDATA_LIST: {
            if (d->list.n_values == 0) {
                write_array_header(b, 0);
            } else {
                buf_push(b, 0x9f);
                for (uint32_t i = 0; i < d->list.n_values; ++i) {
                    encode_data(b, d->list.values[i]);
                }
                buf_push(b, 0xff);
            }
            break;
        }
        case UPLC_RDATA_INTEGER:
            encode_integer(b, d->integer.value);
            break;
        case UPLC_RDATA_BYTESTRING:
            write_bytes(b, d->bytestring.bytes, d->bytestring.len);
            break;
    }
}

void uplcrt_cbor_encode_data(uplc_arena* arena, const uplc_rdata* data,
                             const uint8_t** out_bytes, uint32_t* out_len) {
    cbor_buf b = { arena, NULL, 0, 0 };
    encode_data(&b, data);
    *out_bytes = b.data;
    *out_len = (uint32_t)b.len;
}
