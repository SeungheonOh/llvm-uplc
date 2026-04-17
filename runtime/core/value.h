#ifndef UPLCRT_VALUE_H
#define UPLCRT_VALUE_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime/core/arena.h"
#include "uplc/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generic uplc_value helpers shared between the CEK interpreter and the
 * future LLVM-emitted code path. Anything that refers to an
 * interpreter-specific type (uplc_rterm, uplc_rconstant, uplc_interp_closure,
 * ...) lives under runtime/cek/ instead.
 *
 * VConstr uses a single on-heap layout regardless of execution mode:
 * interpreters fill it from the CEK machine; compiled code uses the same
 * header so that generated IR can interoperate with interpreter values
 * (e.g. when mixing `--interp` inputs with a compiled entry point).
 */

/* Opaque VConstr payload. Definition lives in value.c. */
typedef struct uplc_constr_payload uplc_constr_payload;

/* Wrap an arena-allocated payload pointer as a V_CON tagged uplc_value.
 * `subtag` carries the constant-type discriminator (the exact meaning is
 * chosen by the caller: CEK passes the uplc_rconstant's tag, compiled
 * code will pass its own baked-constant tag). */
uplc_value uplc_make_con_raw(const void* payload, uint8_t subtag);

/* Pack a 64-bit signed integer directly into a V_CON value without
 * touching the arena. The resulting value has subtag
 * UPLC_VCON_INT_INLINE; payload is the signed int reinterpreted as
 * uint64. Callers must route these through uplc_int_view helpers (see
 * runtime/builtins/helpers.h) so the fast / slow paths stay consistent. */
static inline uplc_value uplc_make_int_inline(int64_t v) {
    uplc_value out;
    out.tag     = UPLC_V_CON;
    out.subtag  = UPLC_VCON_INT_INLINE;
    for (int i = 0; i < 6; ++i) out._pad[i] = 0;
    out.payload = (uint64_t)v;
    return out;
}

/* True iff `v` is a V_CON whose payload is an inline integer. */
static inline bool uplc_value_is_int_inline(uplc_value v) {
    return v.tag == UPLC_V_CON && v.subtag == UPLC_VCON_INT_INLINE;
}

static inline int64_t uplc_value_int_inline(uplc_value v) {
    return (int64_t)v.payload;
}

uplc_value           uplc_make_constr_vals(uplc_arena* a, uint64_t tag,
                                           const uplc_value* fields, uint32_t n);
uplc_constr_payload* uplc_constr_of(uplc_value v);
uint64_t             uplc_constr_tag(const uplc_constr_payload* p);
uint32_t             uplc_constr_arity(const uplc_constr_payload* p);
uplc_value           uplc_constr_field(const uplc_constr_payload* p, uint32_t i);

/* Generic payload extraction. Callers cast the result to whatever concrete
 * struct lives at the payload (uplc_rconstant*, a baked const, ...). */
static inline void* uplc_value_payload(uplc_value v) {
    return (void*)(uintptr_t)v.payload;
}

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_VALUE_H */
