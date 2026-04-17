#include "runtime/builtins/helpers.h"
#include "runtime/core/rterm.h"

#include <stdalign.h>
#include <string.h>

/*
 * List builtins. The TS reference stores a `ListConstant` as
 *   { itemType: ConstantType; values: Constant[] }
 * which maps 1:1 to `uplc_rconstant::list { item_type, values, n_values }`.
 * Building a derived list (tail/cons) requires a new rconstant with a new
 * values array allocated out of the arena.
 */

static uplc_rconstant* new_list(uplc_arena* a, uplc_rtype* item_type,
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

uplc_value uplcrt_builtin_headList(uplc_budget* b, uplc_value* a) {
    const uplc_rconstant* list = uplcrt_unwrap_list(b, a[0]);
    if (list->list.n_values == 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    uplc_rconstant* head = list->list.values[0];
    return uplc_make_con_raw(head, head->tag);
}

uplc_value uplcrt_builtin_tailList(uplc_budget* b, uplc_value* a) {
    const uplc_rconstant* list = uplcrt_unwrap_list(b, a[0]);
    if (list->list.n_values == 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uint32_t n = list->list.n_values - 1;
    uplc_rconstant** values = NULL;
    if (n > 0) {
        values = (uplc_rconstant**)uplc_arena_alloc(
            ar, sizeof(uplc_rconstant*) * n, alignof(uplc_rconstant*));
        for (uint32_t i = 0; i < n; ++i) values[i] = list->list.values[i + 1];
    }
    uplc_rconstant* out = new_list(ar, list->list.item_type, values, n);
    return uplc_make_con_raw(out, out->tag);
}

uplc_value uplcrt_builtin_mkCons(uplc_budget* b, uplc_value* a) {
    /* The first argument is any constant; the second is a list whose
     * item type must match. TS checks constantTypeEquals — we approximate
     * by comparing the rconstant tag only (list of pairs will need deep
     * comparison in M3b if conformance demands it).
     *
     * An inline-int head must be materialized into an rconstant because
     * list elements are stored as rconstant pointers, not values. */
    uplc_arena* ar = uplcrt_budget_arena(b);
    const uplc_rconstant* head = NULL;
    if (uplc_value_is_int_inline(a[0])) {
        head = uplc_rconst_integer_si(ar, (long)uplc_value_int_inline(a[0]));
    } else {
        head = uplcrt_as_const(a[0]);
    }
    if (!head) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    const uplc_rconstant* list = uplcrt_unwrap_list(b, a[1]);
    if (head->tag != list->list.item_type->tag) {
        /* Shallow check — M3b upgrades to full type comparison. */
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    uint32_t n = list->list.n_values + 1;
    uplc_rconstant** values = (uplc_rconstant**)uplc_arena_alloc(
        ar, sizeof(uplc_rconstant*) * n, alignof(uplc_rconstant*));
    values[0] = (uplc_rconstant*)head;
    for (uint32_t i = 0; i < list->list.n_values; ++i) {
        values[i + 1] = list->list.values[i];
    }
    uplc_rconstant* out = new_list(ar, list->list.item_type, values, n);
    return uplc_make_con_raw(out, out->tag);
}

uplc_value uplcrt_builtin_nullList(uplc_budget* b, uplc_value* a) {
    const uplc_rconstant* list = uplcrt_unwrap_list(b, a[0]);
    return uplcrt_result_bool(b, list->list.n_values == 0);
}

uplc_value uplcrt_builtin_chooseList(uplc_budget* b, uplc_value* a) {
    /* chooseList : forall a r. list a -> r -> r -> r (two forces).
     * Returns args[1] if empty, args[2] otherwise. */
    const uplc_rconstant* list = uplcrt_unwrap_list(b, a[0]);
    return (list->list.n_values == 0) ? a[1] : a[2];
}

uplc_value uplcrt_builtin_dropList(uplc_budget* b, uplc_value* a) {
    /* dropList : forall a. integer -> list a -> list a (one force). */
    mpz_srcptr n_mpz = uplcrt_unwrap_integer(b, a[0]);
    const uplc_rconstant* list = uplcrt_unwrap_list(b, a[1]);
    uint32_t len = list->list.n_values;

    uint32_t drop;
    if (mpz_sgn(n_mpz) < 0) {
        drop = 0;
    } else if (mpz_cmp_ui(n_mpz, len) >= 0) {
        drop = len;
    } else {
        drop = (uint32_t)mpz_get_ui(n_mpz);
    }

    uplc_arena* ar = uplcrt_budget_arena(b);
    uint32_t remain = len - drop;
    uplc_rconstant** values = NULL;
    if (remain > 0) {
        values = (uplc_rconstant**)uplc_arena_alloc(
            ar, sizeof(uplc_rconstant*) * remain, alignof(uplc_rconstant*));
        for (uint32_t i = 0; i < remain; ++i) {
            values[i] = list->list.values[drop + i];
        }
    }
    uplc_rconstant* out = new_list(ar, list->list.item_type, values, remain);
    return uplc_make_con_raw(out, out->tag);
}
