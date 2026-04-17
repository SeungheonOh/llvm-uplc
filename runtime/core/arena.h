#ifndef UPLCRT_ARENA_H
#define UPLCRT_ARENA_H

#include <stddef.h>
#include <stdint.h>

#include <gmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-evaluation bump arena. Chunks grow on demand; the whole thing is
 * freed wholesale when uplc_arena_destroy is called.
 *
 * The struct layout is published so the `uplc_arena_alloc` fast path can
 * inline into callers. Only `cur` and `end` are considered part of the
 * supported surface — the remaining fields are internal bookkeeping and
 * may change without notice.
 */

struct uplc_arena_chunk;      /* opaque — chunk list node */
struct uplc_arena_mpz_slot;   /* opaque — mpz cleanup list node */

typedef struct uplc_arena {
    /* Bump cursor for the current chunk. Fast-path allocation reads `cur`,
     * rounds up for alignment, checks `cur + bytes <= end`, and writes
     * the new `cur`. On miss, uplc_arena_alloc_slow installs a new chunk
     * and refreshes these. */
    uint8_t* cur;
    uint8_t* end;

    /* Internal — do not touch from outside arena.c. */
    struct uplc_arena_chunk*    _chunks;
    struct uplc_arena_mpz_slot* _mpzs;
    size_t                      _total_allocated;
} uplc_arena;

uplc_arena* uplc_arena_create(void);
void        uplc_arena_destroy(uplc_arena* a);

/* Slow path: allocate a new chunk if needed, then bump. Callers normally
 * go through `uplc_arena_alloc` below, which inlines the fast path. */
void* uplc_arena_alloc_slow(uplc_arena* a, size_t bytes, size_t align);

/* Raw aligned allocation. `align` must be a power of two. Inlined fast
 * path: bump-and-return when the current chunk has room; otherwise jump
 * into `uplc_arena_alloc_slow`. */
static inline void* uplc_arena_alloc(uplc_arena* a, size_t bytes, size_t align) {
    if (!a || bytes == 0) return (void*)0;
    uintptr_t cur     = (uintptr_t)a->cur;
    uintptr_t aligned = (cur + (align - 1)) & ~((uintptr_t)align - 1);
    uint8_t*  next    = (uint8_t*)aligned + bytes;
    if (next <= a->end) {
        a->cur = next;
        a->_total_allocated += bytes;
        return (void*)aligned;
    }
    return uplc_arena_alloc_slow(a, bytes, align);
}

/* Copy `n` bytes into the arena. */
void* uplc_arena_dup(uplc_arena* a, const void* src, size_t n);

/* Intern a NUL-terminated C string. The returned pointer is valid until
 * the arena is destroyed. */
const char* uplc_arena_intern_str(uplc_arena* a, const char* s);

/* Initialise an mpz_t inside the arena and register it for mpz_clear on
 * arena destruction. Returned pointer points to the underlying
 * __mpz_struct; you can pass it to any GMP function taking mpz_ptr. */
mpz_ptr uplc_arena_alloc_mpz(uplc_arena* a);

/* Debug: total bytes allocated from the arena's chunks so far. */
size_t uplc_arena_bytes_allocated(const uplc_arena* a);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_ARENA_H */
