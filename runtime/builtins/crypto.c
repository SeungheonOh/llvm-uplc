#include "runtime/builtins/helpers.h"

#include <string.h>

/*
 * Cryptographic hash and signature-verification builtins.
 *
 * Hash functions:
 *   sha2_256    (18) — libsodium crypto_hash_sha256
 *   sha3_256    (19) — OpenSSL EVP "SHA3-256"
 *   blake2b_256 (20) — libsodium crypto_generichash_blake2b, outlen=32
 *   keccak_256  (71) — OpenSSL EVP "KECCAK-256"
 *   blake2b_224 (72) — libsodium crypto_generichash_blake2b, outlen=28
 *   ripemd_160  (86) — OpenSSL EVP "RIPEMD160"
 *
 * Signature verification:
 *   verifyEd25519Signature (21) — libsodium crypto_sign_verify_detached
 */

/* ---------------------------------------------------------------------------
 * libsodium headers
 * ------------------------------------------------------------------------- */
#include <sodium.h>

/* ---------------------------------------------------------------------------
 * OpenSSL EVP headers (only for sha3_256, keccak_256, ripemd_160)
 * ------------------------------------------------------------------------- */
#include <openssl/evp.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/*
 * Hash one bytestring using an OpenSSL EVP digest. Writes `out_len` bytes
 * into the arena and returns an uplc_value wrapping the result. Raises
 * UPLC_FAIL_MACHINE on an unexpected digest failure.
 */
static uplc_value evp_hash(uplc_budget* b, uplc_value input,
                           const EVP_MD* md, unsigned int expected_len) {
    uint32_t in_len = 0;
    const uint8_t* in = uplcrt_unwrap_bytestring(b, input, &in_len);

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, expected_len, 1);

    unsigned int out_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) uplcrt_fail(b, UPLC_FAIL_MACHINE);
    int ok = EVP_DigestInit_ex(ctx, md, NULL) &&
             EVP_DigestUpdate(ctx, in, in_len) &&
             EVP_DigestFinal_ex(ctx, out, &out_len);
    EVP_MD_CTX_free(ctx);
    if (!ok || out_len != expected_len) uplcrt_fail(b, UPLC_FAIL_MACHINE);

    return uplcrt_result_bytestring(b, out, expected_len);
}

/* ---------------------------------------------------------------------------
 * sha2_256
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_sha2_256(uplc_budget* b, uplc_value* a) {
    uint32_t in_len = 0;
    const uint8_t* in = uplcrt_unwrap_bytestring(b, a[0], &in_len);

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, crypto_hash_sha256_BYTES, 1);

    if (crypto_hash_sha256(out, in, in_len) != 0)
        uplcrt_fail(b, UPLC_FAIL_MACHINE);

    return uplcrt_result_bytestring(b, out, crypto_hash_sha256_BYTES);
}

/* ---------------------------------------------------------------------------
 * sha3_256
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_sha3_256(uplc_budget* b, uplc_value* a) {
    return evp_hash(b, a[0], EVP_sha3_256(), 32);
}

/* ---------------------------------------------------------------------------
 * blake2b_256
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_blake2b_256(uplc_budget* b, uplc_value* a) {
    uint32_t in_len = 0;
    const uint8_t* in = uplcrt_unwrap_bytestring(b, a[0], &in_len);

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, 32, 1);

    if (crypto_generichash_blake2b(out, 32, in, in_len, NULL, 0) != 0)
        uplcrt_fail(b, UPLC_FAIL_MACHINE);

    return uplcrt_result_bytestring(b, out, 32);
}

/* ---------------------------------------------------------------------------
 * blake2b_224
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_blake2b_224(uplc_budget* b, uplc_value* a) {
    uint32_t in_len = 0;
    const uint8_t* in = uplcrt_unwrap_bytestring(b, a[0], &in_len);

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint8_t* out = (uint8_t*)uplc_arena_alloc(ar, 28, 1);

    if (crypto_generichash_blake2b(out, 28, in, in_len, NULL, 0) != 0)
        uplcrt_fail(b, UPLC_FAIL_MACHINE);

    return uplcrt_result_bytestring(b, out, 28);
}

/* ---------------------------------------------------------------------------
 * keccak_256
 * OpenSSL 3.x exposes bare Keccak-256 (0x01 padding, not SHA3's 0x06) under
 * the name "KECCAK-256" via the default provider.
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_keccak_256(uplc_budget* b, uplc_value* a) {
    return evp_hash(b, a[0], EVP_MD_fetch(NULL, "KECCAK-256", NULL), 32);
}

/* ---------------------------------------------------------------------------
 * ripemd_160
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_ripemd_160(uplc_budget* b, uplc_value* a) {
    return evp_hash(b, a[0], EVP_ripemd160(), 20);
}

/* ---------------------------------------------------------------------------
 * verifyEd25519Signature
 *
 * Plutus spec:
 *   verifyEd25519Signature (vk : ByteString) (msg : ByteString) (sig : ByteString)
 *       -> Bool
 *
 * libsodium: crypto_sign_verify_detached(sig, msg, msg_len, vk)
 *   - vk  must be crypto_sign_PUBLICKEYBYTES (32)
 *   - sig must be crypto_sign_BYTES (64)
 * Any size mismatch is a type error (EvaluationFailure in Plutus).
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_verifyEd25519Signature(uplc_budget* b, uplc_value* a) {
    uint32_t vk_len = 0, msg_len = 0, sig_len = 0;
    const uint8_t* vk  = uplcrt_unwrap_bytestring(b, a[0], &vk_len);
    const uint8_t* msg = uplcrt_unwrap_bytestring(b, a[1], &msg_len);
    const uint8_t* sig = uplcrt_unwrap_bytestring(b, a[2], &sig_len);

    if (vk_len  != crypto_sign_PUBLICKEYBYTES ||
        sig_len != crypto_sign_BYTES) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }

    bool ok = (crypto_sign_verify_detached(sig, msg, msg_len, vk) == 0);
    return uplcrt_result_bool(b, ok);
}
