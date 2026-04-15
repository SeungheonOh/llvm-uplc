#include "runtime/cek/env.h"

#include <stdalign.h>

uplc_env uplc_env_extend(uplc_arena* a, uplc_env env, uplc_value v) {
    uplc_env_cell* c = (uplc_env_cell*)uplc_arena_alloc(
        a, sizeof(uplc_env_cell), alignof(uplc_env_cell));
    c->value = v;
    c->next = env;
    return c;
}

bool uplc_env_lookup(uplc_env env, uint32_t index, uplc_value* out) {
    if (index == 0) return false;
    uplc_env_cell* c = env;
    uint32_t i = 1;
    while (c) {
        if (i == index) {
            *out = c->value;
            return true;
        }
        ++i;
        c = c->next;
    }
    return false;
}
