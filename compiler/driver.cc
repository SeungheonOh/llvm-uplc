#include "compiler/driver.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "compiler/ast/arena.h"
#include "compiler/ast/pretty.h"
#include "compiler/ast/term.h"
#include "compiler/frontend/cbor_unwrap.h"
#include "compiler/frontend/flat.h"
#include "compiler/frontend/name_to_debruijn.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/validate.h"

namespace uplc {

std::string frontend_parse_to_debruijn_text(std::string_view source) {
    Arena arena;
    Program named = parse_program(arena, source);
    validate(named);
    Program db      = name_to_debruijn(arena, named);
    validate(db);
    Program renamed = debruijn_to_name(arena, db);
    return pretty_print_program(renamed);
}

std::string frontend_roundtrip_named(std::string_view source) {
    Arena arena;
    Program named = parse_program(arena, source);
    validate(named);
    Program db = name_to_debruijn(arena, named);
    Program renamed = debruijn_to_name(arena, db);
    return pretty_print_program(renamed);
}

void frontend_check(std::string_view source) {
    Arena arena;
    Program named = parse_program(arena, source);
    validate(named);
    Program db = name_to_debruijn(arena, named);
    validate(db);
    (void)db;
}

std::string frontend_parse_flat_bytes(const std::uint8_t* bytes, std::size_t len) {
    Arena arena;
    Program db      = decode_flat(arena, bytes, len);
    validate(db);
    Program renamed = debruijn_to_name(arena, db);
    return pretty_print_program(renamed);
}

std::string frontend_parse_cbor_bytes(const std::uint8_t* bytes, std::size_t len) {
    auto unwrapped = cbor_unwrap(bytes, len);
    return frontend_parse_flat_bytes(unwrapped.data(), unwrapped.size());
}

std::string frontend_flat_round_trip(std::string_view source) {
    Arena arena;
    Program named = parse_program(arena, source);
    Program db      = name_to_debruijn(arena, named);
    auto bytes      = encode_flat(db);
    Program db2     = decode_flat(arena, bytes.data(), bytes.size());
    Program renamed = debruijn_to_name(arena, db2);
    return pretty_print_program(renamed);
}

}  // namespace uplc
