#ifndef UPLCRT_ERRORS_H
#define UPLCRT_ERRORS_H

#include <setjmp.h>
#include <stdint.h>

#include "uplc/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CEK-internal failure propagation. The interpreter's entry point sets a
 * jmp_buf and the step loop longjmps into it on evaluation failure. Each
 * uplc_fail_kind maps to a distinct exit code in the driver.
 *
 * Generated code (M7+) will use the same mechanism: `uplcrt_fail` in abi.h
 * is routed through this module when the runtime is linked as one unit.
 */
typedef struct uplc_fail_ctx {
    jmp_buf        env;
    uplc_fail_kind kind;
    const char*    message;
} uplc_fail_ctx;

/* Install `ctx` as the current failure trampoline. Returns non-zero if we
 * are re-entering via longjmp, in which case `ctx->kind` holds the failure
 * reason. */
void uplcrt_fail_install(uplc_fail_ctx* ctx);

/* Raise a failure. Longjmps into the installed trampoline with `kind` and
 * optional message (borrowed pointer; must outlive the catch site). */
#ifdef __GNUC__
__attribute__((noreturn))
#endif
void uplcrt_raise(uplc_fail_kind kind, const char* message);

#ifdef __cplusplus
}
#endif

#endif  /* UPLCRT_ERRORS_H */
