#include "runtime/builtins/helpers.h"

#include <stdalign.h>
#include <string.h>

/*
 * Pair projections. fstPair and sndPair each carry two forces (one per
 * type variable) and one value argument (the pair). The result is a new
 * VCon wrapping the stored child constant.
 */

static uplc_value project(uplc_budget* b, uplc_value v, bool first) {
    const uplc_rconstant* pair = uplcrt_unwrap_pair(b, v);
    uplc_rconstant* child = first ? pair->pair.first : pair->pair.second;
    /* The child is already an arena-allocated uplc_rconstant — hand it
     * to the shared make_con_raw wrapper without copying. */
    return uplc_make_con_raw(child, child->tag);
}

uplc_value uplcrt_builtin_fstPair(uplc_budget* b, uplc_value* a) {
    return project(b, a[0], true);
}

uplc_value uplcrt_builtin_sndPair(uplc_budget* b, uplc_value* a) {
    return project(b, a[0], false);
}
