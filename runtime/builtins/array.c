#include "runtime/builtins/helpers.h"

#include <stdalign.h>
#include <string.h>

/*
 * Array builtins. UPLC `array` is a distinct constant universe from
 * `list` but stores the same shape (item type + values). All three
 * builtins just inspect / project.
 */

static const uplc_rconstant* unwrap_array(uplc_budget* b, uplc_value v) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_ARRAY) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    return c;
}

uplc_value uplcrt_builtin_listToArray(uplc_budget* b, uplc_value* a) {
    const uplc_rconstant* list = uplcrt_unwrap_list(b, a[0]);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rconstant* out = (uplc_rconstant*)uplc_arena_alloc(
        ar, sizeof(uplc_rconstant), alignof(uplc_rconstant));
    memset(out, 0, sizeof(*out));
    out->tag = UPLC_RCONST_ARRAY;
    out->array.item_type = list->list.item_type;
    out->array.values = list->list.values;
    out->array.n_values = list->list.n_values;
    return uplc_make_con_raw(out, out->tag);
}

uplc_value uplcrt_builtin_lengthOfArray(uplc_budget* b, uplc_value* a) {
    const uplc_rconstant* arr = unwrap_array(b, a[0]);
    return uplcrt_result_integer_si(b, (int64_t)arr->array.n_values);
}

uplc_value uplcrt_builtin_indexArray(uplc_budget* b, uplc_value* a) {
    const uplc_rconstant* arr = unwrap_array(b, a[0]);
    mpz_srcptr idx = uplcrt_unwrap_integer(b, a[1]);
    if (mpz_sgn(idx) < 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    if (mpz_cmp_ui(idx, arr->array.n_values) >= 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    uint32_t i = (uint32_t)mpz_get_ui(idx);
    uplc_rconstant* item = arr->array.values[i];
    return uplc_make_con_raw(item, item->tag);
}
