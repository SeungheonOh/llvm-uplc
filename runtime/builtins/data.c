#include "runtime/builtins/helpers.h"
#include "runtime/cbor_data.h"
#include "runtime/cek/rterm.h"

#include <stdalign.h>
#include <string.h>

/*
 * Data builtins — port of TS cek/builtins/data.ts.
 *
 * These builders/deconstructors do a lot of type manipulation: the
 * uplc_rconstant produced by `unConstrData` is a pair of (integer, list
 * data), etc. We cache the common types (data, list-data, pair-data-data,
 * integer) as per-arena singletons so we don't rebuild them on every call.
 */

/* ---- Shared type builders -------------------------------------------- */

static uplc_rtype* make_simple_type(uplc_arena* a, uplc_rtype_tag tag) {
    uplc_rtype* t = (uplc_rtype*)uplc_arena_alloc(
        a, sizeof(uplc_rtype), alignof(uplc_rtype));
    memset(t, 0, sizeof(*t));
    t->tag = (uint8_t)tag;
    return t;
}

static uplc_rtype* make_list_type(uplc_arena* a, uplc_rtype* elem) {
    uplc_rtype* t = (uplc_rtype*)uplc_arena_alloc(
        a, sizeof(uplc_rtype), alignof(uplc_rtype));
    memset(t, 0, sizeof(*t));
    t->tag = (uint8_t)UPLC_RTYPE_LIST;
    t->list.element = elem;
    return t;
}

static uplc_rtype* make_pair_type(uplc_arena* a, uplc_rtype* fst, uplc_rtype* snd) {
    uplc_rtype* t = (uplc_rtype*)uplc_arena_alloc(
        a, sizeof(uplc_rtype), alignof(uplc_rtype));
    memset(t, 0, sizeof(*t));
    t->tag = (uint8_t)UPLC_RTYPE_PAIR;
    t->pair.first = fst;
    t->pair.second = snd;
    return t;
}

/* ---- rconstant builders ---------------------------------------------- */

static uplc_rconstant* make_data_const(uplc_arena* a, uplc_rdata* d) {
    uplc_rconstant* c = (uplc_rconstant*)uplc_arena_alloc(
        a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
    memset(c, 0, sizeof(*c));
    c->tag = UPLC_RCONST_DATA;
    c->data.value = d;
    return c;
}

static uplc_rconstant* make_int_const(uplc_arena* a, mpz_srcptr v) {
    uplc_rconstant* c = (uplc_rconstant*)uplc_arena_alloc(
        a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
    memset(c, 0, sizeof(*c));
    c->tag = UPLC_RCONST_INTEGER;
    c->integer.value = uplc_arena_alloc_mpz(a);
    mpz_set(c->integer.value, v);
    return c;
}

__attribute__((unused))
static uplc_rconstant* make_bs_const(uplc_arena* a, const uint8_t* bytes, uint32_t len) {
    uplc_rconstant* c = (uplc_rconstant*)uplc_arena_alloc(
        a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
    memset(c, 0, sizeof(*c));
    c->tag = UPLC_RCONST_BYTESTRING;
    c->bytestring.bytes = (const uint8_t*)uplc_arena_dup(a, bytes, len);
    c->bytestring.len = len;
    return c;
}

static uplc_rconstant* make_list_const(uplc_arena* a, uplc_rtype* item_type,
                                       uplc_rconstant** values, uint32_t n) {
    uplc_rconstant* c = (uplc_rconstant*)uplc_arena_alloc(
        a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
    memset(c, 0, sizeof(*c));
    c->tag = UPLC_RCONST_LIST;
    c->list.item_type = item_type;
    c->list.values = values;
    c->list.n_values = n;
    return c;
}

static uplc_rconstant* make_pair_const(uplc_arena* a,
                                       uplc_rtype* fst_type, uplc_rtype* snd_type,
                                       uplc_rconstant* first, uplc_rconstant* second) {
    uplc_rconstant* c = (uplc_rconstant*)uplc_arena_alloc(
        a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
    memset(c, 0, sizeof(*c));
    c->tag = UPLC_RCONST_PAIR;
    c->pair.fst_type = fst_type;
    c->pair.snd_type = snd_type;
    c->pair.first = first;
    c->pair.second = second;
    return c;
}

static uplc_value wrap_con(uplc_rconstant* c) { return uplc_make_con_raw(c, c->tag); }

/* ---- Constructors ----------------------------------------------------- */

uplc_value uplcrt_builtin_iData(uplc_budget* b, uplc_value* a) {
    mpz_srcptr n = uplcrt_unwrap_integer(b, a[0]);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rdata* d = uplc_rdata_integer_mpz(ar, n);
    return uplcrt_result_data(b, d);
}

uplc_value uplcrt_builtin_bData(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[0], &len);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rdata* d = uplc_rdata_bytestring(ar, bs, len);
    return uplcrt_result_data(b, d);
}

uplc_value uplcrt_builtin_constrData(uplc_budget* b, uplc_value* a) {
    mpz_srcptr tag = uplcrt_unwrap_integer(b, a[0]);
    const uplc_rconstant* list = uplcrt_unwrap_list(b, a[1]);
    uplc_arena* ar = uplcrt_budget_arena(b);

    uplc_rdata** fields = NULL;
    uint32_t n = list->list.n_values;
    if (n > 0) {
        fields = (uplc_rdata**)uplc_arena_alloc(
            ar, sizeof(uplc_rdata*) * n, alignof(uplc_rdata*));
    }
    for (uint32_t i = 0; i < n; ++i) {
        uplc_rconstant* item = list->list.values[i];
        if (item->tag != UPLC_RCONST_DATA) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        fields[i] = item->data.value;
    }
    uplc_rdata* d = uplc_rdata_constr(ar, tag, fields, n);
    return uplcrt_result_data(b, d);
}

uplc_value uplcrt_builtin_listData(uplc_budget* b, uplc_value* a) {
    const uplc_rconstant* list = uplcrt_unwrap_list(b, a[0]);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uint32_t n = list->list.n_values;
    uplc_rdata** values = NULL;
    if (n > 0) {
        values = (uplc_rdata**)uplc_arena_alloc(
            ar, sizeof(uplc_rdata*) * n, alignof(uplc_rdata*));
    }
    for (uint32_t i = 0; i < n; ++i) {
        uplc_rconstant* item = list->list.values[i];
        if (item->tag != UPLC_RCONST_DATA) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        values[i] = item->data.value;
    }
    uplc_rdata* d = uplc_rdata_list(ar, values, n);
    return uplcrt_result_data(b, d);
}

uplc_value uplcrt_builtin_mapData(uplc_budget* b, uplc_value* a) {
    const uplc_rconstant* list = uplcrt_unwrap_list(b, a[0]);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uint32_t n = list->list.n_values;
    uplc_rdata_pair* entries = NULL;
    if (n > 0) {
        entries = (uplc_rdata_pair*)uplc_arena_alloc(
            ar, sizeof(uplc_rdata_pair) * n, alignof(uplc_rdata_pair));
    }
    for (uint32_t i = 0; i < n; ++i) {
        uplc_rconstant* item = list->list.values[i];
        if (item->tag != UPLC_RCONST_PAIR) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        if (item->pair.first->tag != UPLC_RCONST_DATA ||
            item->pair.second->tag != UPLC_RCONST_DATA) {
            uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        }
        entries[i].key = item->pair.first->data.value;
        entries[i].value = item->pair.second->data.value;
    }
    uplc_rdata* d = uplc_rdata_map(ar, entries, n);
    return uplcrt_result_data(b, d);
}

/* ---- Deconstructors --------------------------------------------------- */

uplc_value uplcrt_builtin_unIData(uplc_budget* b, uplc_value* a) {
    const uplc_rdata* d = uplcrt_unwrap_data(b, a[0]);
    if (d->tag != UPLC_RDATA_INTEGER) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    /* Prefer inline V_CON when the stored mpz fits in i64. */
    return uplcrt_result_integer_mpz_or_inline(b, d->integer.value);
}

uplc_value uplcrt_builtin_unBData(uplc_budget* b, uplc_value* a) {
    const uplc_rdata* d = uplcrt_unwrap_data(b, a[0]);
    if (d->tag != UPLC_RDATA_BYTESTRING) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    return uplcrt_result_bytestring(b, d->bytestring.bytes, d->bytestring.len);
}

uplc_value uplcrt_builtin_unConstrData(uplc_budget* b, uplc_value* a) {
    const uplc_rdata* d = uplcrt_unwrap_data(b, a[0]);
    if (d->tag != UPLC_RDATA_CONSTR) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    uplc_arena* ar = uplcrt_budget_arena(b);

    /* Wrap each field as a data rconstant. */
    uint32_t n = d->constr.n_fields;
    uplc_rconstant** data_consts = NULL;
    if (n > 0) {
        data_consts = (uplc_rconstant**)uplc_arena_alloc(
            ar, sizeof(uplc_rconstant*) * n, alignof(uplc_rconstant*));
    }
    for (uint32_t i = 0; i < n; ++i) {
        data_consts[i] = make_data_const(ar, d->constr.fields[i]);
    }

    uplc_rtype* t_int = make_simple_type(ar, UPLC_RTYPE_INTEGER);
    uplc_rtype* t_data = make_simple_type(ar, UPLC_RTYPE_DATA);
    uplc_rtype* t_list_data = make_list_type(ar, t_data);

    uplc_rconstant* first  = make_int_const(ar, d->constr.index);
    uplc_rconstant* second = make_list_const(ar, t_data, data_consts, n);
    uplc_rconstant* pair   = make_pair_const(ar, t_int, t_list_data, first, second);
    return wrap_con(pair);
}

uplc_value uplcrt_builtin_unListData(uplc_budget* b, uplc_value* a) {
    const uplc_rdata* d = uplcrt_unwrap_data(b, a[0]);
    if (d->tag != UPLC_RDATA_LIST) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    uplc_arena* ar = uplcrt_budget_arena(b);

    uint32_t n = d->list.n_values;
    uplc_rconstant** values = NULL;
    if (n > 0) {
        values = (uplc_rconstant**)uplc_arena_alloc(
            ar, sizeof(uplc_rconstant*) * n, alignof(uplc_rconstant*));
    }
    for (uint32_t i = 0; i < n; ++i) {
        values[i] = make_data_const(ar, d->list.values[i]);
    }
    uplc_rtype* t_data = make_simple_type(ar, UPLC_RTYPE_DATA);
    uplc_rconstant* list = make_list_const(ar, t_data, values, n);
    return wrap_con(list);
}

uplc_value uplcrt_builtin_unMapData(uplc_budget* b, uplc_value* a) {
    const uplc_rdata* d = uplcrt_unwrap_data(b, a[0]);
    if (d->tag != UPLC_RDATA_MAP) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    uplc_arena* ar = uplcrt_budget_arena(b);

    uplc_rtype* t_data = make_simple_type(ar, UPLC_RTYPE_DATA);
    uplc_rtype* t_pair = make_pair_type(ar, t_data, t_data);

    uint32_t n = d->map.n_entries;
    uplc_rconstant** values = NULL;
    if (n > 0) {
        values = (uplc_rconstant**)uplc_arena_alloc(
            ar, sizeof(uplc_rconstant*) * n, alignof(uplc_rconstant*));
    }
    for (uint32_t i = 0; i < n; ++i) {
        uplc_rconstant* k = make_data_const(ar, d->map.entries[i].key);
        uplc_rconstant* v = make_data_const(ar, d->map.entries[i].value);
        values[i] = make_pair_const(ar, t_data, t_data, k, v);
    }
    uplc_rconstant* list = make_list_const(ar, t_pair, values, n);
    return wrap_con(list);
}

/* ---- equalsData: deep structural equality ----------------------------- */

static bool plutus_data_equals(const uplc_rdata* a, const uplc_rdata* b) {
    if (a->tag != b->tag) return false;
    switch ((uplc_rdata_tag)a->tag) {
        case UPLC_RDATA_CONSTR: {
            if (mpz_cmp(a->constr.index, b->constr.index) != 0) return false;
            if (a->constr.n_fields != b->constr.n_fields) return false;
            for (uint32_t i = 0; i < a->constr.n_fields; ++i) {
                if (!plutus_data_equals(a->constr.fields[i], b->constr.fields[i]))
                    return false;
            }
            return true;
        }
        case UPLC_RDATA_MAP: {
            if (a->map.n_entries != b->map.n_entries) return false;
            for (uint32_t i = 0; i < a->map.n_entries; ++i) {
                if (!plutus_data_equals(a->map.entries[i].key, b->map.entries[i].key))
                    return false;
                if (!plutus_data_equals(a->map.entries[i].value, b->map.entries[i].value))
                    return false;
            }
            return true;
        }
        case UPLC_RDATA_LIST: {
            if (a->list.n_values != b->list.n_values) return false;
            for (uint32_t i = 0; i < a->list.n_values; ++i) {
                if (!plutus_data_equals(a->list.values[i], b->list.values[i]))
                    return false;
            }
            return true;
        }
        case UPLC_RDATA_INTEGER:
            return mpz_cmp(a->integer.value, b->integer.value) == 0;
        case UPLC_RDATA_BYTESTRING:
            if (a->bytestring.len != b->bytestring.len) return false;
            return memcmp(a->bytestring.bytes, b->bytestring.bytes,
                          a->bytestring.len) == 0;
    }
    return false;
}

uplc_value uplcrt_builtin_equalsData(uplc_budget* b, uplc_value* a) {
    const uplc_rdata* x = uplcrt_unwrap_data(b, a[0]);
    const uplc_rdata* y = uplcrt_unwrap_data(b, a[1]);
    return uplcrt_result_bool(b, plutus_data_equals(x, y));
}

/* ---- chooseData ------------------------------------------------------- */

uplc_value uplcrt_builtin_chooseData(uplc_budget* b, uplc_value* a) {
    const uplc_rdata* d = uplcrt_unwrap_data(b, a[0]);
    switch ((uplc_rdata_tag)d->tag) {
        case UPLC_RDATA_CONSTR:     return a[1];
        case UPLC_RDATA_MAP:        return a[2];
        case UPLC_RDATA_LIST:       return a[3];
        case UPLC_RDATA_INTEGER:    return a[4];
        case UPLC_RDATA_BYTESTRING: return a[5];
    }
    uplcrt_fail(b, UPLC_FAIL_EVALUATION);
}

/* ---- mkPairData / mkNil* --------------------------------------------- */

uplc_value uplcrt_builtin_mkPairData(uplc_budget* b, uplc_value* a) {
    const uplc_rdata* x = uplcrt_unwrap_data(b, a[0]);
    const uplc_rdata* y = uplcrt_unwrap_data(b, a[1]);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rtype* t_data = make_simple_type(ar, UPLC_RTYPE_DATA);
    uplc_rconstant* first  = make_data_const(ar, (uplc_rdata*)x);
    uplc_rconstant* second = make_data_const(ar, (uplc_rdata*)y);
    return wrap_con(make_pair_const(ar, t_data, t_data, first, second));
}

static uplc_value unwrap_unit_return(uplc_budget* b, uplc_value v) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_UNIT) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    return v;
}

uplc_value uplcrt_builtin_mkNilData(uplc_budget* b, uplc_value* a) {
    (void)unwrap_unit_return(b, a[0]);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rtype* t_data = make_simple_type(ar, UPLC_RTYPE_DATA);
    return wrap_con(make_list_const(ar, t_data, NULL, 0));
}

uplc_value uplcrt_builtin_mkNilPairData(uplc_budget* b, uplc_value* a) {
    (void)unwrap_unit_return(b, a[0]);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rtype* t_data = make_simple_type(ar, UPLC_RTYPE_DATA);
    uplc_rtype* t_pair = make_pair_type(ar, t_data, t_data);
    return wrap_con(make_list_const(ar, t_pair, NULL, 0));
}

/* ---- serialiseData: CBOR-encode the Data tree ------------------------ */

uplc_value uplcrt_builtin_serialiseData(uplc_budget* b, uplc_value* a) {
    const uplc_rdata* d = uplcrt_unwrap_data(b, a[0]);
    uplc_arena* ar = uplcrt_budget_arena(b);
    const uint8_t* bytes = NULL;
    uint32_t len = 0;
    uplcrt_cbor_encode_data(ar, d, &bytes, &len);
    return uplcrt_result_bytestring(b, bytes, len);
}
