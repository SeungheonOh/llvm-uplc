#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace uplc {

enum class TokenType : std::uint8_t {
    LParen,
    RParen,
    LBracket,
    RBracket,
    Dot,
    Comma,
    Number,
    String,
    ByteString,
    Point,      // 0xHEX literal used for BLS12-381 constants
    Unit,       // the literal `()`
    True,
    False,
    Identifier,
    Lam,
    Delay,
    Force,
    Builtin,
    Con,
    Error,
    Program,
    Constr,
    Case,
    IKw,        // keyword `I` (PlutusData)
    BKw,        // keyword `B`
    ListKw,     // keyword `List`
    MapKw,      // keyword `Map`
    ConstrKw,   // keyword `Constr` (PlutusData)
    List,       // keyword `list` (constant type)
    Pair,       // keyword `pair` (constant type)
    Array,      // keyword `array`
    Eof,
};

struct Token {
    TokenType    type;
    std::string  value;   // lexeme text or decoded value
    std::size_t  position;  // byte offset into source
};

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

// Hand-written lexer for UPLC text syntax. Mirrors TS lexer.ts 1:1.
class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source), pos_(0) {}

    Token next_token();

    std::string_view source() const { return source_; }

private:
    bool is_at_end() const { return pos_ >= source_.size(); }
    char peek() const { return is_at_end() ? '\0' : source_[pos_]; }
    char peek_next() const {
        return (pos_ + 1 >= source_.size()) ? '\0' : source_[pos_ + 1];
    }
    char advance() { return source_[pos_++]; }

    void skip_whitespace_and_comments();

    Token read_number(std::size_t pos, char first);
    Token read_identifier(std::size_t pos, char first);
    Token read_bytestring(std::size_t pos);
    Token read_hex_literal(std::size_t pos, TokenType type);
    Token read_string(std::size_t pos);

    std::string_view source_;
    std::size_t      pos_;
};

}  // namespace uplc
