#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include <gmp.h>

#include "runtime/core/arena.h"
#include "runtime/core/errors.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

/*
 * Compiled-mode constant constructors.
 *
 * For M2.5 we reuse the CEK interpreter's `uplc_rconstant` layout (defined
 * in runtime/cek/rterm.h) so a compiled-path VCon value looks identical to
 * an interpreter-path one when observed by a shared builtin dispatcher. To
 * avoid dragging rterm.h into this translation unit we define a
 * byte-compatible "opaque constant" struct with only the fields we need,
 * and rely on the layout matching. In M3 when builtins move to a shared
 * directory we'll collapse both into a single header.
 *
 * This is a conscious duplication, clearly documented. The struct fields
 * are identical in name, order, and size to the first-level union arms of
 * `uplc_rconstant` so a VCon built here can be consumed by any code that
 * pattern-matches on that type.
 */

typedef struct uplcrt_const_layout {
    uint8_t tag;   /* uplc_rconst_tag */
    uint8_t _pad[7];
    union {
        struct { mpz_ptr value; } integer;
        struct { const uint8_t* bytes; uint32_t len; } bytestring;
        struct { const char* utf8; uint32_t len; } string;
        struct { int value; } boolean;
        /* unit carries no payload */
        struct { const void* value; } data;
        struct { const uint8_t* bytes; } bls_g1;
        struct { const uint8_t* bytes; } bls_g2;
        struct { const uint8_t* bytes; uint32_t len; } bls_ml_result;
    };
} uplcrt_const_layout;

/* Tag values are shared across modes; these mirror uplc_rconst_tag from
 * runtime/cek/rterm.h. */
#define UPLC_CONST_INTEGER             0
#define UPLC_CONST_BYTESTRING          1
#define UPLC_CONST_STRING              2
#define UPLC_CONST_BOOL                3
#define UPLC_CONST_UNIT                4
#define UPLC_CONST_DATA                5
#define UPLC_CONST_BLS12_381_G1        6
#define UPLC_CONST_BLS12_381_G2        7
#define UPLC_CONST_BLS12_381_ML_RESULT 8

static uplcrt_const_layout* alloc_const(uplc_budget* b, uint8_t tag) {
    uplc_arena* a = uplcrt_budget_arena(b);
    if (!a) uplcrt_fail(b, UPLC_FAIL_MACHINE);
    uplcrt_const_layout* c = (uplcrt_const_layout*)uplc_arena_alloc(
        a, sizeof(uplcrt_const_layout), alignof(uplcrt_const_layout));
    memset(c, 0, sizeof(*c));
    c->tag = tag;
    return c;
}

uplc_value uplcrt_const_int_bytes(uplc_budget* b, int negative,
                                   const uint8_t* magnitude, uint32_t nbytes) {
    /* Import a big-endian byte array (architecture-independent). Used by
     * codegen to materialise baked integer constants. */
    uplcrt_const_layout* c = alloc_const(b, UPLC_CONST_INTEGER);
    mpz_ptr v = uplc_arena_alloc_mpz(uplcrt_budget_arena(b));
    if (nbytes == 0 || !magnitude) {
        mpz_set_si(v, 0);
    } else {
        mpz_import(v, nbytes, 1 /*MSB-first*/, 1 /*byte*/, 0, 0, magnitude);
        if (negative && mpz_sgn(v) != 0) mpz_neg(v, v);
    }
    c->integer.value = v;
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_const_int_si(uplc_budget* b, int64_t value) {
    /* Inline fast path — no arena alloc, no mpz. */
    (void)b;
    return uplc_make_int_inline(value);
}

uplc_value uplcrt_const_int_ref(uplc_budget* b, const void* limbs,
                                int32_t sign, uint32_t nlimbs) {
    /* Import a baked big-endian limb array from .rodata. The rodata layout
     * is the raw bytes of the magnitude in big-endian order, plus a sign
     * flag. */
    uplcrt_const_layout* c = alloc_const(b, UPLC_CONST_INTEGER);
    mpz_ptr v = uplc_arena_alloc_mpz(uplcrt_budget_arena(b));
    if (nlimbs == 0 || !limbs) {
        mpz_set_si(v, 0);
    } else {
        mpz_import(v, nlimbs, 1 /*order: MSB first*/, sizeof(mp_limb_t),
                   0 /*native endian within a limb*/, 0 /*nails*/, limbs);
        if (sign < 0) mpz_neg(v, v);
    }
    c->integer.value = v;
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_const_bs_ref(uplc_budget* b, const uint8_t* bytes, uint32_t len) {
    uplcrt_const_layout* c = alloc_const(b, UPLC_CONST_BYTESTRING);
    c->bytestring.bytes = bytes;   /* points into caller-owned rodata */
    c->bytestring.len = len;
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_const_string_ref(uplc_budget* b, const char* utf8, uint32_t byte_len) {
    uplcrt_const_layout* c = alloc_const(b, UPLC_CONST_STRING);
    c->string.utf8 = utf8;
    c->string.len = byte_len;
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_const_bool(uplc_budget* b, int value) {
    uplcrt_const_layout* c = alloc_const(b, UPLC_CONST_BOOL);
    c->boolean.value = value ? 1 : 0;
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_const_unit(uplc_budget* b) {
    uplcrt_const_layout* c = alloc_const(b, UPLC_CONST_UNIT);
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_const_data_ref(uplc_budget* b, const void* baked) {
    /* M2.5 stub: store the rodata pointer verbatim. The baked layout is
     * chosen in M3 when the cost model + PlutusData bake format lands. */
    uplcrt_const_layout* c = alloc_const(b, UPLC_CONST_DATA);
    c->data.value = baked;
    return uplc_make_con_raw(c, c->tag);
}
