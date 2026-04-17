#ifndef UPLCRT_BC_CLOSURE_H
#define UPLCRT_BC_CLOSURE_H

/*
 * Bytecode-VM closure layout. Carried as the payload of V_LAM / V_DELAY
 * values with subtag UPLC_VLAM_BYTECODE.
 *
 * Upvals are captured in the order the emitter planned; VAR_UPVAL looks
 * them up by slot index inside the callee's body. The first slot of the
 * callee's env ([0]) holds the applied argument (for LamAbs) or is unused
 * (for Delay); upvals are shifted accordingly.
 */

#include <stdint.h>

#include "runtime/core/arena.h"
#include "runtime/core/value.h"
#include "uplc/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uplc_bc_closure {
    const struct uplc_bc_fn* fn;  /* points into uplc_bc_program::functions */
    uint32_t   n_upvals;
    uint32_t   _pad;
    uplc_value upvals[];          /* flexible array; captures */
} uplc_bc_closure;

/* Allocate a bytecode closure in the arena and populate it from the
 * given captured values (`upvals` points into the current env). Returns
 * a V_LAM / V_DELAY value (caller picks which tag to set). Fails via
 * uplcrt_raise on arena exhaustion. */
uplc_bc_closure* uplc_bc_closure_new(uplc_arena*              arena,
                                     const struct uplc_bc_fn* fn,
                                     uint32_t                 n_upvals,
                                     const uplc_value*        upvals);

/* Extract the closure payload from a V_LAM / V_DELAY value. The value
 * must already carry subtag UPLC_VLAM_BYTECODE — callers check the
 * subtag themselves and raise EVALUATION on mismatch. */
static inline uplc_bc_closure* uplc_bc_closure_of(uplc_value v) {
    return (uplc_bc_closure*)(uintptr_t)v.payload;
}

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_BC_CLOSURE_H */
