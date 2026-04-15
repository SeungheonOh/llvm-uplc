#include "compiler/lowering.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "compiler/ast/term.h"

namespace uplc {

namespace {

uplc_rdata* lower_data(uplc_arena* a, const PlutusData& d);
uplc_rconstant* lower_constant(uplc_arena* a, const Constant& c);

uplc_rtype* lower_type(uplc_arena* a, const ConstantType& t) {
    switch (t.tag) {
        case ConstantTypeTag::Integer:
            return uplc_rtype_simple(a, UPLC_RTYPE_INTEGER);
        case ConstantTypeTag::ByteString:
            return uplc_rtype_simple(a, UPLC_RTYPE_BYTESTRING);
        case ConstantTypeTag::String:
            return uplc_rtype_simple(a, UPLC_RTYPE_STRING);
        case ConstantTypeTag::Unit:
            return uplc_rtype_simple(a, UPLC_RTYPE_UNIT);
        case ConstantTypeTag::Bool:
            return uplc_rtype_simple(a, UPLC_RTYPE_BOOL);
        case ConstantTypeTag::Data:
            return uplc_rtype_simple(a, UPLC_RTYPE_DATA);
        case ConstantTypeTag::Bls12_381_G1:
            return uplc_rtype_simple(a, UPLC_RTYPE_BLS12_381_G1);
        case ConstantTypeTag::Bls12_381_G2:
            return uplc_rtype_simple(a, UPLC_RTYPE_BLS12_381_G2);
        case ConstantTypeTag::Bls12_381_MlResult:
            return uplc_rtype_simple(a, UPLC_RTYPE_BLS12_381_ML_RESULT);
        case ConstantTypeTag::Value:
            return uplc_rtype_simple(a, UPLC_RTYPE_VALUE);
        case ConstantTypeTag::List:
            return uplc_rtype_list(a, lower_type(a, *t.list.element));
        case ConstantTypeTag::Array:
            return uplc_rtype_array(a, lower_type(a, *t.array.element));
        case ConstantTypeTag::Pair:
            return uplc_rtype_pair(a,
                                   lower_type(a, *t.pair.first),
                                   lower_type(a, *t.pair.second));
    }
    throw std::runtime_error("lower_type: unknown tag");
}

uplc_rdata* lower_data(uplc_arena* a, const PlutusData& d) {
    switch (d.tag) {
        case PlutusDataTag::Integer:
            return uplc_rdata_integer_mpz(a, d.integer.value->value);
        case PlutusDataTag::ByteString:
            return uplc_rdata_bytestring(a, d.bytestring.bytes, d.bytestring.len);
        case PlutusDataTag::List: {
            auto n = d.list.n_values;
            auto* arr = static_cast<uplc_rdata**>(
                uplc_arena_alloc(a, sizeof(uplc_rdata*) * n, alignof(uplc_rdata*)));
            for (std::uint32_t i = 0; i < n; ++i) {
                arr[i] = lower_data(a, *d.list.values[i]);
            }
            return uplc_rdata_list(a, arr, n);
        }
        case PlutusDataTag::Map: {
            auto n = d.map.n_entries;
            auto* arr = static_cast<uplc_rdata_pair*>(uplc_arena_alloc(
                a, sizeof(uplc_rdata_pair) * n, alignof(uplc_rdata_pair)));
            for (std::uint32_t i = 0; i < n; ++i) {
                arr[i].key   = lower_data(a, *d.map.entries[i].key);
                arr[i].value = lower_data(a, *d.map.entries[i].value);
            }
            return uplc_rdata_map(a, arr, n);
        }
        case PlutusDataTag::Constr: {
            auto n = d.constr.n_fields;
            auto* arr = static_cast<uplc_rdata**>(
                uplc_arena_alloc(a, sizeof(uplc_rdata*) * n, alignof(uplc_rdata*)));
            for (std::uint32_t i = 0; i < n; ++i) {
                arr[i] = lower_data(a, *d.constr.fields[i]);
            }
            return uplc_rdata_constr(a, d.constr.index->value, arr, n);
        }
    }
    throw std::runtime_error("lower_data: unknown tag");
}

uplc_rconstant* lower_constant(uplc_arena* a, const Constant& c) {
    switch (c.tag) {
        case ConstTag::Integer:
            return uplc_rconst_integer_mpz(a, c.integer.value->value);
        case ConstTag::ByteString:
            return uplc_rconst_bytestring(a, c.bytestring.bytes, c.bytestring.len);
        case ConstTag::String:
            return uplc_rconst_string(a, c.string.utf8, c.string.len);
        case ConstTag::Bool:
            return uplc_rconst_bool(a, c.boolean.value);
        case ConstTag::Unit:
            return uplc_rconst_unit(a);
        case ConstTag::Data:
            return uplc_rconst_data(a, lower_data(a, *c.data.value));

        case ConstTag::List: {
            uplc_rtype* item_type = lower_type(a, *c.list.item_type);
            auto n = c.list.n_values;
            auto* values = static_cast<uplc_rconstant**>(uplc_arena_alloc(
                a, sizeof(uplc_rconstant*) * n, alignof(uplc_rconstant*)));
            for (std::uint32_t i = 0; i < n; ++i) {
                values[i] = lower_constant(a, *c.list.values[i]);
            }
            auto* out = static_cast<uplc_rconstant*>(
                uplc_arena_alloc(a, sizeof(uplc_rconstant), alignof(uplc_rconstant)));
            std::memset(out, 0, sizeof(*out));
            out->tag = UPLC_RCONST_LIST;
            out->list.item_type = item_type;
            out->list.values = values;
            out->list.n_values = n;
            return out;
        }
        case ConstTag::Pair: {
            uplc_rtype* fst_type = lower_type(a, *c.pair.fst_type);
            uplc_rtype* snd_type = lower_type(a, *c.pair.snd_type);
            uplc_rconstant* first = lower_constant(a, *c.pair.first);
            uplc_rconstant* second = lower_constant(a, *c.pair.second);
            auto* out = static_cast<uplc_rconstant*>(
                uplc_arena_alloc(a, sizeof(uplc_rconstant), alignof(uplc_rconstant)));
            std::memset(out, 0, sizeof(*out));
            out->tag = UPLC_RCONST_PAIR;
            out->pair.fst_type = fst_type;
            out->pair.snd_type = snd_type;
            out->pair.first = first;
            out->pair.second = second;
            return out;
        }
        case ConstTag::Array: {
            uplc_rtype* item_type = lower_type(a, *c.array.item_type);
            auto n = c.array.n_values;
            auto* values = static_cast<uplc_rconstant**>(uplc_arena_alloc(
                a, sizeof(uplc_rconstant*) * n, alignof(uplc_rconstant*)));
            for (std::uint32_t i = 0; i < n; ++i) {
                values[i] = lower_constant(a, *c.array.values[i]);
            }
            auto* out = static_cast<uplc_rconstant*>(
                uplc_arena_alloc(a, sizeof(uplc_rconstant), alignof(uplc_rconstant)));
            std::memset(out, 0, sizeof(*out));
            out->tag = UPLC_RCONST_ARRAY;
            out->array.item_type = item_type;
            out->array.values = values;
            out->array.n_values = n;
            return out;
        }

        case ConstTag::Bls12_381_G1:
            return uplc_rconst_bls_g1(a, c.bls_g1.bytes);
        case ConstTag::Bls12_381_G2:
            return uplc_rconst_bls_g2(a, c.bls_g2.bytes);
        case ConstTag::Bls12_381_MlResult:
            return uplc_rconst_bls_ml_result(a, c.bls_ml_result.bytes,
                                             c.bls_ml_result.len);
        case ConstTag::Value: {
            /* Translate compiler-side LedgerValue → runtime uplc_rledger_value.
             * The parser has already canonicalised the input (sorted, zero
             * tokens dropped), so we can copy entries directly. */
            const LedgerValue& lv = *c.value.value;
            auto* out = static_cast<uplc_rledger_value*>(uplc_arena_alloc(
                a, sizeof(uplc_rledger_value), alignof(uplc_rledger_value)));
            std::memset(out, 0, sizeof(*out));
            out->n_entries = lv.n_entries;
            if (lv.n_entries > 0) {
                auto* es = static_cast<uplc_rledger_entry*>(uplc_arena_alloc(
                    a, sizeof(uplc_rledger_entry) * lv.n_entries,
                    alignof(uplc_rledger_entry)));
                for (std::uint32_t i = 0; i < lv.n_entries; ++i) {
                    const LedgerValueEntry& src = lv.entries[i];
                    uplc_rledger_entry& dst = es[i];
                    dst.currency_bytes = src.currency_bytes;
                    dst.currency_len   = src.currency_len;
                    dst.n_tokens       = src.n_tokens;
                    if (src.n_tokens > 0) {
                        auto* toks = static_cast<uplc_rledger_token*>(
                            uplc_arena_alloc(
                                a, sizeof(uplc_rledger_token) * src.n_tokens,
                                alignof(uplc_rledger_token)));
                        for (std::uint32_t j = 0; j < src.n_tokens; ++j) {
                            toks[j].name_bytes = src.tokens[j].name_bytes;
                            toks[j].name_len   = src.tokens[j].name_len;
                            /* Copy quantity mpz into the runtime arena. */
                            mpz_ptr q = uplc_arena_alloc_mpz(a);
                            mpz_set(q, src.tokens[j].quantity->value);
                            toks[j].quantity = q;
                        }
                        dst.tokens = toks;
                    } else {
                        dst.tokens = nullptr;
                    }
                }
                out->entries = es;
            } else {
                out->entries = nullptr;
            }
            return uplc_rconst_ledger_value(a, out);
        }
    }
    throw std::runtime_error("lower_constant: unknown tag");
}

uplc_rterm* lower_term(uplc_arena* a, const Term& term) {
    switch (term.tag) {
        case TermTag::Var:
            return uplc_rterm_var(a, term.var.binder.id);
        case TermTag::Lambda:
            return uplc_rterm_lambda(a, lower_term(a, *term.lambda.body));
        case TermTag::Apply:
            return uplc_rterm_apply(a,
                                    lower_term(a, *term.apply.function),
                                    lower_term(a, *term.apply.argument));
        case TermTag::Delay:
            return uplc_rterm_delay(a, lower_term(a, *term.delay.term));
        case TermTag::Force:
            return uplc_rterm_force(a, lower_term(a, *term.force.term));
        case TermTag::Constant:
            return uplc_rterm_constant(a, lower_constant(a, *term.constant.value));
        case TermTag::Builtin:
            return uplc_rterm_builtin(a,
                static_cast<std::uint8_t>(term.builtin.function));
        case TermTag::Error:
            return uplc_rterm_error(a);
        case TermTag::Constr: {
            auto n = term.constr.n_fields;
            auto* fields = static_cast<uplc_rterm**>(
                uplc_arena_alloc(a, sizeof(uplc_rterm*) * n, alignof(uplc_rterm*)));
            for (std::uint32_t i = 0; i < n; ++i) {
                fields[i] = lower_term(a, *term.constr.fields[i]);
            }
            return uplc_rterm_constr(a, term.constr.tag_index, fields, n);
        }
        case TermTag::Case: {
            auto n = term.case_.n_branches;
            auto* branches = static_cast<uplc_rterm**>(
                uplc_arena_alloc(a, sizeof(uplc_rterm*) * n, alignof(uplc_rterm*)));
            for (std::uint32_t i = 0; i < n; ++i) {
                branches[i] = lower_term(a, *term.case_.branches[i]);
            }
            return uplc_rterm_case(a,
                                   lower_term(a, *term.case_.scrutinee),
                                   branches, n);
        }
    }
    throw std::runtime_error("lower_term: unknown tag");
}

}  // namespace

uplc_rprogram lower_to_runtime(uplc_arena* rt_arena, const Program& program) {
    if (!program.is_debruijn) {
        throw std::runtime_error(
            "lower_to_runtime: input must already be in de Bruijn form");
    }
    uplc_rprogram out;
    out.version.major = program.version.major;
    out.version.minor = program.version.minor;
    out.version.patch = program.version.patch;
    out.term = lower_term(rt_arena, *program.term);
    return out;
}

// ---------------------------------------------------------------------------
// Reverse lowering: rterm → compiler Term (used by the M4 conformance runner)
// ---------------------------------------------------------------------------

namespace {

ConstantType* lift_type(Arena& a, const uplc_rtype& t);
Constant*     lift_constant(Arena& a, const uplc_rconstant& c);
PlutusData*   lift_data(Arena& a, const uplc_rdata& d);

ConstantType* lift_type(Arena& a, const uplc_rtype& t) {
    switch ((uplc_rtype_tag)t.tag) {
        case UPLC_RTYPE_INTEGER:             return make_type_simple(a, ConstantTypeTag::Integer);
        case UPLC_RTYPE_BYTESTRING:          return make_type_simple(a, ConstantTypeTag::ByteString);
        case UPLC_RTYPE_STRING:              return make_type_simple(a, ConstantTypeTag::String);
        case UPLC_RTYPE_UNIT:                return make_type_simple(a, ConstantTypeTag::Unit);
        case UPLC_RTYPE_BOOL:                return make_type_simple(a, ConstantTypeTag::Bool);
        case UPLC_RTYPE_DATA:                return make_type_simple(a, ConstantTypeTag::Data);
        case UPLC_RTYPE_BLS12_381_G1:        return make_type_simple(a, ConstantTypeTag::Bls12_381_G1);
        case UPLC_RTYPE_BLS12_381_G2:        return make_type_simple(a, ConstantTypeTag::Bls12_381_G2);
        case UPLC_RTYPE_BLS12_381_ML_RESULT: return make_type_simple(a, ConstantTypeTag::Bls12_381_MlResult);
        case UPLC_RTYPE_VALUE:               return make_type_simple(a, ConstantTypeTag::Value);
        case UPLC_RTYPE_LIST:                return make_type_list(a, lift_type(a, *t.list.element));
        case UPLC_RTYPE_ARRAY:               return make_type_array(a, lift_type(a, *t.array.element));
        case UPLC_RTYPE_PAIR:                return make_type_pair(a, lift_type(a, *t.pair.first),
                                                                    lift_type(a, *t.pair.second));
    }
    throw std::runtime_error("lift_type: unknown runtime rtype tag");
}

PlutusData* lift_data(Arena& a, const uplc_rdata& d) {
    switch ((uplc_rdata_tag)d.tag) {
        case UPLC_RDATA_INTEGER: {
            BigInt* v = a.make_bigint();
            mpz_set(v->value, d.integer.value);
            return make_data_integer(a, v);
        }
        case UPLC_RDATA_BYTESTRING:
            return make_data_bytestring(a, d.bytestring.bytes, d.bytestring.len);
        case UPLC_RDATA_LIST: {
            auto n = d.list.n_values;
            auto* arr = a.alloc_array_uninit<PlutusData*>(n);
            for (std::uint32_t i = 0; i < n; ++i) arr[i] = lift_data(a, *d.list.values[i]);
            return make_data_list(a, arr, n);
        }
        case UPLC_RDATA_MAP: {
            auto n = d.map.n_entries;
            auto* arr = a.alloc_array_uninit<PlutusDataPair>(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                arr[i].key   = lift_data(a, *d.map.entries[i].key);
                arr[i].value = lift_data(a, *d.map.entries[i].value);
            }
            return make_data_map(a, arr, n);
        }
        case UPLC_RDATA_CONSTR: {
            BigInt* idx = a.make_bigint();
            mpz_set(idx->value, d.constr.index);
            auto n = d.constr.n_fields;
            auto* arr = a.alloc_array_uninit<PlutusData*>(n);
            for (std::uint32_t i = 0; i < n; ++i) arr[i] = lift_data(a, *d.constr.fields[i]);
            return make_data_constr(a, idx, arr, n);
        }
    }
    throw std::runtime_error("lift_data: unknown runtime rdata tag");
}

Constant* lift_constant(Arena& a, const uplc_rconstant& c) {
    switch ((uplc_rconst_tag)c.tag) {
        case UPLC_RCONST_INTEGER: {
            BigInt* v = a.make_bigint();
            mpz_set(v->value, c.integer.value);
            return make_const_integer(a, v);
        }
        case UPLC_RCONST_BYTESTRING:
            return make_const_bytestring(a, c.bytestring.bytes, c.bytestring.len);
        case UPLC_RCONST_STRING:
            return make_const_string(a, c.string.utf8, c.string.len);
        case UPLC_RCONST_BOOL:
            return make_const_bool(a, c.boolean.value);
        case UPLC_RCONST_UNIT:
            return make_const_unit(a);
        case UPLC_RCONST_DATA:
            return make_const_data(a, lift_data(a, *c.data.value));
        case UPLC_RCONST_LIST: {
            ConstantType* item_type = lift_type(a, *c.list.item_type);
            auto n = c.list.n_values;
            auto* values = a.alloc_array_uninit<Constant*>(n);
            for (std::uint32_t i = 0; i < n; ++i) values[i] = lift_constant(a, *c.list.values[i]);
            return make_const_list(a, item_type, values, n);
        }
        case UPLC_RCONST_PAIR: {
            ConstantType* fst_type = lift_type(a, *c.pair.fst_type);
            ConstantType* snd_type = lift_type(a, *c.pair.snd_type);
            Constant* first  = lift_constant(a, *c.pair.first);
            Constant* second = lift_constant(a, *c.pair.second);
            return make_const_pair(a, fst_type, snd_type, first, second);
        }
        case UPLC_RCONST_ARRAY: {
            ConstantType* item_type = lift_type(a, *c.array.item_type);
            auto n = c.array.n_values;
            auto* values = a.alloc_array_uninit<Constant*>(n);
            for (std::uint32_t i = 0; i < n; ++i) values[i] = lift_constant(a, *c.array.values[i]);
            auto* out = a.alloc<Constant>();
            out->tag = ConstTag::Array;
            out->array.item_type = item_type;
            out->array.values = values;
            out->array.n_values = n;
            return out;
        }
        case UPLC_RCONST_BLS12_381_G1: {
            auto* out = a.alloc<Constant>();
            out->tag = ConstTag::Bls12_381_G1;
            out->bls_g1.bytes = a.intern_bytes(c.bls_g1.bytes, 48);
            return out;
        }
        case UPLC_RCONST_BLS12_381_G2: {
            auto* out = a.alloc<Constant>();
            out->tag = ConstTag::Bls12_381_G2;
            out->bls_g2.bytes = a.intern_bytes(c.bls_g2.bytes, 96);
            return out;
        }
        case UPLC_RCONST_BLS12_381_ML_RESULT: {
            auto* out = a.alloc<Constant>();
            out->tag = ConstTag::Bls12_381_MlResult;
            out->bls_ml_result.bytes =
                a.intern_bytes(c.bls_ml_result.bytes, c.bls_ml_result.len);
            out->bls_ml_result.len = c.bls_ml_result.len;
            return out;
        }
        case UPLC_RCONST_VALUE: {
            const uplc_rledger_value& rv = *c.ledger_value.value;
            LedgerValue* lv = a.alloc<LedgerValue>();
            lv->n_entries = rv.n_entries;
            if (rv.n_entries == 0) {
                lv->entries = nullptr;
            } else {
                auto* es = a.alloc_array_uninit<LedgerValueEntry>(rv.n_entries);
                for (std::uint32_t i = 0; i < rv.n_entries; ++i) {
                    const uplc_rledger_entry& src = rv.entries[i];
                    LedgerValueEntry& dst = es[i];
                    dst.currency_bytes = a.intern_bytes(src.currency_bytes, src.currency_len);
                    dst.currency_len   = src.currency_len;
                    dst.n_tokens       = src.n_tokens;
                    if (src.n_tokens == 0) {
                        dst.tokens = nullptr;
                    } else {
                        auto* toks = a.alloc_array_uninit<LedgerValueToken>(src.n_tokens);
                        for (std::uint32_t j = 0; j < src.n_tokens; ++j) {
                            toks[j].name_bytes = a.intern_bytes(
                                src.tokens[j].name_bytes, src.tokens[j].name_len);
                            toks[j].name_len = src.tokens[j].name_len;
                            BigInt* q = a.make_bigint();
                            mpz_set(q->value, src.tokens[j].quantity);
                            toks[j].quantity = q;
                        }
                        dst.tokens = toks;
                    }
                }
                lv->entries = es;
            }
            auto* out = a.alloc<Constant>();
            out->tag = ConstTag::Value;
            out->value.value = lv;
            return out;
        }
    }
    throw std::runtime_error("lift_constant: unknown runtime rconstant tag");
}

}  // namespace

Term* lift_term_from_runtime(Arena& a, const uplc_rterm& term) {
    switch ((uplc_rterm_tag)term.tag) {
        case UPLC_RTERM_VAR:
            return make_var(a, Binder{term.var.index, nullptr});
        case UPLC_RTERM_LAMBDA:
            return make_lambda(a, Binder{0u, nullptr},
                               lift_term_from_runtime(a, *term.lambda.body));
        case UPLC_RTERM_APPLY:
            return make_apply(a, lift_term_from_runtime(a, *term.apply.fn),
                              lift_term_from_runtime(a, *term.apply.arg));
        case UPLC_RTERM_DELAY:
            return make_delay(a, lift_term_from_runtime(a, *term.delay.term));
        case UPLC_RTERM_FORCE:
            return make_force(a, lift_term_from_runtime(a, *term.force.term));
        case UPLC_RTERM_ERROR:
            return make_error(a);
        case UPLC_RTERM_CONSTANT:
            return make_constant(a, lift_constant(a, *term.constant.value));
        case UPLC_RTERM_BUILTIN: {
            auto tag = builtin_from_tag(term.builtin.tag);
            if (!tag) throw std::runtime_error("lift: unknown builtin tag");
            return make_builtin(a, *tag);
        }
        case UPLC_RTERM_CONSTR: {
            auto n = term.constr.n_fields;
            auto* fields = a.alloc_array_uninit<Term*>(n);
            for (std::uint32_t i = 0; i < n; ++i)
                fields[i] = lift_term_from_runtime(a, *term.constr.fields[i]);
            return make_constr(a, term.constr.tag_index, fields, n);
        }
        case UPLC_RTERM_CASE: {
            auto n = term.case_.n_branches;
            auto* branches = a.alloc_array_uninit<Term*>(n);
            for (std::uint32_t i = 0; i < n; ++i)
                branches[i] = lift_term_from_runtime(a, *term.case_.branches[i]);
            return make_case(a, lift_term_from_runtime(a, *term.case_.scrutinee),
                             branches, n);
        }
    }
    throw std::runtime_error("lift_term_from_runtime: unknown runtime term tag");
}

Program lift_program_from_runtime(Arena& a, const uplc_rprogram& p) {
    Program out;
    out.version = Version{p.version.major, p.version.minor, p.version.patch};
    out.term = lift_term_from_runtime(a, *p.term);
    out.is_debruijn = true;
    return out;
}

}  // namespace uplc
