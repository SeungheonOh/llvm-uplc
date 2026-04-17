#pragma once

// Lower a de-Bruijn runtime rprogram into the bytecode VM's program
// representation. Returns a heap-allocated uplc_bc_program whose internal
// arrays live in `rt_arena` — the caller owns the outer struct (delete)
// and must keep the arena alive for at least the lifetime of any
// subsequent uplc_bc_run call.
//
// Compilation errors (unsupported features, free-variable captures in
// case alts in M-bc-5) are reported as std::runtime_error.

#include "runtime/core/arena.h"
#include "runtime/core/rterm.h"
#include "uplc/bytecode.h"

#include <memory>

namespace uplc_bc {

struct ProgramOwner {
    uplc_bc_program prog;
    // Backing storage kept alive alongside prog.
    std::unique_ptr<uplc_bc_fn[]>    fns_storage;
    std::unique_ptr<uplc_value[]>    consts_storage;

    ~ProgramOwner() = default;
};

std::unique_ptr<ProgramOwner> lower_rprogram(uplc_arena* rt_arena,
                                             const uplc_rprogram& rp);

}  // namespace uplc_bc
