#include "compiler/frontend/parser.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gmp.h>

#include "compiler/ast/builtin_tag.h"

namespace uplc {

namespace {

std::string format_position(std::size_t p) { return std::to_string(p); }

std::string token_type_name(TokenType t) {
    switch (t) {
        case TokenType::LParen:     return "lparen";
        case TokenType::RParen:     return "rparen";
        case TokenType::LBracket:   return "lbracket";
        case TokenType::RBracket:   return "rbracket";
        case TokenType::Dot:        return "dot";
        case TokenType::Comma:      return "comma";
        case TokenType::Number:     return "number";
        case TokenType::String:     return "string";
        case TokenType::ByteString: return "bytestring";
        case TokenType::Point:      return "point";
        case TokenType::Unit:       return "unit";
        case TokenType::True:       return "true";
        case TokenType::False:      return "false";
        case TokenType::Identifier: return "identifier";
        case TokenType::Lam:        return "lam";
        case TokenType::Delay:      return "delay";
        case TokenType::Force:      return "force";
        case TokenType::Builtin:    return "builtin";
        case TokenType::Con:        return "con";
        case TokenType::Error:      return "error";
        case TokenType::Program:    return "program";
        case TokenType::Constr:     return "constr";
        case TokenType::Case:       return "case";
        case TokenType::IKw:        return "I";
        case TokenType::BKw:        return "B";
        case TokenType::ListKw:     return "List";
        case TokenType::MapKw:      return "Map";
        case TokenType::ConstrKw:   return "Constr";
        case TokenType::List:       return "list";
        case TokenType::Pair:       return "pair";
        case TokenType::Array:      return "array";
        case TokenType::Eof:        return "eof";
    }
    return "unknown";
}

// Parse the decimal literal held by `tok.value` into an arena-owned BigInt.
// Accepts an optional leading `+` or `-` (GMP's mpz_set_str only accepts `-`,
// so `+` is stripped before the call).
BigInt* parse_integer_literal(Arena& a, const std::string& literal,
                              std::size_t position) {
    BigInt* bi = a.make_bigint();
    const char* p = literal.c_str();
    if (*p == '+') ++p;
    if (mpz_set_str(bi->value, p, 10) != 0) {
        throw ParseError("invalid integer literal \"" + literal +
                         "\" at position " + format_position(position));
    }
    return bi;
}

// Parse the constr-tag literal (must fit in uint64_t, non-negative).
std::uint64_t parse_constr_tag(const std::string& literal, std::size_t position) {
    mpz_t tmp;
    mpz_init(tmp);
    if (mpz_set_str(tmp, literal.c_str(), 10) != 0) {
        mpz_clear(tmp);
        throw ParseError("invalid constr tag " + literal +
                         " at position " + format_position(position));
    }
    if (mpz_sgn(tmp) < 0 || mpz_sizeinbase(tmp, 2) > 64) {
        mpz_clear(tmp);
        throw ParseError("invalid constr tag " + literal +
                         " at position " + format_position(position));
    }
    std::uint64_t v = mpz_get_ui(tmp);  // OK: <= 64 bits
    mpz_clear(tmp);
    return v;
}

// Copy a hex string into arena-allocated bytes. `hex` must already have
// been validated by the lexer.
const std::uint8_t* hex_to_bytes(Arena& a, const std::string& hex,
                                 std::uint32_t& out_len) {
    out_len = static_cast<std::uint32_t>(hex.size() / 2);
    if (out_len == 0) return nullptr;
    auto* buf = static_cast<std::uint8_t*>(a.alloc_raw(out_len, alignof(std::uint8_t)));
    for (std::uint32_t i = 0; i < out_len; ++i) {
        auto hex_digit = [](char c) -> std::uint8_t {
            if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
            return 0;
        };
        buf[i] = static_cast<std::uint8_t>((hex_digit(hex[i * 2]) << 4) |
                                            hex_digit(hex[i * 2 + 1]));
    }
    return buf;
}

class Parser {
public:
    Parser(Arena& arena, std::string_view source)
        : arena_(arena), lexer_(source), next_unique_(0) {
        current_ = lexer_.next_token();
    }

    Program parse_program();

private:
    void advance() { current_ = lexer_.next_token(); }

    bool is(TokenType t) const { return current_.type == t; }

    void expect(TokenType t) {
        if (current_.type != t) {
            throw ParseError("expected " + token_type_name(t) + ", got " +
                             token_type_name(current_.type) + " at position " +
                             format_position(current_.position));
        }
        advance();
    }

    Binder intern_name(const std::string& text) {
        auto it = interned_.find(text);
        std::uint32_t unique;
        if (it != interned_.end()) {
            unique = it->second;
        } else {
            unique = next_unique_++;
            interned_.emplace(text, unique);
        }
        return Binder{unique, arena_.intern_str(text)};
    }

    bool is_before_1_1_0() const {
        return version_.before_1_1_0();
    }

    Term* parse_term();
    Term* parse_paren_term();
    Term* parse_lambda();
    Term* parse_delay();
    Term* parse_force();
    Term* parse_builtin_term();
    Term* parse_constr();
    Term* parse_case();
    Term* parse_apply();
    Term* parse_constant_term();

    ConstantType* parse_type_spec();
    ConstantType* parse_inner_type_spec();
    Constant*     parse_constant_value(ConstantType* type_spec);
    Constant*     parse_value_constant();
    PlutusData*   parse_plutus_data();

    Arena&   arena_;
    Lexer    lexer_;
    Token    current_;
    std::unordered_map<std::string, std::uint32_t> interned_;
    std::uint32_t next_unique_;
    Version  version_{0, 0, 0};
};

Program Parser::parse_program() {
    expect(TokenType::LParen);
    expect(TokenType::Program);

    std::uint32_t parts[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (!is(TokenType::Number)) {
            throw ParseError("expected version number, got " +
                             token_type_name(current_.type) + " at position " +
                             format_position(current_.position));
        }
        try {
            unsigned long long v = std::stoull(current_.value);
            if (v > 0xffffffffu) throw std::out_of_range("version component");
            parts[i] = static_cast<std::uint32_t>(v);
        } catch (const std::exception&) {
            throw ParseError("invalid version number " + current_.value +
                             " at position " + format_position(current_.position));
        }
        advance();
        if (i < 2) expect(TokenType::Dot);
    }
    version_ = Version{parts[0], parts[1], parts[2]};

    Term* body = parse_term();
    expect(TokenType::RParen);

    if (!is(TokenType::Eof)) {
        throw ParseError("unexpected token " + token_type_name(current_.type) +
                         " after program at position " +
                         format_position(current_.position));
    }

    return Program{version_, body, /*is_debruijn=*/false};
}

Term* Parser::parse_term() {
    if (is(TokenType::Identifier)) {
        Binder b = intern_name(current_.value);
        advance();
        return make_var(arena_, b);
    }
    if (is(TokenType::LParen)) {
        advance();
        return parse_paren_term();
    }
    if (is(TokenType::LBracket)) {
        return parse_apply();
    }
    throw ParseError("unexpected token " + token_type_name(current_.type) +
                     " in term at position " + format_position(current_.position));
}

Term* Parser::parse_paren_term() {
    switch (current_.type) {
        case TokenType::Lam:     return parse_lambda();
        case TokenType::Delay:   return parse_delay();
        case TokenType::Force:   return parse_force();
        case TokenType::Builtin: return parse_builtin_term();
        case TokenType::Con:     return parse_constant_term();
        case TokenType::Error: {
            advance();
            expect(TokenType::RParen);
            return make_error(arena_);
        }
        case TokenType::Constr: return parse_constr();
        case TokenType::Case:   return parse_case();
        default:
            throw ParseError("unexpected token " + token_type_name(current_.type) +
                             " in term at position " + format_position(current_.position));
    }
}

Term* Parser::parse_lambda() {
    expect(TokenType::Lam);
    if (!is(TokenType::Identifier)) {
        throw ParseError("expected identifier, got " +
                         token_type_name(current_.type) + " at position " +
                         format_position(current_.position));
    }
    Binder param = intern_name(current_.value);
    advance();
    Term* body = parse_term();
    expect(TokenType::RParen);
    return make_lambda(arena_, param, body);
}

Term* Parser::parse_delay() {
    expect(TokenType::Delay);
    Term* inner = parse_term();
    expect(TokenType::RParen);
    return make_delay(arena_, inner);
}

Term* Parser::parse_force() {
    expect(TokenType::Force);
    Term* inner = parse_term();
    expect(TokenType::RParen);
    return make_force(arena_, inner);
}

Term* Parser::parse_builtin_term() {
    expect(TokenType::Builtin);
    if (!is(TokenType::Identifier)) {
        throw ParseError("expected builtin name, got " +
                         token_type_name(current_.type) + " at position " +
                         format_position(current_.position));
    }
    auto tag = builtin_from_name(current_.value);
    if (!tag) {
        throw ParseError("unknown builtin function " + current_.value +
                         " at position " + format_position(current_.position));
    }
    advance();
    expect(TokenType::RParen);
    return make_builtin(arena_, *tag);
}

Term* Parser::parse_constr() {
    if (is_before_1_1_0()) {
        throw ParseError("constr can't be used before 1.1.0");
    }
    expect(TokenType::Constr);
    if (!is(TokenType::Number)) {
        throw ParseError("expected tag number, got " +
                         token_type_name(current_.type) + " at position " +
                         format_position(current_.position));
    }
    std::uint64_t index = parse_constr_tag(current_.value, current_.position);
    advance();

    std::vector<Term*> fields;
    while (!is(TokenType::RParen)) fields.push_back(parse_term());
    expect(TokenType::RParen);

    auto n = static_cast<std::uint32_t>(fields.size());
    Term** arr = arena_.alloc_array_uninit<Term*>(n);
    for (std::uint32_t i = 0; i < n; ++i) arr[i] = fields[i];
    return make_constr(arena_, index, arr, n);
}

Term* Parser::parse_case() {
    if (is_before_1_1_0()) {
        throw ParseError("case can't be used before 1.1.0");
    }
    expect(TokenType::Case);
    Term* scrutinee = parse_term();
    std::vector<Term*> branches;
    while (!is(TokenType::RParen)) branches.push_back(parse_term());
    expect(TokenType::RParen);

    auto n = static_cast<std::uint32_t>(branches.size());
    Term** arr = arena_.alloc_array_uninit<Term*>(n);
    for (std::uint32_t i = 0; i < n; ++i) arr[i] = branches[i];
    return make_case(arena_, scrutinee, arr, n);
}

Term* Parser::parse_apply() {
    expect(TokenType::LBracket);
    std::vector<Term*> terms;
    while (!is(TokenType::RBracket)) terms.push_back(parse_term());
    if (terms.size() < 2) {
        throw ParseError("application requires at least two terms, got " +
                         std::to_string(terms.size()) + " at position " +
                         format_position(current_.position));
    }
    expect(TokenType::RBracket);

    Term* result = terms[0];
    for (std::size_t i = 1; i < terms.size(); ++i) {
        result = make_apply(arena_, result, terms[i]);
    }
    return result;
}

Term* Parser::parse_constant_term() {
    expect(TokenType::Con);
    ConstantType* type_spec = parse_type_spec();

    // Top-level `(con data (I 42))` wraps the data value in extra parens.
    if (type_spec->tag == ConstantTypeTag::Data) {
        expect(TokenType::LParen);
        PlutusData* data = parse_plutus_data();
        expect(TokenType::RParen);
        expect(TokenType::RParen);
        Constant* c = make_const_data(arena_, data);
        return make_constant(arena_, c);
    }

    Constant* c = parse_constant_value(type_spec);
    expect(TokenType::RParen);
    return make_constant(arena_, c);
}

ConstantType* Parser::parse_type_spec() {
    if (is(TokenType::List) || is(TokenType::Pair) || is(TokenType::Array)) {
        throw ParseError("expected left parenthesis for " +
                         token_type_name(current_.type) + " type at position " +
                         format_position(current_.position));
    }
    if (is(TokenType::LParen)) {
        advance();
        ConstantType* t = parse_inner_type_spec();
        if (!is(TokenType::RParen)) {
            throw ParseError("expected right parenthesis after type spec, got " +
                             token_type_name(current_.type) + " at position " +
                             format_position(current_.position));
        }
        advance();
        return t;
    }
    return parse_inner_type_spec();
}

ConstantType* Parser::parse_inner_type_spec() {
    if (is(TokenType::Identifier)) {
        const std::string name = current_.value;
        std::size_t prev_pos = current_.position;
        advance();
        if (name == "integer")                return make_type_simple(arena_, ConstantTypeTag::Integer);
        if (name == "bytestring")             return make_type_simple(arena_, ConstantTypeTag::ByteString);
        if (name == "string")                 return make_type_simple(arena_, ConstantTypeTag::String);
        if (name == "unit")                   return make_type_simple(arena_, ConstantTypeTag::Unit);
        if (name == "bool")                   return make_type_simple(arena_, ConstantTypeTag::Bool);
        if (name == "data")                   return make_type_simple(arena_, ConstantTypeTag::Data);
        if (name == "bls12_381_G1_element")   return make_type_simple(arena_, ConstantTypeTag::Bls12_381_G1);
        if (name == "bls12_381_G2_element")   return make_type_simple(arena_, ConstantTypeTag::Bls12_381_G2);
        if (name == "bls12_381_mlresult")     return make_type_simple(arena_, ConstantTypeTag::Bls12_381_MlResult);
        if (name == "value")                  return make_type_simple(arena_, ConstantTypeTag::Value);
        throw ParseError("unknown type " + name + " at position " +
                         format_position(prev_pos));
    }
    if (is(TokenType::List)) {
        advance();
        ConstantType* elem = parse_type_spec();
        return make_type_list(arena_, elem);
    }
    if (is(TokenType::Array)) {
        advance();
        ConstantType* elem = parse_type_spec();
        return make_type_array(arena_, elem);
    }
    if (is(TokenType::Pair)) {
        advance();
        ConstantType* first = parse_type_spec();
        ConstantType* second = parse_type_spec();
        return make_type_pair(arena_, first, second);
    }
    throw ParseError("expected type identifier, got " +
                     token_type_name(current_.type) + " at position " +
                     format_position(current_.position));
}

Constant* Parser::parse_constant_value(ConstantType* type_spec) {
    switch (type_spec->tag) {
        case ConstantTypeTag::Integer: {
            if (!is(TokenType::Number)) {
                throw ParseError("expected integer value, got " +
                                 token_type_name(current_.type) + " at position " +
                                 format_position(current_.position));
            }
            BigInt* v = parse_integer_literal(arena_, current_.value, current_.position);
            advance();
            return make_const_integer(arena_, v);
        }
        case ConstantTypeTag::ByteString: {
            if (!is(TokenType::ByteString)) {
                throw ParseError("expected bytestring value, got " +
                                 token_type_name(current_.type) + " at position " +
                                 format_position(current_.position));
            }
            std::uint32_t len = 0;
            const std::uint8_t* bytes = hex_to_bytes(arena_, current_.value, len);
            advance();
            return make_const_bytestring(arena_, bytes, len);
        }
        case ConstantTypeTag::String: {
            if (!is(TokenType::String)) {
                throw ParseError("expected string value, got " +
                                 token_type_name(current_.type) + " at position " +
                                 format_position(current_.position));
            }
            const std::string& s = current_.value;
            auto len = static_cast<std::uint32_t>(s.size());
            const char* utf8 = arena_.intern_str(s);
            advance();
            return make_const_string(arena_, utf8, len);
        }
        case ConstantTypeTag::Bool: {
            if (is(TokenType::True)) { advance(); return make_const_bool(arena_, true); }
            if (is(TokenType::False)) { advance(); return make_const_bool(arena_, false); }
            throw ParseError("expected bool value, got " +
                             token_type_name(current_.type) + " at position " +
                             format_position(current_.position));
        }
        case ConstantTypeTag::Unit: {
            if (!is(TokenType::Unit)) {
                throw ParseError("expected unit value, got " +
                                 token_type_name(current_.type) + " at position " +
                                 format_position(current_.position));
            }
            advance();
            return make_const_unit(arena_);
        }
        case ConstantTypeTag::Data: {
            // Nested data (inside list/pair) — no wrapping parens.
            PlutusData* d = parse_plutus_data();
            return make_const_data(arena_, d);
        }
        case ConstantTypeTag::List: {
            expect(TokenType::LBracket);
            std::vector<Constant*> values;
            while (!is(TokenType::RBracket)) {
                values.push_back(parse_constant_value(type_spec->list.element));
                if (!is(TokenType::RBracket)) expect(TokenType::Comma);
            }
            expect(TokenType::RBracket);
            auto n = static_cast<std::uint32_t>(values.size());
            Constant** arr = arena_.alloc_array_uninit<Constant*>(n);
            for (std::uint32_t i = 0; i < n; ++i) arr[i] = values[i];
            return make_const_list(arena_, type_spec->list.element, arr, n);
        }
        case ConstantTypeTag::Array: {
            expect(TokenType::LBracket);
            std::vector<Constant*> values;
            while (!is(TokenType::RBracket)) {
                values.push_back(parse_constant_value(type_spec->array.element));
                if (!is(TokenType::RBracket)) expect(TokenType::Comma);
            }
            expect(TokenType::RBracket);
            auto n = static_cast<std::uint32_t>(values.size());
            Constant** arr = arena_.alloc_array_uninit<Constant*>(n);
            for (std::uint32_t i = 0; i < n; ++i) arr[i] = values[i];
            Constant* c = arena_.alloc<Constant>();
            c->tag = ConstTag::Array;
            c->array.item_type = type_spec->array.element;
            c->array.values = arr;
            c->array.n_values = n;
            return c;
        }
        case ConstantTypeTag::Pair: {
            expect(TokenType::LParen);
            Constant* first = parse_constant_value(type_spec->pair.first);
            expect(TokenType::Comma);
            Constant* second = parse_constant_value(type_spec->pair.second);
            expect(TokenType::RParen);
            return make_const_pair(arena_, type_spec->pair.first,
                                   type_spec->pair.second, first, second);
        }
        case ConstantTypeTag::Bls12_381_G1: {
            if (!is(TokenType::Point)) {
                throw ParseError("expected point value, got " +
                                 token_type_name(current_.type) + " at position " +
                                 format_position(current_.position));
            }
            std::uint32_t len = 0;
            const std::uint8_t* bytes = hex_to_bytes(arena_, current_.value, len);
            advance();
            if (len != 48) {
                throw ParseError("bls12_381_G1_element must be 48 bytes, got " +
                                 std::to_string(len));
            }
            // Curve-membership validation deferred to M5 (needs BLST).
            Constant* c = arena_.alloc<Constant>();
            c->tag = ConstTag::Bls12_381_G1;
            c->bls_g1.bytes = bytes;
            return c;
        }
        case ConstantTypeTag::Bls12_381_G2: {
            if (!is(TokenType::Point)) {
                throw ParseError("expected point value, got " +
                                 token_type_name(current_.type) + " at position " +
                                 format_position(current_.position));
            }
            std::uint32_t len = 0;
            const std::uint8_t* bytes = hex_to_bytes(arena_, current_.value, len);
            advance();
            if (len != 96) {
                throw ParseError("bls12_381_G2_element must be 96 bytes, got " +
                                 std::to_string(len));
            }
            Constant* c = arena_.alloc<Constant>();
            c->tag = ConstTag::Bls12_381_G2;
            c->bls_g2.bytes = bytes;
            return c;
        }
        case ConstantTypeTag::Bls12_381_MlResult: {
            if (!is(TokenType::Point)) {
                throw ParseError("expected point value, got " +
                                 token_type_name(current_.type) + " at position " +
                                 format_position(current_.position));
            }
            std::uint32_t len = 0;
            const std::uint8_t* bytes = hex_to_bytes(arena_, current_.value, len);
            advance();
            Constant* c = arena_.alloc<Constant>();
            c->tag = ConstTag::Bls12_381_MlResult;
            c->bls_ml_result.bytes = bytes;
            c->bls_ml_result.len = len;
            return c;
        }
        case ConstantTypeTag::Value:
            return parse_value_constant();
    }
    throw ParseError("internal: unhandled ConstantTypeTag");
}

// Parse a `(con value [...])` literal. Mirrors TS parse.ts parseValueConstant:
// after collecting the raw entries we canonicalise (merge duplicate policies /
// tokens, sum quantities, drop zeros, sort lexicographically) so structurally
// equal values produce the same constant.
//
// Intermediate mpz accumulators are allocated via arena_.make_bigint() so the
// arena's destructor runs mpz_clear for us; temporary "waste" is negligible.
Constant* Parser::parse_value_constant() {
    constexpr std::size_t kMaxKeyBytes = 32;

    // VALUE_LIMIT = 2^127; amounts must satisfy -VALUE_LIMIT <= q < VALUE_LIMIT.
    BigInt* value_limit = arena_.make_bigint();
    mpz_set_ui(value_limit->value, 1);
    mpz_mul_2exp(value_limit->value, value_limit->value, 127);
    BigInt* neg_value_limit = arena_.make_bigint();
    mpz_neg(neg_value_limit->value, value_limit->value);

    struct TokenRaw {
        std::string key;
        BigInt*     qty;  // arena-owned
    };
    struct EntryRaw {
        std::string           currency_key;
        std::vector<TokenRaw> tokens;
    };

    auto bytes_to_key = [](const std::uint8_t* b, std::uint32_t n) {
        return std::string(reinterpret_cast<const char*>(b), n);
    };

    std::vector<EntryRaw> raw;

    expect(TokenType::LBracket);
    while (!is(TokenType::RBracket)) {
        expect(TokenType::LParen);

        if (!is(TokenType::ByteString)) {
            throw ParseError("expected bytestring key for value, got " +
                             token_type_name(current_.type) + " at position " +
                             format_position(current_.position));
        }
        std::uint32_t cur_len = 0;
        const std::uint8_t* cur_bytes = hex_to_bytes(arena_, current_.value, cur_len);
        if (cur_len > kMaxKeyBytes) {
            throw ParseError("policy key too long (" + std::to_string(cur_len) +
                             " bytes) at position " + format_position(current_.position));
        }
        std::string cur_key = bytes_to_key(cur_bytes, cur_len);
        advance();
        expect(TokenType::Comma);

        expect(TokenType::LBracket);
        EntryRaw entry{std::move(cur_key), {}};
        while (!is(TokenType::RBracket)) {
            expect(TokenType::LParen);
            if (!is(TokenType::ByteString)) {
                throw ParseError("expected bytestring in inner pair, got " +
                                 token_type_name(current_.type) + " at position " +
                                 format_position(current_.position));
            }
            std::uint32_t tn_len = 0;
            const std::uint8_t* tn_bytes = hex_to_bytes(arena_, current_.value, tn_len);
            if (tn_len > kMaxKeyBytes) {
                throw ParseError("token key too long (" + std::to_string(tn_len) +
                                 " bytes) at position " + format_position(current_.position));
            }
            std::string tn_key = bytes_to_key(tn_bytes, tn_len);
            advance();
            expect(TokenType::Comma);

            if (!is(TokenType::Number)) {
                throw ParseError("expected integer in inner pair, got " +
                                 token_type_name(current_.type) + " at position " +
                                 format_position(current_.position));
            }
            BigInt* qty = arena_.make_bigint();
            {
                const char* p = current_.value.c_str();
                if (*p == '+') ++p;
                if (mpz_set_str(qty->value, p, 10) != 0) {
                    throw ParseError("invalid integer in value token " +
                                     current_.value + " at position " +
                                     format_position(current_.position));
                }
            }
            if (mpz_cmp(qty->value, value_limit->value) >= 0 ||
                mpz_cmp(qty->value, neg_value_limit->value) < 0) {
                throw ParseError("integer in value token out of range " +
                                 current_.value + " at position " +
                                 format_position(current_.position));
            }
            advance();
            expect(TokenType::RParen);
            entry.tokens.push_back(TokenRaw{std::move(tn_key), qty});
            if (!is(TokenType::RBracket)) expect(TokenType::Comma);
        }
        expect(TokenType::RBracket);
        expect(TokenType::RParen);
        raw.push_back(std::move(entry));
        if (!is(TokenType::RBracket)) expect(TokenType::Comma);
    }
    expect(TokenType::RBracket);

    // Canonicalise: merge duplicate policies / tokens, sum quantities, drop
    // zero entries, sort lexicographically.
    std::map<std::string, std::map<std::string, BigInt*>> merged;
    for (auto& e : raw) {
        auto& token_map = merged[e.currency_key];
        for (auto& t : e.tokens) {
            auto it = token_map.find(t.key);
            if (it == token_map.end()) {
                BigInt* slot = arena_.make_bigint();
                mpz_set(slot->value, t.qty->value);
                token_map.emplace(t.key, slot);
            } else {
                mpz_add(it->second->value, it->second->value, t.qty->value);
            }
        }
    }

    // Validate summed amounts.
    for (auto& [_, tm] : merged) {
        for (auto& [__, bi] : tm) {
            if (mpz_cmp(bi->value, value_limit->value) >= 0 ||
                mpz_cmp(bi->value, neg_value_limit->value) < 0) {
                throw ParseError("summed token amount out of range");
            }
        }
    }

    // Build the arena-owned LedgerValue, dropping zero-quantity tokens.
    std::vector<LedgerValueEntry> out_entries;
    out_entries.reserve(merged.size());
    for (auto& [pol_key, tm] : merged) {
        std::vector<LedgerValueToken> out_tokens;
        out_tokens.reserve(tm.size());
        for (auto& [tok_key, bi] : tm) {
            if (mpz_sgn(bi->value) == 0) continue;
            LedgerValueToken t{};
            t.name_bytes = arena_.intern_bytes(
                reinterpret_cast<const std::uint8_t*>(tok_key.data()),
                tok_key.size());
            t.name_len = static_cast<std::uint32_t>(tok_key.size());
            t.quantity = bi;
            out_tokens.push_back(t);
        }
        if (out_tokens.empty()) continue;
        LedgerValueEntry e{};
        e.currency_bytes = arena_.intern_bytes(
            reinterpret_cast<const std::uint8_t*>(pol_key.data()),
            pol_key.size());
        e.currency_len = static_cast<std::uint32_t>(pol_key.size());
        auto n = static_cast<std::uint32_t>(out_tokens.size());
        auto* toks = arena_.alloc_array_uninit<LedgerValueToken>(n);
        for (std::uint32_t i = 0; i < n; ++i) toks[i] = out_tokens[i];
        e.tokens = toks;
        e.n_tokens = n;
        out_entries.push_back(e);
    }

    LedgerValue* lv = arena_.alloc<LedgerValue>();
    auto n = static_cast<std::uint32_t>(out_entries.size());
    auto* arr = arena_.alloc_array_uninit<LedgerValueEntry>(n);
    for (std::uint32_t i = 0; i < n; ++i) arr[i] = out_entries[i];
    lv->entries = arr;
    lv->n_entries = n;

    Constant* c = arena_.alloc<Constant>();
    c->tag = ConstTag::Value;
    c->value.value = lv;
    return c;
}

PlutusData* Parser::parse_plutus_data() {
    if (is(TokenType::IKw)) {
        advance();
        if (!is(TokenType::Number)) {
            throw ParseError("expected integer value for I, got " +
                             token_type_name(current_.type) + " at position " +
                             format_position(current_.position));
        }
        BigInt* v = parse_integer_literal(arena_, current_.value, current_.position);
        advance();
        return make_data_integer(arena_, v);
    }
    if (is(TokenType::BKw)) {
        advance();
        if (!is(TokenType::ByteString)) {
            throw ParseError("expected bytestring value for B, got " +
                             token_type_name(current_.type) + " at position " +
                             format_position(current_.position));
        }
        std::uint32_t len = 0;
        const std::uint8_t* bytes = hex_to_bytes(arena_, current_.value, len);
        advance();
        return make_data_bytestring(arena_, bytes, len);
    }
    if (is(TokenType::ListKw)) {
        advance();
        expect(TokenType::LBracket);
        std::vector<PlutusData*> items;
        while (!is(TokenType::RBracket)) {
            items.push_back(parse_plutus_data());
            if (!is(TokenType::RBracket)) expect(TokenType::Comma);
        }
        expect(TokenType::RBracket);
        auto n = static_cast<std::uint32_t>(items.size());
        PlutusData** arr = arena_.alloc_array_uninit<PlutusData*>(n);
        for (std::uint32_t i = 0; i < n; ++i) arr[i] = items[i];
        return make_data_list(arena_, arr, n);
    }
    if (is(TokenType::MapKw)) {
        advance();
        expect(TokenType::LBracket);
        std::vector<PlutusDataPair> entries;
        while (!is(TokenType::RBracket)) {
            expect(TokenType::LParen);
            PlutusData* key = parse_plutus_data();
            expect(TokenType::Comma);
            PlutusData* val = parse_plutus_data();
            expect(TokenType::RParen);
            entries.push_back(PlutusDataPair{key, val});
            if (!is(TokenType::RBracket)) expect(TokenType::Comma);
        }
        expect(TokenType::RBracket);
        auto n = static_cast<std::uint32_t>(entries.size());
        PlutusDataPair* arr = arena_.alloc_array_uninit<PlutusDataPair>(n);
        for (std::uint32_t i = 0; i < n; ++i) arr[i] = entries[i];
        return make_data_map(arena_, arr, n);
    }
    if (is(TokenType::ConstrKw)) {
        advance();
        if (!is(TokenType::Number)) {
            throw ParseError("expected tag number for Constr, got " +
                             token_type_name(current_.type) + " at position " +
                             format_position(current_.position));
        }
        BigInt* index = parse_integer_literal(arena_, current_.value, current_.position);
        if (mpz_sgn(index->value) < 0) {
            throw ParseError("invalid Constr tag " + current_.value +
                             " at position " + format_position(current_.position));
        }
        advance();
        expect(TokenType::LBracket);
        std::vector<PlutusData*> fields;
        while (!is(TokenType::RBracket)) {
            fields.push_back(parse_plutus_data());
            if (!is(TokenType::RBracket)) expect(TokenType::Comma);
        }
        expect(TokenType::RBracket);
        auto n = static_cast<std::uint32_t>(fields.size());
        PlutusData** arr = arena_.alloc_array_uninit<PlutusData*>(n);
        for (std::uint32_t i = 0; i < n; ++i) arr[i] = fields[i];
        return make_data_constr(arena_, index, arr, n);
    }
    throw ParseError("expected PlutusData constructor (I, B, List, Map, Constr), got " +
                     token_type_name(current_.type) + " at position " +
                     format_position(current_.position));
}

}  // namespace

Program parse_program(Arena& arena, std::string_view source) {
    Parser p(arena, source);
    return p.parse_program();
}

}  // namespace uplc
