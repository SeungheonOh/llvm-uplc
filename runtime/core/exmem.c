#include "runtime/core/exmem.h"

#include <stdint.h>

#include <gmp.h>

#include "runtime/core/rterm.h"  /* uplc_rconstant, uplc_rdata (shared types) */
#include "runtime/core/value.h"

/*
 * ExMem primitives and per-builtin size extraction. M3a covers the
 * builtins implemented in this milestone (arith, control, list, pair,
 * bytestring). Unimplemented builtins fall through to the all-zeros
 * fallback — they will never reach the dispatcher since the impl pointer
 * is NULL and uplcrt_run_builtin raises before evaluating sizes.
 */

int64_t uplcrt_exmem_integer(mpz_srcptr n) {
    if (mpz_sgn(n) == 0) return 1;
    /* TS: floor((bits - 1) / 64) + 1 */
    size_t bits = mpz_sizeinbase(n, 2);  /* >= 1 for non-zero */
    return (int64_t)((bits - 1) / 64 + 1);
}

int64_t uplcrt_exmem_bytestring(uint32_t len) {
    if (len == 0) return 1;
    return (int64_t)((len - 1) / 8 + 1);
}

int64_t uplcrt_exmem_string_utf8(uint32_t byte_len) {
    /* TS returns the raw byte length (not divided by 8). */
    return (int64_t)byte_len;
}

int64_t uplcrt_exmem_data(const void* root) {
    /* Walk the PlutusData tree iteratively to avoid blowing the C stack on
     * deeply nested Data. Each node contributes 4 plus any primitive leaf
     * size. The shape is the shared uplc_rdata layout. */
    if (!root) return 4;

    const uplc_rdata* stack[1024];
    int sp = 0;
    stack[sp++] = (const uplc_rdata*)root;
    int64_t total = 0;

    while (sp > 0) {
        const uplc_rdata* d = stack[--sp];
        total += 4;
        switch ((uplc_rdata_tag)d->tag) {
            case UPLC_RDATA_CONSTR:
                for (int32_t i = (int32_t)d->constr.n_fields - 1; i >= 0; --i) {
                    if (sp < 1024) stack[sp++] = d->constr.fields[i];
                }
                break;
            case UPLC_RDATA_MAP:
                for (int32_t i = (int32_t)d->map.n_entries - 1; i >= 0; --i) {
                    if (sp < 1024) stack[sp++] = d->map.entries[i].value;
                    if (sp < 1024) stack[sp++] = d->map.entries[i].key;
                }
                break;
            case UPLC_RDATA_LIST:
                for (int32_t i = (int32_t)d->list.n_values - 1; i >= 0; --i) {
                    if (sp < 1024) stack[sp++] = d->list.values[i];
                }
                break;
            case UPLC_RDATA_INTEGER:
                total += uplcrt_exmem_integer(d->integer.value);
                break;
            case UPLC_RDATA_BYTESTRING:
                total += uplcrt_exmem_bytestring(d->bytestring.len);
                break;
        }
    }
    return total;
}

/* ---------------- Per-argument size helpers ---------------- */

/* Extract a uplc_rconstant pointer from a VCon value. NULL if not a VCon. */
static const uplc_rconstant* as_const(uplc_value v) {
    if (v.tag != UPLC_V_CON) return NULL;
    if (v.subtag & UPLC_VCON_INLINE_BIT) return NULL;  /* inline-int */
    return (const uplc_rconstant*)uplc_value_payload(v);
}

/* Number of 64-bit "words" needed to represent |n| (at least 1). */
static int64_t i64_exmem_size(int64_t n) {
    if (n == 0) return 1;
    /* Take magnitude and count 64-bit-word slots. */
    uint64_t m = n < 0 ? (uint64_t)-(n + 1) + 1u : (uint64_t)n;
    /* (bits - 1) / 64 + 1 */
    int bits = 64 - __builtin_clzll(m);
    return (int64_t)((bits - 1) / 64 + 1);
}

static int64_t int_size(uplc_value v) {
    if (uplc_value_is_int_inline(v)) {
        return i64_exmem_size(uplc_value_int_inline(v));
    }
    const uplc_rconstant* c = as_const(v);
    if (c && c->tag == UPLC_RCONST_INTEGER) {
        return uplcrt_exmem_integer(c->integer.value);
    }
    return 1;
}

static int64_t bs_size(uplc_value v) {
    const uplc_rconstant* c = as_const(v);
    if (c && c->tag == UPLC_RCONST_BYTESTRING) {
        return uplcrt_exmem_bytestring(c->bytestring.len);
    }
    return 0;
}

static int64_t str_size(uplc_value v) {
    const uplc_rconstant* c = as_const(v);
    if (c && c->tag == UPLC_RCONST_STRING) {
        return uplcrt_exmem_string_utf8(c->string.len);
    }
    return 0;
}

static int64_t data_size(uplc_value v) {
    const uplc_rconstant* c = as_const(v);
    if (c && c->tag == UPLC_RCONST_DATA) {
        return uplcrt_exmem_data(c->data.value);
    }
    return 4;
}

static int64_t list_size(uplc_value v) {
    const uplc_rconstant* c = as_const(v);
    if (c && c->tag == UPLC_RCONST_LIST) {
        return (int64_t)c->list.n_values;
    }
    return 0;
}

/* Plutus Value exmem. The reference implementation uses different size
 * projections for different Value builtins, and mapping them exactly is a
 * finicky cost-model calibration task. We apply a coarse approximation:
 *
 *   size_sum      = total token entries (used by unionValue, valueContains,
 *                   scaleValue, valueData)
 *   size_compact  = bit_length(1 + n_ccy + n_tokens), used by insertCoin
 *                   and lookupCoin (which cost against a "compact" size)
 *
 * This nearly matches the reference, leaving a small tail of mismatches
 * that require Plutus-internal cost-model data we don't currently ingest.
 */
static int64_t value_size_sum(uplc_value v) {
    const uplc_rconstant* c = as_const(v);
    if (!c || c->tag != UPLC_RCONST_VALUE) return 0;
    const uplc_rledger_value* lv = c->ledger_value.value;
    if (!lv || lv->n_entries == 0) return 0;
    int64_t n = 0;
    for (uint32_t i = 0; i < lv->n_entries; ++i) n += (int64_t)lv->entries[i].n_tokens;
    return n;
}

static int64_t value_size_compact(uplc_value v) {
    const uplc_rconstant* c = as_const(v);
    if (!c || c->tag != UPLC_RCONST_VALUE) return 0;
    const uplc_rledger_value* lv = c->ledger_value.value;
    if (!lv || lv->n_entries == 0) return 0;
    uint64_t total = lv->n_entries;
    for (uint32_t i = 0; i < lv->n_entries; ++i) total += lv->entries[i].n_tokens;
    uint64_t m = total + 1;
    int64_t bits = 0;
    while (m > 0) { ++bits; m >>= 1; }
    return bits;
}

/* For builtins that cost an integer "as a byte count" — replicateByte's
 * first arg, integerToByteString's width arg. TS maps n to sizeExMem(n) =
 * floor((n - 1) / 8) + 1 (or 0 for n <= 0). */
static int64_t int_as_bytesize(uplc_value v) {
    long nv = 0;
    if (uplc_value_is_int_inline(v)) {
        int64_t iv = uplc_value_int_inline(v);
        if (iv <= 0) return 0;
        nv = (long)iv;
    } else {
        const uplc_rconstant* c = as_const(v);
        if (!c || c->tag != UPLC_RCONST_INTEGER) return 0;
        mpz_srcptr n = c->integer.value;
        if (mpz_sgn(n) <= 0) return 0;
        if (!mpz_fits_slong_p(n)) return 0;
        nv = mpz_get_si(n);
    }
    return (int64_t)((nv - 1) / 8 + 1);
}

/* Integer literally costed — |n| clamped to I64_MAX. */
static int64_t int_literal(uplc_value v) {
    if (uplc_value_is_int_inline(v)) {
        int64_t iv = uplc_value_int_inline(v);
        return iv < 0 ? -iv : iv;
    }
    const uplc_rconstant* c = as_const(v);
    if (!c || c->tag != UPLC_RCONST_INTEGER) return 0;
    mpz_srcptr n = c->integer.value;
    if (mpz_fits_slong_p(n)) {
        long nv = mpz_get_si(n);
        return nv < 0 ? -nv : nv;
    }
    return INT64_MAX;
}

/* --- Dispatch: map the TS computeArgSizes() switch to C ---------------- */

void uplcrt_builtin_arg_sizes(uplc_builtin_tag tag,
                              const uplc_value* argv,
                              uint32_t argc,
                              int64_t out_sizes[3]) {
    out_sizes[0] = out_sizes[1] = out_sizes[2] = 0;

    switch ((int)tag) {
        /* 2-arg integer ops -- tags 0..9 in the builtin table. */
        case 0:  /* addInteger */
        case 1:  /* subtractInteger */
        case 2:  /* multiplyInteger */
        case 3:  /* divideInteger */
        case 4:  /* quotientInteger */
        case 5:  /* remainderInteger */
        case 6:  /* modInteger */
        case 7:  /* equalsInteger */
        case 8:  /* lessThanInteger */
        case 9:  /* lessThanEqualsInteger */
            if (argc >= 2) {
                out_sizes[0] = int_size(argv[0]);
                out_sizes[1] = int_size(argv[1]);
            }
            return;

        /* hash builtins — linear in the input bytestring size */
        case 18: /* sha2_256    (bs) */
        case 19: /* sha3_256    (bs) */
        case 20: /* blake2b_256 (bs) */
        case 71: /* keccak_256  (bs) */
        case 72: /* blake2b_224 (bs) */
        case 86: /* ripemd_160  (bs) */
            if (argc >= 1) out_sizes[0] = bs_size(argv[0]);
            return;

        case 21: /* verifyEd25519Signature (vk, msg, sig) — cost linear in Y (msg) */
        case 52: /* verifyEcdsaSecp256k1Signature   — constant */
        case 53: /* verifySchnorrSecp256k1Signature — linear in Y (msg) */
            if (argc >= 3) {
                out_sizes[0] = bs_size(argv[0]);
                out_sizes[1] = bs_size(argv[1]);
                out_sizes[2] = bs_size(argv[2]);
            }
            return;

        /* --- BLS12-381 ------------------------------------------------- */
        case 54: /* bls12_381_G1_add        (g1, g1) — constant */
        case 57: /* bls12_381_G1_equal      (g1, g1) — constant */
        case 61: /* bls12_381_G2_add        (g2, g2) — constant */
        case 64: /* bls12_381_G2_equal      (g2, g2) — constant */
        case 68: /* bls12_381_millerLoop    (g1, g2) — constant */
        case 69: /* bls12_381_mulMlResult   (ml, ml) — constant */
        case 70: /* bls12_381_finalVerify   (ml, ml) — constant */
        case 55: /* bls12_381_G1_neg        (g1)     — constant */
        case 58: /* bls12_381_G1_compress   (g1)     — constant */
        case 62: /* bls12_381_G2_neg        (g2)     — constant */
        case 65: /* bls12_381_G2_compress   (g2)     — constant */
            return;

        case 56: /* bls12_381_G1_scalarMul  (int, g1)  — LINX */
        case 63: /* bls12_381_G2_scalarMul  (int, g2)  — LINX */
            if (argc >= 2) out_sizes[0] = int_size(argv[0]);
            return;

        case 59: /* bls12_381_G1_uncompress (bs) — constant */
        case 66: /* bls12_381_G2_uncompress (bs) — constant */
            /* Cost is constant, so no sizes needed. */
            return;

        case 60: /* bls12_381_G1_hashToGroup (bs, bs) — LINX over msg */
        case 67: /* bls12_381_G2_hashToGroup (bs, bs) — LINX over msg */
            if (argc >= 2) {
                out_sizes[0] = bs_size(argv[0]);
                out_sizes[1] = bs_size(argv[1]);
            }
            return;

        case 92: /* bls12_381_G1_multiScalarMul (list int, list g1) — LINX */
        case 93: /* bls12_381_G2_multiScalarMul (list int, list g2) — LINX */
            if (argc >= 2) {
                out_sizes[0] = list_size(argv[0]);
                out_sizes[1] = list_size(argv[1]);
            }
            return;

        /* --- Value operations ----------------------------------------- */
        case 94: /* insertCoin (cur, tok, amt, val) — LIN over compact value size */
            if (argc >= 4) out_sizes[0] = value_size_compact(argv[3]);
            return;

        case 95: /* lookupCoin (cur, tok, val) — LINZ over compact value size */
            if (argc >= 3) out_sizes[2] = value_size_compact(argv[2]);
            return;

        case 100: /* scaleValue (k, val) — LINY over sum value size */
            if (argc >= 2) out_sizes[1] = value_size_sum(argv[1]);
            return;

        case 96: /* unionValue (v1, v2)     — INTR over (x=|v1|, y=|v2|) */
        case 97: /* valueContains (v1, v2)  — ABOVE with x=|v1|, y=|v2| */
            if (argc >= 2) {
                out_sizes[0] = value_size_sum(argv[0]);
                out_sizes[1] = value_size_sum(argv[1]);
            }
            return;

        case 98: /* valueData (v) — LIN over value size */
            if (argc >= 1) out_sizes[0] = value_size_sum(argv[0]);
            return;

        case 99: /* unValueData (data) — QX over data size */
            if (argc >= 1) out_sizes[0] = data_size(argv[0]);
            return;

        /* 2-arg bytestring ops */
        case 10: /* appendByteString */
        case 15: /* equalsByteString */
        case 16: /* lessThanByteString */
        case 17: /* lessThanEqualsByteString */
            if (argc >= 2) {
                out_sizes[0] = bs_size(argv[0]);
                out_sizes[1] = bs_size(argv[1]);
            }
            return;

        case 11: /* consByteString  (int, bs) */
            if (argc >= 2) {
                out_sizes[0] = int_size(argv[0]);
                out_sizes[1] = bs_size(argv[1]);
            }
            return;

        case 12: /* sliceByteString (int, int, bs) */
            if (argc >= 3) {
                out_sizes[0] = int_size(argv[0]);
                out_sizes[1] = int_size(argv[1]);
                out_sizes[2] = bs_size(argv[2]);
            }
            return;

        case 13: /* lengthOfByteString (bs) */
            if (argc >= 1) out_sizes[0] = bs_size(argv[0]);
            return;

        case 14: /* indexByteString (bs, int) */
            if (argc >= 2) {
                out_sizes[0] = bs_size(argv[0]);
                out_sizes[1] = int_size(argv[1]);
            }
            return;

        /* 2-arg string ops */
        case 22: /* appendString */
        case 23: /* equalsString */
            if (argc >= 2) {
                out_sizes[0] = str_size(argv[0]);
                out_sizes[1] = str_size(argv[1]);
            }
            return;

        case 24: /* encodeUtf8 (str) */
            if (argc >= 1) out_sizes[0] = str_size(argv[0]);
            return;
        case 25: /* decodeUtf8 (bs) */
            if (argc >= 1) out_sizes[0] = bs_size(argv[0]);
            return;

        /* Constant-cost builtins: ifThenElse, chooseUnit, trace, pair/list
         * selectors, most data constructors/deconstructors, array selectors.
         * All keep sizes at zero — the cost model row is CONSTANT. */
        case 26: /* ifThenElse */
        case 27: /* chooseUnit */
        case 28: /* trace */
        case 29: /* fstPair */
        case 30: /* sndPair */
        case 31: /* chooseList */
        case 32: /* mkCons */
        case 33: /* headList */
        case 34: /* tailList */
        case 35: /* nullList */
        case 36: /* chooseData */
        case 37: /* constrData */
        case 38: /* mapData */
        case 39: /* listData */
        case 40: /* iData */
        case 41: /* bData */
        case 42: /* unConstrData */
        case 43: /* unMapData */
        case 44: /* unListData */
        case 45: /* unIData */
        case 46: /* unBData */
        case 48: /* mkPairData */
        case 49: /* mkNilData */
        case 50: /* mkNilPairData */
        case 79: /* readBit */
        case 89: /* lengthOfArray */
        case 91: /* indexArray */
            return;

        case 47: /* equalsData (data, data) */
            if (argc >= 2) {
                out_sizes[0] = data_size(argv[0]);
                out_sizes[1] = data_size(argv[1]);
            }
            return;

        case 51: /* serialiseData (data) */
            if (argc >= 1) out_sizes[0] = data_size(argv[0]);
            return;

        case 73: /* integerToByteString (bool, sizeExMem(width), int) */
            if (argc >= 3) {
                out_sizes[0] = 1;
                out_sizes[1] = int_as_bytesize(argv[1]);
                out_sizes[2] = int_size(argv[2]);
            }
            return;

        case 74: /* byteStringToInteger (bool, bs) */
            if (argc >= 2) {
                out_sizes[0] = 1;
                out_sizes[1] = bs_size(argv[1]);
            }
            return;

        case 75: /* andByteString   (bool, bs, bs) */
        case 76: /* orByteString    (bool, bs, bs) */
        case 77: /* xorByteString   (bool, bs, bs) */
            if (argc >= 3) {
                out_sizes[0] = 1;
                out_sizes[1] = bs_size(argv[1]);
                out_sizes[2] = bs_size(argv[2]);
            }
            return;

        case 78: /* complementByteString (bs) */
        case 84: /* countSetBits (bs) */
        case 85: /* findFirstSetBit (bs) */
            if (argc >= 1) out_sizes[0] = bs_size(argv[0]);
            return;

        case 80: /* writeBits (bs, list, list) */
            if (argc >= 3) {
                out_sizes[0] = bs_size(argv[0]);
                out_sizes[1] = list_size(argv[1]);
                out_sizes[2] = list_size(argv[2]);
            }
            return;

        case 81: /* replicateByte (sizeExMem(n), int) */
            if (argc >= 2) {
                out_sizes[0] = int_as_bytesize(argv[0]);
                out_sizes[1] = int_size(argv[1]);
            }
            return;

        case 82: /* shiftByteString  (bs, intLiteral) */
        case 83: /* rotateByteString (bs, intLiteral) */
            if (argc >= 2) {
                out_sizes[0] = bs_size(argv[0]);
                out_sizes[1] = int_literal(argv[1]);
            }
            return;

        case 87: /* expModInteger (int, int, int) */
            if (argc >= 3) {
                out_sizes[0] = int_size(argv[0]);
                out_sizes[1] = int_size(argv[1]);
                out_sizes[2] = int_size(argv[2]);
            }
            return;

        case 88: /* dropList (intLiteral, list) */
            if (argc >= 2) {
                out_sizes[0] = int_literal(argv[0]);
                out_sizes[1] = list_size(argv[1]);
            }
            return;

        case 90: /* listToArray (list) */
            if (argc >= 1) out_sizes[0] = list_size(argv[0]);
            return;

        default:
            /* Unknown / unimplemented builtin — sizes stay zero. The
             * dispatcher won't actually evaluate cost for these because
             * the impl pointer is NULL and it raises beforehand. */
            return;
    }
}
