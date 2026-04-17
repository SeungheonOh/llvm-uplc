#ifndef UPLCRT_RTERM_H
#define UPLCRT_RTERM_H

#include <stdbool.h>
#include <stdint.h>

#include <gmp.h>

#include "runtime/core/arena.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/term.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CEK-interpreter term AST. This type is only walked by the direct
 * interpreter in runtime/cek/cek.c — compiled-code output (M7+) emits LLVM
 * IR from the compiler's C++ AST and never constructs uplc_rterm.
 *
 * Every node and array lives in a uplc_arena. Caller keeps the arena alive
 * for as long as the tree is used, then destroys it to release everything
 * at once (including registered GMP bigints).
 */

typedef struct uplc_rterm      uplc_rterm;
typedef struct uplc_rconstant  uplc_rconstant;
typedef struct uplc_rtype      uplc_rtype;
typedef struct uplc_rdata      uplc_rdata;

/* --- ConstantType (recursive) ----------------------------------------- */
typedef enum uplc_rtype_tag {
    UPLC_RTYPE_INTEGER              = 0,
    UPLC_RTYPE_BYTESTRING           = 1,
    UPLC_RTYPE_STRING               = 2,
    UPLC_RTYPE_UNIT                 = 3,
    UPLC_RTYPE_BOOL                 = 4,
    UPLC_RTYPE_DATA                 = 5,
    UPLC_RTYPE_BLS12_381_G1         = 6,
    UPLC_RTYPE_BLS12_381_G2         = 7,
    UPLC_RTYPE_BLS12_381_ML_RESULT  = 8,
    UPLC_RTYPE_VALUE                = 9,
    UPLC_RTYPE_LIST                 = 10,
    UPLC_RTYPE_PAIR                 = 11,
    UPLC_RTYPE_ARRAY                = 12,
} uplc_rtype_tag;

struct uplc_rtype {
    uint8_t tag;   /* uplc_rtype_tag */
    uint8_t _pad[7];
    union {
        struct { uplc_rtype* element; } list;
        struct { uplc_rtype* element; } array;
        struct { uplc_rtype* first; uplc_rtype* second; } pair;
    };
};

/* --- PlutusData ------------------------------------------------------- */
typedef enum uplc_rdata_tag {
    UPLC_RDATA_CONSTR      = 0,
    UPLC_RDATA_MAP         = 1,
    UPLC_RDATA_LIST        = 2,
    UPLC_RDATA_INTEGER     = 3,
    UPLC_RDATA_BYTESTRING  = 4,
} uplc_rdata_tag;

typedef struct uplc_rdata_pair {
    uplc_rdata* key;
    uplc_rdata* value;
} uplc_rdata_pair;

struct uplc_rdata {
    uint8_t tag;   /* uplc_rdata_tag */
    uint8_t _pad[7];
    union {
        struct {
            mpz_ptr      index;
            uplc_rdata** fields;
            uint32_t     n_fields;
        } constr;
        struct {
            uplc_rdata_pair* entries;
            uint32_t         n_entries;
        } map;
        struct {
            uplc_rdata** values;
            uint32_t     n_values;
        } list;
        struct { mpz_ptr value; } integer;
        struct {
            const uint8_t* bytes;
            uint32_t       len;
        } bytestring;
    };
};

/* --- Constant --------------------------------------------------------- */
typedef enum uplc_rconst_tag {
    UPLC_RCONST_INTEGER              = 0,
    UPLC_RCONST_BYTESTRING           = 1,
    UPLC_RCONST_STRING               = 2,
    UPLC_RCONST_BOOL                 = 3,
    UPLC_RCONST_UNIT                 = 4,
    UPLC_RCONST_DATA                 = 5,
    UPLC_RCONST_BLS12_381_G1         = 6,
    UPLC_RCONST_BLS12_381_G2         = 7,
    UPLC_RCONST_BLS12_381_ML_RESULT  = 8,
    UPLC_RCONST_VALUE                = 9,
    UPLC_RCONST_LIST                 = 10,
    UPLC_RCONST_PAIR                 = 11,
    UPLC_RCONST_ARRAY                = 12,
} uplc_rconst_tag;

/* --- LedgerValue --------------------------------------------------------- */

/* A single token within a policy (currency symbol). */
typedef struct uplc_rledger_token {
    const uint8_t* name_bytes;   /* TokenName as raw bytes */
    uint32_t       name_len;
    mpz_ptr        quantity;     /* Amount; may be negative */
} uplc_rledger_token;

/* One entry in the outer map: a policy → sorted token map. */
typedef struct uplc_rledger_entry {
    const uint8_t*     currency_bytes;  /* PolicyId / CurrencySymbol */
    uint32_t           currency_len;
    uplc_rledger_token* tokens;
    uint32_t           n_tokens;
} uplc_rledger_entry;

/* The full Value type: a sorted map from CurrencySymbol to token map.
 * Zero-quantity tokens are excluded. The entries and tokens arrays are
 * sorted lexicographically by key for canonical form. */
typedef struct uplc_rledger_value {
    uplc_rledger_entry* entries;
    uint32_t            n_entries;
} uplc_rledger_value;

/* --- Constant --------------------------------------------------------- */

struct uplc_rconstant {
    uint8_t tag;  /* uplc_rconst_tag */
    uint8_t _pad[7];
    union {
        struct { mpz_ptr value; } integer;
        struct { const uint8_t* bytes; uint32_t len; } bytestring;
        struct { const char* utf8; uint32_t len; } string;
        struct { bool value; } boolean;
        struct { uplc_rdata* value; } data;
        struct { const uint8_t* bytes; } bls_g1;          /* 48 bytes compressed */
        struct { const uint8_t* bytes; } bls_g2;          /* 96 bytes compressed */
        struct { const uint8_t* bytes; uint32_t len; } bls_ml_result; /* blst_fp12, 576 bytes */
        struct { uplc_rledger_value* value; } ledger_value;
        struct {
            uplc_rtype*       item_type;
            uplc_rconstant**  values;
            uint32_t          n_values;
        } list;
        struct {
            uplc_rtype*       fst_type;
            uplc_rtype*       snd_type;
            uplc_rconstant*   first;
            uplc_rconstant*   second;
        } pair;
        struct {
            uplc_rtype*       item_type;
            uplc_rconstant**  values;
            uint32_t          n_values;
        } array;
    };
};

/* --- Term ------------------------------------------------------------- */
typedef enum uplc_rterm_tag {
    UPLC_RTERM_VAR      = 0,
    UPLC_RTERM_DELAY    = 1,
    UPLC_RTERM_LAMBDA   = 2,
    UPLC_RTERM_APPLY    = 3,
    UPLC_RTERM_CONSTANT = 4,
    UPLC_RTERM_FORCE    = 5,
    UPLC_RTERM_ERROR    = 6,
    UPLC_RTERM_BUILTIN  = 7,
    UPLC_RTERM_CONSTR   = 8,
    UPLC_RTERM_CASE     = 9,
} uplc_rterm_tag;

struct uplc_rterm {
    uint8_t tag;   /* uplc_rterm_tag */
    uint8_t _pad[7];
    union {
        struct { uint32_t index; } var;            /* de Bruijn index */
        struct { uplc_rterm* body; } lambda;
        struct { uplc_rterm* fn; uplc_rterm* arg; } apply;
        struct { uplc_rconstant* value; } constant;
        struct { uint8_t tag; } builtin;           /* BuiltinTag */
        struct { uplc_rterm* term; } delay;
        struct { uplc_rterm* term; } force;
        struct {
            uint64_t     tag_index;
            uplc_rterm** fields;
            uint32_t     n_fields;
        } constr;
        struct {
            uplc_rterm*  scrutinee;
            uplc_rterm** branches;
            uint32_t     n_branches;
        } case_;
    };
};

typedef struct uplc_rversion {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} uplc_rversion;

typedef struct uplc_rprogram {
    uplc_rversion version;
    uplc_rterm*   term;
} uplc_rprogram;

/* --- Constructors ----------------------------------------------------- */
uplc_rterm*     uplc_rterm_var(uplc_arena* a, uint32_t index);
uplc_rterm*     uplc_rterm_lambda(uplc_arena* a, uplc_rterm* body);
uplc_rterm*     uplc_rterm_apply(uplc_arena* a, uplc_rterm* fn, uplc_rterm* arg);
uplc_rterm*     uplc_rterm_delay(uplc_arena* a, uplc_rterm* inner);
uplc_rterm*     uplc_rterm_force(uplc_arena* a, uplc_rterm* inner);
uplc_rterm*     uplc_rterm_error(uplc_arena* a);
uplc_rterm*     uplc_rterm_builtin(uplc_arena* a, uint8_t tag);
uplc_rterm*     uplc_rterm_constant(uplc_arena* a, uplc_rconstant* c);
uplc_rterm*     uplc_rterm_constr(uplc_arena* a, uint64_t tag,
                                  uplc_rterm** fields, uint32_t n);
uplc_rterm*     uplc_rterm_case(uplc_arena* a, uplc_rterm* scrutinee,
                                uplc_rterm** branches, uint32_t n);

uplc_rconstant* uplc_rconst_integer_si(uplc_arena* a, long v);
uplc_rconstant* uplc_rconst_integer_mpz(uplc_arena* a, mpz_srcptr v);
uplc_rconstant* uplc_rconst_bytestring(uplc_arena* a, const uint8_t* b, uint32_t n);
uplc_rconstant* uplc_rconst_string(uplc_arena* a, const char* s, uint32_t n);
uplc_rconstant* uplc_rconst_bool(uplc_arena* a, bool v);
uplc_rconstant* uplc_rconst_unit(uplc_arena* a);
uplc_rconstant* uplc_rconst_data(uplc_arena* a, uplc_rdata* v);
/* BLS12-381 point / pairing constants (bytes pre-compressed by caller). */
uplc_rconstant* uplc_rconst_bls_g1(uplc_arena* a, const uint8_t bytes[48]);
uplc_rconstant* uplc_rconst_bls_g2(uplc_arena* a, const uint8_t bytes[96]);
uplc_rconstant* uplc_rconst_bls_ml_result(uplc_arena* a, const uint8_t* bytes, uint32_t len);
/* Ledger Value constant. Caller is responsible for canonical form. */
uplc_rconstant* uplc_rconst_ledger_value(uplc_arena* a, uplc_rledger_value* v);

uplc_rtype* uplc_rtype_simple(uplc_arena* a, uint8_t tag);
uplc_rtype* uplc_rtype_list(uplc_arena* a, uplc_rtype* element);
uplc_rtype* uplc_rtype_pair(uplc_arena* a, uplc_rtype* first, uplc_rtype* second);
uplc_rtype* uplc_rtype_array(uplc_arena* a, uplc_rtype* element);

uplc_rdata* uplc_rdata_integer_mpz(uplc_arena* a, mpz_srcptr v);
uplc_rdata* uplc_rdata_bytestring(uplc_arena* a, const uint8_t* b, uint32_t n);
uplc_rdata* uplc_rdata_list(uplc_arena* a, uplc_rdata** values, uint32_t n);
uplc_rdata* uplc_rdata_map(uplc_arena* a, uplc_rdata_pair* entries, uint32_t n);
uplc_rdata* uplc_rdata_constr(uplc_arena* a, mpz_srcptr index,
                              uplc_rdata** fields, uint32_t n);

/* --- uplc_value helpers tying VCon to uplc_rconstant ------------------ */
/* These live here instead of in runtime/value.h because they are specific
 * to the CEK interpreter's constant representation. The compiled-code path
 * will use its own baked constant layout and skip these wrappers. */

static inline uplc_value uplc_make_rcon(uplc_rconstant* c) {
    return uplc_make_con_raw(c, c ? c->tag : 0);
}

/*
 * Unwrap a V_CON into the underlying rconstant pointer. ONLY safe when
 * the caller has verified the value is NOT an inline integer (check
 * `!uplc_value_is_int_inline(v)` first). Returns garbage for inline
 * ints because the payload is an immediate, not a pointer.
 *
 * Callers that need to handle both representations should use
 * `uplc_rcon_of_materialize` instead.
 */
static inline uplc_rconstant* uplc_rcon_of(uplc_value v) {
    return (uplc_rconstant*)uplc_value_payload(v);
}

/*
 * Materializing variant: for inline integers, allocates a fresh
 * arena-backed rconstant and returns it. For any other V_CON, returns
 * the existing payload pointer. Returns NULL for non-VCon values.
 */
static inline uplc_rconstant* uplc_rcon_of_materialize(uplc_arena* a,
                                                        uplc_value v) {
    if (v.tag != UPLC_V_CON) return NULL;
    if (uplc_value_is_int_inline(v)) {
        return uplc_rconst_integer_si(a, (long)uplc_value_int_inline(v));
    }
    return (uplc_rconstant*)uplc_value_payload(v);
}

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_RTERM_H */
