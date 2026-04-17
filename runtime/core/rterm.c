#include "runtime/core/rterm.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Internal helpers                                                      */
/* --------------------------------------------------------------------- */

static uplc_rterm* alloc_term(uplc_arena* a, uplc_rterm_tag tag) {
    uplc_rterm* t = (uplc_rterm*)uplc_arena_alloc(
        a, sizeof(uplc_rterm), alignof(uplc_rterm));
    memset(t, 0, sizeof(*t));
    t->tag = (uint8_t)tag;
    return t;
}

static uplc_rconstant* alloc_const(uplc_arena* a, uplc_rconst_tag tag) {
    uplc_rconstant* c = (uplc_rconstant*)uplc_arena_alloc(
        a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
    memset(c, 0, sizeof(*c));
    c->tag = (uint8_t)tag;
    return c;
}

static uplc_rtype* alloc_type(uplc_arena* a, uplc_rtype_tag tag) {
    uplc_rtype* t = (uplc_rtype*)uplc_arena_alloc(
        a, sizeof(uplc_rtype), alignof(uplc_rtype));
    memset(t, 0, sizeof(*t));
    t->tag = (uint8_t)tag;
    return t;
}

static uplc_rdata* alloc_data(uplc_arena* a, uplc_rdata_tag tag) {
    uplc_rdata* d = (uplc_rdata*)uplc_arena_alloc(
        a, sizeof(uplc_rdata), alignof(uplc_rdata));
    memset(d, 0, sizeof(*d));
    d->tag = (uint8_t)tag;
    return d;
}

/* --------------------------------------------------------------------- */
/* Term constructors                                                     */
/* --------------------------------------------------------------------- */

uplc_rterm* uplc_rterm_var(uplc_arena* a, uint32_t index) {
    uplc_rterm* t = alloc_term(a, UPLC_RTERM_VAR);
    t->var.index = index;
    return t;
}

uplc_rterm* uplc_rterm_lambda(uplc_arena* a, uplc_rterm* body) {
    uplc_rterm* t = alloc_term(a, UPLC_RTERM_LAMBDA);
    t->lambda.body = body;
    return t;
}

uplc_rterm* uplc_rterm_apply(uplc_arena* a, uplc_rterm* fn, uplc_rterm* arg) {
    uplc_rterm* t = alloc_term(a, UPLC_RTERM_APPLY);
    t->apply.fn = fn;
    t->apply.arg = arg;
    return t;
}

uplc_rterm* uplc_rterm_delay(uplc_arena* a, uplc_rterm* inner) {
    uplc_rterm* t = alloc_term(a, UPLC_RTERM_DELAY);
    t->delay.term = inner;
    return t;
}

uplc_rterm* uplc_rterm_force(uplc_arena* a, uplc_rterm* inner) {
    uplc_rterm* t = alloc_term(a, UPLC_RTERM_FORCE);
    t->force.term = inner;
    return t;
}

uplc_rterm* uplc_rterm_error(uplc_arena* a) {
    return alloc_term(a, UPLC_RTERM_ERROR);
}

uplc_rterm* uplc_rterm_builtin(uplc_arena* a, uint8_t tag) {
    uplc_rterm* t = alloc_term(a, UPLC_RTERM_BUILTIN);
    t->builtin.tag = tag;
    return t;
}

uplc_rterm* uplc_rterm_constant(uplc_arena* a, uplc_rconstant* c) {
    uplc_rterm* t = alloc_term(a, UPLC_RTERM_CONSTANT);
    t->constant.value = c;
    return t;
}

uplc_rterm* uplc_rterm_constr(uplc_arena* a, uint64_t tag,
                              uplc_rterm** fields, uint32_t n) {
    uplc_rterm* t = alloc_term(a, UPLC_RTERM_CONSTR);
    t->constr.tag_index = tag;
    t->constr.fields = fields;
    t->constr.n_fields = n;
    return t;
}

uplc_rterm* uplc_rterm_case(uplc_arena* a, uplc_rterm* scrutinee,
                            uplc_rterm** branches, uint32_t n) {
    uplc_rterm* t = alloc_term(a, UPLC_RTERM_CASE);
    t->case_.scrutinee = scrutinee;
    t->case_.branches = branches;
    t->case_.n_branches = n;
    return t;
}

/* --------------------------------------------------------------------- */
/* Constant constructors                                                 */
/* --------------------------------------------------------------------- */

uplc_rconstant* uplc_rconst_integer_si(uplc_arena* a, long v) {
    uplc_rconstant* c = alloc_const(a, UPLC_RCONST_INTEGER);
    c->integer.value = uplc_arena_alloc_mpz(a);
    mpz_set_si(c->integer.value, v);
    return c;
}

uplc_rconstant* uplc_rconst_integer_mpz(uplc_arena* a, mpz_srcptr v) {
    uplc_rconstant* c = alloc_const(a, UPLC_RCONST_INTEGER);
    c->integer.value = uplc_arena_alloc_mpz(a);
    mpz_set(c->integer.value, v);
    return c;
}

uplc_rconstant* uplc_rconst_bytestring(uplc_arena* a, const uint8_t* b, uint32_t n) {
    uplc_rconstant* c = alloc_const(a, UPLC_RCONST_BYTESTRING);
    c->bytestring.bytes = (const uint8_t*)uplc_arena_dup(a, b, n);
    c->bytestring.len = n;
    return c;
}

uplc_rconstant* uplc_rconst_string(uplc_arena* a, const char* s, uint32_t n) {
    uplc_rconstant* c = alloc_const(a, UPLC_RCONST_STRING);
    c->string.utf8 = (const char*)uplc_arena_dup(a, s, n);
    c->string.len = n;
    return c;
}

uplc_rconstant* uplc_rconst_bool(uplc_arena* a, bool v) {
    uplc_rconstant* c = alloc_const(a, UPLC_RCONST_BOOL);
    c->boolean.value = v;
    return c;
}

uplc_rconstant* uplc_rconst_unit(uplc_arena* a) {
    return alloc_const(a, UPLC_RCONST_UNIT);
}

uplc_rconstant* uplc_rconst_data(uplc_arena* a, uplc_rdata* v) {
    uplc_rconstant* c = alloc_const(a, UPLC_RCONST_DATA);
    c->data.value = v;
    return c;
}

uplc_rconstant* uplc_rconst_bls_g1(uplc_arena* a, const uint8_t bytes[48]) {
    uplc_rconstant* c = alloc_const(a, UPLC_RCONST_BLS12_381_G1);
    uint8_t* buf = (uint8_t*)uplc_arena_alloc(a, 48, 1);
    memcpy(buf, bytes, 48);
    c->bls_g1.bytes = buf;
    return c;
}

uplc_rconstant* uplc_rconst_bls_g2(uplc_arena* a, const uint8_t bytes[96]) {
    uplc_rconstant* c = alloc_const(a, UPLC_RCONST_BLS12_381_G2);
    uint8_t* buf = (uint8_t*)uplc_arena_alloc(a, 96, 1);
    memcpy(buf, bytes, 96);
    c->bls_g2.bytes = buf;
    return c;
}

uplc_rconstant* uplc_rconst_bls_ml_result(uplc_arena* a, const uint8_t* bytes, uint32_t len) {
    uplc_rconstant* c = alloc_const(a, UPLC_RCONST_BLS12_381_ML_RESULT);
    uint8_t* buf = (uint8_t*)uplc_arena_alloc(a, len, 1);
    memcpy(buf, bytes, len);
    c->bls_ml_result.bytes = buf;
    c->bls_ml_result.len = len;
    return c;
}

uplc_rconstant* uplc_rconst_ledger_value(uplc_arena* a, uplc_rledger_value* v) {
    uplc_rconstant* c = alloc_const(a, UPLC_RCONST_VALUE);
    c->ledger_value.value = v;
    return c;
}

/* --------------------------------------------------------------------- */
/* Type constructors                                                     */
/* --------------------------------------------------------------------- */

uplc_rtype* uplc_rtype_simple(uplc_arena* a, uint8_t tag) {
    return alloc_type(a, (uplc_rtype_tag)tag);
}
uplc_rtype* uplc_rtype_list(uplc_arena* a, uplc_rtype* element) {
    uplc_rtype* t = alloc_type(a, UPLC_RTYPE_LIST);
    t->list.element = element;
    return t;
}
uplc_rtype* uplc_rtype_pair(uplc_arena* a, uplc_rtype* first, uplc_rtype* second) {
    uplc_rtype* t = alloc_type(a, UPLC_RTYPE_PAIR);
    t->pair.first = first;
    t->pair.second = second;
    return t;
}
uplc_rtype* uplc_rtype_array(uplc_arena* a, uplc_rtype* element) {
    uplc_rtype* t = alloc_type(a, UPLC_RTYPE_ARRAY);
    t->array.element = element;
    return t;
}

/* --------------------------------------------------------------------- */
/* Data constructors                                                     */
/* --------------------------------------------------------------------- */

uplc_rdata* uplc_rdata_integer_mpz(uplc_arena* a, mpz_srcptr v) {
    uplc_rdata* d = alloc_data(a, UPLC_RDATA_INTEGER);
    d->integer.value = uplc_arena_alloc_mpz(a);
    mpz_set(d->integer.value, v);
    return d;
}

uplc_rdata* uplc_rdata_bytestring(uplc_arena* a, const uint8_t* b, uint32_t n) {
    uplc_rdata* d = alloc_data(a, UPLC_RDATA_BYTESTRING);
    d->bytestring.bytes = (const uint8_t*)uplc_arena_dup(a, b, n);
    d->bytestring.len = n;
    return d;
}

uplc_rdata* uplc_rdata_list(uplc_arena* a, uplc_rdata** values, uint32_t n) {
    uplc_rdata* d = alloc_data(a, UPLC_RDATA_LIST);
    d->list.values = values;
    d->list.n_values = n;
    return d;
}

uplc_rdata* uplc_rdata_map(uplc_arena* a, uplc_rdata_pair* entries, uint32_t n) {
    uplc_rdata* d = alloc_data(a, UPLC_RDATA_MAP);
    d->map.entries = entries;
    d->map.n_entries = n;
    return d;
}

uplc_rdata* uplc_rdata_constr(uplc_arena* a, mpz_srcptr index,
                              uplc_rdata** fields, uint32_t n) {
    uplc_rdata* d = alloc_data(a, UPLC_RDATA_CONSTR);
    d->constr.index = uplc_arena_alloc_mpz(a);
    mpz_set(d->constr.index, index);
    d->constr.fields = fields;
    d->constr.n_fields = n;
    return d;
}
