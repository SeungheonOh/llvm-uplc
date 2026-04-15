#include "runtime/arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Pure-C bump arena. Chunks are allocated on demand (64 KB default).
 * BigInts registered via uplc_arena_alloc_mpz are mpz_clear'd when the
 * arena is destroyed. Mirrors the compiler-side C++ arena.
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

struct uplc_arena {
    uplc_arena_chunk*    chunks;   /* singly linked, head = most recent */
    uplc_arena_mpz_slot* mpzs;     /* singly linked, for mpz_clear on destroy */
    size_t               total_allocated;
};

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

uplc_arena* uplc_arena_create(void) {
    uplc_arena* a = (uplc_arena*)calloc(1, sizeof(uplc_arena));
    return a;
}

void uplc_arena_destroy(uplc_arena* a) {
    if (!a) return;

    uplc_arena_mpz_slot* slot = a->mpzs;
    while (slot) {
        mpz_clear(slot->value);
        uplc_arena_mpz_slot* next = slot->next;
        /* slot->value and slot itself both live inside the arena, so we
         * don't free them individually — freeing the chunks releases them. */
        (void)next;
        slot = next;
    }

    uplc_arena_chunk* c = a->chunks;
    while (c) {
        uplc_arena_chunk* next = c->next;
        free(c);
        c = next;
    }
    free(a);
}

void* uplc_arena_alloc(uplc_arena* a, size_t bytes, size_t align) {
    if (!a || bytes == 0) return NULL;

    for (;;) {
        if (!a->chunks) {
            uplc_arena_chunk* c = uplc_arena_new_chunk(bytes + align);
            if (!c) return NULL;
            c->next = a->chunks;
            a->chunks = c;
        }

        uplc_arena_chunk* c = a->chunks;
        uintptr_t cur = (uintptr_t)c->cur;
        uintptr_t aligned = (cur + (align - 1)) & ~((uintptr_t)align - 1);
        if ((uint8_t*)aligned + bytes <= c->end) {
            c->cur = (uint8_t*)aligned + bytes;
            a->total_allocated += bytes;
            return (void*)aligned;
        }

        /* Current chunk is full — allocate a fresh one and retry. */
        uplc_arena_chunk* nc = uplc_arena_new_chunk(bytes + align);
        if (!nc) return NULL;
        nc->next = a->chunks;
        a->chunks = nc;
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
    slot->next  = a->mpzs;
    a->mpzs     = slot;
    return v;
}

size_t uplc_arena_bytes_allocated(const uplc_arena* a) {
    return a ? a->total_allocated : 0;
}
