#ifndef UPLCRT_BUILTIN_STATE_H
#define UPLCRT_BUILTIN_STATE_H

#include <stdint.h>

#include "runtime/core/arena.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared VBuiltin accumulator used by both execution modes. A VBuiltin
 * value's payload points to one of these; the dispatcher transitions it
 * through Force / Apply events until it is saturated, at which point the
 * full argument vector is handed to uplcrt_run_builtin.
 *
 * Layout is fixed in M3a and shared with the arena: each new state is a
 * copy-on-write of the previous one, allocated in the arena (no
 * mutation in place, so multiple partial-application chains can be
 * materialised from a single prefix without aliasing).
 *
 * `args` points to an arena-allocated uplc_value[total_args]; only the
 * first `args_applied` slots carry meaningful data at any given time.
 */
typedef struct uplc_builtin_state {
    uint8_t        tag;             /* builtin tag */
    uint8_t        forces_applied;
    uint8_t        args_applied;
    uint8_t        total_forces;
    uint8_t        total_args;
    uint8_t        _pad[3];
    uplc_value*    args;            /* length = total_args; may be NULL if arity == 0 */
} uplc_builtin_state;

/* Allocate a fresh VBuiltin value from `b`'s arena for the given `tag`.
 * Pulls total_forces / total_args from the runtime builtin metadata
 * table. Raises UPLC_FAIL_EVALUATION if `tag` is out of range. */
uplc_value uplcrt_builtin_fresh(uplc_budget* b, uint8_t tag);

/* Extract the internal state from a VBuiltin value. */
uplc_builtin_state* uplcrt_builtin_state_of(uplc_value v);

/* Apply one Force to the state. Returns an updated VBuiltin value; if the
 * force was the last remaining unit of consumption (no forces or args
 * left), dispatches into uplcrt_run_builtin and returns its result. */
uplc_value uplcrt_builtin_consume_force(uplc_budget* b,
                                        const uplc_builtin_state* s);

/* Apply one value argument to the state. Same return semantics as
 * uplcrt_builtin_consume_force. */
uplc_value uplcrt_builtin_consume_arg(uplc_budget* b,
                                      const uplc_builtin_state* s,
                                      uplc_value arg);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_BUILTIN_STATE_H */
