#include "runtime/compiled/entry.h"

#include <setjmp.h>
#include <string.h>

#include "runtime/errors.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

uplc_compiled_result uplcrt_run_compiled(uplc_program_entry entry,
                                         uplc_arena* arena,
                                         uplc_budget* budget) {
    uplc_compiled_result result;
    memset(&result, 0, sizeof(result));
    result.fail_kind = UPLC_FAIL_MACHINE;

    /* Attach the arena so ABI make_ / const_ helpers can allocate. */
    budget->arena = arena;

    uplc_fail_ctx ctx;
    if (setjmp(ctx.env) != 0) {
        uplcrt_fail_install(NULL);
        result.ok = 0;
        result.fail_kind = ctx.kind;
        result.fail_message = ctx.message;
        uplcrt_budget_flush(budget);
        return result;
    }
    uplcrt_fail_install(&ctx);

    uplc_value value = entry(budget);

    uplcrt_fail_install(NULL);
    uplcrt_budget_flush(budget);
    if (!uplcrt_budget_ok(budget)) {
        result.ok = 0;
        result.fail_kind = UPLC_FAIL_OUT_OF_BUDGET;
        result.fail_message = "out of budget";
        return result;
    }
    result.ok = 1;
    result.value = value;
    return result;
}
