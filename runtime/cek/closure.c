#include "runtime/cek/closure.h"

#include <stdalign.h>
#include <stdint.h>

static uplc_interp_closure* new_closure(uplc_arena* a, uplc_rterm* body,
                                        uplc_env env) {
    uplc_interp_closure* c = (uplc_interp_closure*)uplc_arena_alloc(
        a, sizeof(uplc_interp_closure), alignof(uplc_interp_closure));
    c->body = body;
    c->env = env;
    return c;
}

uplc_value uplc_make_lam_interp(uplc_arena* a, uplc_rterm* body, uplc_env env) {
    uplc_interp_closure* c = new_closure(a, body, env);
    uplc_value v;
    v.tag     = UPLC_V_LAM;
    v.subtag  = UPLC_VLAM_INTERP;
    for (int i = 0; i < 6; ++i) v._pad[i] = 0;
    v.payload = (uint64_t)(uintptr_t)c;
    return v;
}

uplc_value uplc_make_delay_interp(uplc_arena* a, uplc_rterm* body, uplc_env env) {
    uplc_interp_closure* c = new_closure(a, body, env);
    uplc_value v;
    v.tag     = UPLC_V_DELAY;
    v.subtag  = UPLC_VLAM_INTERP;
    for (int i = 0; i < 6; ++i) v._pad[i] = 0;
    v.payload = (uint64_t)(uintptr_t)c;
    return v;
}

uplc_interp_closure* uplc_closure_of(uplc_value v) {
    return (uplc_interp_closure*)(uintptr_t)v.payload;
}
