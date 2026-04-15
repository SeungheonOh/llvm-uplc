#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#include <gmp.h>

namespace uplc {

struct BigInt {
    mpz_t value;
};

// Compile-time bump arena. Backs every AST node allocated during a single
// compilation unit. Frees wholesale in the destructor (no individual
// deallocation is supported). BigInts registered via `register_bigint` are
// mpz_clear'd before the chunks are released.
class Arena {
public:
    Arena();
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    void* alloc_raw(std::size_t bytes, std::size_t align);

    template <typename T, typename... Args>
    T* alloc(Args&&... args) {
        void* p = alloc_raw(sizeof(T), alignof(T));
        return ::new (p) T(std::forward<Args>(args)...);
    }

    // Allocate an uninitialised array of `n` Ts. Caller must initialise each
    // slot. T must be trivially destructible (we do not run destructors).
    template <typename T>
    T* alloc_array_uninit(std::size_t n) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "arena arrays do not run destructors");
        if (n == 0) return nullptr;
        return static_cast<T*>(alloc_raw(sizeof(T) * n, alignof(T)));
    }

    const char* intern_str(std::string_view s);
    const std::uint8_t* intern_bytes(const std::uint8_t* bytes, std::size_t n);

    // Allocate a BigInt initialised to zero; the arena will mpz_clear it on
    // destruction. The returned pointer remains valid until the arena is
    // destroyed.
    BigInt* make_bigint();

    std::size_t bytes_allocated() const { return total_allocated_; }

private:
    struct Chunk {
        std::uint8_t* begin;
        std::uint8_t* end;
        std::uint8_t* cur;
    };

    void grow(std::size_t min_bytes);

    std::vector<Chunk> chunks_;
    std::vector<BigInt*> bigints_;
    std::size_t total_allocated_ = 0;
};

}  // namespace uplc
