#include "runtime/builtins/helpers.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

/*
 * String builtins. UPLC strings are UTF-8 byte sequences; `decodeUtf8`
 * validates the input and raises on malformed runs.
 */

uplc_value uplcrt_builtin_appendString(uplc_budget* b, uplc_value* a) {
    uint32_t la = 0, lb = 0;
    const char* x = uplcrt_unwrap_string(b, a[0], &la);
    const char* y = uplcrt_unwrap_string(b, a[1], &lb);
    uplc_arena* ar = uplcrt_budget_arena(b);
    uint32_t n = la + lb;
    char* buf = (char*)uplc_arena_alloc(ar, n ? n : 1, alignof(char));
    if (la > 0) memcpy(buf, x, la);
    if (lb > 0) memcpy(buf + la, y, lb);
    return uplcrt_result_string(b, buf, n);
}

uplc_value uplcrt_builtin_equalsString(uplc_budget* b, uplc_value* a) {
    uint32_t la = 0, lb = 0;
    const char* x = uplcrt_unwrap_string(b, a[0], &la);
    const char* y = uplcrt_unwrap_string(b, a[1], &lb);
    if (la != lb) return uplcrt_result_bool(b, false);
    return uplcrt_result_bool(b, memcmp(x, y, la) == 0);
}

uplc_value uplcrt_builtin_encodeUtf8(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const char* s = uplcrt_unwrap_string(b, a[0], &len);
    return uplcrt_result_bytestring(b, (const uint8_t*)s, len);
}

/* Validate a byte sequence as UTF-8. Returns true if well-formed. Matches
 * the TS "fatal" TextDecoder — any structural error rejects. */
static bool is_valid_utf8(const uint8_t* s, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        uint32_t extra;
        uint32_t code_min;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xe0) == 0xc0) { extra = 1; code_min = 0x80; }
        else if ((c & 0xf0) == 0xe0) { extra = 2; code_min = 0x800; }
        else if ((c & 0xf8) == 0xf0) { extra = 3; code_min = 0x10000; }
        else return false;

        if (i + 1 + extra > n) return false;
        uint32_t cp = (uint32_t)(c & (0xff >> (extra + 2)));
        for (uint32_t k = 1; k <= extra; ++k) {
            uint8_t cc = s[i + k];
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        if (cp < code_min) return false;
        if (cp > 0x10ffff) return false;
        if (cp >= 0xd800 && cp <= 0xdfff) return false;
        i += 1 + extra;
    }
    return true;
}

uplc_value uplcrt_builtin_decodeUtf8(uplc_budget* b, uplc_value* a) {
    uint32_t len = 0;
    const uint8_t* bs = uplcrt_unwrap_bytestring(b, a[0], &len);
    if (!is_valid_utf8(bs, len)) uplcrt_fail(b, UPLC_FAIL_EVALUATION);
    return uplcrt_result_string(b, (const char*)bs, len);
}
