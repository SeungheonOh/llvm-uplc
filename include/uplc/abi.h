#ifndef UPLC_ABI_H
#define UPLC_ABI_H

#include <stdint.h>
#include "uplc/term.h"
#include "uplc/budget.h"
#include "uplc/costmodel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Value representation.
 *
 * 16 bytes, fits in two GP registers on aarch64 / x86_64 sysv, so codegen can
 * pass `uplc_value` by value everywhere. The `tag` field discriminates the
 * sum; `payload` is either an immediate scalar (bool, unit) or a tagged
 * pointer into arena-allocated heap data.
 * ------------------------------------------------------------------------- */
typedef enum uplc_value_tag {
    UPLC_V_CON     = 0,
    UPLC_V_DELAY   = 1,
    UPLC_V_LAM     = 2,
    UPLC_V_CONSTR  = 3,
    UPLC_V_BUILTIN = 4,
    UPLC_V__COUNT  = 5
} uplc_value_tag;

typedef struct uplc_value {
    uint8_t  tag;     /* uplc_value_tag */
    uint8_t  subtag;  /* for V_CON: uplc_const_tag; for V_LAM/V_DELAY: interp vs compiled */
    uint8_t  _pad[6];
    uint64_t payload; /* pointer into arena, or immediate (bool/unit) */
} uplc_value;

/* Subtag discriminator for V_LAM / V_DELAY.
 *   INTERP   — payload is a uplc_interp_closure { rterm* body; env env }
 *              (CEK interpreter only, see runtime/cek/closure.h).
 *   COMPILED — payload is a uplc_closure { fn; free[] } (below).
 *   BYTECODE — payload is a uplc_bc_closure { fn_id; n_upvals; upvals[] }
 *              (bytecode VM, see runtime/bytecode/closure.h).
 *
 * The three closure shapes never mix: a V_LAM/V_DELAY produced by one
 * execution mode raises an evaluation failure if applied/forced by
 * another. Checked on every apply/force. */
#define UPLC_VLAM_INTERP   0u
#define UPLC_VLAM_COMPILED 1u
#define UPLC_VLAM_BYTECODE 2u

/*
 * Subtag high-bit flag for V_CON values: when set, the payload is an
 * immediate (raw integer, bool, unit, etc.) instead of a pointer to an
 * arena-allocated uplc_rconstant. The low 7 bits still hold the
 * underlying uplc_rconst_tag so pattern-matching code can stay uniform.
 *
 * Currently only INTEGER supports inlining. `payload` holds a signed
 * 64-bit integer reinterpreted as uint64_t (sign-extended). Arithmetic
 * builtins take a fast path when both operands are inline, falling back
 * to GMP when either is boxed or the result overflows i64.
 */
#define UPLC_VCON_INLINE_BIT  0x80u
#define UPLC_VCON_INT_INLINE  (UPLC_VCON_INLINE_BIT | 0u /* INTEGER */)

/* Closure layout referenced by V_LAM / V_DELAY payloads when subtag ==
 * UPLC_VLAM_COMPILED. Generated lambdas have the calling convention:
 *     uplc_value (*fn)(uplc_value* env, uplc_value arg, uplc_budget* b)
 * Generated delays have:
 *     uplc_value (*fn)(uplc_value* env, uplc_budget* b) */
typedef struct uplc_closure {
    void*     fn;
    uint32_t  nfree;
    uint32_t  _pad;
    uplc_value free[];
} uplc_closure;

typedef uplc_value (*uplc_lam_fn)(uplc_value* env, uplc_value arg, uplc_budget* b);
typedef uplc_value (*uplc_delay_fn)(uplc_value* env, uplc_budget* b);

/* Failure categories surfaced through uplcrt_fail and the driver exit code. */
typedef enum uplc_fail_kind {
    UPLC_FAIL_EVALUATION    = 1,  /* script-level evaluation failure */
    UPLC_FAIL_OUT_OF_BUDGET = 2,
    UPLC_FAIL_MACHINE       = 3   /* internal invariant violation (bug) */
} uplc_fail_kind;

/* ---------------------------------------------------------------------------
 * ABI entry points called from LLVM-emitted IR.
 *
 * Every make_* / const_* helper takes a `uplc_budget*` first because they
 * need to heap-allocate into the evaluation arena, and the arena pointer
 * lives inside the budget struct (see include/uplc/budget.h).
 * ------------------------------------------------------------------------- */

uplc_value  uplcrt_apply         (uplc_value fn, uplc_value arg, uplc_budget* b);
uplc_value  uplcrt_force         (uplc_value thunk, uplc_budget* b);

/* Cold-path apply/force used by generated IR after the inline V_LAM /
 * V_DELAY + COMPILED hot path in emit_apply_inline / emit_force_inline
 * falls through. Same semantics as the unsuffixed entries above; the
 * split exists so the hot path can be inlined without bringing in the
 * V_BUILTIN state-machine code at every call site. */
uplc_value  uplcrt_apply_slow    (uplc_value fn, uplc_value arg, uplc_budget* b);
uplc_value  uplcrt_force_slow    (uplc_value thunk, uplc_budget* b);
uplc_value  uplcrt_make_builtin  (uplc_budget* b, uplc_builtin_tag tag);
uplc_value  uplcrt_make_lam      (uplc_budget* b, void* fn,
                                  const uplc_value* free, uint32_t nfree);
uplc_value  uplcrt_make_delay    (uplc_budget* b, void* fn,
                                  const uplc_value* free, uint32_t nfree);
uplc_value  uplcrt_make_constr   (uplc_budget* b, uint64_t tag,
                                  const uplc_value* fields, uint32_t n);
uplc_value  uplcrt_case_dispatch (uplc_value scrutinee,
                                  const uplc_value* alts,
                                  uint32_t n_alts,
                                  uplc_budget* b);

/* Apply branch to fields[0..n_fields-1] left-to-right; used by compiled Case. */
uplc_value  uplcrt_apply_fields  (uplc_value branch, const uplc_value* fields,
                                  uint32_t n_fields, uplc_budget* b);

/* Decompose a case scrutinee writing results to out-params; for LLVM-emitted IR
 * (avoids struct-return ABI mismatch between C and LLVM IR on ARM64/x86-64). */
void        uplcrt_case_decompose_out(uplc_budget* b, uplc_value scrutinee,
                                     uint32_t n_branches, uint64_t* out_tag,
                                     uint32_t* out_n_fields, uplc_value** out_fields);

/* uplcrt_budget_step is `static inline` in uplc/budget.h — it inlines
 * directly at every call site. `uplcrt_budget_step_extern` is the
 * out-of-line wrapper for consumers that need a real symbol address
 * (JIT symbol registration, function-pointer tables). */
void        uplcrt_budget_step_extern(uplc_budget* b, uplc_step_kind kind);
void        uplcrt_budget_flush     (uplc_budget* b);

#ifdef __GNUC__
__attribute__((noreturn))
#endif
void        uplcrt_fail          (uplc_budget* b, uplc_fail_kind kind);

/* Constant constructors called by codegen to materialise baked constants. */
uplc_value  uplcrt_const_int_ref   (uplc_budget* b, const void* limbs,
                                    int32_t sign, uint32_t nlimbs);
uplc_value  uplcrt_const_bs_ref    (uplc_budget* b, const uint8_t* bytes, uint32_t len);
uplc_value  uplcrt_const_string_ref(uplc_budget* b, const char* utf8, uint32_t byte_len);
uplc_value  uplcrt_const_bool      (uplc_budget* b, int value);
uplc_value  uplcrt_const_unit      (uplc_budget* b);
uplc_value  uplcrt_const_data_ref  (uplc_budget* b, const void* baked);

/* Decode a baked-constant blob (Data, List, Pair, Array, BLS, Value) into
 * a fully-formed VCon. Each call allocates a fresh constant tree out of
 * the evaluation arena. The blob layout is documented in
 * runtime/compiled/baked_const.c — the codegen helper
 * uplc::serialize_baked_constant produces it. */
uplc_value  uplcrt_const_baked     (uplc_budget* b, const uint8_t* blob, uint32_t len);

/* Import an integer from a big-endian byte array. Called by generated code
 * for arbitrary-precision integer constants baked into .rodata. */
uplc_value  uplcrt_const_int_bytes (uplc_budget* b, int negative,
                                    const uint8_t* magnitude, uint32_t nbytes);

/* Convenience: test and hand-written fixtures use this to materialise an
 * integer constant from a small signed value without going through a
 * rodata limb array. Not called by generated code. */
uplc_value  uplcrt_const_int_si    (uplc_budget* b, int64_t value);

/* ---------------------------------------------------------------------------
 * Artifact header, emitted as a const symbol `UPLC_HEADER` into every .uplcx.
 * ------------------------------------------------------------------------- */
#define UPLC_HEADER_MAGIC 0x5843504C55ULL /* "UPLCX" LE */

typedef struct uplc_header {
    uint64_t magic;                /* UPLC_HEADER_MAGIC */
    uint32_t abi_version;          /* bumped on any layout change */
    uint32_t plutus_version_major;
    uint32_t plutus_version_minor;
    uint32_t plutus_version_patch;
    uint64_t term_blob_offset;     /* offset from header base to the flat term blob */
    uint64_t term_blob_size;
    uint64_t cost_model_offset;    /* offset to uplc_cost_model */
    uint64_t cost_model_size;
    uint64_t entry_symbol_offset;  /* offset to NUL-terminated entry-point symbol name */
} uplc_header;

#define UPLC_ABI_VERSION 1u

#ifdef __cplusplus
}
#endif

#endif /* UPLC_ABI_H */
