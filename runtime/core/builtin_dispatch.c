#include "runtime/core/builtin_dispatch.h"

#include <stdint.h>

#include "runtime/core/errors.h"
#include "runtime/core/exmem.h"
#include "uplc/abi.h"
#include "uplc/budget.h"
#include "uplc/costmodel.h"
#include "uplc/term.h"

extern const uint32_t UPLC_BUILTIN_COUNT;

/* Defined in runtime/builtin_table.c — the metadata array. */
extern const uplc_builtin_meta UPLC_BUILTIN_META[];

const uplc_builtin_meta* uplcrt_builtin_meta(uint8_t tag) {
    if ((uint32_t)tag >= UPLC_BUILTIN_COUNT) return NULL;
    return &UPLC_BUILTIN_META[tag];
}

static void charge_builtin_cost(uplc_budget* b, const uplc_builtin_meta* meta,
                                int64_t sizes[3]) {
    int64_t cpu = uplcrt_costfn_eval(&meta->cost.cpu, sizes, meta->arity);
    int64_t mem = uplcrt_costfn_eval(&meta->cost.mem, sizes, meta->arity);
    b->cpu = uplcrt_sat_add_i64(b->cpu, -cpu);
    b->mem = uplcrt_sat_add_i64(b->mem, -mem);
    /* OOB is deferred to the terminal flush. TS's reference caps consumed
     * at I64_MAX (saturating positively); a builtin that charges I64_MAX
     * under an I64_MAX initial budget must not trip OOB mid-run because
     * its consumed value saturates at initial, not beyond it. Checking
     * remaining < 0 here would fire spuriously on exactly those fixtures
     * (see builtin/semantics/dropList/dropList-14..16). */
}

uplc_value uplcrt_run_builtin(uplc_budget* b, uint8_t tag,
                              uplc_value* argv, uint32_t argc) {
    const uplc_builtin_meta* meta = uplcrt_builtin_meta(tag);
    if (!meta) {
        uplcrt_raise(UPLC_FAIL_EVALUATION, "builtin: unknown tag");
    }
    if (!meta->impl) {
        uplcrt_raise(UPLC_FAIL_EVALUATION, "builtin: not implemented (M5 crypto or experimental)");
    }

    int64_t sizes[3] = {0, 0, 0};
    uplcrt_builtin_arg_sizes((uplc_builtin_tag)tag, argv, argc, sizes);
    charge_builtin_cost(b, meta, sizes);

    return meta->impl(b, argv);
}
