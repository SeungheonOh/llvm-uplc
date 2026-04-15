#include "runtime/builtins/helpers.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

/*
 * Bytestring builtins. Port of TS cek/builtins/bytestring.ts. Byte
 * buffers are copied into the arena via uplcrt_result_bytestring.
 */

static int64_t mpz_to_clamped_i64(mpz_srcptr v, int64_t hi) {
    if (mpz_sgn(v) < 0) return 0;
    if (mpz_fits_slong_p(v)) {
        long s = mpz_get_si(v);
        return (int64_t)s < 0 ? 0 : ((int64_t)s > hi ? hi : (int64_t)s);
    }
    return hi;
}

uplc_value uplcrt_builtin_appendByteString(uplc_budget* b, uplc_value* a) {
    uint32_t la = 0, lb = 0;
    const uint8_t* x = uplcrt_unwrap_bytestring(b, a[0], &la);
    const uint8_t* y = uplcrt_unwrap_bytestring(b, a[1], &lb);

    uint32_t n = la + lb;
    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* buf = (uint8_t*)uplc_arena_alloc(ar, n ? n : 1, alignof(uint8_t));
    if (la > 0) memcpy(buf, x, la);
    if (lb > 0) memcpy(buf + la, y, lb);
    return uplcrt_result_bytestring(b, buf, n);
}

uplc_value uplcrt_builtin_consByteString(uplc_budget* b, uplc_value* a) {
    mpz_srcptr n_mpz = uplcrt_unwrap_integer(b, a[0]);
    /* Byte value must be in [0, 255]. */
    if (mpz_sgn(n_mpz) < 0 || mpz_cmp_ui(n_mpz, 255) > 0) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    uint8_t byte = (uint8_t)mpz_get_ui(n_mpz);

    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[1], &len);

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* buf = (uint8_t*)uplc_arena_alloc(ar, (size_t)len + 1, alignof(uint8_t));
    buf[0] = byte;
    if (len > 0) memcpy(buf + 1, bs, len);
    return uplcrt_result_bytestring(b, buf, len + 1);
}

uplc_value uplcrt_builtin_sliceByteString(uplc_budget* b, uplc_value* a) {
    mpz_srcptr skip_mpz = uplcrt_unwrap_integer(b, a[0]);
    mpz_srcptr take_mpz = uplcrt_unwrap_integer(b, a[1]);
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[2], &len);

    int64_t skip = mpz_to_clamped_i64(skip_mpz, (int64_t)len);
    int64_t take = mpz_to_clamped_i64(take_mpz, (int64_t)len);
    if (skip > (int64_t)len) skip = (int64_t)len;
    int64_t remaining = (int64_t)len - skip;
    if (take > remaining) take = remaining;
    if (take < 0) take = 0;

    return uplcrt_result_bytestring(
        b, bs + skip, (uint32_t)take);
}

uplc_value uplcrt_builtin_lengthOfByteString(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    (void)uplcrt_unwrap_bytestring(b, a[0], &len);
    return uplcrt_result_integer_si(b, (int64_t)len);
}

uplc_value uplcrt_builtin_indexByteString(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[0], &len);
    mpz_srcptr idx_mpz = uplcrt_unwrap_integer(b, a[1]);

    if (mpz_sgn(idx_mpz) < 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    if (mpz_cmp_ui(idx_mpz, len) >= 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    uint32_t idx = (uint32_t)mpz_get_ui(idx_mpz);
    return uplcrt_result_integer_si(b, (int64_t)bs[idx]);
}

uplc_value uplcrt_builtin_equalsByteString(uplc_budget* b, uplc_value* a) {
    uint32_t la = 0, lb = 0;
    const uint8_t* x = uplcrt_unwrap_bytestring(b, a[0], &la);
    const uint8_t* y = uplcrt_unwrap_bytestring(b, a[1], &lb);
    if (la != lb) return uplcrt_result_bool(b, false);
    return uplcrt_result_bool(b, memcmp(x, y, la) == 0);
}

static int bs_compare(const uint8_t* x, uint32_t lx,
                      const uint8_t* y, uint32_t ly) {
    uint32_t n = lx < ly ? lx : ly;
    int cmp = (n > 0) ? memcmp(x, y, n) : 0;
    if (cmp != 0) return cmp;
    if (lx < ly) return -1;
    if (lx > ly) return 1;
    return 0;
}

uplc_value uplcrt_builtin_lessThanByteString(uplc_budget* b, uplc_value* a) {
    uint32_t la = 0, lb = 0;
    const uint8_t* x = uplcrt_unwrap_bytestring(b, a[0], &la);
    const uint8_t* y = uplcrt_unwrap_bytestring(b, a[1], &lb);
    return uplcrt_result_bool(b, bs_compare(x, la, y, lb) < 0);
}

uplc_value uplcrt_builtin_lessThanEqualsByteString(uplc_budget* b, uplc_value* a) {
    uint32_t la = 0, lb = 0;
    const uint8_t* x = uplcrt_unwrap_bytestring(b, a[0], &la);
    const uint8_t* y = uplcrt_unwrap_bytestring(b, a[1], &lb);
    return uplcrt_result_bool(b, bs_compare(x, la, y, lb) <= 0);
}
