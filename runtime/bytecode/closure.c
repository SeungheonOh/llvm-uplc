#include "runtime/bytecode/closure.h"

#include <stdalign.h>
#include <string.h>

#include "runtime/core/errors.h"
#include "uplc/abi.h"

uplc_bc_closure* uplc_bc_closure_new(uplc_arena*              arena,
                                     const struct uplc_bc_fn* fn,
                                     uint32_t                 n_upvals,
                                     const uplc_value*        upvals) {
    size_t bytes = sizeof(uplc_bc_closure) + sizeof(uplc_value) * n_upvals;
    uplc_bc_closure* c = (uplc_bc_closure*)uplc_arena_alloc(
        arena, bytes, _Alignof(uplc_bc_closure));
    if (!c) {
        uplcrt_raise(UPLC_FAIL_MACHINE, "bytecode VM: closure allocation failed");
    }
    c->fn       = fn;
    c->n_upvals = n_upvals;
    c->_pad     = 0;
    if (n_upvals > 0 && upvals != NULL) {
        memcpy(c->upvals, upvals, sizeof(uplc_value) * n_upvals);
    }
    return c;
}
