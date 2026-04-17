#ifndef UPLCRT_BUILTINS_HELPERS_H
#define UPLCRT_BUILTINS_HELPERS_H

#include <stdbool.h>
#include <stdint.h>

#include <gmp.h>

#include "runtime/core/arena.h"
#include "runtime/core/rterm.h"   /* shared uplc_rconstant layout */
#include "runtime/core/errors.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unwrap / wrap helpers used by every builtin implementation.
 *
 * All "unwrap_*" helpers raise UPLC_FAIL_EVALUATION on a type mismatch —
 * matching the TS reference's EvaluationError. All "make_*" helpers
 * allocate the payload out of the current evaluation arena (read from
 * `budget->arena`).
 */

/* ---- Introspection ---- */

static inline const uplc_rconstant* uplcrt_as_const(uplc_value v) {
    if (v.tag != UPLC_V_CON) return NULL;
    /* Inline values (int, and future bool/unit inline) have no backing
     * rconstant — payload holds an immediate, not a pointer. */
    if (v.subtag & UPLC_VCON_INLINE_BIT) return NULL;
    return (const uplc_rconstant*)uplc_value_payload(v);
}

/* ---- Unwrap ---- */

mpz_srcptr      uplcrt_unwrap_integer   (uplc_budget* b, uplc_value v);
const uint8_t*  uplcrt_unwrap_bytestring(uplc_budget* b, uplc_value v, uint32_t* out_len);
const char*     uplcrt_unwrap_string    (uplc_budget* b, uplc_value v, uint32_t* out_len);
bool            uplcrt_unwrap_bool      (uplc_budget* b, uplc_value v);
const uplc_rconstant* uplcrt_unwrap_list (uplc_budget* b, uplc_value v);
const uplc_rconstant* uplcrt_unwrap_pair (uplc_budget* b, uplc_value v);
const uplc_rdata*     uplcrt_unwrap_data (uplc_budget* b, uplc_value v);

/*
 * Discriminated integer view — lets arithmetic builtins take a fast
 * native path when both operands happen to be inline-int V_CONs,
 * falling back to GMP when either side is a boxed big integer.
 *
 * Callers pattern-match on `is_inline`:
 *
 *   if (x.is_inline && y.is_inline) {
 *       // native i64 arith with overflow check
 *   } else {
 *       // materialize and use x.mpz / y.mpz
 *   }
 *
 * For the slow path, materialize() produces an mpz pointer whose
 * lifetime is tied to the evaluation arena.
 */
typedef struct uplc_int_view {
    bool    is_inline;
    int64_t inline_val;  /* valid only when is_inline */
    mpz_srcptr mpz;      /* valid only when !is_inline */
} uplc_int_view;

uplc_int_view uplcrt_unwrap_integer_view(uplc_budget* b, uplc_value v);
mpz_srcptr    uplcrt_materialize_int_view(uplc_budget* b, uplc_int_view view);

/* ---- Wrap / result builders ---- */

uplc_value uplcrt_result_integer_mpz (uplc_budget* b, mpz_srcptr v);
uplc_value uplcrt_result_integer_si  (uplc_budget* b, int64_t v);
/* Pack a 64-bit int directly into a V_CON — no arena alloc, no mpz. */
uplc_value uplcrt_result_integer_i64 (uplc_budget* b, int64_t v);
/* If `v` fits in i64, inline it; otherwise box into an mpz. */
uplc_value uplcrt_result_integer_mpz_or_inline(uplc_budget* b, mpz_srcptr v);
uplc_value uplcrt_result_bytestring  (uplc_budget* b, const uint8_t* bytes, uint32_t len);
uplc_value uplcrt_result_string      (uplc_budget* b, const char* utf8, uint32_t len);
uplc_value uplcrt_result_bool        (uplc_budget* b, bool v);
uplc_value uplcrt_result_unit        (uplc_budget* b);
uplc_value uplcrt_result_data        (uplc_budget* b, const uplc_rdata* d);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_BUILTINS_HELPERS_H */
