#ifndef UPLC_TERM_H
#define UPLC_TERM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Term-tag enum. Values are chosen to match the flat-encoding wire tags so
 * the decoder and codegen can share one enum.
 * ------------------------------------------------------------------------- */
typedef enum uplc_term_tag {
    UPLC_TERM_VAR      = 0,
    UPLC_TERM_DELAY    = 1,
    UPLC_TERM_LAMBDA   = 2,
    UPLC_TERM_APPLY    = 3,
    UPLC_TERM_CONSTANT = 4,
    UPLC_TERM_FORCE    = 5,
    UPLC_TERM_ERROR    = 6,
    UPLC_TERM_BUILTIN  = 7,
    UPLC_TERM_CONSTR   = 8,  /* Plutus >= 1.1.0 */
    UPLC_TERM_CASE     = 9,  /* Plutus >= 1.1.0 */
    UPLC_TERM__COUNT   = 10
} uplc_term_tag;

/* ---------------------------------------------------------------------------
 * Constant-type-tag enum. Again matches the flat wire encoding.
 * ------------------------------------------------------------------------- */
typedef enum uplc_const_tag {
    UPLC_CONST_INTEGER          = 0,
    UPLC_CONST_BYTESTRING       = 1,
    UPLC_CONST_STRING           = 2,
    UPLC_CONST_UNIT             = 3,
    UPLC_CONST_BOOL             = 4,
    UPLC_CONST_LIST             = 5,  /* parameterised: list element type follows */
    UPLC_CONST_PAIR             = 6,  /* parameterised: two element types follow */
    UPLC_CONST_APPLICATION      = 7,  /* meta tag used by nested type parser */
    UPLC_CONST_DATA             = 8,
    UPLC_CONST_BLS12_381_G1     = 9,
    UPLC_CONST_BLS12_381_G2     = 10,
    UPLC_CONST_BLS12_381_ML_RES = 11,
    UPLC_CONST__COUNT           = 12
} uplc_const_tag;

/* ---------------------------------------------------------------------------
 * Builtin tag. Concrete enum values are defined in runtime/builtin_table.c
 * to match the flat encoding's BUILTIN_TAG_TO_NAME assignment. For the M0
 * stub we only need the type to exist.
 * ------------------------------------------------------------------------- */
typedef uint8_t uplc_builtin_tag;

/* Sentinel used for "no such builtin" slots. */
#define UPLC_BUILTIN_INVALID ((uplc_builtin_tag)0xff)

/* Total count of known builtins, filled in by the builtin table module. */
extern const uint32_t UPLC_BUILTIN_COUNT;

#ifdef __cplusplus
}
#endif

#endif /* UPLC_TERM_H */
