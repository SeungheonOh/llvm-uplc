#ifndef UPLCRT_COMPILED_ENTRY_H
#define UPLCRT_COMPILED_ENTRY_H

#include <stdint.h>

#include "runtime/core/arena.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Top-level entry used by uplcr to drive a compiled program. The result
 * shape mirrors uplc_cek_result (runtime/cek/cek.h) so driver code can
 * handle both modes through a common union later.
 */
typedef struct uplc_compiled_result {
    int            ok;
    uplc_fail_kind fail_kind;
    const char*    fail_message;
    uplc_value     value;
} uplc_compiled_result;

/*
 * Program entry point signature: a parameterless function that evaluates
 * the program and returns its top-level value. LLVM-emitted code will
 * synthesize one of these per .uplcx artifact; tests and the future
 * differential harness hand-write them.
 *
 * The entry takes the current budget so it can access the evaluation
 * arena via uplcrt_budget_arena. (UPLC programs are always closed at the
 * top level — there is no env parameter.)
 */
typedef uplc_value (*uplc_program_entry)(uplc_budget* b);

/*
 * Install a failure trampoline, attach the arena to the budget, run the
 * entry point, flush residual slippage, and return a populated result.
 * On longjmp-caught failure, `ok` is 0 and `fail_kind` / `fail_message`
 * describe the category.
 */
uplc_compiled_result uplcrt_run_compiled(uplc_program_entry entry,
                                         uplc_arena* arena,
                                         uplc_budget* budget);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_COMPILED_ENTRY_H */
