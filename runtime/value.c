#include "runtime/value.h"

#include "runtime/arena.h"
#include "uplc/abi.h"

#include <stdalign.h>
#include <stdint.h>

/*
 * Shared uplc_value helpers. No dependency on runtime/cek/ — so both the
 * direct interpreter and (later) compiled-code paths can link against the
 * same implementation.
 */

uplc_value uplc_make_con_raw(const void* payload, uint8_t subtag) {
    uplc_value v;
    v.tag     = UPLC_V_CON;
    v.subtag  = subtag;
    for (int i = 0; i < 6; ++i) v._pad[i] = 0;
    v.payload = (uint64_t)(uintptr_t)payload;
    return v;
}

struct uplc_constr_payload {
    uint64_t   tag;
    uint32_t   n;
    uint32_t   _pad;
    uplc_value fields[];
};

uplc_value uplc_make_constr_vals(uplc_arena* a, uint64_t tag,
                                 const uplc_value* fields, uint32_t n) {
    size_t bytes = sizeof(uplc_constr_payload) + (size_t)n * sizeof(uplc_value);
    uplc_constr_payload* p = (uplc_constr_payload*)uplc_arena_alloc(
        a, bytes, alignof(uplc_constr_payload));
    p->tag = tag;
    p->n = n;
    p->_pad = 0;
    for (uint32_t i = 0; i < n; ++i) p->fields[i] = fields[i];

    uplc_value v;
    v.tag     = UPLC_V_CONSTR;
    v.subtag  = 0;
    for (int i = 0; i < 6; ++i) v._pad[i] = 0;
    v.payload = (uint64_t)(uintptr_t)p;
    return v;
}

uplc_constr_payload* uplc_constr_of(uplc_value v) {
    return (uplc_constr_payload*)(uintptr_t)v.payload;
}

uint64_t uplc_constr_tag(const uplc_constr_payload* p) { return p->tag; }
uint32_t uplc_constr_arity(const uplc_constr_payload* p) { return p->n; }
uplc_value uplc_constr_field(const uplc_constr_payload* p, uint32_t i) {
    return p->fields[i];
}
