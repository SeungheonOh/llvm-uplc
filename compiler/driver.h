#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace uplc {

// Drive the full text-frontend pipeline on `source`:
//   lex -> parse -> validate (named) -> name_to_debruijn -> validate (db) ->
//   pretty-print (de-Bruijn)
//
// Returns the pretty-printed program on success. Throws ParseError,
// ConvertError, or ValidationError (all subclasses of std::runtime_error)
// on failure.
std::string frontend_parse_to_debruijn_text(std::string_view source);

// Round-trip: parse -> convert -> dename -> pretty-print named. Used by the
// round-trip test: the resulting text should parse again to the same
// (alpha-equivalent) de-Bruijn program.
std::string frontend_roundtrip_named(std::string_view source);

// Parse and discard, returning success/failure via exception. Used by the
// `check` subcommand.
void frontend_check(std::string_view source);

// Decode a flat-encoded program (already de-Bruijn) and pretty-print it.
std::string frontend_parse_flat_bytes(const std::uint8_t* bytes, std::size_t len);

// CBOR-unwrap (possibly double-wrapped) and then flat-decode + pretty-print.
std::string frontend_parse_cbor_bytes(const std::uint8_t* bytes, std::size_t len);

// Full flat round-trip: parse text -> convert -> encode flat -> decode flat
// -> pretty-print. Returns the final de-Bruijn text. Used by tests.
std::string frontend_flat_round_trip(std::string_view source);

}  // namespace uplc
