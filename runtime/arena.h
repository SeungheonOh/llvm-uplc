#ifndef UPLCRT_ARENA_H
#define UPLCRT_ARENA_H

#include <stddef.h>
#include <stdint.h>

#include <gmp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uplc_arena uplc_arena;

uplc_arena* uplc_arena_create(void);
void        uplc_arena_destroy(uplc_arena* a);

// Raw aligned allocation. align must be a power of two.
void* uplc_arena_alloc(uplc_arena* a, size_t bytes, size_t align);

// Copy `n` bytes into the arena.
void* uplc_arena_dup(uplc_arena* a, const void* src, size_t n);

// Intern a NUL-terminated C string. The returned pointer is valid until the
// arena is destroyed.
const char* uplc_arena_intern_str(uplc_arena* a, const char* s);

// Initialise an mpz_t inside the arena and register it for mpz_clear on
// arena destruction. Returned pointer points to the underlying
// __mpz_struct; you can pass it to any GMP function taking mpz_ptr.
mpz_ptr uplc_arena_alloc_mpz(uplc_arena* a);

// Debug: total bytes allocated from the arena's chunks so far.
size_t uplc_arena_bytes_allocated(const uplc_arena* a);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_ARENA_H */
