#include "runtime/builtins/helpers.h"
#include "runtime/core/cbor_data.h"
#include "runtime/core/rterm.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <gmp.h>

/*
 * Plutus Value builtins (tags 94–100).
 *
 * Value is a canonical nested map:
 *   Value := SortedMap CurrencySymbol (SortedMap TokenName Integer)
 * where:
 *   - entries are sorted lexicographically by key (currency, then token name)
 *   - zero-quantity tokens are excluded
 *   - the empty Value is the identity for `unionValue`
 *
 * All results allocate into the evaluation arena.
 */

/* ---------------------------------------------------------------------------
 * Internal unwrap
 * ------------------------------------------------------------------------- */

static const uplc_rledger_value* unwrap_value(uplc_budget* b, uplc_value v) {
    const uplc_rconstant* c = uplcrt_as_const(v);
    if (!c || c->tag != UPLC_RCONST_VALUE) {
        uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }
    return c->ledger_value.value;
}

/* Compare two raw byte keys (lexicographic, shorter first on tie). */
static int bytecmp(const uint8_t* a, uint32_t alen,
                   const uint8_t* b, uint32_t blen) {
    uint32_t n = alen < blen ? alen : blen;
    int r = memcmp(a, b, n);
    if (r != 0) return r;
    if (alen < blen) return -1;
    if (alen > blen) return +1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * result_value: wrap a uplc_rledger_value in a VCon
 * ------------------------------------------------------------------------- */
static uplc_value result_value(uplc_budget* b, uplc_rledger_value* lv) {
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rconstant* c = uplc_rconst_ledger_value(ar, lv);
    return uplc_make_rcon(c);
}

/* Allocate a fresh arena-backed empty uplc_rledger_value. */
static uplc_rledger_value* alloc_empty_value(uplc_arena* ar) {
    uplc_rledger_value* lv = (uplc_rledger_value*)uplc_arena_alloc(
        ar, sizeof(uplc_rledger_value), alignof(uplc_rledger_value));
    lv->entries = NULL;
    lv->n_entries = 0;
    return lv;
}

/* Deep-copy a quantity mpz into the arena. */
static mpz_ptr copy_mpz(uplc_arena* ar, mpz_srcptr src) {
    mpz_ptr q = uplc_arena_alloc_mpz(ar);
    mpz_set(q, src);
    return q;
}

/* ---------------------------------------------------------------------------
 * insertCoin
 *
 *   insertCoin (cur : ByteString) (tok : ByteString) (amount : Integer)
 *              (val : Value) -> Value
 *
 * This is a REPLACE (set) operation, not an accumulate: the pre-existing
 * (cur, tok) quantity — if any — is ignored and replaced by `amount`.
 * `amount == 0` removes the entry (canonical form has no zero tokens).
 *
 * Semantic rules:
 *   - |amount| must be < 2^127 (canonical Plutus Value range), else fail.
 *   - When amount != 0, both keys must be ≤ 32 bytes.
 *   - When amount == 0 with an absent (cur, tok), the call is a no-op
 *     regardless of key length.
 * ------------------------------------------------------------------------- */

/* Plutus Value amount range: [-2^127, 2^127). Returns true if in range. */
static bool value_in_range(mpz_srcptr x) {
    /* max = 2^127 - 1, min = -2^127 */
    mpz_t max, min;
    mpz_init_set_str(max, "170141183460469231731687303715884105727", 10);
    mpz_init_set_str(min, "-170141183460469231731687303715884105728", 10);
    int ok = (mpz_cmp(x, max) <= 0) && (mpz_cmp(x, min) >= 0);
    mpz_clear(max);
    mpz_clear(min);
    return ok;
}

static uplc_rledger_value* insert_coin_set(uplc_arena* ar, uplc_budget* b,
                                           const uint8_t* cur, uint32_t cur_len,
                                           const uint8_t* tok, uint32_t tok_len,
                                           mpz_srcptr amount,
                                           const uplc_rledger_value* old) {
    bool is_zero = (mpz_sgn(amount) == 0);

    /* Range check: amount in [-2^127, 2^127). */
    if (!is_zero) {
        if (!value_in_range(amount)) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        if (cur_len > 32 || tok_len > 32) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    }

    uint32_t old_n = old->n_entries;

    /* Find matching currency entry, if any, and where it would sort. */
    int32_t  found_cur = -1;
    uint32_t cur_insert = old_n;
    for (uint32_t i = 0; i < old_n; ++i) {
        int cmp = bytecmp(old->entries[i].currency_bytes, old->entries[i].currency_len,
                          cur, cur_len);
        if (cmp == 0) { found_cur = (int32_t)i; cur_insert = i; break; }
        if (cmp > 0)  { cur_insert = i; break; }
    }

    if (found_cur < 0) {
        /* Currency absent. Zero amount = no-op; nonzero amount = fresh entry. */
        if (is_zero) {
            /* Shallow clone of old. */
            if (old_n == 0) return alloc_empty_value(ar);
            uplc_rledger_value* out = alloc_empty_value(ar);
            out->n_entries = old_n;
            out->entries = (uplc_rledger_entry*)uplc_arena_alloc(
                ar, sizeof(uplc_rledger_entry) * old_n, alignof(uplc_rledger_entry));
            memcpy(out->entries, old->entries, sizeof(uplc_rledger_entry) * old_n);
            return out;
        }

        /* Fresh currency entry with one token. */
        uplc_rledger_token* toks = (uplc_rledger_token*)uplc_arena_alloc(
            ar, sizeof(uplc_rledger_token), alignof(uplc_rledger_token));
        uint8_t* tok_copy = (uint8_t*)uplc_arena_alloc(ar, tok_len ? tok_len : 1, 1);
        if (tok_len) memcpy(tok_copy, tok, tok_len);
        toks[0].name_bytes = tok_copy;
        toks[0].name_len   = tok_len;
        toks[0].quantity   = copy_mpz(ar, amount);

        uplc_rledger_entry* es = (uplc_rledger_entry*)uplc_arena_alloc(
            ar, sizeof(uplc_rledger_entry) * (old_n + 1), alignof(uplc_rledger_entry));
        for (uint32_t i = 0; i < cur_insert; ++i) es[i] = old->entries[i];
        uint8_t* cur_copy = (uint8_t*)uplc_arena_alloc(ar, cur_len ? cur_len : 1, 1);
        if (cur_len) memcpy(cur_copy, cur, cur_len);
        es[cur_insert].currency_bytes = cur_copy;
        es[cur_insert].currency_len   = cur_len;
        es[cur_insert].tokens         = toks;
        es[cur_insert].n_tokens       = 1;
        for (uint32_t i = cur_insert; i < old_n; ++i) es[i + 1] = old->entries[i];

        uplc_rledger_value* out = alloc_empty_value(ar);
        out->entries   = es;
        out->n_entries = old_n + 1;
        return out;
    }

    /* Currency matched — now locate the token. */
    const uplc_rledger_entry* old_entry = &old->entries[found_cur];
    uint32_t old_tn = old_entry->n_tokens;
    int32_t  found_tok = -1;
    uint32_t tok_insert = old_tn;
    for (uint32_t i = 0; i < old_tn; ++i) {
        int cmp = bytecmp(old_entry->tokens[i].name_bytes, old_entry->tokens[i].name_len,
                          tok, tok_len);
        if (cmp == 0) { found_tok = (int32_t)i; tok_insert = i; break; }
        if (cmp > 0)  { tok_insert = i; break; }
    }

    uplc_rledger_token* new_toks;
    uint32_t            new_tn;

    if (is_zero) {
        if (found_tok < 0) {
            /* Token absent; zero-set is a no-op. Shallow-clone old. */
            uplc_rledger_value* out = alloc_empty_value(ar);
            out->n_entries = old_n;
            if (old_n > 0) {
                out->entries = (uplc_rledger_entry*)uplc_arena_alloc(
                    ar, sizeof(uplc_rledger_entry) * old_n, alignof(uplc_rledger_entry));
                memcpy(out->entries, old->entries, sizeof(uplc_rledger_entry) * old_n);
            }
            return out;
        }
        /* Remove the token. If it was the only one in the currency entry,
         * remove the currency entry as well. */
        if (old_tn == 1) {
            uplc_rledger_entry* es = NULL;
            if (old_n > 1) {
                es = (uplc_rledger_entry*)uplc_arena_alloc(
                    ar, sizeof(uplc_rledger_entry) * (old_n - 1),
                    alignof(uplc_rledger_entry));
                uint32_t k = 0;
                for (uint32_t i = 0; i < old_n; ++i) {
                    if ((int32_t)i != found_cur) es[k++] = old->entries[i];
                }
            }
            uplc_rledger_value* out = alloc_empty_value(ar);
            out->entries   = es;
            out->n_entries = old_n - 1;
            return out;
        }
        new_tn   = old_tn - 1;
        new_toks = (uplc_rledger_token*)uplc_arena_alloc(
            ar, sizeof(uplc_rledger_token) * new_tn, alignof(uplc_rledger_token));
        uint32_t k = 0;
        for (uint32_t i = 0; i < old_tn; ++i) {
            if ((int32_t)i != found_tok) new_toks[k++] = old_entry->tokens[i];
        }
    } else if (found_tok >= 0) {
        /* Replace the existing token's quantity. */
        new_tn   = old_tn;
        new_toks = (uplc_rledger_token*)uplc_arena_alloc(
            ar, sizeof(uplc_rledger_token) * new_tn, alignof(uplc_rledger_token));
        memcpy(new_toks, old_entry->tokens, sizeof(uplc_rledger_token) * new_tn);
        new_toks[found_tok].quantity = copy_mpz(ar, amount);
    } else {
        /* Insert a new token in sorted order. */
        new_tn   = old_tn + 1;
        new_toks = (uplc_rledger_token*)uplc_arena_alloc(
            ar, sizeof(uplc_rledger_token) * new_tn, alignof(uplc_rledger_token));
        for (uint32_t i = 0; i < tok_insert; ++i) new_toks[i] = old_entry->tokens[i];
        uint8_t* tok_copy = (uint8_t*)uplc_arena_alloc(ar, tok_len ? tok_len : 1, 1);
        if (tok_len) memcpy(tok_copy, tok, tok_len);
        new_toks[tok_insert].name_bytes = tok_copy;
        new_toks[tok_insert].name_len   = tok_len;
        new_toks[tok_insert].quantity   = copy_mpz(ar, amount);
        for (uint32_t i = tok_insert; i < old_tn; ++i) new_toks[i + 1] = old_entry->tokens[i];
    }

    /* Clone the outer entries with the patched token list. */
    uplc_rledger_entry* es = (uplc_rledger_entry*)uplc_arena_alloc(
        ar, sizeof(uplc_rledger_entry) * old_n, alignof(uplc_rledger_entry));
    for (uint32_t i = 0; i < old_n; ++i) es[i] = old->entries[i];
    es[found_cur].tokens   = new_toks;
    es[found_cur].n_tokens = new_tn;

    uplc_rledger_value* out = alloc_empty_value(ar);
    out->entries   = es;
    out->n_entries = old_n;
    return out;
}

uplc_value uplcrt_builtin_insertCoin(uplc_budget* b, uplc_value* a) {
    uint32_t cur_len = 0, tok_len = 0;
    const uint8_t* cur = uplcrt_unwrap_bytestring(b, a[0], &cur_len);
    const uint8_t* tok = uplcrt_unwrap_bytestring(b, a[1], &tok_len);
    mpz_srcptr     amt = uplcrt_unwrap_integer  (b, a[2]);
    const uplc_rledger_value* old = unwrap_value(b, a[3]);

    uplc_arena* ar = uplcrt_budget_arena(b);
    return result_value(b, insert_coin_set(ar, b, cur, cur_len, tok, tok_len, amt, old));
}

/* ---------------------------------------------------------------------------
 * lookupCoin
 *
 *   lookupCoin (cur : ByteString) (tok : ByteString) (val : Value) -> Integer
 *
 * Returns 0 if the token is absent. Long keys simply miss; they do not
 * trigger EvaluationFailure.
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_lookupCoin(uplc_budget* b, uplc_value* a) {
    uint32_t cur_len = 0, tok_len = 0;
    const uint8_t* cur = uplcrt_unwrap_bytestring(b, a[0], &cur_len);
    const uint8_t* tok = uplcrt_unwrap_bytestring(b, a[1], &tok_len);
    const uplc_rledger_value* v = unwrap_value(b, a[2]);

    for (uint32_t i = 0; i < v->n_entries; ++i) {
        const uplc_rledger_entry* e = &v->entries[i];
        if (bytecmp(e->currency_bytes, e->currency_len, cur, cur_len) != 0) continue;
        for (uint32_t j = 0; j < e->n_tokens; ++j) {
            if (bytecmp(e->tokens[j].name_bytes, e->tokens[j].name_len,
                        tok, tok_len) == 0) {
                return uplcrt_result_integer_mpz(b, e->tokens[j].quantity);
            }
        }
        break;  /* currency matched but token absent */
    }
    return uplcrt_result_integer_si(b, 0);
}

/* ---------------------------------------------------------------------------
 * unionValue
 *
 *   unionValue (v1 : Value) (v2 : Value) -> Value
 *
 * Pointwise sum. Zero tokens drop out of the result.
 * ------------------------------------------------------------------------- */

/* Merge two sorted currency maps — produces a new sorted currency map. */
static uplc_rledger_value* union_values(uplc_arena* ar,
                                         const uplc_rledger_value* x,
                                         const uplc_rledger_value* y) {
    uint32_t xi = 0, yi = 0;
    uint32_t cap = x->n_entries + y->n_entries;
    uplc_rledger_entry* out_es = NULL;
    uint32_t out_n = 0;
    if (cap > 0) {
        out_es = (uplc_rledger_entry*)uplc_arena_alloc(
            ar, sizeof(uplc_rledger_entry) * cap, alignof(uplc_rledger_entry));
    }

    while (xi < x->n_entries && yi < y->n_entries) {
        const uplc_rledger_entry* xe = &x->entries[xi];
        const uplc_rledger_entry* ye = &y->entries[yi];
        int cmp = bytecmp(xe->currency_bytes, xe->currency_len,
                          ye->currency_bytes, ye->currency_len);
        if (cmp < 0) {
            out_es[out_n++] = *xe;
            ++xi;
        } else if (cmp > 0) {
            out_es[out_n++] = *ye;
            ++yi;
        } else {
            /* Merge token maps; drop tokens that sum to zero. */
            uint32_t txi = 0, tyi = 0;
            uint32_t tcap = xe->n_tokens + ye->n_tokens;
            uplc_rledger_token* merged = (uplc_rledger_token*)uplc_arena_alloc(
                ar, sizeof(uplc_rledger_token) * (tcap ? tcap : 1),
                alignof(uplc_rledger_token));
            uint32_t tn = 0;
            while (txi < xe->n_tokens && tyi < ye->n_tokens) {
                const uplc_rledger_token* xt = &xe->tokens[txi];
                const uplc_rledger_token* yt = &ye->tokens[tyi];
                int tc = bytecmp(xt->name_bytes, xt->name_len,
                                 yt->name_bytes, yt->name_len);
                if (tc < 0) {
                    merged[tn++] = *xt;
                    ++txi;
                } else if (tc > 0) {
                    merged[tn++] = *yt;
                    ++tyi;
                } else {
                    mpz_ptr s = uplc_arena_alloc_mpz(ar);
                    mpz_add(s, xt->quantity, yt->quantity);
                    if (mpz_sgn(s) != 0) {
                        merged[tn].name_bytes = xt->name_bytes;
                        merged[tn].name_len   = xt->name_len;
                        merged[tn].quantity   = s;
                        ++tn;
                    }
                    ++txi;
                    ++tyi;
                }
            }
            while (txi < xe->n_tokens) merged[tn++] = xe->tokens[txi++];
            while (tyi < ye->n_tokens) merged[tn++] = ye->tokens[tyi++];

            if (tn > 0) {
                out_es[out_n].currency_bytes = xe->currency_bytes;
                out_es[out_n].currency_len   = xe->currency_len;
                out_es[out_n].tokens         = merged;
                out_es[out_n].n_tokens       = tn;
                ++out_n;
            }
            ++xi;
            ++yi;
        }
    }
    while (xi < x->n_entries) out_es[out_n++] = x->entries[xi++];
    while (yi < y->n_entries) out_es[out_n++] = y->entries[yi++];

    uplc_rledger_value* out = alloc_empty_value(ar);
    out->entries   = out_n == 0 ? NULL : out_es;
    out->n_entries = out_n;
    return out;
}

uplc_value uplcrt_builtin_unionValue(uplc_budget* b, uplc_value* a) {
    const uplc_rledger_value* x = unwrap_value(b, a[0]);
    const uplc_rledger_value* y = unwrap_value(b, a[1]);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uplc_rledger_value* out = union_values(ar, x, y);

    /* Enforce the Value amount bound [-2^127, 2^127) on every summed quantity. */
    for (uint32_t i = 0; i < out->n_entries; ++i) {
        const uplc_rledger_entry* e = &out->entries[i];
        for (uint32_t j = 0; j < e->n_tokens; ++j) {
            if (!value_in_range(e->tokens[j].quantity)) {
                uplcrt_fail(b, UPLC_FAIL_EVALUATION);
            }
        }
    }
    return result_value(b, out);
}

/* ---------------------------------------------------------------------------
 * valueContains
 *
 *   valueContains (outer : Value) (inner : Value) -> Bool
 *
 * True iff for every (cur, tok) in `inner`, outer[cur][tok] >= inner[cur][tok].
 * Negative quantities flip the inequality (inner may carry debts).
 * ------------------------------------------------------------------------- */
/* Abort if any token amount in `v` is negative. valueContains is only
 * defined for non-negative values; the reference CEK raises on negatives. */
static void require_nonneg(uplc_budget* b, const uplc_rledger_value* v) {
    for (uint32_t i = 0; i < v->n_entries; ++i) {
        const uplc_rledger_entry* e = &v->entries[i];
        for (uint32_t j = 0; j < e->n_tokens; ++j) {
            if (mpz_sgn(e->tokens[j].quantity) < 0) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
        }
    }
}

uplc_value uplcrt_builtin_valueContains(uplc_budget* b, uplc_value* a) {
    const uplc_rledger_value* outer = unwrap_value(b, a[0]);
    const uplc_rledger_value* inner = unwrap_value(b, a[1]);

    /* Both operands must be non-negative. */
    require_nonneg(b, outer);
    require_nonneg(b, inner);

    for (uint32_t ii = 0; ii < inner->n_entries; ++ii) {
        const uplc_rledger_entry* ie = &inner->entries[ii];
        const uplc_rledger_entry* oe = NULL;
        for (uint32_t oi = 0; oi < outer->n_entries; ++oi) {
            if (bytecmp(outer->entries[oi].currency_bytes,
                        outer->entries[oi].currency_len,
                        ie->currency_bytes, ie->currency_len) == 0) {
                oe = &outer->entries[oi];
                break;
            }
        }
        for (uint32_t tj = 0; tj < ie->n_tokens; ++tj) {
            const uplc_rledger_token* it = &ie->tokens[tj];
            mpz_srcptr outer_q = NULL;
            if (oe) {
                for (uint32_t tk = 0; tk < oe->n_tokens; ++tk) {
                    if (bytecmp(oe->tokens[tk].name_bytes, oe->tokens[tk].name_len,
                                it->name_bytes, it->name_len) == 0) {
                        outer_q = oe->tokens[tk].quantity;
                        break;
                    }
                }
            }
            if (outer_q) {
                if (mpz_cmp(outer_q, it->quantity) < 0) {
                    return uplcrt_result_bool(b, false);
                }
            } else {
                if (mpz_sgn(it->quantity) > 0) {
                    return uplcrt_result_bool(b, false);
                }
            }
        }
    }
    return uplcrt_result_bool(b, true);
}

/* ---------------------------------------------------------------------------
 * valueData — Value → Data (nested Map encoding)
 *
 *   valueData v = Map [ (B cur, Map [ (B tok, I qty), ... ]), ... ]
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_valueData(uplc_budget* b, uplc_value* a) {
    const uplc_rledger_value* v = unwrap_value(b, a[0]);
    uplc_arena* ar = uplcrt_budget_arena(b);

    uplc_rdata_pair* outer = NULL;
    if (v->n_entries > 0) {
        outer = (uplc_rdata_pair*)uplc_arena_alloc(
            ar, sizeof(uplc_rdata_pair) * v->n_entries, alignof(uplc_rdata_pair));
    }
    for (uint32_t i = 0; i < v->n_entries; ++i) {
        const uplc_rledger_entry* e = &v->entries[i];
        outer[i].key = uplc_rdata_bytestring(ar, e->currency_bytes, e->currency_len);

        uplc_rdata_pair* inner = NULL;
        if (e->n_tokens > 0) {
            inner = (uplc_rdata_pair*)uplc_arena_alloc(
                ar, sizeof(uplc_rdata_pair) * e->n_tokens, alignof(uplc_rdata_pair));
        }
        for (uint32_t j = 0; j < e->n_tokens; ++j) {
            inner[j].key   = uplc_rdata_bytestring(ar, e->tokens[j].name_bytes,
                                                    e->tokens[j].name_len);
            inner[j].value = uplc_rdata_integer_mpz(ar, e->tokens[j].quantity);
        }
        outer[i].value = uplc_rdata_map(ar, inner, e->n_tokens);
    }

    uplc_rdata* d = uplc_rdata_map(ar, outer, v->n_entries);
    return uplcrt_result_data(b, d);
}

/* ---------------------------------------------------------------------------
 * unValueData — Data → Value
 *
 * Fails (EvaluationFailure) if the Data is not a properly-shaped nested Map,
 * or if any key exceeds 32 bytes, or if the result cannot be canonicalised
 * (duplicate keys in already-sorted input, etc.).
 * ------------------------------------------------------------------------- */
static void require(uplc_budget* b, bool cond) {
    if (!cond) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
}

uplc_value uplcrt_builtin_unValueData(uplc_budget* b, uplc_value* a) {
    const uplc_rdata* d = uplcrt_unwrap_data(b, a[0]);
    require(b, d && d->tag == UPLC_RDATA_MAP);

    uplc_arena* ar = uplcrt_budget_arena(b);

    uint32_t outer_n = d->map.n_entries;
    uplc_rledger_entry* es = NULL;
    if (outer_n > 0) {
        es = (uplc_rledger_entry*)uplc_arena_alloc(
            ar, sizeof(uplc_rledger_entry) * outer_n, alignof(uplc_rledger_entry));
    }

    /* Walk the outer map strictly in order. Reject:
     *   - non-BS keys / non-Map values at the outer level
     *   - keys > 32 bytes
     *   - out-of-order or duplicate currency keys
     *   - empty token maps
     *   - non-BS token keys or non-Int token values
     *   - token keys > 32 bytes
     *   - out-of-order or duplicate token keys
     *   - zero-quantity tokens
     *   - |quantity| >= 2^127
     */
    for (uint32_t i = 0; i < outer_n; ++i) {
        const uplc_rdata* k = d->map.entries[i].key;
        const uplc_rdata* v = d->map.entries[i].value;
        if (!k || k->tag != UPLC_RDATA_BYTESTRING) goto fail;
        if (!v || v->tag != UPLC_RDATA_MAP)        goto fail;
        if (k->bytestring.len > 32)                goto fail;

        /* Strict ordering vs previous currency. */
        if (i > 0) {
            const uplc_rdata* pk = d->map.entries[i - 1].key;
            int cmp = bytecmp(pk->bytestring.bytes, pk->bytestring.len,
                              k->bytestring.bytes,  k->bytestring.len);
            if (cmp >= 0) goto fail;  /* unordered or duplicate */
        }

        uint32_t inner_n = v->map.n_entries;
        if (inner_n == 0) goto fail;  /* empty token maps are non-canonical */

        uplc_rledger_token* toks = (uplc_rledger_token*)uplc_arena_alloc(
            ar, sizeof(uplc_rledger_token) * inner_n, alignof(uplc_rledger_token));

        for (uint32_t j = 0; j < inner_n; ++j) {
            const uplc_rdata* tk = v->map.entries[j].key;
            const uplc_rdata* tv = v->map.entries[j].value;
            if (!tk || tk->tag != UPLC_RDATA_BYTESTRING) goto fail;
            if (!tv || tv->tag != UPLC_RDATA_INTEGER)    goto fail;
            if (tk->bytestring.len > 32)                 goto fail;
            if (mpz_sgn(tv->integer.value) == 0)         goto fail;

            /* q ∈ [-2^127, 2^127) */
            if (!value_in_range(tv->integer.value)) goto fail;

            if (j > 0) {
                const uplc_rdata* ptk = v->map.entries[j - 1].key;
                int cmp = bytecmp(ptk->bytestring.bytes, ptk->bytestring.len,
                                  tk->bytestring.bytes,   tk->bytestring.len);
                if (cmp >= 0) goto fail;
            }

            toks[j].name_bytes = tk->bytestring.bytes;
            toks[j].name_len   = tk->bytestring.len;
            toks[j].quantity   = copy_mpz(ar, tv->integer.value);
        }

        es[i].currency_bytes = k->bytestring.bytes;
        es[i].currency_len   = k->bytestring.len;
        es[i].tokens         = toks;
        es[i].n_tokens       = inner_n;
    }

    uplc_rledger_value* out = alloc_empty_value(ar);
    out->entries   = outer_n == 0 ? NULL : es;
    out->n_entries = outer_n;
    return result_value(b, out);

fail:
    uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    __builtin_unreachable();
}

/* ---------------------------------------------------------------------------
 * scaleValue — multiply every amount by a scalar
 *
 *   scaleValue (k : Integer) (v : Value) -> Value
 *
 * k == 0 yields the empty Value.
 * ------------------------------------------------------------------------- */
uplc_value uplcrt_builtin_scaleValue(uplc_budget* b, uplc_value* a) {
    mpz_srcptr k = uplcrt_unwrap_integer(b, a[0]);
    const uplc_rledger_value* v = unwrap_value(b, a[1]);
    uplc_arena* ar = uplcrt_budget_arena(b);

    if (mpz_sgn(k) == 0) {
        return result_value(b, alloc_empty_value(ar));
    }

    uplc_rledger_entry* es = NULL;
    if (v->n_entries > 0) {
        es = (uplc_rledger_entry*)uplc_arena_alloc(
            ar, sizeof(uplc_rledger_entry) * v->n_entries, alignof(uplc_rledger_entry));
    }
    for (uint32_t i = 0; i < v->n_entries; ++i) {
        const uplc_rledger_entry* e = &v->entries[i];
        uplc_rledger_token* toks = NULL;
        if (e->n_tokens > 0) {
            toks = (uplc_rledger_token*)uplc_arena_alloc(
                ar, sizeof(uplc_rledger_token) * e->n_tokens, alignof(uplc_rledger_token));
        }
        for (uint32_t j = 0; j < e->n_tokens; ++j) {
            mpz_ptr q = uplc_arena_alloc_mpz(ar);
            mpz_mul(q, e->tokens[j].quantity, k);
            if (!value_in_range(q)) {
                uplcrt_fail(b, UPLC_FAIL_EVALUATION);
            }
            toks[j].name_bytes = e->tokens[j].name_bytes;
            toks[j].name_len   = e->tokens[j].name_len;
            toks[j].quantity   = q;
        }
        es[i].currency_bytes = e->currency_bytes;
        es[i].currency_len   = e->currency_len;
        es[i].tokens         = toks;
        es[i].n_tokens       = e->n_tokens;
    }

    uplc_rledger_value* out = alloc_empty_value(ar);
    out->entries   = es;
    out->n_entries = v->n_entries;
    return result_value(b, out);
}
