#include "compiler/frontend/lexer.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace uplc {

namespace {

const std::unordered_map<std::string_view, TokenType>& keywords() {
    static const std::unordered_map<std::string_view, TokenType> k = {
        {"lam",     TokenType::Lam},
        {"delay",   TokenType::Delay},
        {"force",   TokenType::Force},
        {"builtin", TokenType::Builtin},
        {"con",     TokenType::Con},
        {"error",   TokenType::Error},
        {"program", TokenType::Program},
        {"constr",  TokenType::Constr},
        {"case",    TokenType::Case},
        {"True",    TokenType::True},
        {"False",   TokenType::False},
        {"I",       TokenType::IKw},
        {"B",       TokenType::BKw},
        {"List",    TokenType::ListKw},
        {"Map",     TokenType::MapKw},
        {"Constr",  TokenType::ConstrKw},
        {"list",    TokenType::List},
        {"pair",    TokenType::Pair},
        {"array",   TokenType::Array},
    };
    return k;
}

const std::unordered_map<std::string_view, int>& named_escapes() {
    static const std::unordered_map<std::string_view, int> k = {
        {"NUL", 0x00}, {"SOH", 0x01}, {"STX", 0x02}, {"ETX", 0x03},
        {"EOT", 0x04}, {"ENQ", 0x05}, {"ACK", 0x06}, {"BEL", 0x07},
        {"BS",  0x08}, {"HT",  0x09}, {"LF",  0x0a}, {"VT",  0x0b},
        {"FF",  0x0c}, {"CR",  0x0d}, {"SO",  0x0e}, {"SI",  0x0f},
        {"DLE", 0x10}, {"DC1", 0x11}, {"DC2", 0x12}, {"DC3", 0x13},
        {"DC4", 0x14}, {"NAK", 0x15}, {"SYN", 0x16}, {"ETB", 0x17},
        {"CAN", 0x18}, {"EM",  0x19}, {"SUB", 0x1a}, {"ESC", 0x1b},
        {"FS",  0x1c}, {"GS",  0x1d}, {"RS",  0x1e}, {"US",  0x1f},
        {"SP",  0x20}, {"DEL", 0x7f},
    };
    return k;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_alphanumeric(char c) { return is_alpha(c) || is_digit(c); }
bool is_whitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_octal_digit(char c) { return c >= '0' && c <= '7'; }

// Encodes a Unicode code point as UTF-8, appending to `out`. Matches JS
// String.fromCodePoint() behaviour (4-byte max).
void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

std::string format_position(std::size_t p) { return std::to_string(p); }

}  // namespace

void Lexer::skip_whitespace_and_comments() {
    while (!is_at_end()) {
        char c = peek();
        if (is_whitespace(c)) {
            advance();
        } else if (c == '-' && peek_next() == '-') {
            while (!is_at_end() && peek() != '\n') advance();
        } else {
            break;
        }
    }
}

Token Lexer::next_token() {
    skip_whitespace_and_comments();

    const std::size_t position = pos_;
    if (is_at_end()) return Token{TokenType::Eof, "", position};

    char c = advance();
    switch (c) {
        case '(':
            if (peek() == ')') {
                advance();
                return Token{TokenType::Unit, "()", position};
            }
            return Token{TokenType::LParen, "(", position};
        case ')': return Token{TokenType::RParen, ")", position};
        case '[': return Token{TokenType::LBracket, "[", position};
        case ']': return Token{TokenType::RBracket, "]", position};
        case '.': return Token{TokenType::Dot, ".", position};
        case ',': return Token{TokenType::Comma, ",", position};
        case '#': return read_bytestring(position);
        case '"': return read_string(position);
        case '0':
            if (peek() == 'x') {
                advance();
                return read_hex_literal(position, TokenType::Point);
            }
            return read_number(position, c);
        default: break;
    }

    if (c == '-' || c == '+') {
        if (!is_at_end() && is_digit(peek())) {
            return read_number(position, c);
        }
        return read_identifier(position, c);
    }
    if (is_digit(c)) return read_number(position, c);
    if (is_alpha(c) || c == '\'') return read_identifier(position, c);

    throw ParseError("unexpected character '" + std::string(1, c) +
                     "' at position " + format_position(position));
}

Token Lexer::read_number(std::size_t position, char first) {
    std::string literal(1, first);
    while (!is_at_end() && is_digit(peek())) literal.push_back(advance());
    return Token{TokenType::Number, literal, position};
}

Token Lexer::read_identifier(std::size_t position, char first) {
    std::string literal(1, first);
    while (!is_at_end()) {
        char c = peek();
        if (is_alphanumeric(c) || c == '_' || c == '\'' || c == '-') {
            literal.push_back(advance());
        } else {
            break;
        }
    }
    const auto& kw = keywords();
    auto it = kw.find(literal);
    TokenType type = (it != kw.end()) ? it->second : TokenType::Identifier;
    return Token{type, literal, position};
}

Token Lexer::read_bytestring(std::size_t position) {
    std::string hex;
    while (!is_at_end()) {
        char ch = peek();
        if (is_whitespace(ch) || ch == ')' || ch == ']' || ch == ',') break;
        if (!is_hex_digit(ch)) {
            throw ParseError("invalid bytestring character '" +
                             std::string(1, ch) + "' at position " +
                             format_position(pos_));
        }
        hex.push_back(advance());
    }
    if (hex.size() % 2 != 0) {
        throw ParseError("bytestring #" + hex + " has odd length at position " +
                         format_position(position));
    }
    return Token{TokenType::ByteString, hex, position};
}

Token Lexer::read_hex_literal(std::size_t position, TokenType type) {
    std::string hex;
    while (!is_at_end()) {
        char ch = peek();
        if (is_whitespace(ch) || ch == ')' || ch == ']' || ch == ',') break;
        if (!is_hex_digit(ch)) {
            throw ParseError("invalid hex character '" + std::string(1, ch) +
                             "' at position " + format_position(pos_));
        }
        hex.push_back(advance());
    }
    if (type == TokenType::ByteString && hex.size() % 2 != 0) {
        throw ParseError("bytestring has odd length at position " +
                         format_position(position));
    }
    return Token{type, hex, position};
}

Token Lexer::read_string(std::size_t position) {
    std::string result;
    while (!is_at_end() && peek() != '"') {
        if (peek() != '\\') {
            result.push_back(advance());
            continue;
        }
        advance();  // consume backslash
        if (is_at_end()) {
            throw ParseError("unterminated string escape at position " +
                             format_position(pos_));
        }
        char esc = advance();

        // Simple escapes
        switch (esc) {
            case 'a':  result.push_back('\x07'); continue;
            case 'b':  result.push_back('\x08'); continue;
            case 'f':  result.push_back('\x0c'); continue;
            case 'n':  result.push_back('\n');   continue;
            case 'r':  result.push_back('\r');   continue;
            case 't':  result.push_back('\t');   continue;
            case 'v':  result.push_back('\x0b'); continue;
            case '"':  result.push_back('"');    continue;
            case '\\': result.push_back('\\');   continue;
        }

        // \uXXXX — 1-4 hex digits, a Unicode code point
        if (esc == 'u') {
            std::string hex;
            while (!is_at_end() && hex.size() < 4 && is_hex_digit(peek())) {
                hex.push_back(advance());
            }
            if (hex.empty()) {
                throw ParseError("invalid unicode escape sequence at position " +
                                 format_position(position));
            }
            append_utf8(result, static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16)));
            continue;
        }

        // \xHH — 1-2 hex digits, a single byte
        if (esc == 'x') {
            std::string hex;
            while (!is_at_end() && hex.size() < 2 && is_hex_digit(peek())) {
                hex.push_back(advance());
            }
            if (hex.empty()) {
                throw ParseError("invalid hex escape sequence at position " +
                                 format_position(position));
            }
            result.push_back(static_cast<char>(std::stoul(hex, nullptr, 16)));
            continue;
        }

        // \oNNN — 1-3 octal digits, a Unicode code point
        if (esc == 'o') {
            std::string oct;
            while (!is_at_end() && oct.size() < 3 && is_octal_digit(peek())) {
                oct.push_back(advance());
            }
            if (oct.empty()) {
                throw ParseError("invalid octal escape sequence at position " +
                                 format_position(position));
            }
            append_utf8(result, static_cast<std::uint32_t>(std::stoul(oct, nullptr, 8)));
            continue;
        }

        // Named or letter-run escape: \DEL, \NUL, etc.
        if (is_letter(esc)) {
            std::string name(1, esc);
            while (!is_at_end() && is_letter(peek())) name.push_back(advance());
            const auto& tbl = named_escapes();
            auto it = tbl.find(name);
            if (it != tbl.end()) {
                result.push_back(static_cast<char>(it->second));
            } else {
                result.push_back('\\');
                result += name;
            }
            continue;
        }

        // Decimal escape: \NNN...
        if (is_digit(esc)) {
            std::string dec(1, esc);
            while (!is_at_end() && is_digit(peek())) dec.push_back(advance());
            append_utf8(result, static_cast<std::uint32_t>(std::stoul(dec, nullptr, 10)));
            continue;
        }

        // Unknown escape — output literally
        result.push_back('\\');
        result.push_back(esc);
    }

    if (is_at_end()) {
        throw ParseError("unterminated string at position " + format_position(position));
    }

    advance();  // consume closing quote
    return Token{TokenType::String, result, position};
}

}  // namespace uplc
