#ifndef UPLCRT_COMPILED_BAKED_CONST_H
#define UPLCRT_COMPILED_BAKED_CONST_H

#include <stddef.h>
#include <stdint.h>

#include "uplc/abi.h"
#include "uplc/budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a baked-constant blob into a fully-formed uplc_value (VCon).
 *
 * The blob format is little-endian and self-describing — see
 * compiler/codegen/baked_const.cc for the encoder. Each invocation
 * allocates a fresh constant tree out of the budget arena, so callers
 * may invoke this once per CONST step without aliasing earlier results.
 *
 * Raises UPLC_FAIL_MACHINE on a malformed blob (which would indicate
 * a codegen bug, not a user-facing failure).
 */
uplc_value uplcrt_const_baked(uplc_budget* b, const uint8_t* blob, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_COMPILED_BAKED_CONST_H */
