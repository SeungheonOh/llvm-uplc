#ifndef UPLCRT_CLOSURE_H
#define UPLCRT_CLOSURE_H

#include "runtime/core/arena.h"
#include "runtime/cek/env.h"
#include "runtime/core/rterm.h"
#include "uplc/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Interpreter closure: a (term, env) pair captured at VLam / VDelay
 * creation time. Both constructors allocate into the evaluation arena.
 *
 * The subtag field on the enclosing uplc_value (UPLC_VLAM_INTERP vs
 * UPLC_VLAM_COMPILED) is defined in uplc/abi.h. The direct interpreter
 * only produces / consumes UPLC_VLAM_INTERP closures; compiled-code
 * entrypoints only handle UPLC_VLAM_COMPILED ones.
 */
typedef struct uplc_interp_closure {
    uplc_rterm* body;   /* lambda body OR delayed term */
    uplc_env    env;
} uplc_interp_closure;

uplc_value uplc_make_lam_interp(uplc_arena* a, uplc_rterm* body, uplc_env env);
uplc_value uplc_make_delay_interp(uplc_arena* a, uplc_rterm* body, uplc_env env);

/* Extract the underlying closure. Caller must first check tag == UPLC_V_LAM
 * or UPLC_V_DELAY and subtag == UPLC_VLAM_INTERP. */
uplc_interp_closure* uplc_closure_of(uplc_value v);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_CLOSURE_H */
