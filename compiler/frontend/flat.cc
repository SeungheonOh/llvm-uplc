#include "compiler/frontend/flat.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gmp.h>

#include "compiler/ast/cbor_data.h"
#include "compiler/ast/builtin_tag.h"
#include "compiler/frontend/lexer.h"  // for ParseError

namespace uplc {

namespace {

// ---------------------------------------------------------------------------
// FlatDecoder — bit-level reader mirroring TS flat.ts.
// ---------------------------------------------------------------------------
class FlatDecoder {
public:
    FlatDecoder(const std::uint8_t* data, std::size_t len)
        : data_(data), len_(len) {}

    bool bit() {
        if (pos_ >= len_) throw ParseError("flat: end of input");
        bool b = (data_[pos_] & (128u >> used_bits_)) != 0;
        if (used_bits_ == 7) { used_bits_ = 0; ++pos_; }
        else                 { ++used_bits_; }
        return b;
    }

    std::uint32_t bits8(std::uint32_t n) {
        std::uint32_t r = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            r = (r << 1) | (bit() ? 1u : 0u);
        }
        return r;
    }

    // Unsigned varint — repeat 8-bit chunks until the high bit is 0.
    std::uint64_t word() {
        std::uint64_t r = 0;
        std::uint32_t shift = 0;
        for (;;) {
            std::uint32_t b = bits8(8);
            r |= static_cast<std::uint64_t>(b & 0x7fu) << shift;
            if ((b & 0x80u) == 0) break;
            shift += 7;
            if (shift >= 64) throw ParseError("flat: word overflow");
        }
        return r;
    }

    // Big-integer varint — same framing but into an mpz_t, for `integer`
    // constants which can be arbitrarily large.
    void big_word(mpz_ptr out) {
        // Collect 7-bit chunks (little-endian); then build an mpz.
        std::vector<std::uint8_t> chunks;
        for (;;) {
            std::uint32_t b = bits8(8);
            chunks.push_back(static_cast<std::uint8_t>(b & 0x7f));
            if ((b & 0x80u) == 0) break;
        }
        mpz_set_ui(out, 0);
        for (std::size_t i = chunks.size(); i-- > 0; ) {
            mpz_mul_2exp(out, out, 7);
            mpz_add_ui(out, out, chunks[i]);
        }
    }

    // Signed integer via zigzag on top of big_word.
    void integer(mpz_ptr out) {
        mpz_t bw;
        mpz_init(bw);
        big_word(bw);
        // unzigzag: (bw & 1) == 0 -> bw >> 1, else -(bw >> 1) - 1
        if ((mpz_get_ui(bw) & 1u) == 0) {
            mpz_fdiv_q_2exp(out, bw, 1);
        } else {
            mpz_fdiv_q_2exp(out, bw, 1);
            mpz_add_ui(out, out, 1);
            mpz_neg(out, out);
        }
        mpz_clear(bw);
    }

    void filler() {
        while (!bit()) {}
    }

    // Read a chunked byte-string: skip filler, then read chunk-length prefixed
    // runs until a 0-length chunk.
    std::vector<std::uint8_t> bytes() {
        filler();
        std::vector<std::uint8_t> out;
        for (;;) {
            if (pos_ >= len_) throw ParseError("flat: end of input");
            std::uint8_t chunk_len = data_[pos_++];
            if (chunk_len == 0) break;
            if (pos_ + chunk_len > len_) throw ParseError("flat: end of input");
            out.insert(out.end(), data_ + pos_, data_ + pos_ + chunk_len);
            pos_ += chunk_len;
        }
        return out;
    }

    bool at_end() const { return pos_ >= len_; }

private:
    const std::uint8_t* data_;
    std::size_t         len_;
    std::size_t         pos_ = 0;
    std::uint32_t       used_bits_ = 0;
};

// ---------------------------------------------------------------------------
// Constant decoder (recursive — constant types are bounded-depth).
// ---------------------------------------------------------------------------
class ConstantDecoder {
public:
    ConstantDecoder(Arena& arena, FlatDecoder& d) : arena_(arena), d_(d) {}

    Constant* decode() {
        // Collect the prefix type-tag bit-string (1bit + 4bits repeated).
        tags_.clear();
        while (d_.bit()) {
            tags_.push_back(static_cast<std::uint8_t>(d_.bits8(4)));
        }
        tag_idx_ = 0;
        ConstantType* type = parse_type();
        return decode_value(type);
    }

private:
    ConstantType* parse_type() {
        if (tag_idx_ >= tags_.size()) {
            throw ParseError("flat: invalid constant type tags");
        }
        std::uint8_t tag = tags_[tag_idx_++];
        switch (tag) {
            case 0: return make_type_simple(arena_, ConstantTypeTag::Integer);
            case 1: return make_type_simple(arena_, ConstantTypeTag::ByteString);
            case 2: return make_type_simple(arena_, ConstantTypeTag::String);
            case 3: return make_type_simple(arena_, ConstantTypeTag::Unit);
            case 4: return make_type_simple(arena_, ConstantTypeTag::Bool);
            case 5: {
                ConstantType* elem = parse_type();
                return make_type_list(arena_, elem);
            }
            case 6: {
                ConstantType* first = parse_type();
                ConstantType* second = parse_type();
                return make_type_pair(arena_, first, second);
            }
            case 7: {
                if (tag_idx_ >= tags_.size()) {
                    throw ParseError("flat: invalid constant type tags");
                }
                std::uint8_t next = tags_[tag_idx_++];
                if (next == 5) {
                    ConstantType* elem = parse_type();
                    return make_type_list(arena_, elem);
                }
                if (next == 7) {
                    if (tag_idx_ >= tags_.size()) {
                        throw ParseError("flat: invalid constant type tags");
                    }
                    std::uint8_t inner = tags_[tag_idx_++];
                    if (inner == 6) {
                        ConstantType* first = parse_type();
                        ConstantType* second = parse_type();
                        return make_type_pair(arena_, first, second);
                    }
                    throw ParseError(std::string("flat: unknown type application 7,7,") +
                                     std::to_string(inner));
                }
                throw ParseError(std::string("flat: unknown type application 7,") +
                                 std::to_string(next));
            }
            case 8: return make_type_simple(arena_, ConstantTypeTag::Data);
            default:
                throw ParseError(std::string("flat: unknown constant type tag ") +
                                 std::to_string(tag));
        }
    }

    Constant* decode_value(ConstantType* type) {
        switch (type->tag) {
            case ConstantTypeTag::Integer: {
                BigInt* bi = arena_.make_bigint();
                d_.integer(bi->value);
                return make_const_integer(arena_, bi);
            }
            case ConstantTypeTag::ByteString: {
                auto bytes = d_.bytes();
                auto n = static_cast<std::uint32_t>(bytes.size());
                const std::uint8_t* owned = arena_.intern_bytes(bytes.data(), n);
                return make_const_bytestring(arena_, owned, n);
            }
            case ConstantTypeTag::String: {
                auto bytes = d_.bytes();
                auto n = static_cast<std::uint32_t>(bytes.size());
                const char* utf8 = reinterpret_cast<const char*>(
                    arena_.intern_bytes(bytes.data(), n));
                return make_const_string(arena_, utf8, n);
            }
            case ConstantTypeTag::Unit:
                return make_const_unit(arena_);
            case ConstantTypeTag::Bool:
                return make_const_bool(arena_, d_.bit());
            case ConstantTypeTag::Data: {
                auto bytes = d_.bytes();
                PlutusData* data = decode_plutus_data(arena_, bytes.data(), bytes.size());
                return make_const_data(arena_, data);
            }
            case ConstantTypeTag::List: {
                std::vector<Constant*> values;
                while (d_.bit()) {
                    values.push_back(decode_value(type->list.element));
                }
                auto n = static_cast<std::uint32_t>(values.size());
                Constant** arr = arena_.alloc_array_uninit<Constant*>(n);
                for (std::uint32_t i = 0; i < n; ++i) arr[i] = values[i];
                return make_const_list(arena_, type->list.element, arr, n);
            }
            case ConstantTypeTag::Pair: {
                Constant* first  = decode_value(type->pair.first);
                Constant* second = decode_value(type->pair.second);
                return make_const_pair(arena_, type->pair.first, type->pair.second,
                                       first, second);
            }
            case ConstantTypeTag::Array:
            case ConstantTypeTag::Bls12_381_G1:
            case ConstantTypeTag::Bls12_381_G2:
            case ConstantTypeTag::Bls12_381_MlResult:
            case ConstantTypeTag::Value:
                throw ParseError("flat: constant type not encodable in flat format");
        }
        throw ParseError("flat: internal unhandled constant type");
    }

    Arena&                     arena_;
    FlatDecoder&               d_;
    std::vector<std::uint8_t>  tags_;
    std::size_t                tag_idx_ = 0;
};

// ---------------------------------------------------------------------------
// Term decoder — explicit work stack so deeply nested programs don't blow
// the C++ stack. Mirrors TS flat.ts decodeTerm.
// ---------------------------------------------------------------------------
enum class DecodeFrameKind : std::uint8_t {
    Visit,
    BuildDelay,
    BuildLambda,
    BuildApply,
    BuildForce,
    BuildConstr,
    ContinueConstr,
    BuildCase,
    ContinueCase,
};

struct DecodeFrame {
    DecodeFrameKind kind;
    std::uint64_t   index;  // Constr tag, when applicable
    std::uint32_t   count;  // accumulated field/branch count
};

Term* decode_term(Arena& arena, FlatDecoder& d) {
    std::vector<Term*>       results;
    std::vector<DecodeFrame> stack;
    stack.push_back(DecodeFrame{DecodeFrameKind::Visit, 0, 0});

    while (!stack.empty()) {
        DecodeFrame frame = stack.back();
        stack.pop_back();

        switch (frame.kind) {
            case DecodeFrameKind::Visit: {
                std::uint32_t tag = d.bits8(4);
                switch (tag) {
                    case 0: {  // var
                        std::uint64_t idx = d.word();
                        results.push_back(make_var(
                            arena,
                            Binder{static_cast<std::uint32_t>(idx), nullptr}));
                        break;
                    }
                    case 1:
                        stack.push_back(DecodeFrame{DecodeFrameKind::BuildDelay, 0, 0});
                        stack.push_back(DecodeFrame{DecodeFrameKind::Visit, 0, 0});
                        break;
                    case 2:
                        stack.push_back(DecodeFrame{DecodeFrameKind::BuildLambda, 0, 0});
                        stack.push_back(DecodeFrame{DecodeFrameKind::Visit, 0, 0});
                        break;
                    case 3:
                        stack.push_back(DecodeFrame{DecodeFrameKind::BuildApply, 0, 0});
                        stack.push_back(DecodeFrame{DecodeFrameKind::Visit, 0, 0});  // argument
                        stack.push_back(DecodeFrame{DecodeFrameKind::Visit, 0, 0});  // function
                        break;
                    case 4: {
                        ConstantDecoder cd(arena, d);
                        Constant* c = cd.decode();
                        results.push_back(make_constant(arena, c));
                        break;
                    }
                    case 5:
                        stack.push_back(DecodeFrame{DecodeFrameKind::BuildForce, 0, 0});
                        stack.push_back(DecodeFrame{DecodeFrameKind::Visit, 0, 0});
                        break;
                    case 6:
                        results.push_back(make_error(arena));
                        break;
                    case 7: {
                        std::uint8_t fn_tag = static_cast<std::uint8_t>(d.bits8(7));
                        auto b = builtin_from_tag(fn_tag);
                        if (!b) {
                            throw ParseError("flat: invalid builtin tag " +
                                             std::to_string(fn_tag));
                        }
                        results.push_back(make_builtin(arena, *b));
                        break;
                    }
                    case 8: {
                        std::uint64_t idx = d.word();
                        stack.push_back(DecodeFrame{
                            DecodeFrameKind::ContinueConstr, idx, 0});
                        break;
                    }
                    case 9:
                        stack.push_back(DecodeFrame{DecodeFrameKind::ContinueCase, 0, 0});
                        stack.push_back(DecodeFrame{DecodeFrameKind::Visit, 0, 0});
                        break;
                    default:
                        throw ParseError("flat: invalid term tag " +
                                         std::to_string(tag));
                }
                break;
            }
            case DecodeFrameKind::BuildDelay: {
                Term* inner = results.back(); results.pop_back();
                results.push_back(make_delay(arena, inner));
                break;
            }
            case DecodeFrameKind::BuildLambda: {
                Term* inner = results.back(); results.pop_back();
                results.push_back(
                    make_lambda(arena, Binder{0u, nullptr}, inner));
                break;
            }
            case DecodeFrameKind::BuildApply: {
                Term* arg = results.back(); results.pop_back();
                Term* fn  = results.back(); results.pop_back();
                results.push_back(make_apply(arena, fn, arg));
                break;
            }
            case DecodeFrameKind::BuildForce: {
                Term* inner = results.back(); results.pop_back();
                results.push_back(make_force(arena, inner));
                break;
            }
            case DecodeFrameKind::ContinueConstr: {
                if (d.bit()) {
                    stack.push_back(DecodeFrame{
                        DecodeFrameKind::ContinueConstr,
                        frame.index,
                        frame.count + 1});
                    stack.push_back(DecodeFrame{DecodeFrameKind::Visit, 0, 0});
                } else {
                    stack.push_back(DecodeFrame{
                        DecodeFrameKind::BuildConstr,
                        frame.index,
                        frame.count});
                }
                break;
            }
            case DecodeFrameKind::BuildConstr: {
                std::uint32_t n = frame.count;
                Term** fields = arena.alloc_array_uninit<Term*>(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    fields[n - 1 - i] = results.back();
                    results.pop_back();
                }
                results.push_back(make_constr(arena, frame.index, fields, n));
                break;
            }
            case DecodeFrameKind::ContinueCase: {
                if (d.bit()) {
                    stack.push_back(DecodeFrame{
                        DecodeFrameKind::ContinueCase, 0, frame.count + 1});
                    stack.push_back(DecodeFrame{DecodeFrameKind::Visit, 0, 0});
                } else {
                    stack.push_back(DecodeFrame{
                        DecodeFrameKind::BuildCase, 0, frame.count});
                }
                break;
            }
            case DecodeFrameKind::BuildCase: {
                std::uint32_t n = frame.count;
                Term** branches = arena.alloc_array_uninit<Term*>(n);
                for (std::uint32_t i = 0; i < n; ++i) {
                    branches[n - 1 - i] = results.back();
                    results.pop_back();
                }
                Term* scrutinee = results.back(); results.pop_back();
                results.push_back(make_case(arena, scrutinee, branches, n));
                break;
            }
        }
    }

    if (results.size() != 1) {
        throw ParseError("flat: decoder left " + std::to_string(results.size()) +
                         " results on stack");
    }
    return results.back();
}

// ---------------------------------------------------------------------------
// FlatEncoder — bit-level writer mirroring TS flat.ts.
// ---------------------------------------------------------------------------
class FlatEncoder {
public:
    void bit(bool v) {
        if (v) current_ |= (128u >> used_bits_);
        if (used_bits_ == 7) {
            out_.push_back(current_);
            current_ = 0;
            used_bits_ = 0;
        } else {
            ++used_bits_;
        }
    }

    void bits(std::uint32_t n, std::uint32_t val) {
        for (std::int32_t i = static_cast<std::int32_t>(n) - 1; i >= 0; --i) {
            bit((val & (1u << i)) != 0);
        }
    }

    void byte8(std::uint8_t v) { bits(8, v); }

    void word(std::uint64_t v) {
        for (;;) {
            std::uint8_t chunk = static_cast<std::uint8_t>(v & 0x7f);
            v >>= 7;
            if (v == 0) { byte8(chunk); break; }
            byte8(static_cast<std::uint8_t>(chunk | 0x80u));
        }
    }

    // Write an mpz (non-negative) in 7-bit little-endian chunks.
    void big_word_mpz(mpz_srcptr v) {
        if (mpz_sgn(v) == 0) {
            byte8(0);
            return;
        }
        mpz_t tmp;
        mpz_init_set(tmp, v);
        std::vector<std::uint8_t> chunks;
        while (mpz_sgn(tmp) > 0) {
            chunks.push_back(static_cast<std::uint8_t>(
                mpz_fdiv_ui(tmp, 128)));
            mpz_fdiv_q_2exp(tmp, tmp, 7);
        }
        mpz_clear(tmp);
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            bool last = (i + 1 == chunks.size());
            byte8(static_cast<std::uint8_t>(
                chunks[i] | (last ? 0u : 0x80u)));
        }
    }

    // Signed integer via zigzag.
    void integer_mpz(mpz_srcptr v) {
        mpz_t zz;
        mpz_init(zz);
        if (mpz_sgn(v) >= 0) {
            mpz_mul_2exp(zz, v, 1);  // zz = v * 2
        } else {
            mpz_neg(zz, v);          // zz = -v
            mpz_mul_2exp(zz, zz, 1); // zz = -v * 2
            mpz_sub_ui(zz, zz, 1);   // zz = -v * 2 - 1
        }
        big_word_mpz(zz);
        mpz_clear(zz);
    }

    void byte_array(const std::uint8_t* data, std::size_t len) {
        filler();
        std::size_t off = 0;
        while (off < len) {
            std::size_t chunk = std::min(len - off, static_cast<std::size_t>(255));
            out_.push_back(static_cast<std::uint8_t>(chunk));
            out_.insert(out_.end(), data + off, data + off + chunk);
            off += chunk;
        }
        out_.push_back(0);
    }

    void filler() {
        // Round up to the next byte boundary, ending with a 1-bit.
        if (used_bits_ == 0) {
            out_.push_back(0x01);
        } else {
            std::uint32_t remaining = 8 - used_bits_;
            for (std::uint32_t i = 0; i < remaining - 1; ++i) bit(false);
            bit(true);
        }
    }

    std::vector<std::uint8_t> take() {
        return std::move(out_);
    }

private:
    std::vector<std::uint8_t> out_;
    std::uint8_t              current_ = 0;
    std::uint32_t             used_bits_ = 0;
};

void encode_constant(FlatEncoder& e, const Constant& c);

void encode_type_tag(FlatEncoder& e, std::uint32_t tag) {
    e.bit(true);
    e.bits(4, tag);
}
void encode_type_end(FlatEncoder& e) { e.bit(false); }

void encode_type_tags(FlatEncoder& e, const ConstantType& t) {
    switch (t.tag) {
        case ConstantTypeTag::Integer:            encode_type_tag(e, 0); break;
        case ConstantTypeTag::ByteString:         encode_type_tag(e, 1); break;
        case ConstantTypeTag::String:             encode_type_tag(e, 2); break;
        case ConstantTypeTag::Unit:               encode_type_tag(e, 3); break;
        case ConstantTypeTag::Bool:               encode_type_tag(e, 4); break;
        case ConstantTypeTag::Data:               encode_type_tag(e, 8); break;
        case ConstantTypeTag::List:
            encode_type_tag(e, 7);
            encode_type_tag(e, 5);
            encode_type_tags(e, *t.list.element);
            break;
        case ConstantTypeTag::Pair:
            encode_type_tag(e, 7);
            encode_type_tag(e, 7);
            encode_type_tag(e, 6);
            encode_type_tags(e, *t.pair.first);
            encode_type_tags(e, *t.pair.second);
            break;
        default:
            throw ParseError("flat: unsupported type for encoding");
    }
}

void encode_constant_value(FlatEncoder& e, const Constant& c) {
    switch (c.tag) {
        case ConstTag::Integer:
            e.integer_mpz(c.integer.value->value);
            break;
        case ConstTag::ByteString:
            e.byte_array(c.bytestring.bytes, c.bytestring.len);
            break;
        case ConstTag::String:
            e.byte_array(reinterpret_cast<const std::uint8_t*>(c.string.utf8),
                         c.string.len);
            break;
        case ConstTag::Unit:
            break;
        case ConstTag::Bool:
            e.bit(c.boolean.value);
            break;
        case ConstTag::Data: {
            auto bytes = encode_plutus_data(*c.data.value);
            e.byte_array(bytes.data(), bytes.size());
            break;
        }
        case ConstTag::List:
            for (std::uint32_t i = 0; i < c.list.n_values; ++i) {
                e.bit(true);
                encode_constant_value(e, *c.list.values[i]);
            }
            e.bit(false);
            break;
        case ConstTag::Pair:
            encode_constant_value(e, *c.pair.first);
            encode_constant_value(e, *c.pair.second);
            break;
        default:
            throw ParseError("flat: unsupported constant value for encoding");
    }
}

void encode_constant(FlatEncoder& e, const Constant& c) {
    switch (c.tag) {
        case ConstTag::Integer:
            encode_type_tag(e, 0);
            encode_type_end(e);
            e.integer_mpz(c.integer.value->value);
            break;
        case ConstTag::ByteString:
            encode_type_tag(e, 1);
            encode_type_end(e);
            e.byte_array(c.bytestring.bytes, c.bytestring.len);
            break;
        case ConstTag::String:
            encode_type_tag(e, 2);
            encode_type_end(e);
            e.byte_array(reinterpret_cast<const std::uint8_t*>(c.string.utf8),
                         c.string.len);
            break;
        case ConstTag::Unit:
            encode_type_tag(e, 3);
            encode_type_end(e);
            break;
        case ConstTag::Bool:
            encode_type_tag(e, 4);
            encode_type_end(e);
            e.bit(c.boolean.value);
            break;
        case ConstTag::Data: {
            encode_type_tag(e, 8);
            encode_type_end(e);
            auto bytes = encode_plutus_data(*c.data.value);
            e.byte_array(bytes.data(), bytes.size());
            break;
        }
        case ConstTag::List:
            encode_type_tag(e, 7);
            encode_type_tag(e, 5);
            encode_type_tags(e, *c.list.item_type);
            encode_type_end(e);
            for (std::uint32_t i = 0; i < c.list.n_values; ++i) {
                e.bit(true);
                encode_constant_value(e, *c.list.values[i]);
            }
            e.bit(false);
            break;
        case ConstTag::Pair:
            encode_type_tag(e, 7);
            encode_type_tag(e, 7);
            encode_type_tag(e, 6);
            encode_type_tags(e, *c.pair.fst_type);
            encode_type_tags(e, *c.pair.snd_type);
            encode_type_end(e);
            encode_constant_value(e, *c.pair.first);
            encode_constant_value(e, *c.pair.second);
            break;
        default:
            throw ParseError("flat: constant type not encodable in flat format");
    }
}

// Iterative term encoder — mirrors TS flat.ts encodeTermFlat.
enum class EncodeFrameKind : std::uint8_t { Term, EmitBit };

struct EncodeFrame {
    EncodeFrameKind kind;
    Term*           term;
    bool            bit_value;
};

void encode_term(FlatEncoder& e, Term* root) {
    std::vector<EncodeFrame> stack;
    stack.push_back(EncodeFrame{EncodeFrameKind::Term, root, false});

    while (!stack.empty()) {
        EncodeFrame frame = stack.back();
        stack.pop_back();

        if (frame.kind == EncodeFrameKind::EmitBit) {
            e.bit(frame.bit_value);
            continue;
        }

        Term* t = frame.term;
        switch (t->tag) {
            case TermTag::Var:
                e.bits(4, 0);
                e.word(t->var.binder.id);
                break;
            case TermTag::Delay:
                e.bits(4, 1);
                stack.push_back(EncodeFrame{EncodeFrameKind::Term, t->delay.term, false});
                break;
            case TermTag::Lambda:
                e.bits(4, 2);
                stack.push_back(EncodeFrame{EncodeFrameKind::Term, t->lambda.body, false});
                break;
            case TermTag::Apply:
                e.bits(4, 3);
                stack.push_back(EncodeFrame{EncodeFrameKind::Term, t->apply.argument, false});
                stack.push_back(EncodeFrame{EncodeFrameKind::Term, t->apply.function, false});
                break;
            case TermTag::Constant:
                e.bits(4, 4);
                encode_constant(e, *t->constant.value);
                break;
            case TermTag::Force:
                e.bits(4, 5);
                stack.push_back(EncodeFrame{EncodeFrameKind::Term, t->force.term, false});
                break;
            case TermTag::Error:
                e.bits(4, 6);
                break;
            case TermTag::Builtin: {
                e.bits(4, 7);
                e.bits(7, static_cast<std::uint32_t>(t->builtin.function));
                break;
            }
            case TermTag::Constr: {
                e.bits(4, 8);
                e.word(t->constr.tag_index);
                stack.push_back(EncodeFrame{EncodeFrameKind::EmitBit, nullptr, false});
                for (std::int32_t i = static_cast<std::int32_t>(t->constr.n_fields) - 1;
                     i >= 0; --i) {
                    stack.push_back(EncodeFrame{EncodeFrameKind::Term, t->constr.fields[i], false});
                    stack.push_back(EncodeFrame{EncodeFrameKind::EmitBit, nullptr, true});
                }
                break;
            }
            case TermTag::Case: {
                e.bits(4, 9);
                stack.push_back(EncodeFrame{EncodeFrameKind::EmitBit, nullptr, false});
                for (std::int32_t i = static_cast<std::int32_t>(t->case_.n_branches) - 1;
                     i >= 0; --i) {
                    stack.push_back(EncodeFrame{EncodeFrameKind::Term, t->case_.branches[i], false});
                    stack.push_back(EncodeFrame{EncodeFrameKind::EmitBit, nullptr, true});
                }
                stack.push_back(EncodeFrame{EncodeFrameKind::Term, t->case_.scrutinee, false});
                break;
            }
        }
    }
}

}  // namespace

Program decode_flat(Arena& arena, const std::uint8_t* bytes, std::size_t len) {
    FlatDecoder d(bytes, len);
    auto major = static_cast<std::uint32_t>(d.word());
    auto minor = static_cast<std::uint32_t>(d.word());
    auto patch = static_cast<std::uint32_t>(d.word());
    Term* term = decode_term(arena, d);
    d.filler();
    return Program{Version{major, minor, patch}, term, /*is_debruijn=*/true};
}

std::vector<std::uint8_t> encode_flat(const Program& program) {
    if (!program.is_debruijn) {
        throw ParseError("flat: encoder requires a de-Bruijn program");
    }
    FlatEncoder e;
    e.word(program.version.major);
    e.word(program.version.minor);
    e.word(program.version.patch);
    encode_term(e, program.term);
    e.filler();
    return e.take();
}

}  // namespace uplc
