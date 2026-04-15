#ifndef UPLCRT_CBOR_DATA_H
#define UPLCRT_CBOR_DATA_H

#include <stddef.h>
#include <stdint.h>

#include "runtime/arena.h"
#include "runtime/cek/rterm.h"   /* uplc_rdata — the shared Data layout */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Port of compiler/ast/cbor_data.cc encode path. Walks a uplc_rdata tree
 * and emits CBOR bytes allocated out of `arena`. Byte-for-byte compatible
 * with the compiler-side encoder, so runtime serialiseData matches both
 * TS reference output and anything baked by the compiler into .rodata.
 *
 * Returns the buffer pointer via *out_bytes / *out_len. Raises via
 * uplcrt_fail on any internal error (e.g. over-large integer). The
 * returned pointer is owned by the arena.
 */
void uplcrt_cbor_encode_data(uplc_arena*       arena,
                             const uplc_rdata* data,
                             const uint8_t**   out_bytes,
                             uint32_t*         out_len);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_CBOR_DATA_H */
