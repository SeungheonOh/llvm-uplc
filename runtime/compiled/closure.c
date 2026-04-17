#include "runtime/compiled/closure.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include "runtime/core/arena.h"
#include "runtime/core/errors.h"
#include "uplc/abi.h"
#include "uplc/budget.h"

/*
 * Allocate a uplc_closure struct sized for `nfree` captured free vars, copy
 * them in, stamp the function pointer, and wrap it in a uplc_value. Both
 * VLam and VDelay share this layout — the only difference is the outer
 * value tag. The function pointer's type is determined by the caller (see
 * uplc_lam_fn / uplc_delay_fn in abi.h).
 */
static uplc_value make_closure_value(uplc_budget* b, uint8_t value_tag,
                                     void* fn, const uplc_value* free_vars,
                                     uint32_t nfree) {
    uplc_arena* a = uplcrt_budget_arena(b);
    if (!a) {
        uplcrt_fail(b, UPLC_FAIL_MACHINE);
    }
    size_t bytes = sizeof(uplc_closure) + (size_t)nfree * sizeof(uplc_value);
    uplc_closure* c = (uplc_closure*)uplc_arena_alloc(a, bytes, alignof(uplc_closure));
    c->fn = fn;
    c->nfree = nfree;
    c->_pad = 0;
    if (nfree > 0 && free_vars) {
        memcpy(c->free, free_vars, (size_t)nfree * sizeof(uplc_value));
    }

    uplc_value v;
    v.tag     = value_tag;
    v.subtag  = UPLC_VLAM_COMPILED;
    for (int i = 0; i < 6; ++i) v._pad[i] = 0;
    v.payload = (uint64_t)(uintptr_t)c;
    return v;
}

uplc_value uplcrt_make_lam(uplc_budget* b, void* fn,
                           const uplc_value* free_vars, uint32_t nfree) {
    return make_closure_value(b, UPLC_V_LAM, fn, free_vars, nfree);
}

uplc_value uplcrt_make_delay(uplc_budget* b, void* fn,
                             const uplc_value* free_vars, uint32_t nfree) {
    return make_closure_value(b, UPLC_V_DELAY, fn, free_vars, nfree);
}

uplc_closure* uplcrt_closure_of(uplc_value v) {
    return (uplc_closure*)(uintptr_t)v.payload;
}
