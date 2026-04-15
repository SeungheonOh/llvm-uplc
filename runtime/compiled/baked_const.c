#include "runtime/compiled/baked_const.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include <gmp.h>

#include "runtime/arena.h"
#include "runtime/cek/rterm.h"
#include "runtime/errors.h"
#include "runtime/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

/*
 * Baked-constant blob decoder.
 *
 * Wire format (little-endian, no padding):
 *
 *   constant := u8:rconst_tag payload
 *
 *   payload by tag:
 *     INTEGER (0)   u8:sign u32:nbytes [nbytes BE magnitude]
 *     BYTESTRING(1) u32:len [len bytes]
 *     STRING (2)    u32:len [len utf8 bytes]
 *     BOOL (3)      u8:value
 *     UNIT (4)      (empty)
 *     DATA (5)      data
 *     BLS_G1 (6)    [48 bytes]
 *     BLS_G2 (7)    [96 bytes]
 *     BLS_ML (8)    u32:len [len bytes]
 *     VALUE (9)     u32:n_entries [entries]
 *                     entry = u32:cur_len [cur_len] u32:n_tokens [tokens]
 *                       token = u32:tok_len [tok_len] integer_payload
 *     LIST (10)     type u32:count [count constants]
 *     PAIR (11)     type type constant constant
 *     ARRAY (12)    type u32:count [count constants]
 *
 *   type := u8:rtype_tag [recursive type for LIST/PAIR/ARRAY]
 *
 *   data := u8:rdata_tag payload
 *     CONSTR (0)    integer_payload u32:n_fields [n_fields data]
 *     MAP (1)       u32:n_entries [n_entries data×2]
 *     LIST (2)      u32:n_values [n_values data]
 *     INTEGER (3)   integer_payload
 *     BYTESTRING (4) u32:len [len bytes]
 *
 *   integer_payload := u8:sign u32:nbytes [nbytes BE magnitude]
 */

/* ---------------------------------------------------------------------------
 * Cursor + helpers
 * ------------------------------------------------------------------------- */

typedef struct {
    const uint8_t* p;
    const uint8_t* end;
    uplc_budget*   b;
    uplc_arena*    a;
} bcd_t;

static void bcd_fail(bcd_t* c) {
    uplcrt_fail(c->b, UPLC_FAIL_MACHINE);
}

static uint8_t read_u8(bcd_t* c) {
    if (c->p + 1 > c->end) bcd_fail(c);
    return *c->p++;
}

static uint32_t read_u32(bcd_t* c) {
    if (c->p + 4 > c->end) bcd_fail(c);
    uint32_t v = (uint32_t)c->p[0]
               | ((uint32_t)c->p[1] << 8)
               | ((uint32_t)c->p[2] << 16)
               | ((uint32_t)c->p[3] << 24);
    c->p += 4;
    return v;
}

static const uint8_t* read_bytes(bcd_t* c, uint32_t n) {
    if (c->p + n > c->end) bcd_fail(c);
    const uint8_t* r = c->p;
    c->p += n;
    return r;
}

/* Decode (sign, nbytes, magnitude) into an arena-allocated mpz. */
static mpz_ptr read_integer(bcd_t* c) {
    uint8_t  sign   = read_u8(c);
    uint32_t nbytes = read_u32(c);
    mpz_ptr v = uplc_arena_alloc_mpz(c->a);
    if (nbytes == 0) {
        mpz_set_ui(v, 0);
    } else {
        const uint8_t* mag = read_bytes(c, nbytes);
        mpz_import(v, nbytes, 1, 1, 0, 0, mag);
        if (sign) mpz_neg(v, v);
    }
    return v;
}

/* ---------------------------------------------------------------------------
 * Type decoder
 * ------------------------------------------------------------------------- */

static uplc_rtype* read_type(bcd_t* c) {
    uint8_t tag = read_u8(c);
    switch ((uplc_rtype_tag)tag) {
        case UPLC_RTYPE_INTEGER:
        case UPLC_RTYPE_BYTESTRING:
        case UPLC_RTYPE_STRING:
        case UPLC_RTYPE_UNIT:
        case UPLC_RTYPE_BOOL:
        case UPLC_RTYPE_DATA:
        case UPLC_RTYPE_BLS12_381_G1:
        case UPLC_RTYPE_BLS12_381_G2:
        case UPLC_RTYPE_BLS12_381_ML_RESULT:
        case UPLC_RTYPE_VALUE:
            return uplc_rtype_simple(c->a, tag);
        case UPLC_RTYPE_LIST:
            return uplc_rtype_list(c->a, read_type(c));
        case UPLC_RTYPE_ARRAY:
            return uplc_rtype_array(c->a, read_type(c));
        case UPLC_RTYPE_PAIR: {
            uplc_rtype* fst = read_type(c);
            uplc_rtype* snd = read_type(c);
            return uplc_rtype_pair(c->a, fst, snd);
        }
    }
    bcd_fail(c);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * PlutusData decoder
 * ------------------------------------------------------------------------- */

static uplc_rdata* read_data(bcd_t* c) {
    uint8_t tag = read_u8(c);
    switch ((uplc_rdata_tag)tag) {
        case UPLC_RDATA_CONSTR: {
            mpz_ptr  idx = read_integer(c);
            uint32_t nf  = read_u32(c);
            uplc_rdata** fields = NULL;
            if (nf > 0) {
                fields = (uplc_rdata**)uplc_arena_alloc(
                    c->a, sizeof(uplc_rdata*) * nf, alignof(uplc_rdata*));
                for (uint32_t i = 0; i < nf; ++i) fields[i] = read_data(c);
            }
            return uplc_rdata_constr(c->a, idx, fields, nf);
        }
        case UPLC_RDATA_MAP: {
            uint32_t n = read_u32(c);
            uplc_rdata_pair* entries = NULL;
            if (n > 0) {
                entries = (uplc_rdata_pair*)uplc_arena_alloc(
                    c->a, sizeof(uplc_rdata_pair) * n, alignof(uplc_rdata_pair));
                for (uint32_t i = 0; i < n; ++i) {
                    entries[i].key   = read_data(c);
                    entries[i].value = read_data(c);
                }
            }
            return uplc_rdata_map(c->a, entries, n);
        }
        case UPLC_RDATA_LIST: {
            uint32_t n = read_u32(c);
            uplc_rdata** vs = NULL;
            if (n > 0) {
                vs = (uplc_rdata**)uplc_arena_alloc(
                    c->a, sizeof(uplc_rdata*) * n, alignof(uplc_rdata*));
                for (uint32_t i = 0; i < n; ++i) vs[i] = read_data(c);
            }
            return uplc_rdata_list(c->a, vs, n);
        }
        case UPLC_RDATA_INTEGER: {
            mpz_ptr v = read_integer(c);
            uplc_rdata* d = uplc_rdata_integer_mpz(c->a, v);
            return d;
        }
        case UPLC_RDATA_BYTESTRING: {
            uint32_t len = read_u32(c);
            const uint8_t* bytes = read_bytes(c, len);
            return uplc_rdata_bytestring(c->a, bytes, len);
        }
    }
    bcd_fail(c);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Constant decoder
 * ------------------------------------------------------------------------- */

static uplc_rconstant* read_constant(bcd_t* c) {
    uint8_t tag = read_u8(c);
    switch ((uplc_rconst_tag)tag) {
        case UPLC_RCONST_INTEGER: {
            mpz_ptr v = read_integer(c);
            return uplc_rconst_integer_mpz(c->a, v);
        }
        case UPLC_RCONST_BYTESTRING: {
            uint32_t len = read_u32(c);
            const uint8_t* bytes = read_bytes(c, len);
            return uplc_rconst_bytestring(c->a, bytes, len);
        }
        case UPLC_RCONST_STRING: {
            uint32_t len = read_u32(c);
            const uint8_t* bytes = read_bytes(c, len);
            return uplc_rconst_string(c->a, (const char*)bytes, len);
        }
        case UPLC_RCONST_BOOL:
            return uplc_rconst_bool(c->a, read_u8(c) != 0);
        case UPLC_RCONST_UNIT:
            return uplc_rconst_unit(c->a);
        case UPLC_RCONST_DATA:
            return uplc_rconst_data(c->a, read_data(c));
        case UPLC_RCONST_BLS12_381_G1: {
            const uint8_t* bytes = read_bytes(c, 48);
            return uplc_rconst_bls_g1(c->a, bytes);
        }
        case UPLC_RCONST_BLS12_381_G2: {
            const uint8_t* bytes = read_bytes(c, 96);
            return uplc_rconst_bls_g2(c->a, bytes);
        }
        case UPLC_RCONST_BLS12_381_ML_RESULT: {
            uint32_t len = read_u32(c);
            const uint8_t* bytes = read_bytes(c, len);
            return uplc_rconst_bls_ml_result(c->a, bytes, len);
        }
        case UPLC_RCONST_VALUE: {
            uint32_t n_entries = read_u32(c);
            uplc_rledger_value* lv = (uplc_rledger_value*)uplc_arena_alloc(
                c->a, sizeof(uplc_rledger_value), alignof(uplc_rledger_value));
            lv->n_entries = n_entries;
            lv->entries = NULL;
            if (n_entries > 0) {
                lv->entries = (uplc_rledger_entry*)uplc_arena_alloc(
                    c->a, sizeof(uplc_rledger_entry) * n_entries,
                    alignof(uplc_rledger_entry));
                for (uint32_t i = 0; i < n_entries; ++i) {
                    uint32_t cur_len = read_u32(c);
                    const uint8_t* cur = read_bytes(c, cur_len);
                    uint32_t n_tokens = read_u32(c);
                    uplc_rledger_token* toks = NULL;
                    if (n_tokens > 0) {
                        toks = (uplc_rledger_token*)uplc_arena_alloc(
                            c->a, sizeof(uplc_rledger_token) * n_tokens,
                            alignof(uplc_rledger_token));
                        for (uint32_t j = 0; j < n_tokens; ++j) {
                            uint32_t tn_len = read_u32(c);
                            const uint8_t* tn = read_bytes(c, tn_len);
                            mpz_ptr q = read_integer(c);
                            toks[j].name_bytes = tn;
                            toks[j].name_len   = tn_len;
                            toks[j].quantity   = q;
                        }
                    }
                    lv->entries[i].currency_bytes = cur;
                    lv->entries[i].currency_len   = cur_len;
                    lv->entries[i].tokens         = toks;
                    lv->entries[i].n_tokens       = n_tokens;
                }
            }
            return uplc_rconst_ledger_value(c->a, lv);
        }
        case UPLC_RCONST_LIST: {
            uplc_rtype* item_type = read_type(c);
            uint32_t n = read_u32(c);
            uplc_rconstant** values = NULL;
            if (n > 0) {
                values = (uplc_rconstant**)uplc_arena_alloc(
                    c->a, sizeof(uplc_rconstant*) * n, alignof(uplc_rconstant*));
                for (uint32_t i = 0; i < n; ++i) values[i] = read_constant(c);
            }
            uplc_rconstant* out = (uplc_rconstant*)uplc_arena_alloc(
                c->a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
            memset(out, 0, sizeof(*out));
            out->tag = UPLC_RCONST_LIST;
            out->list.item_type = item_type;
            out->list.values    = values;
            out->list.n_values  = n;
            return out;
        }
        case UPLC_RCONST_PAIR: {
            uplc_rtype* fst_type = read_type(c);
            uplc_rtype* snd_type = read_type(c);
            uplc_rconstant* fst  = read_constant(c);
            uplc_rconstant* snd  = read_constant(c);
            uplc_rconstant* out = (uplc_rconstant*)uplc_arena_alloc(
                c->a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
            memset(out, 0, sizeof(*out));
            out->tag = UPLC_RCONST_PAIR;
            out->pair.fst_type = fst_type;
            out->pair.snd_type = snd_type;
            out->pair.first    = fst;
            out->pair.second   = snd;
            return out;
        }
        case UPLC_RCONST_ARRAY: {
            uplc_rtype* item_type = read_type(c);
            uint32_t n = read_u32(c);
            uplc_rconstant** values = NULL;
            if (n > 0) {
                values = (uplc_rconstant**)uplc_arena_alloc(
                    c->a, sizeof(uplc_rconstant*) * n, alignof(uplc_rconstant*));
                for (uint32_t i = 0; i < n; ++i) values[i] = read_constant(c);
            }
            uplc_rconstant* out = (uplc_rconstant*)uplc_arena_alloc(
                c->a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
            memset(out, 0, sizeof(*out));
            out->tag = UPLC_RCONST_ARRAY;
            out->array.item_type = item_type;
            out->array.values    = values;
            out->array.n_values  = n;
            return out;
        }
    }
    bcd_fail(c);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Public entry
 * ------------------------------------------------------------------------- */

uplc_value uplcrt_const_baked(uplc_budget* b, const uint8_t* blob, uint32_t len) {
    uplc_arena* a = uplcrt_budget_arena(b);
    if (!a) uplcrt_fail(b, UPLC_FAIL_MACHINE);

    bcd_t c;
    c.p   = blob;
    c.end = blob + len;
    c.b   = b;
    c.a   = a;

    uplc_rconstant* k = read_constant(&c);
    return uplc_make_rcon(k);
}
