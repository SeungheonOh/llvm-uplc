#ifndef UPLCRT_CEK_H
#define UPLCRT_CEK_H

#include <stdint.h>

#include "runtime/arena.h"
#include "runtime/cek/rterm.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Direct CEK interpreter result. `ok` is 1 on success (value is valid), 0
 * on failure (fail_kind identifies the category).
 *
 * The runtime's budget struct is flushed before cek_run returns so the
 * caller always sees the final (cpu, mem) charges.
 */
typedef struct uplc_cek_result {
    int            ok;
    uplc_fail_kind fail_kind;
    const char*    fail_message;
    uplc_value     value;
} uplc_cek_result;

uplc_cek_result uplc_cek_run(uplc_arena* arena, uplc_rterm* root,
                             uplc_budget* budget);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_CEK_H */
