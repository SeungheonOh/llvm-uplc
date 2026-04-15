#include "runtime/errors.h"

#include <setjmp.h>
#include <stdlib.h>

#include "uplc/abi.h"

/*
 * A single thread-local pointer to the current failure trampoline. The
 * interpreter stores a uplc_fail_ctx on its stack frame, calls
 * uplcrt_fail_install to arm it, and clears it on return by letting the
 * pointer fall out of scope (cleared when the caller installs its own).
 *
 * This is not re-entrancy-safe for concurrent threads yet — we stash the
 * pointer in _Thread_local so at least individual threads are isolated.
 */
static _Thread_local uplc_fail_ctx* g_current = NULL;

void uplcrt_fail_install(uplc_fail_ctx* ctx) {
    g_current = ctx;
}

void uplcrt_raise(uplc_fail_kind kind, const char* message) {
    if (!g_current) {
        /* No trampoline installed — genuinely fatal. Fall through to
         * abort() so we get a core dump instead of silent corruption. */
        abort();
    }
    g_current->kind = kind;
    g_current->message = message;
    longjmp(g_current->env, 1);
}

/* Bridge the public abi.h uplcrt_fail entry point to the internal raise
 * helper. Generated code in M7 calls this directly. */
void uplcrt_fail(uplc_budget* b, uplc_fail_kind kind) {
    (void)b;
    uplcrt_raise(kind, NULL);
}
