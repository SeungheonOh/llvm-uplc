#ifndef UPLCRT_BC_FRAMES_H
#define UPLCRT_BC_FRAMES_H

/*
 * Fixed-size return-frame stack for the bytecode VM.
 *
 * Every APPLY / FORCE into a bytecode closure pushes one frame recording
 * the caller's pc and env. The callee's RETURN pops one frame; when the
 * frame pointer reaches `frame_base` we have finished the top-level
 * entry and hand the result back through uplc_bc_run.
 *
 * The stack is arena-allocated by uplc_bc_run. For M-bc-3 fixtures a
 * couple hundred frames is plenty; later milestones honour a
 * per-program high-water mark.
 */

#include <stdint.h>

#include "uplc/bytecode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uplc_bc_frame {
    const uplc_bc_word* ret_pc;   /* opcode to resume at in the caller */
    uplc_value*         ret_env;  /* caller's environment */
} uplc_bc_frame;

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_BC_FRAMES_H */
