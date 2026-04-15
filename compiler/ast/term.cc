#include "compiler/ast/term.h"

#include <cstdint>
#include <cstring>

namespace uplc {

// ---------------------------------------------------------------------------
// Term constructors
// ---------------------------------------------------------------------------

Term* make_var(Arena& a, Binder binder) {
    Term* t = a.alloc<Term>();
    t->tag = TermTag::Var;
    t->var.binder = binder;
    return t;
}

Term* make_lambda(Arena& a, Binder parameter, Term* body) {
    Term* t = a.alloc<Term>();
    t->tag = TermTag::Lambda;
    t->lambda.parameter = parameter;
    t->lambda.body = body;
    return t;
}

Term* make_apply(Arena& a, Term* function, Term* argument) {
    Term* t = a.alloc<Term>();
    t->tag = TermTag::Apply;
    t->apply.function = function;
    t->apply.argument = argument;
    return t;
}

Term* make_delay(Arena& a, Term* inner) {
    Term* t = a.alloc<Term>();
    t->tag = TermTag::Delay;
    t->delay.term = inner;
    return t;
}

Term* make_force(Arena& a, Term* inner) {
    Term* t = a.alloc<Term>();
    t->tag = TermTag::Force;
    t->force.term = inner;
    return t;
}

Term* make_error(Arena& a) {
    Term* t = a.alloc<Term>();
    t->tag = TermTag::Error;
    return t;
}

Term* make_builtin(Arena& a, BuiltinTag fn) {
    Term* t = a.alloc<Term>();
    t->tag = TermTag::Builtin;
    t->builtin.function = fn;
    return t;
}

Term* make_constant(Arena& a, Constant* c) {
    Term* t = a.alloc<Term>();
    t->tag = TermTag::Constant;
    t->constant.value = c;
    return t;
}

Term* make_constr(Arena& a, std::uint64_t index, Term** fields, std::uint32_t n_fields) {
    Term* t = a.alloc<Term>();
    t->tag = TermTag::Constr;
    t->constr.tag_index = index;
    t->constr.fields = fields;
    t->constr.n_fields = n_fields;
    return t;
}

Term* make_case(Arena& a, Term* scrutinee, Term** branches, std::uint32_t n_branches) {
    Term* t = a.alloc<Term>();
    t->tag = TermTag::Case;
    t->case_.scrutinee = scrutinee;
    t->case_.branches = branches;
    t->case_.n_branches = n_branches;
    return t;
}

// ---------------------------------------------------------------------------
// Constant constructors
// ---------------------------------------------------------------------------

Constant* make_const_integer(Arena& a, BigInt* value) {
    Constant* c = a.alloc<Constant>();
    c->tag = ConstTag::Integer;
    c->integer.value = value;
    return c;
}

Constant* make_const_bytestring(Arena& a, const std::uint8_t* bytes, std::uint32_t len) {
    Constant* c = a.alloc<Constant>();
    c->tag = ConstTag::ByteString;
    c->bytestring.bytes = bytes;
    c->bytestring.len = len;
    return c;
}

Constant* make_const_string(Arena& a, const char* utf8, std::uint32_t len) {
    Constant* c = a.alloc<Constant>();
    c->tag = ConstTag::String;
    c->string.utf8 = utf8;
    c->string.len = len;
    return c;
}

Constant* make_const_bool(Arena& a, bool value) {
    Constant* c = a.alloc<Constant>();
    c->tag = ConstTag::Bool;
    c->boolean.value = value;
    return c;
}

Constant* make_const_unit(Arena& a) {
    Constant* c = a.alloc<Constant>();
    c->tag = ConstTag::Unit;
    return c;
}

Constant* make_const_data(Arena& a, PlutusData* value) {
    Constant* c = a.alloc<Constant>();
    c->tag = ConstTag::Data;
    c->data.value = value;
    return c;
}

Constant* make_const_list(Arena& a, ConstantType* item_type, Constant** values, std::uint32_t n) {
    Constant* c = a.alloc<Constant>();
    c->tag = ConstTag::List;
    c->list.item_type = item_type;
    c->list.values = values;
    c->list.n_values = n;
    return c;
}

Constant* make_const_pair(Arena& a, ConstantType* fst_type, ConstantType* snd_type,
                          Constant* first, Constant* second) {
    Constant* c = a.alloc<Constant>();
    c->tag = ConstTag::Pair;
    c->pair.fst_type = fst_type;
    c->pair.snd_type = snd_type;
    c->pair.first = first;
    c->pair.second = second;
    return c;
}

// ---------------------------------------------------------------------------
// ConstantType constructors
// ---------------------------------------------------------------------------

ConstantType* make_type_simple(Arena& a, ConstantTypeTag tag) {
    ConstantType* t = a.alloc<ConstantType>();
    std::memset(t, 0, sizeof(*t));
    t->tag = tag;
    return t;
}

ConstantType* make_type_list(Arena& a, ConstantType* element) {
    ConstantType* t = a.alloc<ConstantType>();
    t->tag = ConstantTypeTag::List;
    t->list.element = element;
    return t;
}

ConstantType* make_type_array(Arena& a, ConstantType* element) {
    ConstantType* t = a.alloc<ConstantType>();
    t->tag = ConstantTypeTag::Array;
    t->array.element = element;
    return t;
}

ConstantType* make_type_pair(Arena& a, ConstantType* first, ConstantType* second) {
    ConstantType* t = a.alloc<ConstantType>();
    t->tag = ConstantTypeTag::Pair;
    t->pair.first = first;
    t->pair.second = second;
    return t;
}

// ---------------------------------------------------------------------------
// PlutusData constructors
// ---------------------------------------------------------------------------

PlutusData* make_data_integer(Arena& a, BigInt* value) {
    PlutusData* d = a.alloc<PlutusData>();
    d->tag = PlutusDataTag::Integer;
    d->integer.value = value;
    return d;
}

PlutusData* make_data_bytestring(Arena& a, const std::uint8_t* bytes, std::uint32_t len) {
    PlutusData* d = a.alloc<PlutusData>();
    d->tag = PlutusDataTag::ByteString;
    d->bytestring.bytes = bytes;
    d->bytestring.len = len;
    return d;
}

PlutusData* make_data_list(Arena& a, PlutusData** values, std::uint32_t n) {
    PlutusData* d = a.alloc<PlutusData>();
    d->tag = PlutusDataTag::List;
    d->list.values = values;
    d->list.n_values = n;
    return d;
}

PlutusData* make_data_map(Arena& a, PlutusDataPair* entries, std::uint32_t n) {
    PlutusData* d = a.alloc<PlutusData>();
    d->tag = PlutusDataTag::Map;
    d->map.entries = entries;
    d->map.n_entries = n;
    return d;
}

PlutusData* make_data_constr(Arena& a, BigInt* index, PlutusData** fields, std::uint32_t n) {
    PlutusData* d = a.alloc<PlutusData>();
    d->tag = PlutusDataTag::Constr;
    d->constr.index = index;
    d->constr.fields = fields;
    d->constr.n_fields = n;
    return d;
}

}  // namespace uplc
