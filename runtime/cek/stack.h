#ifndef UPLCRT_STACK_H
#define UPLCRT_STACK_H

#include <stdbool.h>
#include <stdint.h>

#include "runtime/arena.h"
#include "runtime/cek/env.h"
#include "runtime/cek/rterm.h"
#include "uplc/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Frame stack for the direct CEK interpreter. Frames mirror the TS
 * reference (cek/context.ts).
 */
typedef enum uplc_frame_kind {
    UPLC_FRAME_FORCE         = 0,  /* force continuation */
    UPLC_FRAME_APP_LEFT      = 1,  /* have the function, evaluate the argument */
    UPLC_FRAME_APP_RIGHT     = 2,  /* have the argument, apply it to this function */
    UPLC_FRAME_CONSTR        = 3,  /* accumulating a constructor's fields */
    UPLC_FRAME_CASE          = 4,  /* waiting for a constr scrutinee */
} uplc_frame_kind;

typedef struct uplc_frame_constr {
    uint64_t       tag_index;
    uplc_rterm**   remaining_fields;  /* slice owned by the rterm */
    uint32_t       remaining_count;
    uint32_t       collected_count;
    uplc_value*    collected;         /* growing array, arena-allocated */
    uplc_env       env;
} uplc_frame_constr;

typedef struct uplc_frame_case {
    uplc_rterm**   branches;
    uint32_t       n_branches;
    uplc_env       env;
} uplc_frame_case;

typedef struct uplc_frame {
    uint8_t kind;
    uint8_t _pad[7];
    union {
        /* FORCE — no payload */
        /* APP_LEFT: evaluate argument `arg` under `env`, then apply to the
         * already-evaluated function waiting on the stack. */
        struct {
            uplc_rterm* arg;
            uplc_env    env;
        } app_left;
        /* APP_RIGHT: the function side has been fully evaluated, hold it
         * while the argument is being computed. */
        struct { uplc_value fn; } app_right;
        uplc_frame_constr constr;
        uplc_frame_case   case_;
    };
} uplc_frame;

typedef struct uplc_stack {
    uplc_frame* frames;
    uint32_t    size;
    uint32_t    capacity;
    uplc_arena* arena;
} uplc_stack;

void        uplc_stack_init(uplc_stack* s, uplc_arena* a);
void        uplc_stack_push(uplc_stack* s, const uplc_frame* f);
uplc_frame  uplc_stack_pop(uplc_stack* s);
bool        uplc_stack_empty(const uplc_stack* s);
uint32_t    uplc_stack_size(const uplc_stack* s);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_STACK_H */
