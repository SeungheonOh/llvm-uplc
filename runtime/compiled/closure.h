#ifndef UPLCRT_COMPILED_CLOSURE_H
#define UPLCRT_COMPILED_CLOSURE_H

#include <stdint.h>

#include "uplc/abi.h"
#include "uplc/budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compiled-mode closures: opaque to generated code, manipulated via the
 * ABI entry points in abi.h. Layout is `uplc_closure` (declared in abi.h):
 *
 *     struct uplc_closure {
 *         void*      fn;              // pointer to the emitted body
 *         uint32_t   nfree;
 *         uint32_t   _pad;
 *         uplc_value free[];          // captured free variables
 *     };
 *
 * VLam(subtag=COMPILED).payload and VDelay(subtag=COMPILED).payload both
 * point to an instance of this struct allocated out of the evaluation
 * arena. Lambda bodies are cast to `uplc_lam_fn`; delay bodies to
 * `uplc_delay_fn` (both typedefs live in abi.h).
 *
 * `uplcrt_closure_of` is the internal accessor used by apply/force.c.
 */
uplc_closure* uplcrt_closure_of(uplc_value v);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_COMPILED_CLOSURE_H */
