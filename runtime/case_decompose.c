#include "runtime/case_decompose.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#include <gmp.h>

#include "runtime/cek/rterm.h"    /* shared uplc_rconstant layout */
#include "runtime/errors.h"
#include "runtime/value.h"

/*
 * Decompose a case scrutinee. Matches TS constantToConstr exactly:
 *
 *   integer n       → tag = n            (requires n >= 0, fits u64)
 *   bool False/True → tag = 0 or 1       (n_branches <= 2)
 *   unit            → tag = 0            (n_branches <= 1)
 *   pair (a, b)     → tag = 0, fields=[a, b]   (n_branches <= 1)
 *   list []         → tag = 1            (n_branches <= 2)
 *   list (x:xs)     → tag = 0, fields=[x, xs]  (n_branches <= 2)
 *   VConstr         → tag/fields from the constr value  (no n_branches bound)
 */

static uplc_value* alloc_field_array(uplc_arena* a, uint32_t n) {
    return (uplc_value*)uplc_arena_alloc(a, sizeof(uplc_value) * n, alignof(uplc_value));
}

/* Build a fresh list constant representing the tail (values[1..]) of src. */
static uplc_rconstant* list_tail(uplc_arena* a, const uplc_rconstant* src) {
    uint32_t n = src->list.n_values - 1;
    uplc_rconstant** values = NULL;
    if (n > 0) {
        values = (uplc_rconstant**)uplc_arena_alloc(
            a, sizeof(uplc_rconstant*) * n, alignof(uplc_rconstant*));
        for (uint32_t i = 0; i < n; ++i) values[i] = src->list.values[i + 1];
    }
    uplc_rconstant* out = (uplc_rconstant*)uplc_arena_alloc(
        a, sizeof(uplc_rconstant), alignof(uplc_rconstant));
    memset(out, 0, sizeof(*out));
    out->tag = UPLC_RCONST_LIST;
    out->list.item_type = src->list.item_type;
    out->list.values = values;
    out->list.n_values = n;
    return out;
}

static uplc_value wrap_const(uplc_rconstant* c) {
    return uplc_make_con_raw(c, c->tag);
}

uplc_case_decomp uplcrt_case_decompose(uplc_budget* b,
                                       uplc_value   scrutinee,
                                       uint32_t     n_branches) {
    uplc_case_decomp out = {0, 0, NULL};
    uplc_arena* arena = uplcrt_budget_arena(b);

    /* VConstr — direct tag/fields, no arity bound */
    if (scrutinee.tag == UPLC_V_CONSTR) {
        uplc_constr_payload* cp = uplc_constr_of(scrutinee);
        out.tag = uplc_constr_tag(cp);
        out.n_fields = uplc_constr_arity(cp);
        if (out.n_fields > 0) {
            out.fields = alloc_field_array(arena, out.n_fields);
            for (uint32_t i = 0; i < out.n_fields; ++i) {
                out.fields[i] = uplc_constr_field(cp, i);
            }
        }
        goto check_bounds;
    }

    /* VCon — the interesting case */
    if (scrutinee.tag == UPLC_V_CON) {
        /* Inline int fast path — no rconstant backing. */
        if (uplc_value_is_int_inline(scrutinee)) {
            int64_t v = uplc_value_int_inline(scrutinee);
            if (v < 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
            out.tag = (uint64_t)v;
            goto check_bounds;
        }
        const uplc_rconstant* c = (const uplc_rconstant*)uplc_value_payload(scrutinee);
        switch ((uplc_rconst_tag)c->tag) {
            case UPLC_RCONST_INTEGER: {
                mpz_srcptr v = c->integer.value;
                if (mpz_sgn(v) < 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
                if (mpz_sizeinbase(v, 2) > 63) {
                    /* TS caps at 2^53 (MAX_SAFE_INTEGER) but anything that
                     * large is already way beyond a plausible branch count. */
                    uplcrt_fail(b, UPLC_FAIL_EVALUATION);
                }
                out.tag = (uint64_t)mpz_get_ui(v);
                /* no n_branches bound for integer */
                goto check_bounds;
            }
            case UPLC_RCONST_BOOL:
                if (n_branches > 2) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
                out.tag = c->boolean.value ? 1u : 0u;
                goto check_bounds;
            case UPLC_RCONST_UNIT:
                if (n_branches > 1) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
                out.tag = 0;
                goto check_bounds;
            case UPLC_RCONST_PAIR:
                if (n_branches > 1) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
                out.tag = 0;
                out.n_fields = 2;
                out.fields = alloc_field_array(arena, 2);
                out.fields[0] = wrap_const(c->pair.first);
                out.fields[1] = wrap_const(c->pair.second);
                goto check_bounds;
            case UPLC_RCONST_LIST: {
                if (n_branches > 2) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
                if (c->list.n_values == 0) {
                    out.tag = 1;
                } else {
                    out.tag = 0;
                    out.n_fields = 2;
                    out.fields = alloc_field_array(arena, 2);
                    out.fields[0] = wrap_const(c->list.values[0]);
                    out.fields[1] = wrap_const(list_tail(arena, c));
                }
                goto check_bounds;
            }
            default:
                uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        }
    }

    uplcrt_fail(b, UPLC_FAIL_EVALUATION);

check_bounds:
    /* Standard out-of-bounds check, applied uniformly regardless of source. */
    if (out.tag >= n_branches) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    return out;
}

void uplcrt_case_decompose_out(uplc_budget* b, uplc_value scrutinee,
                               uint32_t n_branches, uint64_t* out_tag,
                               uint32_t* out_n_fields, uplc_value** out_fields) {
    uplc_case_decomp d = uplcrt_case_decompose(b, scrutinee, n_branches);
    *out_tag      = d.tag;
    *out_n_fields = d.n_fields;
    *out_fields   = d.fields;
}
