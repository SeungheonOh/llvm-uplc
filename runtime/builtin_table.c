#include "runtime/builtin_dispatch.h"
#include "uplc/abi.h"
#include "uplc/costmodel.h"
#include "uplc/term.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Builtin metadata table, populated with real Conway-era cost parameters
 * ported from TS cek/costs.ts (DEFAULT_BUILTIN_COSTS).
 *
 * Each row carries:
 *   - arity       (value-argument count)
 *   - force_count (number of forces required before saturation)
 *   - cost        (per-CPU + per-MEM cost function, uplc_cost_row)
 *   - impl        (pointer to the implementation, NULL for M5 stubs)
 *
 * Implementations live in runtime/builtins/*.c. Tags match the flat
 * builtin-encoding table (compiler/ast/builtin_table.cc). Unimplemented
 * entries raise EvaluationFailure with a "not implemented (M5 ...)"
 * message via uplcrt_run_builtin; tests/conformance/eval_conformance_test.cc
 * classifies those as deferred rather than regressions.
 */

/* ---------------------------------------------------------------------- */
/* Forward-declared implementations                                        */
/* ---------------------------------------------------------------------- */

uplc_value uplcrt_builtin_addInteger           (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_subtractInteger      (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_multiplyInteger      (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_divideInteger        (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_quotientInteger      (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_remainderInteger     (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_modInteger           (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_equalsInteger        (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_lessThanInteger      (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_lessThanEqualsInteger(uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_sha2_256               (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_sha3_256               (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_blake2b_256            (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_blake2b_224            (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_keccak_256             (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_ripemd_160             (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_verifyEd25519Signature (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_verifyEcdsaSecp256k1Signature   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_verifySchnorrSecp256k1Signature (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G1_add          (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G1_neg          (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G1_scalarMul    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G1_equal        (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G1_compress     (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G1_uncompress   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G1_hashToGroup  (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G2_add          (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G2_neg          (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G2_scalarMul    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G2_equal        (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G2_compress     (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G2_uncompress   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G2_hashToGroup  (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_millerLoop     (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_mulMlResult    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_finalVerify    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G1_multiScalarMul (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bls12_381_G2_multiScalarMul (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_insertCoin    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_lookupCoin    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_unionValue    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_valueContains (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_valueData     (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_unValueData   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_scaleValue    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_appendByteString        (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_consByteString          (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_sliceByteString         (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_lengthOfByteString      (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_indexByteString         (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_equalsByteString        (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_lessThanByteString      (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_lessThanEqualsByteString(uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_ifThenElse (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_chooseUnit (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_trace      (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_fstPair    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_sndPair    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_chooseList (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_mkCons     (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_headList   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_tailList   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_nullList   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_dropList   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_appendString(uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_equalsString(uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_encodeUtf8  (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_decodeUtf8  (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_iData        (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_bData        (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_constrData   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_mapData      (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_listData     (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_unIData      (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_unBData      (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_unConstrData (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_unListData   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_unMapData    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_equalsData   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_chooseData   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_mkPairData   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_mkNilData    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_mkNilPairData(uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_serialiseData(uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_listToArray  (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_lengthOfArray(uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_indexArray   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_andByteString       (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_orByteString        (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_xorByteString       (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_complementByteString(uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_shiftByteString     (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_rotateByteString    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_readBit         (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_writeBits       (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_countSetBits    (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_findFirstSetBit (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_replicateByte   (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_integerToByteString (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_byteStringToInteger (uplc_budget* b, uplc_value* a);
uplc_value uplcrt_builtin_expModInteger       (uplc_budget* b, uplc_value* a);

/* ---------------------------------------------------------------------- */
/* Cost-function literal macros (per shape)                                */
/* ---------------------------------------------------------------------- */

#define C(v)                 { UPLC_COSTSHAPE_CONSTANT,                    {0}, { (v), 0, 0, 0, 0, 0, 0, 0 } }
#define LIN(i, s)            { UPLC_COSTSHAPE_LINEAR_IN_X,                 {0}, { (i), (s), 0, 0, 0, 0, 0, 0 } }
#define LINX(i, s)           { UPLC_COSTSHAPE_LINEAR_IN_X,                 {0}, { (i), (s), 0, 0, 0, 0, 0, 0 } }
#define LINY(i, s)           { UPLC_COSTSHAPE_LINEAR_IN_Y,                 {0}, { (i), (s), 0, 0, 0, 0, 0, 0 } }
#define LINZ(i, s)           { UPLC_COSTSHAPE_LINEAR_IN_Z,                 {0}, { (i), (s), 0, 0, 0, 0, 0, 0 } }
#define ADDED(i, s)          { UPLC_COSTSHAPE_ADDED_SIZES,                 {0}, { (i), (s), 0, 0, 0, 0, 0, 0 } }
#define MULT(i, s)           { UPLC_COSTSHAPE_MULTIPLIED_SIZES,            {0}, { (i), (s), 0, 0, 0, 0, 0, 0 } }
#define MIN_SZ(i, s)         { UPLC_COSTSHAPE_MIN_SIZE,                    {0}, { (i), (s), 0, 0, 0, 0, 0, 0 } }
#define MAX_SZ(i, s)         { UPLC_COSTSHAPE_MAX_SIZE,                    {0}, { (i), (s), 0, 0, 0, 0, 0, 0 } }
#define SUB(i, s, m)         { UPLC_COSTSHAPE_SUBTRACTED_SIZES,            {0}, { (i), (s), (m), 0, 0, 0, 0, 0 } }
#define ONDIAG(i, s, c)      { UPLC_COSTSHAPE_LINEAR_ON_DIAGONAL,          {0}, { (i), (s), (c), 0, 0, 0, 0, 0 } }
#define QX(c0, c1, c2)       { UPLC_COSTSHAPE_QUADRATIC_IN_X,              {0}, { (c0), (c1), (c2), 0, 0, 0, 0, 0 } }
#define QY(c0, c1, c2)       { UPLC_COSTSHAPE_QUADRATIC_IN_Y,              {0}, { (c0), (c1), (c2), 0, 0, 0, 0, 0 } }
#define QZ(c0, c1, c2)       { UPLC_COSTSHAPE_QUADRATIC_IN_Z,              {0}, { (c0), (c1), (c2), 0, 0, 0, 0, 0 } }
#define INTR(c00, c10, c01, c11) \
    { UPLC_COSTSHAPE_WITH_INTERACTION, {0}, { (c00), (c10), (c01), (c11), 0, 0, 0, 0 } }
#define LINYZ(i, sy, sz)     { UPLC_COSTSHAPE_LINEAR_IN_Y_AND_Z,           {0}, { (i), (sy), (sz), 0, 0, 0, 0, 0 } }
#define LINMAXYZ(i, s)       { UPLC_COSTSHAPE_LINEAR_IN_MAX_YZ,            {0}, { (i), (s), 0, 0, 0, 0, 0, 0 } }
#define LITY_LINZ(i, s)      { UPLC_COSTSHAPE_LITERAL_IN_Y_OR_LINEAR_IN_Z, {0}, { (i), (s), 0, 0, 0, 0, 0, 0 } }
#define EXPMOD(c00, c11, c12) { UPLC_COSTSHAPE_EXP_MOD,                    {0}, { (c00), (c11), (c12), 0, 0, 0, 0, 0 } }
#define ABOVE(ct, mn, c00, c10, c01, c20, c11, c02) \
    { UPLC_COSTSHAPE_CONST_ABOVE_DIAG, {0}, { (ct), (mn), (c00), (c10), (c01), (c20), (c11), (c02) } }

/* Braces are initializer lists — we can't wrap them in `(...)` in any
 * intermediate macro without triggering GCC/Clang's GNU statement-
 * expression interpretation. So both M and STUB inline the cost-row
 * initializer directly. */
#define M(ar, fc, cpu_fn, mem_fn, impl) \
    { (ar), (fc), {0}, { cpu_fn, mem_fn, (ar), {0} }, (impl) }
#define STUB(ar, fc, cpu_fn, mem_fn) \
    { (ar), (fc), {0}, { cpu_fn, mem_fn, (ar), {0} }, NULL }

/* ---------------------------------------------------------------------- */
/* The table — 101 rows, tag-indexed                                       */
/* ---------------------------------------------------------------------- */

const uplc_builtin_meta UPLC_BUILTIN_META[] = {
    /*  0 addInteger            */ M(2, 0, MAX_SZ(100788, 420), MAX_SZ(1, 1), uplcrt_builtin_addInteger),
    /*  1 subtractInteger       */ M(2, 0, MAX_SZ(100788, 420), MAX_SZ(1, 1), uplcrt_builtin_subtractInteger),
    /*  2 multiplyInteger       */ M(2, 0, MULT(90434, 519),    ADDED(0, 1),  uplcrt_builtin_multiplyInteger),
    /*  3 divideInteger         */ M(2, 0,
        ABOVE(85848, 85848, 123203, 1716, 7305, 57, 549, -900),
        SUB(0, 1, 1),
        uplcrt_builtin_divideInteger),
    /*  4 quotientInteger       */ M(2, 0,
        ABOVE(85848, 85848, 123203, 1716, 7305, 57, 549, -900),
        SUB(0, 1, 1),
        uplcrt_builtin_quotientInteger),
    /*  5 remainderInteger      */ M(2, 0,
        ABOVE(85848, 85848, 123203, 1716, 7305, 57, 549, -900),
        LINY(0, 1),
        uplcrt_builtin_remainderInteger),
    /*  6 modInteger            */ M(2, 0,
        ABOVE(85848, 85848, 123203, 1716, 7305, 57, 549, -900),
        LINY(0, 1),
        uplcrt_builtin_modInteger),
    /*  7 equalsInteger         */ M(2, 0, MIN_SZ(51775, 558), C(1), uplcrt_builtin_equalsInteger),
    /*  8 lessThanInteger       */ M(2, 0, MIN_SZ(44749, 541), C(1), uplcrt_builtin_lessThanInteger),
    /*  9 lessThanEqualsInteger */ M(2, 0, MIN_SZ(43285, 552), C(1), uplcrt_builtin_lessThanEqualsInteger),

    /* 10 appendByteString  */ M(2, 0, ADDED(1000, 173), ADDED(0, 1), uplcrt_builtin_appendByteString),
    /* 11 consByteString    */ M(2, 0, LINY(72010, 178), ADDED(0, 1), uplcrt_builtin_consByteString),
    /* 12 sliceByteString   */ M(3, 0, LINZ(20467, 1),   LINZ(4, 0),   uplcrt_builtin_sliceByteString),
    /* 13 lengthOfByteString*/ M(1, 0, C(22100),         C(10),        uplcrt_builtin_lengthOfByteString),
    /* 14 indexByteString   */ M(2, 0, C(13169),         C(4),         uplcrt_builtin_indexByteString),
    /* 15 equalsByteString  */ M(2, 0, ONDIAG(29498, 38, 24548), C(1), uplcrt_builtin_equalsByteString),
    /* 16 lessThanByteString*/ M(2, 0, MIN_SZ(28999, 74), C(1),        uplcrt_builtin_lessThanByteString),
    /* 17 lessThanEqualsBS  */ M(2, 0, MIN_SZ(28999, 74), C(1),        uplcrt_builtin_lessThanEqualsByteString),

    /* 18 sha2_256              */ M(1, 0, LIN(270652, 22588),   C(4),  uplcrt_builtin_sha2_256),
    /* 19 sha3_256              */ M(1, 0, LIN(1457325, 64566),  C(4),  uplcrt_builtin_sha3_256),
    /* 20 blake2b_256           */ M(1, 0, LIN(201305, 8356),    C(4),  uplcrt_builtin_blake2b_256),
    /* 21 verifyEd25519Signature*/ M(3, 0, LINY(53384111, 14333), C(10), uplcrt_builtin_verifyEd25519Signature),

    /* 22 appendString  */ M(2, 0, ADDED(1000, 59957), ADDED(4, 1), uplcrt_builtin_appendString),
    /* 23 equalsString  */ M(2, 0, ONDIAG(1000, 60594, 39184), C(1), uplcrt_builtin_equalsString),
    /* 24 encodeUtf8    */ M(1, 0, LIN(1000, 42921), LIN(4, 2), uplcrt_builtin_encodeUtf8),
    /* 25 decodeUtf8    */ M(1, 0, LIN(91189, 769),  LIN(4, 2), uplcrt_builtin_decodeUtf8),

    /* 26 ifThenElse */ M(3, 1, C(76049), C(1),  uplcrt_builtin_ifThenElse),
    /* 27 chooseUnit */ M(2, 1, C(61462), C(4),  uplcrt_builtin_chooseUnit),
    /* 28 trace      */ M(2, 1, C(59498), C(32), uplcrt_builtin_trace),

    /* 29 fstPair */ M(1, 2, C(141895), C(32), uplcrt_builtin_fstPair),
    /* 30 sndPair */ M(1, 2, C(141992), C(32), uplcrt_builtin_sndPair),

    /* 31 chooseList */ M(3, 2, C(132994), C(32), uplcrt_builtin_chooseList),
    /* 32 mkCons     */ M(2, 1, C(72362),  C(32), uplcrt_builtin_mkCons),
    /* 33 headList   */ M(1, 1, C(83150),  C(32), uplcrt_builtin_headList),
    /* 34 tailList   */ M(1, 1, C(81663),  C(32), uplcrt_builtin_tailList),
    /* 35 nullList   */ M(1, 1, C(74433),  C(32), uplcrt_builtin_nullList),

    /* 36 chooseData   */ M(6, 1, C(94375), C(32), uplcrt_builtin_chooseData),
    /* 37 constrData   */ M(2, 0, C(22151), C(32), uplcrt_builtin_constrData),
    /* 38 mapData      */ M(1, 0, C(68246), C(32), uplcrt_builtin_mapData),
    /* 39 listData     */ M(1, 0, C(33852), C(32), uplcrt_builtin_listData),
    /* 40 iData        */ M(1, 0, C(15299), C(32), uplcrt_builtin_iData),
    /* 41 bData        */ M(1, 0, C(11183), C(32), uplcrt_builtin_bData),
    /* 42 unConstrData */ M(1, 0, C(24588), C(32), uplcrt_builtin_unConstrData),
    /* 43 unMapData    */ M(1, 0, C(24623), C(32), uplcrt_builtin_unMapData),
    /* 44 unListData   */ M(1, 0, C(25933), C(32), uplcrt_builtin_unListData),
    /* 45 unIData      */ M(1, 0, C(20744), C(32), uplcrt_builtin_unIData),
    /* 46 unBData      */ M(1, 0, C(20142), C(32), uplcrt_builtin_unBData),
    /* 47 equalsData   */ M(2, 0, MIN_SZ(898148, 27279), C(1), uplcrt_builtin_equalsData),
    /* 48 mkPairData   */ M(2, 0, C(11546), C(32), uplcrt_builtin_mkPairData),
    /* 49 mkNilData    */ M(1, 0, C(7243),  C(32), uplcrt_builtin_mkNilData),
    /* 50 mkNilPairData*/ M(1, 0, C(7391),  C(32), uplcrt_builtin_mkNilPairData),
    /* 51 serialiseData*/ M(1, 0, LIN(955506, 213312), LIN(0, 2), uplcrt_builtin_serialiseData),

    /* 52 verifyEcdsaSecp256k1Signature   */ M(3, 0, C(43053543),          C(10), uplcrt_builtin_verifyEcdsaSecp256k1Signature),
    /* 53 verifySchnorrSecp256k1Signature */ M(3, 0, LINY(43574283, 26308), C(10), uplcrt_builtin_verifySchnorrSecp256k1Signature),

    /* 54 bls12_381_G1_add          */ M(2, 0, C(962335),   C(18), uplcrt_builtin_bls12_381_G1_add),
    /* 55 bls12_381_G1_neg          */ M(1, 0, C(267929),   C(18), uplcrt_builtin_bls12_381_G1_neg),
    /* 56 bls12_381_G1_scalarMul    */ M(2, 0, LINX(76433006, 8868), C(18), uplcrt_builtin_bls12_381_G1_scalarMul),
    /* 57 bls12_381_G1_equal        */ M(2, 0, C(442008),   C(1),  uplcrt_builtin_bls12_381_G1_equal),
    /* 58 bls12_381_G1_compress     */ M(1, 0, C(2780678),  C(6),  uplcrt_builtin_bls12_381_G1_compress),
    /* 59 bls12_381_G1_uncompress   */ M(1, 0, C(52948122), C(18), uplcrt_builtin_bls12_381_G1_uncompress),
    /* 60 bls12_381_G1_hashToGroup  */ M(2, 0, LINX(52538055, 3756), C(18), uplcrt_builtin_bls12_381_G1_hashToGroup),
    /* 61 bls12_381_G2_add          */ M(2, 0, C(1995836),  C(36), uplcrt_builtin_bls12_381_G2_add),
    /* 62 bls12_381_G2_neg          */ M(1, 0, C(284546),   C(36), uplcrt_builtin_bls12_381_G2_neg),
    /* 63 bls12_381_G2_scalarMul    */ M(2, 0, LINX(158221314, 26549), C(36), uplcrt_builtin_bls12_381_G2_scalarMul),
    /* 64 bls12_381_G2_equal        */ M(2, 0, C(901022),   C(1),  uplcrt_builtin_bls12_381_G2_equal),
    /* 65 bls12_381_G2_compress     */ M(1, 0, C(3227919),  C(12), uplcrt_builtin_bls12_381_G2_compress),
    /* 66 bls12_381_G2_uncompress   */ M(1, 0, C(74698472), C(36), uplcrt_builtin_bls12_381_G2_uncompress),
    /* 67 bls12_381_G2_hashToGroup  */ M(2, 0, LINX(166917843, 4307), C(36), uplcrt_builtin_bls12_381_G2_hashToGroup),
    /* 68 bls12_381_millerLoop      */ M(2, 0, C(254006273), C(72), uplcrt_builtin_bls12_381_millerLoop),
    /* 69 bls12_381_mulMlResult     */ M(2, 0, C(2174038),   C(72), uplcrt_builtin_bls12_381_mulMlResult),
    /* 70 bls12_381_finalVerify     */ M(2, 0, C(333849714), C(1),  uplcrt_builtin_bls12_381_finalVerify),

    /* 71 keccak_256  */ M(1, 0, LIN(2261318, 64571), C(4), uplcrt_builtin_keccak_256),
    /* 72 blake2b_224 */ M(1, 0, LIN(207616, 8310),   C(4), uplcrt_builtin_blake2b_224),

    /* 73 integerToByteString */ M(3, 0, QZ(1293828, 28716, 63),  LITY_LINZ(0, 1), uplcrt_builtin_integerToByteString),
    /* 74 byteStringToInteger */ M(2, 0, QY(1006041, 43623, 251), LINY(0, 1),      uplcrt_builtin_byteStringToInteger),

    /* 75 andByteString        */ M(3, 0, LINYZ(100181, 726, 719), LINMAXYZ(0, 1), uplcrt_builtin_andByteString),
    /* 76 orByteString         */ M(3, 0, LINYZ(100181, 726, 719), LINMAXYZ(0, 1), uplcrt_builtin_orByteString),
    /* 77 xorByteString        */ M(3, 0, LINYZ(100181, 726, 719), LINMAXYZ(0, 1), uplcrt_builtin_xorByteString),
    /* 78 complementByteString */ M(1, 0, LIN(107878, 680),        LIN(0, 1),       uplcrt_builtin_complementByteString),
    /* 79 readBit              */ M(2, 0, C(95336),                C(1),            uplcrt_builtin_readBit),
    /* 80 writeBits            */ M(3, 0, LINY(281145, 18848),     LINX(0, 1),      uplcrt_builtin_writeBits),
    /* 81 replicateByte        */ M(2, 0, LINX(180194, 159),       LINX(1, 1),      uplcrt_builtin_replicateByte),
    /* 82 shiftByteString      */ M(2, 0, LINX(158519, 8942),      LINX(0, 1),      uplcrt_builtin_shiftByteString),
    /* 83 rotateByteString     */ M(2, 0, LINX(159378, 8813),      LINX(0, 1),      uplcrt_builtin_rotateByteString),
    /* 84 countSetBits         */ M(1, 0, LIN(107490, 3298),       C(1),            uplcrt_builtin_countSetBits),
    /* 85 findFirstSetBit      */ M(1, 0, LIN(106057, 655),        C(1),            uplcrt_builtin_findFirstSetBit),
    /* 86 ripemd_160           */ M(1, 0, LIN(1964219, 24520),  C(3), uplcrt_builtin_ripemd_160),
    /* 87 expModInteger        */ M(3, 0, EXPMOD(607153, 231697, 53144), LINZ(0, 1), uplcrt_builtin_expModInteger),
    /* 88 dropList             */ M(2, 1, LINX(116711, 1957),      C(4),            uplcrt_builtin_dropList),

    /* 89 lengthOfArray */ M(1, 1, C(231883),              C(10), uplcrt_builtin_lengthOfArray),
    /* 90 listToArray   */ M(1, 1, LIN(1000, 24838),       LIN(7, 1), uplcrt_builtin_listToArray),
    /* 91 indexArray    */ M(2, 1, C(232010),              C(32), uplcrt_builtin_indexArray),

    /* 92 bls12_381_G1_multiScalarMul */ M(2, 0, LINX(321837444, 25087669), C(18), uplcrt_builtin_bls12_381_G1_multiScalarMul),
    /* 93 bls12_381_G2_multiScalarMul */ M(2, 0, LINX(617887431, 67302824), C(36), uplcrt_builtin_bls12_381_G2_multiScalarMul),

    /* 94 insertCoin     */ M(4, 0, LIN(356924, 18413), LIN(45, 21), uplcrt_builtin_insertCoin),
    /* 95 lookupCoin     */ M(3, 0, LINZ(219951, 9444), C(1),        uplcrt_builtin_lookupCoin),
    /* 96 unionValue     */ M(2, 0, INTR(1000, 172116, 183150, 6), ADDED(24, 21), uplcrt_builtin_unionValue),
    /* 97 valueContains  */ M(2, 0,
        ABOVE(213283, 0, 618401, 1998, 28258, 0, 0, 0),
        C(1),
        uplcrt_builtin_valueContains),
    /* 98 valueData      */ M(1, 0, LIN(1000, 38159),   LIN(2, 22),  uplcrt_builtin_valueData),
    /* 99 unValueData    */ M(1, 0, QX(1000, 95933, 1), LIN(1, 11),  uplcrt_builtin_unValueData),
    /*100 scaleValue     */ M(2, 0, LINY(1000, 277577), LINY(12, 21), uplcrt_builtin_scaleValue),
};

const uint32_t UPLC_BUILTIN_COUNT =
    (uint32_t)(sizeof(UPLC_BUILTIN_META) / sizeof(UPLC_BUILTIN_META[0]));
