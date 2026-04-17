#include "runtime/builtins/helpers.h"
#include "runtime/core/rterm.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

/*
 * BLS12-381 builtins (tags 54–70, 92–93) implemented via BLST.
 *
 * Internal convention:
 *   • G1 constants store 48-byte compressed points (BLST_ERROR if invalid).
 *   • G2 constants store 96-byte compressed points.
 *   • MlResult constants store a raw blst_fp12 (576 bytes).
 *
 * Every operation decompresses inputs, runs BLST arithmetic, then
 * re-compresses the result back into an arena-allocated constant.
 * This matches the Haskell/Cardano reference implementation's approach.
 */

#include "blst.h"

/* --------------------------------------------------------------------- */
/* Internal unwrap helpers                                               */
/* --------------------------------------------------------------------- */

static const uint8_t* unwrap_g1(uplc_budget* b, uplc_value v) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_BLS12_381_G1) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    return c->bls_g1.bytes;   /* 48 compressed bytes */
}

static const uint8_t* unwrap_g2(uplc_budget* b, uplc_value v) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_BLS12_381_G2) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    return c->bls_g2.bytes;   /* 96 compressed bytes */
}

static const blst_fp12* unwrap_ml(uplc_budget* b, uplc_value v) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_BLS12_381_ML_RESULT ||
        c->bls_ml_result.len != sizeof(blst_fp12)) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    return (const blst_fp12*)c->bls_ml_result.bytes;
}

/* Decompress 48 bytes → blst_p1_affine. Raises on invalid point. */
static void g1_uncompress(uplc_budget* b, blst_p1_affine* out, const uint8_t* bytes) {
    if (blst_p1_uncompress(out, bytes) != BLST_SUCCESS) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
}

/* Decompress 96 bytes → blst_p2_affine. Raises on invalid point. */
static void g2_uncompress(uplc_budget* b, blst_p2_affine* out, const uint8_t* bytes) {
    if (blst_p2_uncompress(out, bytes) != BLST_SUCCESS) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
}

/* Wrap 48-byte compressed G1 into a new uplc_value. */
static uplc_value result_g1(uplc_budget* b, const blst_p1* p) {
    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* buf = (uint8_t*)uplc_arena_alloc(ar, 48, 1);
    blst_p1_compress(buf, p);
    uplc_rconstant* c = uplc_rconst_bls_g1(ar, buf);
    return uplc_make_rcon(c);
}

/* Wrap 96-byte compressed G2 into a new uplc_value. */
static uplc_value result_g2(uplc_budget* b, const blst_p2* p) {
    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* buf = (uint8_t*)uplc_arena_alloc(ar, 96, 1);
    blst_p2_compress(buf, p);
    uplc_rconstant* c = uplc_rconst_bls_g2(ar, buf);
    return uplc_make_rcon(c);
}

/* Wrap a blst_fp12 as a MlResult. */
static uplc_value result_ml(uplc_budget* b, const blst_fp12* fp) {
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rconstant* c = uplc_rconst_bls_ml_result(
        ar, (const uint8_t*)fp, sizeof(blst_fp12));
    return uplc_make_rcon(c);
}

/*
 * Convert a GMP mpz integer to a BLST scalar byte array (little-endian,
 * 32 bytes — BLST expects scalars in little-endian order). Negative
 * scalars are first reduced into [0, r) mod the BLS12-381 scalar field
 * order; this matches the Plutus spec, which accepts any integer scalar.
 */
static void mpz_to_scalar_bytes(mpz_srcptr n, uint8_t out[32]);

/* Plutus hard-cap for multi-scalar-mul scalars: the signed 4096-bit
 * range [-2^4095, 2^4095 - 1]. Anything outside raises EvaluationFailure. */
static void check_msm_scalar_bound(uplc_budget* b, mpz_srcptr n) {
    size_t bits = mpz_sizeinbase(n, 2);
    if (bits < 4096) return;                       /* |n| < 2^4095 — OK */
    if (bits > 4096) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    /* bits == 4096: only -2^4095 is allowed. */
    if (mpz_sgn(n) >= 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    /* Verify n is exactly -2^4095 and not some other 4096-bit negative. */
    mpz_t neg_min;
    mpz_init(neg_min);
    mpz_ui_pow_ui(neg_min, 2, 4095);
    mpz_neg(neg_min, neg_min);
    int eq = (mpz_cmp(n, neg_min) == 0);
    mpz_clear(neg_min);
    if (!eq) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
}

static void mpz_to_scalar_bytes(mpz_srcptr n, uint8_t out[32]) {
    /* BLS12-381 scalar field order r (big-endian for mpz_import). */
    static const uint8_t r_be[32] = {
        0x73, 0xed, 0xa7, 0x53, 0x29, 0x9d, 0x7d, 0x48,
        0x33, 0x39, 0xd8, 0x08, 0x09, 0xa1, 0xd8, 0x05,
        0x53, 0xbd, 0xa4, 0x02, 0xff, 0xfe, 0x5b, 0xfe,
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01
    };

    mpz_t r, tmp;
    mpz_init(r);
    mpz_init(tmp);
    mpz_import(r, 32, 1, 1, 1, 0, r_be);

    mpz_mod(tmp, n, r);   /* mpz_mod returns the non-negative remainder */

    /* Export as little-endian. mpz_export with endian=-1 and nails=0 gives
     * LSB-first, LSB-bit first within each byte. */
    memset(out, 0, 32);
    size_t count = 0;
    mpz_export(out, &count, -1, 1, -1, 0, tmp);
    (void)count; /* remainder of buffer is already zero */

    mpz_clear(tmp);
    mpz_clear(r);
}

/* --------------------------------------------------------------------- */
/* G1 operations                                                         */
/* --------------------------------------------------------------------- */

uplc_value uplcrt_builtin_bls12_381_G1_add(uplc_budget* b, uplc_value* a) {
    blst_p1_affine pa, pb;
    g1_uncompress(b, &pa, unwrap_g1(b, a[0]));
    g1_uncompress(b, &pb, unwrap_g1(b, a[1]));
    blst_p1 pa_proj, result;
    blst_p1_from_affine(&pa_proj, &pa);
    blst_p1_add_or_double_affine(&result, &pa_proj, &pb);
    return result_g1(b, &result);
}

uplc_value uplcrt_builtin_bls12_381_G1_neg(uplc_budget* b, uplc_value* a) {
    blst_p1_affine pa;
    g1_uncompress(b, &pa, unwrap_g1(b, a[0]));
    blst_p1 p;
    blst_p1_from_affine(&p, &pa);
    blst_p1_cneg(&p, 1);
    return result_g1(b, &p);
}

uplc_value uplcrt_builtin_bls12_381_G1_scalarMul(uplc_budget* b, uplc_value* a) {
    mpz_srcptr n  = uplcrt_unwrap_integer(b, a[0]);
    blst_p1_affine pa;
    g1_uncompress(b, &pa, unwrap_g1(b, a[1]));

    uint8_t scalar[32];
    mpz_to_scalar_bytes(n, scalar);

    /* blst_p1_mult wants a projective input, not affine. */
    blst_p1 proj;
    blst_p1_from_affine(&proj, &pa);
    blst_p1 result;
    blst_p1_mult(&result, &proj, scalar, 256);
    return result_g1(b, &result);
}

uplc_value uplcrt_builtin_bls12_381_G1_equal(uplc_budget* b, uplc_value* a) {
    blst_p1_affine pa, pb;
    g1_uncompress(b, &pa, unwrap_g1(b, a[0]));
    g1_uncompress(b, &pb, unwrap_g1(b, a[1]));
    bool eq = blst_p1_affine_is_equal(&pa, &pb);
    return uplcrt_result_bool(b, eq);
}

uplc_value uplcrt_builtin_bls12_381_G1_compress(uplc_budget* b, uplc_value* a) {
    const uint8_t* bytes = unwrap_g1(b, a[0]);
    /* Validate the point first. */
    blst_p1_affine pa;
    g1_uncompress(b, &pa, bytes);
    /* Return the compressed bytes as a ByteString. */
    return uplcrt_result_bytestring(b, bytes, 48);
}

uplc_value uplcrt_builtin_bls12_381_G1_uncompress(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bytes = uplcrt_unwrap_bytestring(b, a[0], &len);
    if (len != 48) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    blst_p1_affine pa;
    if (blst_p1_uncompress(&pa, bytes) != BLST_SUCCESS) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    /* Verify the point is on the curve and in the correct subgroup. */
    if (!blst_p1_affine_in_g1(&pa)) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rconstant* c = uplc_rconst_bls_g1(ar, bytes);
    return uplc_make_rcon(c);
}

uplc_value uplcrt_builtin_bls12_381_G1_hashToGroup(uplc_budget* b, uplc_value* a) {
    uint32_t msg_len = 0, dst_len = 0;
    const uint8_t* msg = uplcrt_unwrap_bytestring(b, a[0], &msg_len);
    const uint8_t* dst = uplcrt_unwrap_bytestring(b, a[1], &dst_len);

    /* BLS hash-to-curve (RFC 9380) restricts DST to ≤ 255 bytes. */
    if (dst_len > 255) uplcrt_fail(b, UPLC_FAIL_EVALUATION);

    blst_p1 result;
    blst_hash_to_g1(&result, msg, msg_len, dst, dst_len, NULL, 0);
    return result_g1(b, &result);
}

/* --------------------------------------------------------------------- */
/* G2 operations                                                         */
/* --------------------------------------------------------------------- */

uplc_value uplcrt_builtin_bls12_381_G2_add(uplc_budget* b, uplc_value* a) {
    blst_p2_affine pa, pb;
    g2_uncompress(b, &pa, unwrap_g2(b, a[0]));
    g2_uncompress(b, &pb, unwrap_g2(b, a[1]));
    blst_p2 pa_proj, result;
    blst_p2_from_affine(&pa_proj, &pa);
    blst_p2_add_or_double_affine(&result, &pa_proj, &pb);
    return result_g2(b, &result);
}

uplc_value uplcrt_builtin_bls12_381_G2_neg(uplc_budget* b, uplc_value* a) {
    blst_p2_affine pa;
    g2_uncompress(b, &pa, unwrap_g2(b, a[0]));
    blst_p2 p;
    blst_p2_from_affine(&p, &pa);
    blst_p2_cneg(&p, 1);
    return result_g2(b, &p);
}

uplc_value uplcrt_builtin_bls12_381_G2_scalarMul(uplc_budget* b, uplc_value* a) {
    mpz_srcptr n = uplcrt_unwrap_integer(b, a[0]);
    blst_p2_affine pa;
    g2_uncompress(b, &pa, unwrap_g2(b, a[1]));

    uint8_t scalar[32];
    mpz_to_scalar_bytes(n, scalar);

    blst_p2 proj;
    blst_p2_from_affine(&proj, &pa);
    blst_p2 result;
    blst_p2_mult(&result, &proj, scalar, 256);
    return result_g2(b, &result);
}

uplc_value uplcrt_builtin_bls12_381_G2_equal(uplc_budget* b, uplc_value* a) {
    blst_p2_affine pa, pb;
    g2_uncompress(b, &pa, unwrap_g2(b, a[0]));
    g2_uncompress(b, &pb, unwrap_g2(b, a[1]));
    bool eq = blst_p2_affine_is_equal(&pa, &pb);
    return uplcrt_result_bool(b, eq);
}

uplc_value uplcrt_builtin_bls12_381_G2_compress(uplc_budget* b, uplc_value* a) {
    const uint8_t* bytes = unwrap_g2(b, a[0]);
    blst_p2_affine pa;
    g2_uncompress(b, &pa, bytes);
    return uplcrt_result_bytestring(b, bytes, 96);
}

uplc_value uplcrt_builtin_bls12_381_G2_uncompress(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bytes = uplcrt_unwrap_bytestring(b, a[0], &len);
    if (len != 96) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    blst_p2_affine pa;
    if (blst_p2_uncompress(&pa, bytes) != BLST_SUCCESS) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    if (!blst_p2_affine_in_g2(&pa)) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rconstant* c = uplc_rconst_bls_g2(ar, bytes);
    return uplc_make_rcon(c);
}

uplc_value uplcrt_builtin_bls12_381_G2_hashToGroup(uplc_budget* b, uplc_value* a) {
    uint32_t msg_len = 0, dst_len = 0;
    const uint8_t* msg = uplcrt_unwrap_bytestring(b, a[0], &msg_len);
    const uint8_t* dst = uplcrt_unwrap_bytestring(b, a[1], &dst_len);

    if (dst_len > 255) uplcrt_fail(b, UPLC_FAIL_EVALUATION);

    blst_p2 result;
    blst_hash_to_g2(&result, msg, msg_len, dst, dst_len, NULL, 0);
    return result_g2(b, &result);
}

/* --------------------------------------------------------------------- */
/* Pairing operations                                                    */
/* --------------------------------------------------------------------- */

uplc_value uplcrt_builtin_bls12_381_millerLoop(uplc_budget* b, uplc_value* a) {
    blst_p1_affine p1;
    blst_p2_affine p2;
    g1_uncompress(b, &p1, unwrap_g1(b, a[0]));
    g2_uncompress(b, &p2, unwrap_g2(b, a[1]));

    uplc_arena* ar = uplcrt_budget_arena(b);
    blst_fp12* fp  = (blst_fp12*)uplc_arena_alloc(ar, sizeof(blst_fp12), alignof(blst_fp12));
    blst_miller_loop(fp, &p2, &p1);
    return result_ml(b, fp);
}

uplc_value uplcrt_builtin_bls12_381_mulMlResult(uplc_budget* b, uplc_value* a) {
    const blst_fp12* r1 = unwrap_ml(b, a[0]);
    const blst_fp12* r2 = unwrap_ml(b, a[1]);

    uplc_arena* ar = uplcrt_budget_arena(b);
    blst_fp12* product = (blst_fp12*)uplc_arena_alloc(ar, sizeof(blst_fp12), alignof(blst_fp12));
    blst_fp12_mul(product, r1, r2);
    return result_ml(b, product);
}

uplc_value uplcrt_builtin_bls12_381_finalVerify(uplc_budget* b, uplc_value* a) {
    const blst_fp12* r1 = unwrap_ml(b, a[0]);
    const blst_fp12* r2 = unwrap_ml(b, a[1]);
    bool ok = blst_fp12_finalverify(r1, r2);
    return uplcrt_result_bool(b, ok);
}

/* --------------------------------------------------------------------- */
/* Multi-scalar multiplication                                           */
/* --------------------------------------------------------------------- */

/*
 * bls12_381_G1_multiScalarMul
 *   arg[0]: (list integer)  — scalars
 *   arg[1]: (list bls12_381_G1_element) — G1 points
 *
 * Plain accumulator loop over scalarMul. BLST's Pippenger API is faster
 * for large inputs but requires affine arrays and scratch buffers; the
 * naive loop is simpler and suffices for the conformance surface.
 */
uplc_value uplcrt_builtin_bls12_381_G1_multiScalarMul(uplc_budget* b, uplc_value* a) {
    const uplc_rconstant* scalars_c = uplcrt_unwrap_list(b, a[0]);
    const uplc_rconstant* points_c  = uplcrt_unwrap_list(b, a[1]);

    /* When the two lists differ in length, extra trailing elements of the
     * longer list are ignored — n = min(|scalars|, |points|). */
    uint32_t n = scalars_c->list.n_values;
    if (points_c->list.n_values < n) n = points_c->list.n_values;

    /* Start with the identity (point at infinity) in projective form.
     * The canonical way to get identity in BLST is to multiply any point
     * by the zero scalar. */
    blst_p1 acc;
    {
        blst_p1_affine id_aff;
        static const uint8_t zero_bytes[48] = {
            0xc0 /* compressed infinity: bits 1100_0000 */
        };
        if (blst_p1_uncompress(&id_aff, zero_bytes) != BLST_SUCCESS) {
            uplcrt_fail(b, UPLC_FAIL_MACHINE);
        }
        blst_p1_from_affine(&acc, &id_aff);
    }

    for (uint32_t i = 0; i < n; ++i) {
        const uplc_rconstant* sc = scalars_c->list.values[i];
        const uplc_rconstant* pc = points_c->list.values[i];
        if (!sc || sc->tag != UPLC_RCONST_INTEGER) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        if (!pc || pc->tag != UPLC_RCONST_BLS12_381_G1) uplcrt_fail(b, UPLC_FAIL_EVALUATION);

        check_msm_scalar_bound(b, sc->integer.value);

        blst_p1_affine pa;
        g1_uncompress(b, &pa, pc->bls_g1.bytes);

        uint8_t scalar_bytes[32];
        mpz_to_scalar_bytes(sc->integer.value, scalar_bytes);

        blst_p1 p_proj, term;
        blst_p1_from_affine(&p_proj, &pa);
        blst_p1_mult(&term, &p_proj, scalar_bytes, 256);

        blst_p1_add_or_double(&acc, &acc, &term);
    }

    return result_g1(b, &acc);
}

/*
 * bls12_381_G2_multiScalarMul
 *   arg[0]: (list integer)  — scalars
 *   arg[1]: (list bls12_381_G2_element) — G2 points
 */
uplc_value uplcrt_builtin_bls12_381_G2_multiScalarMul(uplc_budget* b, uplc_value* a) {
    const uplc_rconstant* scalars_c = uplcrt_unwrap_list(b, a[0]);
    const uplc_rconstant* points_c  = uplcrt_unwrap_list(b, a[1]);

    uint32_t n = scalars_c->list.n_values;
    if (points_c->list.n_values < n) n = points_c->list.n_values;

    blst_p2 acc;
    {
        blst_p2_affine id_aff;
        static const uint8_t zero_bytes[96] = { 0xc0 };
        if (blst_p2_uncompress(&id_aff, zero_bytes) != BLST_SUCCESS) {
            uplcrt_fail(b, UPLC_FAIL_MACHINE);
        }
        blst_p2_from_affine(&acc, &id_aff);
    }

    for (uint32_t i = 0; i < n; ++i) {
        const uplc_rconstant* sc = scalars_c->list.values[i];
        const uplc_rconstant* pc = points_c->list.values[i];
        if (!sc || sc->tag != UPLC_RCONST_INTEGER) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        if (!pc || pc->tag != UPLC_RCONST_BLS12_381_G2) uplcrt_fail(b, UPLC_FAIL_EVALUATION);

        check_msm_scalar_bound(b, sc->integer.value);

        blst_p2_affine pa;
        g2_uncompress(b, &pa, pc->bls_g2.bytes);

        uint8_t scalar_bytes[32];
        mpz_to_scalar_bytes(sc->integer.value, scalar_bytes);

        blst_p2 p_proj, term;
        blst_p2_from_affine(&p_proj, &pa);
        blst_p2_mult(&term, &p_proj, scalar_bytes, 256);

        blst_p2_add_or_double(&acc, &acc, &term);
    }

    return result_g2(b, &acc);
}
