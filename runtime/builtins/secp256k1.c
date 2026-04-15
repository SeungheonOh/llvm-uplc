#include "runtime/builtins/helpers.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * secp256k1 signature-verification builtins.
 *
 *   verifyEcdsaSecp256k1Signature   (52)
 *     vk  : 33-byte compressed SEC1 public key
 *     msg : 32-byte hash (pre-hashed by the caller)
 *     sig : 64-byte compact (r,s) ECDSA signature
 *     → Bool
 *
 *   verifySchnorrSecp256k1Signature (53)
 *     vk  : 32-byte x-only BIP-340 public key
 *     msg : variable-length message (not pre-hashed)
 *     sig : 64-byte BIP-340 Schnorr signature
 *     → Bool
 *
 * Size mismatch (wrong key / sig length) raises EvaluationFailure per
 * the Plutus spec — it does NOT return False.
 */

#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>

/* Lazily-created, permanently-cached verify context. Thread-safety is
 * not a concern in single-threaded CEK / JIT evaluation. */
static secp256k1_context* g_secp_ctx = NULL;

static secp256k1_context* get_ctx(void) {
    if (!g_secp_ctx) {
        g_secp_ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    }
    return g_secp_ctx;
}

/* ---------------------------------------------------------------------------
 * verifyEcdsaSecp256k1Signature
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_verifyEcdsaSecp256k1Signature(uplc_budget* b, uplc_value* a) {
    uint32_t vk_len = 0, msg_len = 0, sig_len = 0;
    const uint8_t* vk  = uplcrt_unwrap_bytestring(b, a[0], &vk_len);
    const uint8_t* msg = uplcrt_unwrap_bytestring(b, a[1], &msg_len);
    const uint8_t* sig = uplcrt_unwrap_bytestring(b, a[2], &sig_len);

    /* Exact sizes required by Plutus spec; wrong sizes → EvaluationFailure. */
    if (vk_len != 33 || msg_len != 32 || sig_len != 64) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }

    secp256k1_context*        ctx = get_ctx();
    secp256k1_pubkey          pubkey;
    secp256k1_ecdsa_signature ecdsa_sig;

    /* Plutus semantics: a malformed pubkey is a hard failure. */
    if (!secp256k1_ec_pubkey_parse(ctx, &pubkey, vk, 33)) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }

    /* Compact 64-byte signature. libsecp256k1 rejects signatures whose r
     * or s are >= the group order; Plutus treats that as a hard failure
     * (see BIP340 vectors 17/18). */
    if (!secp256k1_ecdsa_signature_parse_compact(ctx, &ecdsa_sig, sig)) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }

    /* Plutus only accepts low-s form signatures. secp256k1_ecdsa_verify
     * internally requires low-s, so we do NOT normalize. */
    bool ok = secp256k1_ecdsa_verify(ctx, &ecdsa_sig, msg, &pubkey) == 1;
    return uplcrt_result_bool(b, ok);
}

/* ---------------------------------------------------------------------------
 * verifySchnorrSecp256k1Signature
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_verifySchnorrSecp256k1Signature(uplc_budget* b, uplc_value* a) {
    uint32_t vk_len = 0, msg_len = 0, sig_len = 0;
    const uint8_t* vk  = uplcrt_unwrap_bytestring(b, a[0], &vk_len);
    const uint8_t* msg = uplcrt_unwrap_bytestring(b, a[1], &msg_len);
    const uint8_t* sig = uplcrt_unwrap_bytestring(b, a[2], &sig_len);

    /* vk must be 32 bytes (x-only), sig must be 64 bytes. */
    if (vk_len != 32 || sig_len != 64) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }

    secp256k1_context*    ctx = get_ctx();
    secp256k1_xonly_pubkey xonly_pk;

    /* Plutus semantics: a malformed x-only pubkey is a hard failure, not
     * a False result. See BIP-340 test vectors 5 (pubkey not on curve) and
     * 14 (pubkey x-coordinate exceeds field size). */
    if (!secp256k1_xonly_pubkey_parse(ctx, &xonly_pk, vk)) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }

    bool ok = secp256k1_schnorrsig_verify(ctx, sig, msg, (size_t)msg_len, &xonly_pk) == 1;
    return uplcrt_result_bool(b, ok);
}
