#ifndef UPLCRT_ENV_H
#define UPLCRT_ENV_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime/arena.h"
#include "uplc/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Environment: a right-growing cons list of values indexed by de Bruijn.
 * Entry N (1-based) is the Nth value from the head. New bindings are
 * prepended (innermost-first lookup), matching the TS CEK machine.
 *
 * All cells live in the evaluation arena; destroying the arena frees them.
 */
typedef struct uplc_env_cell uplc_env_cell;

struct uplc_env_cell {
    uplc_value            value;
    struct uplc_env_cell* next;
};

typedef uplc_env_cell* uplc_env;  /* NULL = empty */

uplc_env uplc_env_extend(uplc_arena* a, uplc_env env, uplc_value v);

/* 1-based lookup. Returns true and writes the value to *out on success;
 * returns false if the index is out of range (caller should treat this as
 * an evaluation failure). */
bool uplc_env_lookup(uplc_env env, uint32_t index, uplc_value* out);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_ENV_H */
