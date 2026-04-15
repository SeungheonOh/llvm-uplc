#include "runtime/builtins/helpers.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

/*
 * Bitwise + integer/bytestring builtins. Port of TS cek/builtins/bitwise.ts.
 * Bit indexing follows the Plutus MSB0 convention for shiftByteString and
 * LSB0 (bit 0 is the LSB of the last byte) for readBit / writeBits.
 */

#define INT2BS_MAX_OUTPUT 8192u

/* ---- Logical ops ---- */

static uplc_value logical_op(uplc_budget* b, uplc_value* a,
                             uint8_t (*op)(uint8_t, uint8_t), uint8_t pad) {
    bool should_pad = uplcrt_unwrap_bool(b, a[0]);
    uint32_t la = 0, lb = 0;
    const uint8_t* bs1 = uplcrt_unwrap_bytestring(b, a[1], &la);
    const uint8_t* bs2 = uplcrt_unwrap_bytestring(b, a[2], &lb);

    const uint8_t* shorter = la <= lb ? bs1 : bs2;
    const uint8_t* longer  = la <= lb ? bs2 : bs1;
    uint32_t shorter_len   = la <= lb ? la : lb;
    uint32_t longer_len    = la <= lb ? lb : la;

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint32_t n = should_pad ? longer_len : shorter_len;
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, n ? n : 1, alignof(uint8_t));

    if (should_pad) {
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t x = longer[i];
            uint8_t y = (i < shorter_len) ? shorter[i] : pad;
            out[i] = op(x, y);
        }
    } else {
        for (uint32_t i = 0; i < n; ++i) {
            out[i] = op(bs1[i], bs2[i]);
        }
    }
    return uplcrt_result_bytestring(b, out, n);
}

static uint8_t op_and(uint8_t x, uint8_t y) { return (uint8_t)(x & y); }
static uint8_t op_or (uint8_t x, uint8_t y) { return (uint8_t)(x | y); }
static uint8_t op_xor(uint8_t x, uint8_t y) { return (uint8_t)(x ^ y); }

uplc_value uplcrt_builtin_andByteString(uplc_budget* b, uplc_value* a) {
    return logical_op(b, a, op_and, 0xff);
}
uplc_value uplcrt_builtin_orByteString(uplc_budget* b, uplc_value* a) {
    return logical_op(b, a, op_or, 0x00);
}
uplc_value uplcrt_builtin_xorByteString(uplc_budget* b, uplc_value* a) {
    return logical_op(b, a, op_xor, 0x00);
}

uplc_value uplcrt_builtin_complementByteString(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[0], &len);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, len ? len : 1, alignof(uint8_t));
    for (uint32_t i = 0; i < len; ++i) out[i] = (uint8_t)(bs[i] ^ 0xff);
    return uplcrt_result_bytestring(b, out, len);
}

/* ---- Shift / rotate ---- */

uplc_value uplcrt_builtin_shiftByteString(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[0], &len);
    mpz_srcptr shift_mpz = uplcrt_unwrap_integer(b, a[1]);

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, len ? len : 1, alignof(uint8_t));
    memset(out, 0, len ? len : 1);
    if (len == 0) return uplcrt_result_bytestring(b, out, 0);

    int64_t total_bits = (int64_t)len * 8;

    /* Abs shift > total bits ⇒ all zeros. */
    mpz_t neg_total;
    mpz_init_set_si(neg_total, -total_bits);
    if (mpz_cmp_si(shift_mpz, total_bits) > 0 ||
        mpz_cmp(shift_mpz, neg_total) < 0) {
        mpz_clear(neg_total);
        return uplcrt_result_bytestring(b, out, len);
    }
    mpz_clear(neg_total);

    long shift = mpz_get_si(shift_mpz);
    if (shift == 0) {
        memcpy(out, bs, len);
        return uplcrt_result_bytestring(b, out, len);
    }

    if (shift > 0) {
        /* Left shift (toward MSB). */
        for (int64_t i = 0; i < total_bits - shift; ++i) {
            int64_t src_idx = i + shift;
            int64_t src_byte = src_idx >> 3;
            int64_t src_bit  = 7 - (src_idx & 7);
            if ((bs[src_byte] >> src_bit) & 1) {
                int64_t dst_byte = i >> 3;
                int64_t dst_bit  = 7 - (i & 7);
                out[dst_byte] = (uint8_t)(out[dst_byte] | (1u << dst_bit));
            }
        }
    } else {
        int64_t abs_shift = -shift;
        for (int64_t i = abs_shift; i < total_bits; ++i) {
            int64_t src_idx = i - abs_shift;
            int64_t src_byte = src_idx >> 3;
            int64_t src_bit  = 7 - (src_idx & 7);
            if ((bs[src_byte] >> src_bit) & 1) {
                int64_t dst_byte = i >> 3;
                int64_t dst_bit  = 7 - (i & 7);
                out[dst_byte] = (uint8_t)(out[dst_byte] | (1u << dst_bit));
            }
        }
    }
    return uplcrt_result_bytestring(b, out, len);
}

uplc_value uplcrt_builtin_rotateByteString(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[0], &len);
    mpz_srcptr rot_mpz = uplcrt_unwrap_integer(b, a[1]);

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, len ? len : 1, alignof(uint8_t));
    if (len == 0) return uplcrt_result_bytestring(b, out, 0);

    /* normalised = ((rot % totalBits) + totalBits) % totalBits */
    mpz_t total_bits, tmp;
    mpz_init_set_ui(total_bits, (unsigned long)(len * 8));
    mpz_init(tmp);
    mpz_fdiv_r(tmp, rot_mpz, total_bits);  /* Euclidean remainder */

    unsigned long normalised = mpz_get_ui(tmp);
    mpz_clear(tmp);
    mpz_clear(total_bits);

    if (normalised == 0) {
        memcpy(out, bs, len);
        return uplcrt_result_bytestring(b, out, len);
    }

    uint32_t byte_shift = (uint32_t)(normalised / 8);
    uint32_t bit_shift  = (uint32_t)(normalised % 8);

    for (uint32_t i = 0; i < len; ++i) {
        uint32_t src  = (i + byte_shift) % len;
        uint32_t next = (src + 1) % len;
        uint32_t v = ((uint32_t)bs[src] << bit_shift) |
                     ((uint32_t)bs[next] >> (8 - bit_shift));
        out[i] = (uint8_t)(v & 0xff);
    }
    return uplcrt_result_bytestring(b, out, len);
}

/* ---- Bit access ---- */

uplc_value uplcrt_builtin_readBit(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[0], &len);
    mpz_srcptr idx_mpz = uplcrt_unwrap_integer(b, a[1]);
    if (mpz_sgn(idx_mpz) < 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    if (mpz_cmp_ui(idx_mpz, (unsigned long)len * 8) >= 0) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    unsigned long bit_idx = mpz_get_ui(idx_mpz);
    uint32_t byte_index = (uint32_t)(bit_idx >> 3);
    uint32_t bit_offset = (uint32_t)(bit_idx & 7);
    uint32_t flipped = len - 1 - byte_index;
    return uplcrt_result_bool(b, ((bs[flipped] >> bit_offset) & 1u) == 1u);
}

uplc_value uplcrt_builtin_writeBits(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[0], &len);
    const uplc_rconstant* indices = uplcrt_unwrap_list(b, a[1]);
    bool set_val = uplcrt_unwrap_bool(b, a[2]);

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, len ? len : 1, alignof(uint8_t));
    if (len > 0) memcpy(out, bs, len);

    uint64_t total_bits = (uint64_t)len * 8;
    for (uint32_t i = 0; i < indices->list.n_values; ++i) {
        uplc_rconstant* c = indices->list.values[i];
        if (c->tag != UPLC_RCONST_INTEGER) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        mpz_srcptr idx = c->integer.value;
        if (mpz_sgn(idx) < 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        if (mpz_cmp_ui(idx, (unsigned long)total_bits) >= 0) {
            uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        }
        unsigned long bit_idx = mpz_get_ui(idx);
        uint32_t byte_index = (uint32_t)(bit_idx >> 3);
        uint32_t bit_offset = (uint32_t)(bit_idx & 7);
        uint32_t flipped = len - 1 - byte_index;
        if (set_val) {
            out[flipped] = (uint8_t)(out[flipped] | (1u << bit_offset));
        } else {
            out[flipped] = (uint8_t)(out[flipped] & ~(1u << bit_offset));
        }
    }
    return uplcrt_result_bytestring(b, out, len);
}

/* ---- Count / find ---- */

uplc_value uplcrt_builtin_countSetBits(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[0], &len);
    uint64_t count = 0;
    for (uint32_t i = 0; i < len; ++i) count += (uint64_t)__builtin_popcount(bs[i]);
    return uplcrt_result_integer_si(b, (int64_t)count);
}

uplc_value uplcrt_builtin_findFirstSetBit(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[0], &len);
    for (int32_t i = (int32_t)len - 1; i >= 0; --i) {
        uint8_t byte = bs[i];
        if (byte != 0) {
            int ctz = __builtin_ctz(byte);
            int64_t bit_index = ctz + ((int64_t)(len - 1 - (uint32_t)i)) * 8;
            return uplcrt_result_integer_si(b, bit_index);
        }
    }
    return uplcrt_result_integer_si(b, -1);
}

/* ---- Replicate ---- */

uplc_value uplcrt_builtin_replicateByte(uplc_budget* b, uplc_value* a) {
    mpz_srcptr size_mpz = uplcrt_unwrap_integer(b, a[0]);
    mpz_srcptr byte_mpz = uplcrt_unwrap_integer(b, a[1]);
    if (mpz_sgn(size_mpz) < 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    if (mpz_cmp_ui(size_mpz, INT2BS_MAX_OUTPUT) > 0) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    if (mpz_sgn(byte_mpz) < 0 || mpz_cmp_ui(byte_mpz, 255) > 0) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    uint32_t n = (uint32_t)mpz_get_ui(size_mpz);
    uint8_t byte = (uint8_t)mpz_get_ui(byte_mpz);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, n ? n : 1, alignof(uint8_t));
    if (n > 0) memset(out, byte, n);
    return uplcrt_result_bytestring(b, out, n);
}
