#ifndef UPLC_COSTMODEL_H
#define UPLC_COSTMODEL_H

#include <stdint.h>
#include "uplc/budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Cost-model shapes. The full set from TS cek/costing.ts.
 *
 * Parameter layout (all parameters are int64, unused slots are zero):
 *   CONSTANT                    c[0] = value
 *   LINEAR_IN_X / _Y / _Z       c[0] = intercept, c[1] = slope
 *   ADDED_SIZES                 c[0] = intercept, c[1] = slope   (eval: slope*(x+y)+intercept)
 *   MIN_SIZE / MAX_SIZE         c[0] = intercept, c[1] = slope
 *   MULTIPLIED_SIZES            c[0] = intercept, c[1] = slope
 *   SUBTRACTED_SIZES            c[0] = intercept, c[1] = slope, c[2] = minimum
 *   LINEAR_ON_DIAGONAL          c[0] = intercept, c[1] = slope, c[2] = constant (off-diag value)
 *   QUADRATIC_IN_X / _Y / _Z    c[0] = coeff0, c[1] = coeff1, c[2] = coeff2
 *   WITH_INTERACTION            c[0] = c00, c[1] = c10, c[2] = c01, c[3] = c11
 *   LINEAR_IN_Y_AND_Z           c[0] = intercept, c[1] = slopeY, c[2] = slopeZ
 *   LINEAR_IN_MAX_YZ            c[0] = intercept, c[1] = slope
 *   LITERAL_IN_Y_OR_LINEAR_IN_Z c[0] = intercept, c[1] = slope   (eval: max(y, slope*z + intercept))
 *   EXP_MOD                     c[0] = coeff00, c[1] = coeff11, c[2] = coeff12
 *   CONST_ABOVE_DIAG /
 *   CONST_BELOW_DIAG            c[0] = constant, c[1] = minimum,
 *                               c[2..7] = coeff00, coeff10, coeff01,
 *                                          coeff20, coeff11, coeff02
 *
 * Any new shape must be appended (never reordered) so baked .uplcx
 * artifacts stay decodable. */
typedef enum uplc_costshape {
    UPLC_COSTSHAPE_CONSTANT                    = 0,
    UPLC_COSTSHAPE_LINEAR_IN_X                 = 1,
    UPLC_COSTSHAPE_LINEAR_IN_Y                 = 2,
    UPLC_COSTSHAPE_LINEAR_IN_Z                 = 3,
    UPLC_COSTSHAPE_ADDED_SIZES                 = 4,
    UPLC_COSTSHAPE_MIN_SIZE                    = 5,
    UPLC_COSTSHAPE_MAX_SIZE                    = 6,
    UPLC_COSTSHAPE_MULTIPLIED_SIZES            = 7,
    UPLC_COSTSHAPE_SUBTRACTED_SIZES            = 8,
    UPLC_COSTSHAPE_LINEAR_ON_DIAGONAL          = 9,
    UPLC_COSTSHAPE_CONST_ABOVE_DIAG            = 10,
    UPLC_COSTSHAPE_CONST_BELOW_DIAG            = 11,
    UPLC_COSTSHAPE_QUADRATIC_IN_X              = 12,
    UPLC_COSTSHAPE_QUADRATIC_IN_Y              = 13,
    UPLC_COSTSHAPE_QUADRATIC_IN_Z              = 14,
    UPLC_COSTSHAPE_WITH_INTERACTION            = 15,
    UPLC_COSTSHAPE_LINEAR_IN_Y_AND_Z           = 16,
    UPLC_COSTSHAPE_LINEAR_IN_MAX_YZ            = 17,
    UPLC_COSTSHAPE_LITERAL_IN_Y_OR_LINEAR_IN_Z = 18,
    UPLC_COSTSHAPE_EXP_MOD                     = 19,
    UPLC_COSTSHAPE__COUNT                      = 20
} uplc_costshape;

/* A single cost function. `shape` selects the formula; `c[]` stores up to
 * eight int64 parameters (see comment above for layout per shape). */
typedef struct uplc_costfn {
    uint8_t  shape;         /* uplc_costshape */
    uint8_t  _pad[7];
    int64_t  c[8];
} uplc_costfn;

/* Per-builtin row: one cost function for cpu, one for mem, plus the arity
 * used to drive argument-size extraction in exmem.c. */
typedef struct uplc_cost_row {
    uplc_costfn cpu;
    uplc_costfn mem;
    uint8_t     arity;      /* 1 | 2 | 3 | 6 */
    uint8_t     _pad[7];
} uplc_cost_row;

/* Startup + per-step costs. Matches TS MACHINE_COSTS + startup constant. */
typedef struct uplc_step_cost_table {
    struct { int64_t cpu; int64_t mem; } step[UPLC_STEP__COUNT];
    struct { int64_t cpu; int64_t mem; } startup;
} uplc_step_cost_table;

/* Full cost model baked into a .uplcx artifact. `builtin_count` is the
 * number of rows that follow, indexed by uplc_builtin_tag. */
typedef struct uplc_cost_model {
    uint32_t              version;        /* 1 for Conway */
    uint32_t              builtin_count;
    uplc_step_cost_table  steps;
    uplc_cost_row         builtin[];      /* flexible array member */
} uplc_cost_model;

/* Evaluate a cost function at argument sizes (sizes[0..arity-1]). Uses
 * saturating i64 arithmetic. */
int64_t uplcrt_costfn_eval(const uplc_costfn* fn,
                           const int64_t*     sizes,
                           uint8_t            arity);

#ifdef __cplusplus
}
#endif

#endif /* UPLC_COSTMODEL_H */
