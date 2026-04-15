#include "runtime/builtins/helpers.h"

#include <stdalign.h>
#include <string.h>

/*
 * All helpers allocate out of budget->arena. Failures raise via
 * uplcrt_fail which longjmps into whichever trampoline the current
 * execution mode installed.
 */

static uplc_arena* arena_or_fail(uplc_budget* b) {
    uplc_arena* a = uplcrt_budget_arena(b);
    if (!a) uplcrt_fail(b, UPLC_FAIL_MACHINE);
    return a;
}

static uplc_rconstant* new_rcon(uplc_arena* a, uplc_rconst_tag tag) {
    uplc_rconstant* c = (uplc_rconstant*)uplc_arena_alloc(
        a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
    memset(c, 0, sizeof(*c));
    c->tag = (uint8_t)tag;
    return c;
}

/* ---- unwrap ---- */

mpz_srcptr uplcrt_unwrap_integer(uplc_budget* b, uplc_value v) {
    /* Inline int fast-path: materialize into an arena mpz on demand. */
    if (uplc_value_is_int_inline(v)) {
        uplc_arena* a = arena_or_fail(b);
        mpz_ptr z = uplc_arena_alloc_mpz(a);
        mpz_set_si(z, (long)uplc_value_int_inline(v));
        return z;
    }
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_INTEGER) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    return c->integer.value;
}

uplc_int_view uplcrt_unwrap_integer_view(uplc_budget* b, uplc_value v) {
    uplc_int_view out;
    out.is_inline  = false;
    out.inline_val = 0;
    out.mpz        = NULL;

    if (uplc_value_is_int_inline(v)) {
        out.is_inline  = true;
        out.inline_val = uplc_value_int_inline(v);
        return out;
    }
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_INTEGER) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    out.mpz = c->integer.value;
    return out;
}

mpz_srcptr uplcrt_materialize_int_view(uplc_budget* b, uplc_int_view view) {
    if (!view.is_inline) return view.mpz;
    uplc_arena* a = arena_or_fail(b);
    mpz_ptr z = uplc_arena_alloc_mpz(a);
    mpz_set_si(z, (long)view.inline_val);
    return z;
}

const uint8_t* uplcrt_unwrap_bytestring(uplc_budget* b, uplc_value v, uint32_t* out_len) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_BYTESTRING) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    if (out_len) *out_len = c->bytestring.len;
    return c->bytestring.bytes;
}

const char* uplcrt_unwrap_string(uplc_budget* b, uplc_value v, uint32_t* out_len) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_STRING) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    if (out_len) *out_len = c->string.len;
    return c->string.utf8;
}

bool uplcrt_unwrap_bool(uplc_budget* b, uplc_value v) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_BOOL) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    return c->boolean.value;
}

const uplc_rconstant* uplcrt_unwrap_list(uplc_budget* b, uplc_value v) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_LIST) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    return c;
}

const uplc_rconstant* uplcrt_unwrap_pair(uplc_budget* b, uplc_value v) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_PAIR) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    return c;
}

const uplc_rdata* uplcrt_unwrap_data(uplc_budget* b, uplc_value v) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_DATA) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    return c->data.value;
}

/* ---- wrap ---- */

uplc_value uplcrt_result_integer_mpz(uplc_budget* b, mpz_srcptr v) {
    uplc_arena* a = arena_or_fail(b);
    uplc_rconstant* c = new_rcon(a, UPLC_RCONST_INTEGER);
    c->integer.value = uplc_arena_alloc_mpz(a);
    mpz_set(c->integer.value, v);
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_result_integer_si(uplc_budget* b, int64_t v) {
    (void)b;
    return uplc_make_int_inline(v);
}

uplc_value uplcrt_result_integer_i64(uplc_budget* b, int64_t v) {
    (void)b;
    return uplc_make_int_inline(v);
}

/* If the mpz fits in an i64, return an inline V_CON; otherwise box. */
uplc_value uplcrt_result_integer_mpz_or_inline(uplc_budget* b, mpz_srcptr v) {
    if (mpz_fits_slong_p(v)) {
        long s = mpz_get_si(v);
        return uplc_make_int_inline((int64_t)s);
    }
    return uplcrt_result_integer_mpz(b, v);
}

uplc_value uplcrt_result_bytestring(uplc_budget* b, const uint8_t* bytes, uint32_t len) {
    uplc_arena* a = arena_or_fail(b);
    uplc_rconstant* c = new_rcon(a, UPLC_RCONST_BYTESTRING);
    c->bytestring.bytes = (const uint8_t*)uplc_arena_dup(a, bytes, len);
    c->bytestring.len = len;
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_result_string(uplc_budget* b, const char* utf8, uint32_t len) {
    uplc_arena* a = arena_or_fail(b);
    uplc_rconstant* c = new_rcon(a, UPLC_RCONST_STRING);
    c->string.utf8 = (const char*)uplc_arena_dup(a, utf8, len);
    c->string.len = len;
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_result_bool(uplc_budget* b, bool v) {
    uplc_arena* a = arena_or_fail(b);
    uplc_rconstant* c = new_rcon(a, UPLC_RCONST_BOOL);
    c->boolean.value = v;
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_result_unit(uplc_budget* b) {
    uplc_arena* a = arena_or_fail(b);
    uplc_rconstant* c = new_rcon(a, UPLC_RCONST_UNIT);
    return uplc_make_con_raw(c, c->tag);
}

uplc_value uplcrt_result_data(uplc_budget* b, const uplc_rdata* d) {
    uplc_arena* a = arena_or_fail(b);
    uplc_rconstant* c = new_rcon(a, UPLC_RCONST_DATA);
    c->data.value = (uplc_rdata*)d;  /* borrowed — caller owns lifetime */
    return uplc_make_con_raw(c, c->tag);
}
