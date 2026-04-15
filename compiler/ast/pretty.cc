#include "compiler/ast/pretty.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <gmp.h>

#include "compiler/ast/builtin_tag.h"

namespace uplc {

namespace {

void append_hex(std::string& out, const std::uint8_t* bytes, std::uint32_t len) {
    static const char* kHex = "0123456789abcdef";
    for (std::uint32_t i = 0; i < len; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0f]);
    }
}

void append_bigint(std::string& out, const BigInt& bi) {
    // mpz_get_str allocates; compute the buffer size then hand it a stack
    // buffer when possible.
    std::size_t sz = mpz_sizeinbase(bi.value, 10) + 2;
    std::string tmp(sz, '\0');
    mpz_get_str(tmp.data(), 10, bi.value);
    tmp.resize(std::strlen(tmp.data()));
    out += tmp;
}

void escape_string(std::string& out, const char* s, std::uint32_t len) {
    for (std::uint32_t i = 0; i < len; ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case 0x5c: out += "\\\\"; break;
            case 0x22: out += "\\\""; break;
            case 0x0a: out += "\\n"; break;
            case 0x09: out += "\\t"; break;
            case 0x0d: out += "\\r"; break;
            case 0x07: out += "\\a"; break;
            case 0x08: out += "\\b"; break;
            case 0x0c: out += "\\f"; break;
            case 0x0b: out += "\\v"; break;
            case 0x7f: out += "\\DEL"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
}

void append_type(std::string& out, const ConstantType& t);
void append_constant(std::string& out, const Constant& c);
void append_constant_inner(std::string& out, const Constant& c);
void append_plutus_data(std::string& out, const PlutusData& d);

void append_type(std::string& out, const ConstantType& t) {
    switch (t.tag) {
        case ConstantTypeTag::Integer:             out += "integer"; break;
        case ConstantTypeTag::ByteString:          out += "bytestring"; break;
        case ConstantTypeTag::String:              out += "string"; break;
        case ConstantTypeTag::Bool:                out += "bool"; break;
        case ConstantTypeTag::Unit:                out += "unit"; break;
        case ConstantTypeTag::Data:                out += "data"; break;
        case ConstantTypeTag::Bls12_381_G1:        out += "bls12_381_G1_element"; break;
        case ConstantTypeTag::Bls12_381_G2:        out += "bls12_381_G2_element"; break;
        case ConstantTypeTag::Bls12_381_MlResult:  out += "bls12_381_mlresult"; break;
        case ConstantTypeTag::Value:               out += "value"; break;
        case ConstantTypeTag::List:
            out += "(list ";
            append_type(out, *t.list.element);
            out += ")";
            break;
        case ConstantTypeTag::Array:
            out += "(array ";
            append_type(out, *t.array.element);
            out += ")";
            break;
        case ConstantTypeTag::Pair:
            out += "(pair ";
            append_type(out, *t.pair.first);
            out += " ";
            append_type(out, *t.pair.second);
            out += ")";
            break;
    }
}

void append_plutus_data(std::string& out, const PlutusData& d) {
    switch (d.tag) {
        case PlutusDataTag::Integer:
            out += "I ";
            append_bigint(out, *d.integer.value);
            break;
        case PlutusDataTag::ByteString:
            out += "B #";
            append_hex(out, d.bytestring.bytes, d.bytestring.len);
            break;
        case PlutusDataTag::List: {
            out += "List [";
            for (std::uint32_t i = 0; i < d.list.n_values; ++i) {
                if (i) out += ", ";
                append_plutus_data(out, *d.list.values[i]);
            }
            out += "]";
            break;
        }
        case PlutusDataTag::Map: {
            out += "Map [";
            for (std::uint32_t i = 0; i < d.map.n_entries; ++i) {
                if (i) out += ", ";
                out += "(";
                append_plutus_data(out, *d.map.entries[i].key);
                out += ", ";
                append_plutus_data(out, *d.map.entries[i].value);
                out += ")";
            }
            out += "]";
            break;
        }
        case PlutusDataTag::Constr: {
            out += "Constr ";
            append_bigint(out, *d.constr.index);
            out += " [";
            for (std::uint32_t i = 0; i < d.constr.n_fields; ++i) {
                if (i) out += ", ";
                append_plutus_data(out, *d.constr.fields[i]);
            }
            out += "]";
            break;
        }
    }
}

// Print only the value part of a constant (no type prefix). Used for
// elements of list/pair/array constants and for the inner values of `value`
// entries. Matches the Plutus Haskell pretty printer's nested format.
void append_constant_inner(std::string& out, const Constant& c) {
    switch (c.tag) {
        case ConstTag::Integer:
            append_bigint(out, *c.integer.value);
            break;
        case ConstTag::ByteString:
            out += "#";
            append_hex(out, c.bytestring.bytes, c.bytestring.len);
            break;
        case ConstTag::String:
            out += "\"";
            escape_string(out, c.string.utf8, c.string.len);
            out += "\"";
            break;
        case ConstTag::Bool:
            out += c.boolean.value ? "True" : "False";
            break;
        case ConstTag::Unit:
            out += "()";
            break;
        case ConstTag::Data:
            append_plutus_data(out, *c.data.value);
            break;
        case ConstTag::List: {
            out += "[";
            for (std::uint32_t i = 0; i < c.list.n_values; ++i) {
                if (i) out += ", ";
                append_constant_inner(out, *c.list.values[i]);
            }
            out += "]";
            break;
        }
        case ConstTag::Array: {
            out += "[";
            for (std::uint32_t i = 0; i < c.array.n_values; ++i) {
                if (i) out += ",";
                append_constant_inner(out, *c.array.values[i]);
            }
            out += "]";
            break;
        }
        case ConstTag::Pair: {
            out += "(";
            append_constant_inner(out, *c.pair.first);
            out += ", ";
            append_constant_inner(out, *c.pair.second);
            out += ")";
            break;
        }
        case ConstTag::Bls12_381_G1:
            out += "0x";
            append_hex(out, c.bls_g1.bytes, 48);
            break;
        case ConstTag::Bls12_381_G2:
            out += "0x";
            append_hex(out, c.bls_g2.bytes, 96);
            break;
        case ConstTag::Bls12_381_MlResult:
            out += "...";
            break;
        case ConstTag::Value:
            // Value constants never appear as inner elements in valid UPLC.
            out += "<value as inner>";
            break;
    }
}

void append_constant(std::string& out, const Constant& c) {
    switch (c.tag) {
        case ConstTag::Integer:
            out += "integer ";
            append_bigint(out, *c.integer.value);
            break;
        case ConstTag::ByteString:
            out += "bytestring #";
            append_hex(out, c.bytestring.bytes, c.bytestring.len);
            break;
        case ConstTag::String:
            out += "string \"";
            escape_string(out, c.string.utf8, c.string.len);
            out += "\"";
            break;
        case ConstTag::Bool:
            out += c.boolean.value ? "bool True" : "bool False";
            break;
        case ConstTag::Unit:
            out += "unit ()";
            break;
        case ConstTag::Data:
            out += "data (";
            append_plutus_data(out, *c.data.value);
            out += ")";
            break;
        case ConstTag::List: {
            out += "(list ";
            append_type(out, *c.list.item_type);
            out += ") [";
            for (std::uint32_t i = 0; i < c.list.n_values; ++i) {
                if (i) out += ", ";
                append_constant_inner(out, *c.list.values[i]);
            }
            out += "]";
            break;
        }
        case ConstTag::Array: {
            out += "(array ";
            append_type(out, *c.array.item_type);
            out += ") [";
            for (std::uint32_t i = 0; i < c.array.n_values; ++i) {
                if (i) out += ",";
                append_constant_inner(out, *c.array.values[i]);
            }
            out += "]";
            break;
        }
        case ConstTag::Pair: {
            out += "(pair ";
            append_type(out, *c.pair.fst_type);
            out += " ";
            append_type(out, *c.pair.snd_type);
            out += ") (";
            append_constant_inner(out, *c.pair.first);
            out += ", ";
            append_constant_inner(out, *c.pair.second);
            out += ")";
            break;
        }
        case ConstTag::Bls12_381_G1:
            out += "bls12_381_G1_element 0x";
            append_hex(out, c.bls_g1.bytes, 48);
            break;
        case ConstTag::Bls12_381_G2:
            out += "bls12_381_G2_element 0x";
            append_hex(out, c.bls_g2.bytes, 96);
            break;
        case ConstTag::Bls12_381_MlResult:
            out += "bls12_381_mlresult ...";
            break;
        case ConstTag::Value: {
            out += "value [";
            const LedgerValue& lv = *c.value.value;
            for (std::uint32_t i = 0; i < lv.n_entries; ++i) {
                if (i) out += ", ";
                const LedgerValueEntry& e = lv.entries[i];
                out += "(#";
                append_hex(out, e.currency_bytes, e.currency_len);
                out += ", [";
                for (std::uint32_t j = 0; j < e.n_tokens; ++j) {
                    if (j) out += ", ";
                    out += "(#";
                    append_hex(out, e.tokens[j].name_bytes, e.tokens[j].name_len);
                    out += ", ";
                    append_bigint(out, *e.tokens[j].quantity);
                    out += ")";
                }
                out += "])";
            }
            out += "]";
            break;
        }
    }
}

void append_term_debruijn(std::string& out, Term* term) {
    switch (term->tag) {
        case TermTag::Var:
            out += "i";
            out += std::to_string(term->var.binder.id);
            break;
        case TermTag::Lambda:
            out += "(lam i";
            out += std::to_string(term->lambda.parameter.id);
            out += " ";
            append_term_debruijn(out, term->lambda.body);
            out += ")";
            break;
        case TermTag::Apply:
            out += "[";
            append_term_debruijn(out, term->apply.function);
            out += " ";
            append_term_debruijn(out, term->apply.argument);
            out += "]";
            break;
        case TermTag::Delay:
            out += "(delay ";
            append_term_debruijn(out, term->delay.term);
            out += ")";
            break;
        case TermTag::Force:
            out += "(force ";
            append_term_debruijn(out, term->force.term);
            out += ")";
            break;
        case TermTag::Constr: {
            out += "(constr ";
            out += std::to_string(term->constr.tag_index);
            for (std::uint32_t i = 0; i < term->constr.n_fields; ++i) {
                out += " ";
                append_term_debruijn(out, term->constr.fields[i]);
            }
            out += ")";
            break;
        }
        case TermTag::Case: {
            out += "(case ";
            append_term_debruijn(out, term->case_.scrutinee);
            for (std::uint32_t i = 0; i < term->case_.n_branches; ++i) {
                out += " ";
                append_term_debruijn(out, term->case_.branches[i]);
            }
            out += ")";
            break;
        }
        case TermTag::Constant: {
            out += "(con ";
            append_constant(out, *term->constant.value);
            out += ")";
            break;
        }
        case TermTag::Builtin: {
            out += "(builtin ";
            out += std::string(builtin_name(term->builtin.function));
            out += ")";
            break;
        }
        case TermTag::Error:
            out += "(error)";
            break;
    }
}

void append_term_named(std::string& out, Term* term) {
    switch (term->tag) {
        case TermTag::Var:
            out += term->var.binder.text ? term->var.binder.text : "<unnamed>";
            break;
        case TermTag::Lambda:
            out += "(lam ";
            out += term->lambda.parameter.text ? term->lambda.parameter.text : "<unnamed>";
            out += " ";
            append_term_named(out, term->lambda.body);
            out += ")";
            break;
        case TermTag::Apply:
            out += "[";
            append_term_named(out, term->apply.function);
            out += " ";
            append_term_named(out, term->apply.argument);
            out += "]";
            break;
        case TermTag::Delay:
            out += "(delay ";
            append_term_named(out, term->delay.term);
            out += ")";
            break;
        case TermTag::Force:
            out += "(force ";
            append_term_named(out, term->force.term);
            out += ")";
            break;
        case TermTag::Constr: {
            out += "(constr ";
            out += std::to_string(term->constr.tag_index);
            for (std::uint32_t i = 0; i < term->constr.n_fields; ++i) {
                out += " ";
                append_term_named(out, term->constr.fields[i]);
            }
            out += ")";
            break;
        }
        case TermTag::Case: {
            out += "(case ";
            append_term_named(out, term->case_.scrutinee);
            for (std::uint32_t i = 0; i < term->case_.n_branches; ++i) {
                out += " ";
                append_term_named(out, term->case_.branches[i]);
            }
            out += ")";
            break;
        }
        case TermTag::Constant: {
            out += "(con ";
            append_constant(out, *term->constant.value);
            out += ")";
            break;
        }
        case TermTag::Builtin: {
            out += "(builtin ";
            out += std::string(builtin_name(term->builtin.function));
            out += ")";
            break;
        }
        case TermTag::Error:
            out += "(error)";
            break;
    }
}

}  // namespace

std::string pretty_print_debruijn(Term* term) {
    std::string out;
    append_term_debruijn(out, term);
    return out;
}

std::string pretty_print_named(Term* term) {
    std::string out;
    append_term_named(out, term);
    return out;
}

std::string pretty_print_program(const Program& program) {
    std::string out = "(program " +
                      std::to_string(program.version.major) + "." +
                      std::to_string(program.version.minor) + "." +
                      std::to_string(program.version.patch) + " ";
    if (program.is_debruijn) {
        append_term_debruijn(out, program.term);
    } else {
        append_term_named(out, program.term);
    }
    out += ")";
    return out;
}

std::string print_constant(const Constant& c) {
    std::string out;
    append_constant(out, c);
    return out;
}

std::string print_constant_type(const ConstantType& t) {
    std::string out;
    append_type(out, t);
    return out;
}

std::string print_plutus_data(const PlutusData& d) {
    std::string out;
    append_plutus_data(out, d);
    return out;
}

}  // namespace uplc
