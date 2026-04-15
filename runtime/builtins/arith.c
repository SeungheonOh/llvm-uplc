#include "runtime/builtins/helpers.h"

#include <stdint.h>

#include <gmp.h>

/*
 * Integer builtins. Port of TS cek/builtins/arithmetic.ts.
 *
 * Each op has two paths:
 *
 *   Fast path  — both operands are inline 64-bit ints. We do the
 *                arithmetic in native i64 with overflow / division-by-
 *                zero checks. On success we return an inline result; on
 *                overflow we fall through to the slow path.
 *
 *   Slow path  — either operand (or the result after overflow) needs
 *                arbitrary precision. Materialize both sides into
 *                arena-owned mpz handles and use GMP.
 *
 * `divideInteger` / `modInteger` use Euclidean (floor-toward-negative-
 * infinity) semantics; `quotientInteger` / `remainderInteger` use
 * truncated division (toward zero).
 */

/* ---------------------------------------------------------------------- */
/* Slow-path helpers                                                      */
/* ---------------------------------------------------------------------- */

static uplc_value slow_binop(uplc_budget* b,
                             uplc_int_view x, uplc_int_view y,
                             void (*op)(mpz_ptr, mpz_srcptr, mpz_srcptr)) {
    mpz_srcptr xm = uplcrt_materialize_int_view(b, x);
    mpz_srcptr ym = uplcrt_materialize_int_view(b, y);
    uplc_arena* ar = uplcrt_budget_arena(b);
    mpz_ptr r = uplc_arena_alloc_mpz(ar);
    op(r, xm, ym);
    return uplcrt_result_integer_mpz_or_inline(b, r);
}

static void check_divisor_i64(uplc_budget* b, int64_t y) {
    if (y == 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
}

static void check_divisor_mpz(uplc_budget* b, mpz_srcptr y) {
    if (mpz_sgn(y) == 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
}

/* ---------------------------------------------------------------------- */
/* Fast-path arithmetic                                                   */
/* ---------------------------------------------------------------------- */

uplc_value uplcrt_builtin_addInteger(uplc_budget* b, uplc_value* a) {
    uplc_int_view x = uplcrt_unwrap_integer_view(b, a[0]);
    uplc_int_view y = uplcrt_unwrap_integer_view(b, a[1]);
    if (x.is_inline && y.is_inline) {
        int64_t r;
        if (!__builtin_add_overflow(x.inline_val, y.inline_val, &r)) {
            return uplc_make_int_inline(r);
        }
    }
    return slow_binop(b, x, y, mpz_add);
}

uplc_value uplcrt_builtin_subtractInteger(uplc_budget* b, uplc_value* a) {
    uplc_int_view x = uplcrt_unwrap_integer_view(b, a[0]);
    uplc_int_view y = uplcrt_unwrap_integer_view(b, a[1]);
    if (x.is_inline && y.is_inline) {
        int64_t r;
        if (!__builtin_sub_overflow(x.inline_val, y.inline_val, &r)) {
            return uplc_make_int_inline(r);
        }
    }
    return slow_binop(b, x, y, mpz_sub);
}

uplc_value uplcrt_builtin_multiplyInteger(uplc_budget* b, uplc_value* a) {
    uplc_int_view x = uplcrt_unwrap_integer_view(b, a[0]);
    uplc_int_view y = uplcrt_unwrap_integer_view(b, a[1]);
    if (x.is_inline && y.is_inline) {
        int64_t r;
        if (!__builtin_mul_overflow(x.inline_val, y.inline_val, &r)) {
            return uplc_make_int_inline(r);
        }
    }
    return slow_binop(b, x, y, mpz_mul);
}

/* ---------------------------------------------------------------------- */
/* Division-family                                                        */
/* ---------------------------------------------------------------------- */

/*
 * Euclidean (floor-division) quotient. When both operands are inline
 * and division can be done without INT64_MIN / -1 overflow, we do it
 * natively; otherwise fall back to mpz_fdiv_q.
 */
uplc_value uplcrt_builtin_divideInteger(uplc_budget* b, uplc_value* a) {
    uplc_int_view x = uplcrt_unwrap_integer_view(b, a[0]);
    uplc_int_view y = uplcrt_unwrap_integer_view(b, a[1]);
    if (x.is_inline && y.is_inline) {
        check_divisor_i64(b, y.inline_val);
        /* INT64_MIN / -1 overflows; defer to mpz for that one case. */
        if (!(x.inline_val == INT64_MIN && y.inline_val == -1)) {
            int64_t q = x.inline_val / y.inline_val;
            int64_t r = x.inline_val % y.inline_val;
            /* Euclidean correction: if remainder has opposite sign to y,
             * q-- and r += y. */
            if (r != 0 && ((r < 0) != (y.inline_val < 0))) q -= 1;
            return uplc_make_int_inline(q);
        }
    }
    mpz_srcptr xm = uplcrt_materialize_int_view(b, x);
    mpz_srcptr ym = uplcrt_materialize_int_view(b, y);
    check_divisor_mpz(b, ym);
    uplc_arena* ar = uplcrt_budget_arena(b);
    mpz_ptr r = uplc_arena_alloc_mpz(ar);
    mpz_fdiv_q(r, xm, ym);
    return uplcrt_result_integer_mpz_or_inline(b, r);
}

uplc_value uplcrt_builtin_quotientInteger(uplc_budget* b, uplc_value* a) {
    uplc_int_view x = uplcrt_unwrap_integer_view(b, a[0]);
    uplc_int_view y = uplcrt_unwrap_integer_view(b, a[1]);
    if (x.is_inline && y.is_inline) {
        check_divisor_i64(b, y.inline_val);
        if (!(x.inline_val == INT64_MIN && y.inline_val == -1)) {
            return uplc_make_int_inline(x.inline_val / y.inline_val);
        }
    }
    mpz_srcptr xm = uplcrt_materialize_int_view(b, x);
    mpz_srcptr ym = uplcrt_materialize_int_view(b, y);
    check_divisor_mpz(b, ym);
    uplc_arena* ar = uplcrt_budget_arena(b);
    mpz_ptr r = uplc_arena_alloc_mpz(ar);
    mpz_tdiv_q(r, xm, ym);
    return uplcrt_result_integer_mpz_or_inline(b, r);
}

uplc_value uplcrt_builtin_remainderInteger(uplc_budget* b, uplc_value* a) {
    uplc_int_view x = uplcrt_unwrap_integer_view(b, a[0]);
    uplc_int_view y = uplcrt_unwrap_integer_view(b, a[1]);
    if (x.is_inline && y.is_inline) {
        check_divisor_i64(b, y.inline_val);
        if (!(x.inline_val == INT64_MIN && y.inline_val == -1)) {
            return uplc_make_int_inline(x.inline_val % y.inline_val);
        }
    }
    mpz_srcptr xm = uplcrt_materialize_int_view(b, x);
    mpz_srcptr ym = uplcrt_materialize_int_view(b, y);
    check_divisor_mpz(b, ym);
    uplc_arena* ar = uplcrt_budget_arena(b);
    mpz_ptr r = uplc_arena_alloc_mpz(ar);
    mpz_tdiv_r(r, xm, ym);
    return uplcrt_result_integer_mpz_or_inline(b, r);
}

uplc_value uplcrt_builtin_modInteger(uplc_budget* b, uplc_value* a) {
    uplc_int_view x = uplcrt_unwrap_integer_view(b, a[0]);
    uplc_int_view y = uplcrt_unwrap_integer_view(b, a[1]);
    if (x.is_inline && y.is_inline) {
        check_divisor_i64(b, y.inline_val);
        if (!(x.inline_val == INT64_MIN && y.inline_val == -1)) {
            int64_t r = x.inline_val % y.inline_val;
            if (r != 0 && ((r < 0) != (y.inline_val < 0))) r += y.inline_val;
            return uplc_make_int_inline(r);
        }
    }
    mpz_srcptr xm = uplcrt_materialize_int_view(b, x);
    mpz_srcptr ym = uplcrt_materialize_int_view(b, y);
    check_divisor_mpz(b, ym);
    uplc_arena* ar = uplcrt_budget_arena(b);
    mpz_ptr r = uplc_arena_alloc_mpz(ar);
    mpz_fdiv_r(r, xm, ym);
    return uplcrt_result_integer_mpz_or_inline(b, r);
}

/* ---------------------------------------------------------------------- */
/* Comparisons                                                            */
/* ---------------------------------------------------------------------- */

static int compare_ints(uplc_budget* b, uplc_value xv, uplc_value yv) {
    uplc_int_view x = uplcrt_unwrap_integer_view(b, xv);
    uplc_int_view y = uplcrt_unwrap_integer_view(b, yv);
    if (x.is_inline && y.is_inline) {
        return (x.inline_val < y.inline_val) ? -1
             : (x.inline_val > y.inline_val) ?  1 : 0;
    }
    mpz_srcptr xm = uplcrt_materialize_int_view(b, x);
    mpz_srcptr ym = uplcrt_materialize_int_view(b, y);
    return mpz_cmp(xm, ym);
}

uplc_value uplcrt_builtin_equalsInteger(uplc_budget* b, uplc_value* a) {
    return uplcrt_result_bool(b, compare_ints(b, a[0], a[1]) == 0);
}

uplc_value uplcrt_builtin_lessThanInteger(uplc_budget* b, uplc_value* a) {
    return uplcrt_result_bool(b, compare_ints(b, a[0], a[1]) <  0);
}

uplc_value uplcrt_builtin_lessThanEqualsInteger(uplc_budget* b, uplc_value* a) {
    return uplcrt_result_bool(b, compare_ints(b, a[0], a[1]) <= 0);
}
