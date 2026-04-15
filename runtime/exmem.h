#ifndef UPLCRT_EXMEM_H
#define UPLCRT_EXMEM_H

#include <stdint.h>
#include <stddef.h>

#include <gmp.h>

#include "uplc/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Primitive ExMem helpers — mirror TS cek/exmem.ts. All return int64 so
 * they can flow straight into the saturating cost-model evaluator.
 *
 * For the built-in size extraction, `uplcrt_builtin_arg_sizes` inspects a
 * run of evaluated argument values and writes the three per-argument size
 * slots the cost model expects. Unused slots are zeroed.
 */

int64_t uplcrt_exmem_integer(mpz_srcptr n);
int64_t uplcrt_exmem_bytestring(uint32_t len);
int64_t uplcrt_exmem_string_utf8(uint32_t byte_len);

/* Recursive traversal of a PlutusData tree. `root` is a pointer to the
 * opaque "Data" payload used by both execution modes; we walk it via a
 * manual stack and accumulate 4 per node + integer/bytestring leaf sizes. */
int64_t uplcrt_exmem_data(const void* root);

/* Populate sizes[0..2] from the runtime arguments of a fully-saturated
 * builtin call, dispatching on `tag`. Arguments that the cost model
 * ignores for a particular builtin are written as 0.
 *
 * Unknown / unimplemented builtins fill all three slots with 0 (cheap
 * fallback — doesn't affect correctness because the dispatch runner
 * refuses to saturate unimplemented builtins). */
void uplcrt_builtin_arg_sizes(uplc_builtin_tag tag,
                              const uplc_value* argv,
                              uint32_t argc,
                              int64_t out_sizes[3]);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_EXMEM_H */
