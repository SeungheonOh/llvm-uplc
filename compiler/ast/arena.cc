#include "compiler/ast/arena.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace uplc {

namespace {
constexpr std::size_t kDefaultChunkBytes = 64 * 1024;
}  // namespace

Arena::Arena() { chunks_.reserve(4); }

Arena::~Arena() {
    for (BigInt* b : bigints_) {
        mpz_clear(b->value);
    }
    for (Chunk& c : chunks_) {
        std::free(c.begin);
    }
}

void Arena::grow(std::size_t min_bytes) {
    std::size_t sz = std::max(min_bytes, kDefaultChunkBytes);
    auto* mem = static_cast<std::uint8_t*>(std::malloc(sz));
    if (!mem) throw std::bad_alloc();
    chunks_.push_back(Chunk{mem, mem + sz, mem});
}

void* Arena::alloc_raw(std::size_t bytes, std::size_t align) {
    if (bytes == 0) return nullptr;
    if (chunks_.empty()) grow(bytes + align);

    for (;;) {
        Chunk& c = chunks_.back();
        auto cur = reinterpret_cast<std::uintptr_t>(c.cur);
        std::uintptr_t aligned = (cur + (align - 1)) & ~(static_cast<std::uintptr_t>(align) - 1);
        auto* aligned_ptr = reinterpret_cast<std::uint8_t*>(aligned);
        if (aligned_ptr + bytes <= c.end) {
            c.cur = aligned_ptr + bytes;
            total_allocated_ += bytes;
            return aligned_ptr;
        }
        grow(bytes + align);
    }
}

const char* Arena::intern_str(std::string_view s) {
    std::size_t n = s.size();
    auto* p = static_cast<char*>(alloc_raw(n + 1, alignof(char)));
    if (n > 0) std::memcpy(p, s.data(), n);
    p[n] = '\0';
    return p;
}

const std::uint8_t* Arena::intern_bytes(const std::uint8_t* bytes, std::size_t n) {
    if (n == 0) return nullptr;
    auto* p = static_cast<std::uint8_t*>(alloc_raw(n, alignof(std::uint8_t)));
    std::memcpy(p, bytes, n);
    return p;
}

BigInt* Arena::make_bigint() {
    BigInt* b = alloc<BigInt>();
    mpz_init(b->value);
    bigints_.push_back(b);
    return b;
}

}  // namespace uplc
