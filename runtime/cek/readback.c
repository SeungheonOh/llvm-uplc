#include "runtime/cek/readback.h"

#include <stdalign.h>
#include <stdint.h>

#include "runtime/bytecode/closure.h"
#include "runtime/core/builtin_state.h"
#include "runtime/cek/closure.h"
#include "runtime/cek/env.h"
#include "runtime/core/value.h"
#include "uplc/bytecode.h"

/*
 * Value → rterm readback, including faithful closure readback via env
 * substitution (the "closeOver" pass from TS Readback.lean).
 *
 * The primary consumer is the M4 conformance driver: after the CEK
 * machine terminates, we round-trip the produced value into the same
 * textual form the TS reference emits so we can diff against
 * `.uplc.expected`.
 *
 * Closure readback walks the captured env into a flat array (innermost-
 * first) then substitutes each env value into the body's free de Bruijn
 * indices. `depth` is 1 for a VLam (the outer lambda we emit introduces
 * one binding on top of the body) and 0 for a VDelay.
 */

/* Walk the env linked list and collect into a flat innermost-first array. */
static uint32_t env_length(uplc_env env) {
    uint32_t n = 0;
    for (uplc_env_cell* c = env; c != NULL; c = c->next) ++n;
    return n;
}

static void env_collect(uplc_arena* a, uplc_env env, uplc_value** out_arr,
                        uint32_t* out_len) {
    uint32_t n = env_length(env);
    *out_len = n;
    if (n == 0) { *out_arr = NULL; return; }
    uplc_value* arr = (uplc_value*)uplc_arena_alloc(
        a, sizeof(uplc_value) * n, alignof(uplc_value));
    uint32_t i = 0;
    for (uplc_env_cell* c = env; c != NULL; c = c->next) arr[i++] = c->value;
    *out_arr = arr;
}

static uplc_rterm* substitute(uplc_arena* a, uplc_rterm* term,
                              uplc_value* env_arr, uint32_t env_len,
                              uint32_t depth) {
    switch ((uplc_rterm_tag)term->tag) {
        case UPLC_RTERM_VAR: {
            uint32_t i = term->var.index;
            if (i == 0 || i <= depth) return term;        /* local binding */
            uint32_t idx = i - depth - 1;
            if (idx >= env_len) return uplc_rterm_error(a); /* unreachable for closed terms */
            return uplcrt_readback(a, env_arr[idx]);
        }
        case UPLC_RTERM_LAMBDA:
            return uplc_rterm_lambda(a,
                substitute(a, term->lambda.body, env_arr, env_len, depth + 1));
        case UPLC_RTERM_DELAY:
            return uplc_rterm_delay(a,
                substitute(a, term->delay.term, env_arr, env_len, depth));
        case UPLC_RTERM_FORCE:
            return uplc_rterm_force(a,
                substitute(a, term->force.term, env_arr, env_len, depth));
        case UPLC_RTERM_APPLY:
            return uplc_rterm_apply(a,
                substitute(a, term->apply.fn, env_arr, env_len, depth),
                substitute(a, term->apply.arg, env_arr, env_len, depth));
        case UPLC_RTERM_CONSTR: {
            uint32_t n = term->constr.n_fields;
            uplc_rterm** fields = NULL;
            if (n > 0) {
                fields = (uplc_rterm**)uplc_arena_alloc(
                    a, sizeof(uplc_rterm*) * n, alignof(uplc_rterm*));
                for (uint32_t i = 0; i < n; ++i) {
                    fields[i] = substitute(a, term->constr.fields[i], env_arr, env_len, depth);
                }
            }
            return uplc_rterm_constr(a, term->constr.tag_index, fields, n);
        }
        case UPLC_RTERM_CASE: {
            uint32_t n = term->case_.n_branches;
            uplc_rterm** branches = NULL;
            if (n > 0) {
                branches = (uplc_rterm**)uplc_arena_alloc(
                    a, sizeof(uplc_rterm*) * n, alignof(uplc_rterm*));
                for (uint32_t i = 0; i < n; ++i) {
                    branches[i] = substitute(a, term->case_.branches[i], env_arr, env_len, depth);
                }
            }
            uplc_rterm* sc = substitute(a, term->case_.scrutinee, env_arr, env_len, depth);
            return uplc_rterm_case(a, sc, branches, n);
        }
        case UPLC_RTERM_CONSTANT:
        case UPLC_RTERM_BUILTIN:
        case UPLC_RTERM_ERROR:
            return term;
    }
    return uplc_rterm_error(a);
}

uplc_rterm* uplcrt_readback_close(uplc_arena* arena, uplc_rterm* body,
                                  struct uplc_env_cell* env, uint32_t initial_depth) {
    uplc_value* arr = NULL;
    uint32_t len = 0;
    env_collect(arena, env, &arr, &len);
    return substitute(arena, body, arr, len, initial_depth);
}

/* Readback helper for bytecode-VM closures (subtag UPLC_VLAM_BYTECODE).
 *
 * Unlike the CEK's cons-list env, a bytecode closure stores only the
 * free vars its body actually references, in a flat array, plus a
 * parallel array of "outer deBruijn index for each upval slot" that the
 * lowerer produces. We reconstruct a dense env array — large enough to
 * cover the maximum deBruijn the body references — filling unreferenced
 * slots with a placeholder value (never read by substitute). Then we
 * reuse the same `substitute` the CEK path uses. */
uplc_rterm* uplcrt_readback_bc_closure(uplc_arena* arena, uplc_value v,
                                       uint32_t initial_depth) {
    uplc_bc_closure*   c  = uplc_bc_closure_of(v);
    const uplc_bc_fn*  fn = c->fn;
    if (fn == NULL || fn->body_rterm == NULL) {
        return uplc_rterm_error(arena);
    }

    uint32_t env_len = 0;
    for (uint32_t i = 0; i < fn->n_upvals; ++i) {
        uint32_t d = fn->upval_outer_db[i];
        if (d + 1u > env_len) env_len = d + 1u;
    }

    uplc_value* env_arr = NULL;
    if (env_len > 0) {
        env_arr = (uplc_value*)uplc_arena_alloc(
            arena, sizeof(uplc_value) * env_len, alignof(uplc_value));
        /* Placeholder for slots the body doesn't reference. `substitute`
         * only touches env_arr[i] when the body contains a matching Var,
         * so the placeholder is never read for well-formed programs. */
        uplc_value placeholder = uplc_make_int_inline(0);
        for (uint32_t i = 0; i < env_len; ++i) env_arr[i] = placeholder;
        for (uint32_t i = 0; i < fn->n_upvals; ++i) {
            env_arr[fn->upval_outer_db[i]] = c->upvals[i];
        }
    }
    return substitute(arena, (uplc_rterm*)fn->body_rterm,
                      env_arr, env_len, initial_depth);
}

uplc_rterm* uplcrt_readback(uplc_arena* arena, uplc_value v) {
    switch ((uplc_value_tag)v.tag) {
        case UPLC_V_CON: {
            /* Inline int: materialize an rconstant backed by a fresh
             * arena-allocated mpz so the readback consumer (pretty
             * printer, test harness, etc.) sees uniform VCon shapes. */
            if (uplc_value_is_int_inline(v)) {
                uplc_rconstant* c = uplc_rconst_integer_si(
                    arena, (long)uplc_value_int_inline(v));
                return uplc_rterm_constant(arena, c);
            }
            uplc_rconstant* c = (uplc_rconstant*)uplc_value_payload(v);
            return uplc_rterm_constant(arena, c);
        }
        case UPLC_V_CONSTR: {
            uplc_constr_payload* cp = uplc_constr_of(v);
            uint32_t n = uplc_constr_arity(cp);
            uplc_rterm** fields = NULL;
            if (n > 0) {
                fields = (uplc_rterm**)uplc_arena_alloc(
                    arena, sizeof(uplc_rterm*) * n, alignof(uplc_rterm*));
                for (uint32_t i = 0; i < n; ++i) {
                    fields[i] = uplcrt_readback(arena, uplc_constr_field(cp, i));
                }
            }
            return uplc_rterm_constr(arena, uplc_constr_tag(cp), fields, n);
        }
        case UPLC_V_BUILTIN: {
            uplc_builtin_state* s = uplcrt_builtin_state_of(v);
            uplc_rterm* t = uplc_rterm_builtin(arena, s->tag);
            for (uint8_t i = 0; i < s->forces_applied; ++i) {
                t = uplc_rterm_force(arena, t);
            }
            for (uint8_t i = 0; i < s->args_applied; ++i) {
                uplc_rterm* arg = uplcrt_readback(arena, s->args[i]);
                t = uplc_rterm_apply(arena, t, arg);
            }
            return t;
        }
        case UPLC_V_LAM: {
            if (v.subtag == UPLC_VLAM_INTERP) {
                uplc_interp_closure* c = uplc_closure_of(v);
                return uplc_rterm_lambda(
                    arena, uplcrt_readback_close(arena, c->body, c->env, 1));
            }
            if (v.subtag == UPLC_VLAM_BYTECODE) {
                return uplc_rterm_lambda(
                    arena, uplcrt_readback_bc_closure(arena, v, 1));
            }
            /* Compiled-mode closure — not walkable as a term. */
            return uplc_rterm_lambda(arena, uplc_rterm_error(arena));
        }
        case UPLC_V_DELAY: {
            if (v.subtag == UPLC_VLAM_INTERP) {
                uplc_interp_closure* c = uplc_closure_of(v);
                return uplc_rterm_delay(
                    arena, uplcrt_readback_close(arena, c->body, c->env, 0));
            }
            if (v.subtag == UPLC_VLAM_BYTECODE) {
                return uplc_rterm_delay(
                    arena, uplcrt_readback_bc_closure(arena, v, 0));
            }
            return uplc_rterm_delay(arena, uplc_rterm_error(arena));
        }
        case UPLC_V__COUNT:
            break;
    }
    return uplc_rterm_error(arena);
}
