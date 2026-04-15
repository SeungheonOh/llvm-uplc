#include "runtime/cek/stack.h"

#include <stdalign.h>
#include <string.h>

void uplc_stack_init(uplc_stack* s, uplc_arena* a) {
    s->frames = NULL;
    s->size = 0;
    s->capacity = 0;
    s->arena = a;
}

static void uplc_stack_grow(uplc_stack* s) {
    uint32_t new_cap = s->capacity == 0 ? 16u : s->capacity * 2u;
    uplc_frame* new_frames = (uplc_frame*)uplc_arena_alloc(
        s->arena, (size_t)new_cap * sizeof(uplc_frame), alignof(uplc_frame));
    if (s->size > 0) {
        memcpy(new_frames, s->frames, (size_t)s->size * sizeof(uplc_frame));
    }
    s->frames = new_frames;
    s->capacity = new_cap;
}

void uplc_stack_push(uplc_stack* s, const uplc_frame* f) {
    if (s->size >= s->capacity) uplc_stack_grow(s);
    s->frames[s->size++] = *f;
}

uplc_frame uplc_stack_pop(uplc_stack* s) {
    return s->frames[--s->size];
}

bool uplc_stack_empty(const uplc_stack* s) { return s->size == 0; }

uint32_t uplc_stack_size(const uplc_stack* s) { return s->size; }
