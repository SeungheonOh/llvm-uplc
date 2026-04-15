#include "uplc/costmodel.h"

#include <stdbool.h>
#include <stdint.h>

#include "uplc/budget.h"

/*
 * Cost-model evaluator — port of TS cek/costing.ts.
 *
 * All arithmetic is saturating at INT64_MAX / INT64_MIN. The TS reference
 * only clamps the *upper* bound (to I64_MAX) because a well-formed Conway
 * cost parameter set never produces negative intermediate values; we go
 * further and clamp both ends to keep pathological models well-defined.
 *
 * Parameter indexing per shape is documented in include/uplc/costmodel.h.
 * Anything unimplemented (the two-variable quadratic shapes used by BLS
 * pairing cost entries) returns INT64_MAX so a program that somehow hits
 * one gets an immediate OutOfBudget rather than silent miscosting.
 */

static inline int64_t sat_add(int64_t a, int64_t b) {
    return uplcrt_sat_add_i64(a, b);
}
static inline int64_t sat_mul(int64_t a, int64_t b) {
    return uplcrt_sat_mul_i64(a, b);
}
static inline int64_t imax(int64_t a, int64_t b) { return a > b ? a : b; }
static inline int64_t imin(int64_t a, int64_t b) { return a < b ? a : b; }

/* linear(intercept, slope, x) = intercept + slope * x */
static int64_t linear_cost(int64_t intercept, int64_t slope, int64_t x) {
    return sat_add(intercept, sat_mul(slope, x));
}

/* quadratic(c0, c1, c2, x) = c0 + c1*x + c2*x^2 */
static int64_t quadratic_cost(int64_t c0, int64_t c1, int64_t c2, int64_t x) {
    int64_t xx = sat_mul(x, x);
    return sat_add(sat_add(c0, sat_mul(c1, x)), sat_mul(c2, xx));
}

int64_t uplcrt_costfn_eval(const uplc_costfn* fn,
                           const int64_t*     sizes,
                           uint8_t            arity) {
    (void)arity;
    const int64_t x = sizes[0];
    const int64_t y = sizes[1];
    const int64_t z = sizes[2];

    switch ((uplc_costshape)fn->shape) {
        case UPLC_COSTSHAPE_CONSTANT:
            return fn->c[0];

        case UPLC_COSTSHAPE_LINEAR_IN_X:
            return linear_cost(fn->c[0], fn->c[1], x);
        case UPLC_COSTSHAPE_LINEAR_IN_Y:
            return linear_cost(fn->c[0], fn->c[1], y);
        case UPLC_COSTSHAPE_LINEAR_IN_Z:
            return linear_cost(fn->c[0], fn->c[1], z);

        case UPLC_COSTSHAPE_ADDED_SIZES:
            return linear_cost(fn->c[0], fn->c[1], sat_add(x, y));
        case UPLC_COSTSHAPE_MULTIPLIED_SIZES:
            return linear_cost(fn->c[0], fn->c[1], sat_mul(x, y));
        case UPLC_COSTSHAPE_MIN_SIZE:
            return linear_cost(fn->c[0], fn->c[1], imin(x, y));
        case UPLC_COSTSHAPE_MAX_SIZE:
            return linear_cost(fn->c[0], fn->c[1], imax(x, y));
        case UPLC_COSTSHAPE_SUBTRACTED_SIZES: {
            int64_t raw = sat_add(fn->c[0], sat_mul(fn->c[1], sat_add(x, -y)));
            return imax(fn->c[2], raw);
        }
        case UPLC_COSTSHAPE_LINEAR_ON_DIAGONAL:
            return (x == y)
                       ? sat_add(fn->c[0], sat_mul(fn->c[1], x))
                       : fn->c[2];

        case UPLC_COSTSHAPE_QUADRATIC_IN_X:
            return quadratic_cost(fn->c[0], fn->c[1], fn->c[2], x);
        case UPLC_COSTSHAPE_QUADRATIC_IN_Y:
            return quadratic_cost(fn->c[0], fn->c[1], fn->c[2], y);
        case UPLC_COSTSHAPE_QUADRATIC_IN_Z:
            return quadratic_cost(fn->c[0], fn->c[1], fn->c[2], z);

        case UPLC_COSTSHAPE_WITH_INTERACTION: {
            /* c00 + c10*x + c01*y + c11*x*y */
            int64_t t = sat_add(fn->c[0], sat_mul(fn->c[1], x));
            t = sat_add(t, sat_mul(fn->c[2], y));
            t = sat_add(t, sat_mul(fn->c[3], sat_mul(x, y)));
            return t;
        }

        case UPLC_COSTSHAPE_LINEAR_IN_Y_AND_Z: {
            int64_t t = sat_add(fn->c[0], sat_mul(fn->c[1], y));
            t = sat_add(t, sat_mul(fn->c[2], z));
            return t;
        }
        case UPLC_COSTSHAPE_LINEAR_IN_MAX_YZ:
            return linear_cost(fn->c[0], fn->c[1], imax(y, z));
        case UPLC_COSTSHAPE_LITERAL_IN_Y_OR_LINEAR_IN_Z:
            return imax(y, linear_cost(fn->c[0], fn->c[1], z));
        case UPLC_COSTSHAPE_EXP_MOD: {
            /* base = c00 + c11*y*z + c12*y*z^2  ; if x > z scale by 1.5 */
            int64_t yz = sat_mul(y, z);
            int64_t base = sat_add(
                fn->c[0],
                sat_add(sat_mul(fn->c[1], yz), sat_mul(fn->c[2], sat_mul(yz, z))));
            return (x > z) ? sat_add(base, base / 2) : base;
        }

        case UPLC_COSTSHAPE_CONST_ABOVE_DIAG:
        case UPLC_COSTSHAPE_CONST_BELOW_DIAG: {
            /* c[0] = constant returned above/below the diagonal,
             * c[1] = minimum clamp, c[2..7] = c00 c10 c01 c20 c11 c02. */
            bool use_const = (fn->shape == UPLC_COSTSHAPE_CONST_ABOVE_DIAG)
                                 ? (x < y)
                                 : (x > y);
            if (use_const) return fn->c[0];
            int64_t raw = sat_add(fn->c[2],             sat_mul(fn->c[3], x));
            raw         = sat_add(raw,                   sat_mul(fn->c[4], y));
            raw         = sat_add(raw,                   sat_mul(fn->c[5], sat_mul(x, x)));
            raw         = sat_add(raw,                   sat_mul(fn->c[6], sat_mul(x, y)));
            raw         = sat_add(raw,                   sat_mul(fn->c[7], sat_mul(y, y)));
            return imax(fn->c[1], raw);
        }

        case UPLC_COSTSHAPE__COUNT:
            break;
    }
    return INT64_MAX;
}
