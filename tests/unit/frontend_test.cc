// Inline unit tests for the M1 frontend: lexer + parser + deBruijn convert +
// pretty printer. Small, self-contained fixtures — the bulk-over-conformance
// sweep lives in tests/conformance/round_trip_test.cc.
//
// This is a hand-rolled test harness (no gtest dependency yet) so M1 can land
// without pulling in a test framework. Each CHECK prints PASS/FAIL and updates
// the return code.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "compiler/ast/arena.h"
#include "compiler/ast/cbor_data.h"
#include "compiler/ast/pretty.h"
#include "compiler/ast/term.h"
#include "compiler/driver.h"
#include "compiler/frontend/cbor_unwrap.h"
#include "compiler/frontend/flat.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/validate.h"

#include <cstdint>
#include <vector>

namespace {

int g_failures = 0;
int g_total    = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_total;                                                       \
        if (!(cond)) {                                                   \
            ++g_failures;                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
        }                                                                \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                   \
    do {                                                                 \
        ++g_total;                                                       \
        std::string a_ = (actual);                                       \
        std::string e_ = (expected);                                     \
        if (a_ != e_) {                                                  \
            ++g_failures;                                                \
            std::fprintf(stderr,                                         \
                         "FAIL %s:%d:\n  expected: %s\n  actual:   %s\n", \
                         __FILE__, __LINE__, e_.c_str(), a_.c_str());    \
        }                                                                \
    } while (0)

// ---------------------------------------------------------------------------
// Parser smoke tests — simple programs round-trip to a stable deBruijn form.
// ---------------------------------------------------------------------------
void test_identity_lambda() {
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text("(program 1.0.0 (lam x x))"),
        "(program 1.0.0 (lam v0 v0))");
}

void test_const_integer() {
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text("(program 1.0.0 (con integer 42))"),
        "(program 1.0.0 (con integer 42))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text("(program 1.0.0 (con integer -7))"),
        "(program 1.0.0 (con integer -7))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text("(program 1.0.0 (con integer +7))"),
        "(program 1.0.0 (con integer 7))");
}

void test_error() {
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text("(program 1.0.0 (error))"),
        "(program 1.0.0 (error))");
}

void test_apply_left_nested() {
    // [ a b c ] should become [[a b] c]
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (lam a (lam b (lam c [ a b c ]))))"),
        "(program 1.0.0 (lam v0 (lam v1 (lam v2 [[v0 v1] v2]))))");
}

void test_church_succ() {
    // The canonical churchSucc fixture.
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (lam n (delay (lam z (lam f [ f [ [ (force n) z ] f ] ])))))"),
        "(program 1.0.0 (lam v0 (delay (lam v1 (lam v2 [v2 [[(force v0) v1] v2]])))))");
}

void test_builtins() {
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (builtin addInteger))"),
        "(program 1.0.0 (builtin addInteger))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (builtin ifThenElse))"),
        "(program 1.0.0 (builtin ifThenElse))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (builtin bls12_381_finalVerify))"),
        "(program 1.0.0 (builtin bls12_381_finalVerify))");
}

void test_bytestring_and_string() {
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con bytestring #deadbeef))"),
        "(program 1.0.0 (con bytestring #deadbeef))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con bytestring #))"),
        "(program 1.0.0 (con bytestring #))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con string \"hello\"))"),
        "(program 1.0.0 (con string \"hello\"))");
}

void test_unit_and_bool() {
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con unit ()))"),
        "(program 1.0.0 (con unit ()))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con bool True))"),
        "(program 1.0.0 (con bool True))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con bool False))"),
        "(program 1.0.0 (con bool False))");
}

void test_constr_case_1_1_0() {
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.1.0 (constr 3 (con integer 1) (con integer 2)))"),
        "(program 1.1.0 (constr 3 (con integer 1) (con integer 2)))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.1.0 (case (constr 0) (lam x (error))))"),
        "(program 1.1.0 (case (constr 0) (lam v0 (error))))");
}

void test_constr_rejected_before_1_1_0() {
    bool threw = false;
    try {
        (void)uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (constr 0))");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// Free variable handling — parser accepts, converter rejects (matches TS
// split: `(program 1.0.0 x)` is a parse-OK, eval-fail fixture).
// ---------------------------------------------------------------------------
void test_free_var_is_convert_error() {
    bool threw_at_convert = false;
    try {
        uplc::Arena arena;
        uplc::Program named = uplc::parse_program(arena, "(program 1.0.0 x)");
        uplc::validate(named);  // must not throw
        (void)uplc::name_to_debruijn(arena, named);  // must throw
    } catch (const uplc::ConvertError&) {
        threw_at_convert = true;
    }
    CHECK(threw_at_convert);
}

// ---------------------------------------------------------------------------
// Round-trip: parse -> convert -> dename -> pretty -> parse -> convert
// must produce the same de-Bruijn string as the direct path.
// ---------------------------------------------------------------------------
void test_round_trip_stability() {
    const char* src =
        "(program 1.0.0 (lam f (lam x [ [ f x ] [ f x ] ])))";

    std::string direct = uplc::frontend_parse_to_debruijn_text(src);
    std::string round  = uplc::frontend_roundtrip_named(src);
    std::string stable = uplc::frontend_parse_to_debruijn_text(round);
    CHECK_EQ_STR(stable, direct);
}

// ---------------------------------------------------------------------------
// Data constants.
// ---------------------------------------------------------------------------
void test_data_constants() {
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con data (I 42)))"),
        "(program 1.0.0 (con data (I 42)))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con data (B #cafe)))"),
        "(program 1.0.0 (con data (B #cafe)))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con data (List [I 1, I 2, I 3])))"),
        "(program 1.0.0 (con data (List [I 1, I 2, I 3])))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con data (Constr 0 [I 1, B #ff])))"),
        "(program 1.0.0 (con data (Constr 0 [I 1, B #ff])))");
}

// ---------------------------------------------------------------------------
// List / pair / array constants.
// ---------------------------------------------------------------------------
void test_list_pair_array_constants() {
    // Inner elements are printed without their type prefix, matching the
    // Plutus conformance `.expected` format.
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con (list integer) [1, 2, 3]))"),
        "(program 1.0.0 (con (list integer) [1, 2, 3]))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con (pair integer bool) (1, True)))"),
        "(program 1.0.0 (con (pair integer bool) (1, True)))");
    CHECK_EQ_STR(
        uplc::frontend_parse_to_debruijn_text(
            "(program 1.0.0 (con (array integer) [1,2,3]))"),
        "(program 1.0.0 (con (array integer) [1,2,3]))");
}

// ---------------------------------------------------------------------------
// CBOR Data codec: encode/decode round-trip on a mix of shapes.
// ---------------------------------------------------------------------------
void test_cbor_data_roundtrip() {
    const char* sources[] = {
        "(program 1.0.0 (con data (I 0)))",
        "(program 1.0.0 (con data (I 42)))",
        "(program 1.0.0 (con data (I -7)))",
        "(program 1.0.0 (con data (I 123456789012345678901234567890)))",
        "(program 1.0.0 (con data (I -123456789012345678901234567890)))",
        "(program 1.0.0 (con data (B #)))",
        "(program 1.0.0 (con data (B #deadbeefcafebabef00dfeedfaceb00c)))",
        "(program 1.0.0 (con data (List [])))",
        "(program 1.0.0 (con data (List [I 1, I 2, I 3])))",
        "(program 1.0.0 (con data (Map [])))",
        "(program 1.0.0 (con data (Map [(I 1, B #aa), (I 2, List [])])))",
        "(program 1.0.0 (con data (Constr 0 [])))",
        "(program 1.0.0 (con data (Constr 7 [I 1, B #ff])))",
        "(program 1.0.0 (con data (Constr 128 [I 1])))",
    };

    for (const char* src : sources) {
        uplc::Arena a;
        uplc::Program p = uplc::parse_program(a, src);
        // Reach into the wrapped data constant and round-trip through CBOR.
        auto* term = p.term;
        if (term->tag != uplc::TermTag::Constant) { CHECK(false); continue; }
        auto* con = term->constant.value;
        if (con->tag != uplc::ConstTag::Data) { CHECK(false); continue; }
        auto bytes = uplc::encode_plutus_data(*con->data.value);
        uplc::Arena a2;
        auto* decoded = uplc::decode_plutus_data(a2, bytes.data(), bytes.size());
        CHECK_EQ_STR(uplc::print_plutus_data(*decoded),
                     uplc::print_plutus_data(*con->data.value));
    }
}

// ---------------------------------------------------------------------------
// Flat round-trip: parse text -> convert -> encode flat -> decode flat ->
// pretty-print must match the direct text->deBruijn path.
// ---------------------------------------------------------------------------
void test_flat_round_trip() {
    const char* sources[] = {
        "(program 1.0.0 (lam x x))",
        "(program 1.0.0 (con integer 42))",
        "(program 1.0.0 (error))",
        "(program 1.0.0 (lam f (lam x [f x])))",
        "(program 1.0.0 (delay (force (con unit ()))))",
        "(program 1.0.0 [ (builtin addInteger) (con integer 1) (con integer 2) ])",
        "(program 1.0.0 (con bytestring #deadbeef))",
        "(program 1.0.0 (con string \"hello\"))",
        "(program 1.0.0 (con bool True))",
        "(program 1.0.0 (con (list integer) [1, 2, 3]))",
        "(program 1.0.0 (con (pair integer bool) (7, False)))",
        "(program 1.0.0 (con data (Constr 0 [I 1, B #abcd])))",
        "(program 1.1.0 (constr 3 (con integer 1) (error)))",
        "(program 1.1.0 (case (constr 0) (lam x x)))",
    };

    for (const char* src : sources) {
        std::string direct = uplc::frontend_parse_to_debruijn_text(src);
        std::string round  = uplc::frontend_flat_round_trip(src);
        CHECK_EQ_STR(round, direct);
    }
}

// ---------------------------------------------------------------------------
// CBOR unwrap — exercise the peel-once and peel-twice paths with
// hand-constructed inputs.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> cbor_wrap_bytes(const std::vector<std::uint8_t>& payload) {
    // Always emit a 2-byte-length (additional info 25) header so we cover the
    // non-small-header path.
    std::vector<std::uint8_t> out;
    out.push_back(0x40 | 25);  // major 2 (byte string), 25 = 2-byte length
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void test_cbor_unwrap_identity_on_raw_flat() {
    // A raw flat blob doesn't start with a major-2 byte, so cbor_unwrap
    // should return it unchanged.
    std::vector<std::uint8_t> raw = {0x00, 0x01, 0x02};
    auto out = uplc::cbor_unwrap(raw.data(), raw.size());
    CHECK(out == raw);
}

void test_cbor_unwrap_single() {
    std::vector<std::uint8_t> payload = {0xca, 0xfe, 0xba, 0xbe};
    auto wrapped = cbor_wrap_bytes(payload);
    auto unwrapped = uplc::cbor_unwrap(wrapped.data(), wrapped.size());
    CHECK(unwrapped == payload);
}

void test_cbor_unwrap_double() {
    std::vector<std::uint8_t> payload = {0xde, 0xad, 0xbe, 0xef};
    auto single = cbor_wrap_bytes(payload);
    auto doubled = cbor_wrap_bytes(single);
    auto unwrapped = uplc::cbor_unwrap(doubled.data(), doubled.size());
    CHECK(unwrapped == payload);
}

void test_cbor_unwrap_flat_program_round_trip() {
    // Parse text -> encode flat -> cbor-wrap once -> cbor_unwrap ->
    // decode_flat -> pretty-print.  Must match the direct text->deBruijn path.
    const char* src =
        "(program 1.0.0 (lam x [ (builtin addInteger) x (con integer 1) ]))";

    uplc::Arena arena;
    uplc::Program named = uplc::parse_program(arena, src);
    uplc::Program db    = uplc::name_to_debruijn(arena, named);
    auto flat = uplc::encode_flat(db);
    auto wrapped = cbor_wrap_bytes(flat);

    auto unwrapped = uplc::cbor_unwrap(wrapped.data(), wrapped.size());
    uplc::Arena a2;
    uplc::Program db2     = uplc::decode_flat(a2, unwrapped.data(), unwrapped.size());
    uplc::Program renamed = uplc::debruijn_to_name(a2, db2);

    CHECK_EQ_STR(uplc::pretty_print_program(renamed),
                 uplc::frontend_parse_to_debruijn_text(src));
}

}  // namespace

int main() {
    test_identity_lambda();
    test_const_integer();
    test_error();
    test_apply_left_nested();
    test_church_succ();
    test_builtins();
    test_bytestring_and_string();
    test_unit_and_bool();
    test_constr_case_1_1_0();
    test_constr_rejected_before_1_1_0();
    test_free_var_is_convert_error();
    test_round_trip_stability();
    test_data_constants();
    test_list_pair_array_constants();
    test_cbor_data_roundtrip();
    test_flat_round_trip();
    test_cbor_unwrap_identity_on_raw_flat();
    test_cbor_unwrap_single();
    test_cbor_unwrap_double();
    test_cbor_unwrap_flat_program_round_trip();

    std::fprintf(stderr, "%d/%d passed\n", g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
