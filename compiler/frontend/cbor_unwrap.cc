#include "compiler/frontend/cbor_unwrap.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace uplc {

namespace {

// Parse a single CBOR byte-string head at `pos`. Returns true and the
// payload span via out-params if the next item is a byte string; otherwise
// returns false (leaving `pos` unchanged).
bool peel_one(const std::uint8_t* data, std::size_t len, std::size_t& pos,
              std::vector<std::uint8_t>& out_payload) {
    if (pos >= len) return false;
    std::uint8_t initial = data[pos];
    std::uint8_t major = initial >> 5;
    if (major != 2) return false;
    std::uint8_t additional = initial & 0x1fu;
    ++pos;

    if (additional == 0x1f) {
        // Indefinite-length byte string: sequence of definite-length chunks
        // terminated by 0xff.
        for (;;) {
            if (pos >= len) throw std::runtime_error("cbor_unwrap: end of input");
            std::uint8_t nb = data[pos++];
            if (nb == 0xff) break;
            if ((nb >> 5) != 2) throw std::runtime_error("cbor_unwrap: bad chunk");
            std::uint8_t add2 = nb & 0x1fu;
            std::uint64_t clen;
            if (add2 < 24)      clen = add2;
            else if (add2 == 24) {
                if (pos >= len) throw std::runtime_error("cbor_unwrap: eof in length");
                clen = data[pos++];
            } else if (add2 == 25) {
                if (pos + 2 > len) throw std::runtime_error("cbor_unwrap: eof in length");
                clen = (static_cast<std::uint64_t>(data[pos]) << 8) | data[pos + 1];
                pos += 2;
            } else if (add2 == 26) {
                if (pos + 4 > len) throw std::runtime_error("cbor_unwrap: eof in length");
                clen = (static_cast<std::uint64_t>(data[pos]) << 24) |
                       (static_cast<std::uint64_t>(data[pos + 1]) << 16) |
                       (static_cast<std::uint64_t>(data[pos + 2]) << 8) |
                       data[pos + 3];
                pos += 4;
            } else if (add2 == 27) {
                if (pos + 8 > len) throw std::runtime_error("cbor_unwrap: eof in length");
                clen = 0;
                for (int i = 0; i < 8; ++i) {
                    clen = (clen << 8) | data[pos + static_cast<std::size_t>(i)];
                }
                pos += 8;
            } else {
                throw std::runtime_error(
                    "cbor_unwrap: bad additional info " + std::to_string(add2));
            }
            if (pos + clen > len) throw std::runtime_error("cbor_unwrap: eof in chunk");
            out_payload.insert(out_payload.end(), data + pos, data + pos + clen);
            pos += clen;
        }
        return true;
    }

    // Definite-length byte string.
    std::uint64_t n;
    if (additional < 24) n = additional;
    else if (additional == 24) {
        if (pos >= len) throw std::runtime_error("cbor_unwrap: eof in length");
        n = data[pos++];
    } else if (additional == 25) {
        if (pos + 2 > len) throw std::runtime_error("cbor_unwrap: eof in length");
        n = (static_cast<std::uint64_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
    } else if (additional == 26) {
        if (pos + 4 > len) throw std::runtime_error("cbor_unwrap: eof in length");
        n = (static_cast<std::uint64_t>(data[pos]) << 24) |
            (static_cast<std::uint64_t>(data[pos + 1]) << 16) |
            (static_cast<std::uint64_t>(data[pos + 2]) << 8) |
            data[pos + 3];
        pos += 4;
    } else if (additional == 27) {
        if (pos + 8 > len) throw std::runtime_error("cbor_unwrap: eof in length");
        n = 0;
        for (int i = 0; i < 8; ++i) {
            n = (n << 8) | data[pos + static_cast<std::size_t>(i)];
        }
        pos += 8;
    } else {
        throw std::runtime_error(
            "cbor_unwrap: bad additional info " + std::to_string(additional));
    }
    if (pos + n > len) throw std::runtime_error("cbor_unwrap: eof in payload");
    out_payload.insert(out_payload.end(), data + pos, data + pos + n);
    pos += static_cast<std::size_t>(n);
    return true;
}

}  // namespace

std::vector<std::uint8_t> cbor_unwrap(const std::uint8_t* bytes, std::size_t len) {
    // Peel up to two layers — on-chain scripts are usually single-wrapped, but
    // the script-context format double-wraps. Stop as soon as the next item
    // isn't a CBOR byte string.
    std::vector<std::uint8_t> current(bytes, bytes + len);
    for (int i = 0; i < 2; ++i) {
        std::vector<std::uint8_t> peeled;
        std::size_t pos = 0;
        bool ok = peel_one(current.data(), current.size(), pos, peeled);
        if (!ok) break;
        if (pos != current.size()) {
            // Extra trailing bytes — leave unpeeled.
            break;
        }
        current = std::move(peeled);
    }
    return current;
}

}  // namespace uplc
