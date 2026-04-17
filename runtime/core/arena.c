#include "runtime/core/arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Pure-C bump arena. Chunks are allocated on demand (64 KB default).
 * BigInts registered via uplc_arena_alloc_mpz are mpz_clear'd when the
 * arena is destroyed. Mirrors the compiler-side C++ arena.
 *
 * The hot-path fast allocation lives in runtime/core/arena.h as an inline
 * function so every TU that includes the header gets the bump-and-check
 * compiled directly into the call site. This TU provides the slow-path
 * chunk-install logic, plus the destructor and the non-hot helpers.
 */

#define UPLC_ARENA_CHUNK_BYTES (64u * 1024u)

typedef struct uplc_arena_chunk {
    uint8_t*                  begin;
    uint8_t*                  end;
    uint8_t*                  cur;
    struct uplc_arena_chunk*  next;
} uplc_arena_chunk;

typedef struct uplc_arena_mpz_slot {
    mpz_ptr                       value;
    struct uplc_arena_mpz_slot*   next;
} uplc_arena_mpz_slot;

static uplc_arena_chunk* uplc_arena_new_chunk(size_t at_least) {
    size_t bytes = UPLC_ARENA_CHUNK_BYTES;
    if (at_least > bytes) bytes = at_least;
    uint8_t* mem = (uint8_t*)malloc(sizeof(uplc_arena_chunk) + bytes);
    if (!mem) return NULL;
    uplc_arena_chunk* c = (uplc_arena_chunk*)mem;
    c->begin = mem + sizeof(uplc_arena_chunk);
    c->end   = c->begin + bytes;
    c->cur   = c->begin;
    c->next  = NULL;
    return c;
}

/* Promote a chunk to be the current one for fast-path bump allocation.
 * The chunk's own cursor is what the header's inline reads via a->cur /
 * a->end; we therefore sync the arena's cursor pair to the chunk's. */
static void uplc_arena_install_chunk(uplc_arena* a, uplc_arena_chunk* c) {
    /* Persist the bump cursor of whatever was current back into its chunk
     * so we can resume from it later (not strictly needed today — we
     * never revisit old chunks — but leaves the door open). */
    if (a->_chunks) {
        a->_chunks->cur = a->cur;
    }
    c->next = a->_chunks;
    a->_chunks = c;
    a->cur = c->cur;
    a->end = c->end;
}

uplc_arena* uplc_arena_create(void) {
    uplc_arena* a = (uplc_arena*)calloc(1, sizeof(uplc_arena));
    return a;
}

void uplc_arena_destroy(uplc_arena* a) {
    if (!a) return;

    uplc_arena_mpz_slot* slot = a->_mpzs;
    while (slot) {
        mpz_clear(slot->value);
        /* slot and slot->value both live inside the arena, so freeing
         * the chunks below releases them. */
        slot = slot->next;
    }

    uplc_arena_chunk* c = a->_chunks;
    while (c) {
        uplc_arena_chunk* next = c->next;
        free(c);
        c = next;
    }
    free(a);
}

void* uplc_arena_alloc_slow(uplc_arena* a, size_t bytes, size_t align) {
    if (!a || bytes == 0) return NULL;

    for (;;) {
        if (!a->_chunks) {
            uplc_arena_chunk* c = uplc_arena_new_chunk(bytes + align);
            if (!c) return NULL;
            uplc_arena_install_chunk(a, c);
        }

        uintptr_t cur     = (uintptr_t)a->cur;
        uintptr_t aligned = (cur + (align - 1)) & ~((uintptr_t)align - 1);
        uint8_t*  next    = (uint8_t*)aligned + bytes;
        if (next <= a->end) {
            a->cur = next;
            a->_total_allocated += bytes;
            return (void*)aligned;
        }

        /* Current chunk is full — allocate a fresh one and retry. */
        uplc_arena_chunk* nc = uplc_arena_new_chunk(bytes + align);
        if (!nc) return NULL;
        uplc_arena_install_chunk(a, nc);
    }
}

void* uplc_arena_dup(uplc_arena* a, const void* src, size_t n) {
    if (n == 0) return NULL;
    void* p = uplc_arena_alloc(a, n, 1);
    if (!p) return NULL;
    memcpy(p, src, n);
    return p;
}

const char* uplc_arena_intern_str(uplc_arena* a, const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* p = (char*)uplc_arena_alloc(a, n + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

mpz_ptr uplc_arena_alloc_mpz(uplc_arena* a) {
    mpz_ptr v = (mpz_ptr)uplc_arena_alloc(a, sizeof(__mpz_struct),
                                           _Alignof(__mpz_struct));
    if (!v) return NULL;
    mpz_init(v);
    uplc_arena_mpz_slot* slot = (uplc_arena_mpz_slot*)uplc_arena_alloc(
        a, sizeof(uplc_arena_mpz_slot), _Alignof(uplc_arena_mpz_slot));
    if (!slot) return NULL;
    slot->value = v;
    slot->next  = a->_mpzs;
    a->_mpzs    = slot;
    return v;
}

size_t uplc_arena_bytes_allocated(const uplc_arena* a) {
    return a ? a->_total_allocated : 0;
}
