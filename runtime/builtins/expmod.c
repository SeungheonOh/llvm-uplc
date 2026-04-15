#include "runtime/builtins/helpers.h"

#include <gmp.h>

/*
 * expModInteger(base, exp, mod) — port of TS arithmetic.ts expModInteger.
 *
 * Rules:
 *   mod <= 0                 → EvaluationFailure
 *   mod == 1                 → 0
 *   exp == 0                 → 1
 *   exp > 0                  → base ^ exp mod mod
 *   exp < 0 && base == 0     → EvaluationFailure
 *   exp < 0                  → require gcd(base, mod) == 1, return inv(base)^(-exp) mod mod
 *
 * GMP's mpz_powm does exactly the positive-exponent case. For negative
 * exponents we flip via mpz_invert (fails if gcd != 1).
 */
uplc_value uplcrt_builtin_expModInteger(uplc_budget* b, uplc_value* a) {
    mpz_srcptr base = uplcrt_unwrap_integer(b, a[0]);
    mpz_srcptr exp  = uplcrt_unwrap_integer(b, a[1]);
    mpz_srcptr mod  = uplcrt_unwrap_integer(b, a[2]);

    if (mpz_sgn(mod) <= 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    if (mpz_cmp_ui(mod, 1) == 0) return uplcrt_result_integer_si(b, 0);
    if (mpz_sgn(exp) == 0)       return uplcrt_result_integer_si(b, 1);

    uplc_arena* ar = uplcrt_budget_arena(b);
    mpz_ptr result = uplc_arena_alloc_mpz(ar);

    if (mpz_sgn(exp) > 0) {
        mpz_powm(result, base, exp, mod);
        return uplcrt_result_integer_mpz(b, result);
    }

    /* exp < 0 */
    if (mpz_sgn(base) == 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);

    mpz_ptr inv = uplc_arena_alloc_mpz(ar);
    if (mpz_invert(inv, base, mod) == 0) {
        /* base has no inverse mod mod ⇒ not coprime */
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    mpz_ptr pos_exp = uplc_arena_alloc_mpz(ar);
    mpz_neg(pos_exp, exp);
    mpz_powm(result, inv, pos_exp, mod);
    return uplcrt_result_integer_mpz(b, result);
}
