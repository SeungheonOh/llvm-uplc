#include "runtime/builtins/helpers.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include <gmp.h>

/*
 * integerToByteString / byteStringToInteger. The conversion follows the
 * CIP-0087 "integer <-> bytestring" semantics used by the TS reference.
 */

#define INT2BS_MAX_OUTPUT 8192u

uplc_value uplcrt_builtin_integerToByteString(uplc_budget* b, uplc_value* a) {
    bool big_endian = uplcrt_unwrap_bool(b, a[0]);
    mpz_srcptr size_mpz = uplcrt_unwrap_integer(b, a[1]);
    mpz_srcptr input = uplcrt_unwrap_integer(b, a[2]);

    if (mpz_sgn(input) < 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    if (mpz_sgn(size_mpz) < 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    if (mpz_cmp_ui(size_mpz, INT2BS_MAX_OUTPUT) > 0) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    uint32_t requested = (uint32_t)mpz_get_ui(size_mpz);

    /* Export input big-endian into a freshly allocated buffer. Zero is a
     * special case because mpz_sizeinbase returns 1 for it but we want
     * an empty significand. */
    uint32_t sig_len = 0;
    uint8_t* sig_bytes = NULL;
    if (mpz_sgn(input) != 0) {
        size_t need = (mpz_sizeinbase(input, 2) + 7) / 8;
        sig_bytes = (uint8_t*)uplc_arena_alloc(
            uplcrt_budget_arena(b), need ? need : 1, alignof(uint8_t));
        size_t written = 0;
        mpz_export(sig_bytes, &written, 1, 1, 1, 0, input);
        sig_len = (uint32_t)written;
    }

    if (requested == 0) {
        /* Unbounded — minimal representation. */
        if (sig_len > INT2BS_MAX_OUTPUT) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        if (big_endian) {
            return uplcrt_result_bytestring(b, sig_bytes, sig_len);
        }
        uplc_arena* ar = uplcrt_budget_arena(b);
        uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, sig_len ? sig_len : 1, alignof(uint8_t));
        for (uint32_t i = 0; i < sig_len; ++i) out[i] = sig_bytes[sig_len - 1 - i];
        return uplcrt_result_bytestring(b, out, sig_len);
    }

    if (sig_len > requested) uplcrt_fail(b, UPLC_FAIL_EVALUATION);

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, requested ? requested : 1, alignof(uint8_t));
    memset(out, 0, requested);
    if (big_endian) {
        memcpy(out + (requested - sig_len), sig_bytes, sig_len);
    } else {
        for (uint32_t i = 0; i < sig_len; ++i) out[i] = sig_bytes[sig_len - 1 - i];
    }
    return uplcrt_result_bytestring(b, out, requested);
}

uplc_value uplcrt_builtin_byteStringToInteger(uplc_budget* b, uplc_value* a) {
    bool big_endian = uplcrt_unwrap_bool(b, a[0]);
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[1], &len);

    if (len == 0) return uplcrt_result_integer_si(b, 0);

    uplc_arena* ar = uplcrt_budget_arena(b);
    mpz_ptr out = uplc_arena_alloc_mpz(ar);
    if (big_endian) {
        mpz_import(out, len, 1 /* MSB first */, 1, 1, 0, bs);
    } else {
        mpz_import(out, len, -1 /* LSB first */, 1, 1, 0, bs);
    }
    return uplcrt_result_integer_mpz(b, out);
}
