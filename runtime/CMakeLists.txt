# libuplcrt — the pure-C runtime.
#
# Structure:
#
#   Four OBJECT libraries group the source files by role:
#
#     uplcrt_core      — arena, value, budget, cost model, Data/CBOR, error
#                        plumbing, builtin dispatch table. No execution
#                        model. No crypto.
#     uplcrt_builtins  — all 101 Plutus builtins + crypto link deps
#                        (libsodium / OpenSSL / libsecp256k1 / BLST).
#     uplcrt_compiled  — JIT-side helpers (apply/force/case/constr/closure/
#                        entry) called by LLVM-emitted IR.
#     uplcrt_cek       — the tree-walking CEK interpreter. Scheduled for
#                        retirement in M-bc-9 once the bytecode VM
#                        (plan-bc.md) reaches parity.
#
#   A STATIC library `uplcrt` merges all four OBJECT libs into a single
#   archive. This archive is force-loaded by every existing consumer
#   (uplcc, uplcr, uplcrunner, tests, tools/bench) so their link lines
#   don't change — and `$<TARGET_FILE:uplcrt>` remains a real path that
#   `uplcc emit-exe` can embed in the generated build_paths.h.
#
#   Downstream consumers that need a narrower slice (e.g. the future
#   `uplci` bytecode binary in plan-bc.md) compose their own STATIC
#   archive from the OBJECT libs directly, or link against them as
#   TARGET_OBJECTS.
#
#   PUBLIC link-library propagation on OBJECT libs (CMake ≥ 3.12) carries
#   crypto, GMP, and header deps through to whichever merge archive the
#   consumer chooses.

# ---------------------------------------------------------------------------
# 1. Core
# ---------------------------------------------------------------------------
set(UPLCRT_CORE_SOURCES
    core/version.c
    core/arena.c
    core/value.c
    core/budget.c
    core/errors.c
    core/costmodel.c
    core/exmem.c
    core/builtin_state.c
    core/builtin_dispatch.c
    core/builtin_table.c
    core/cbor_data.c
    core/case_decompose.c
    core/rterm.c
)

add_library(uplcrt_core OBJECT ${UPLCRT_CORE_SOURCES})
add_library(uplc::runtime_core ALIAS uplcrt_core)

target_link_libraries(uplcrt_core
    PUBLIC  uplc::headers GMP::GMP
    PRIVATE uplc::compile_options
)
target_include_directories(uplcrt_core PUBLIC ${PROJECT_SOURCE_DIR})
set_target_properties(uplcrt_core PROPERTIES
    C_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
)

# ---------------------------------------------------------------------------
# 2. Builtins (crypto live here)
# ---------------------------------------------------------------------------
set(UPLCRT_BUILTIN_SOURCES
    builtins/helpers.c
    builtins/arith.c
    builtins/control.c
    builtins/pair.c
    builtins/list.c
    builtins/bytestring.c
    builtins/data.c
    builtins/string.c
    builtins/array.c
    builtins/expmod.c
    builtins/bitwise.c
    builtins/intbs.c
    builtins/crypto.c
    builtins/secp256k1.c
    builtins/bls12_381.c
    builtins/value.c
)

add_library(uplcrt_builtins OBJECT ${UPLCRT_BUILTIN_SOURCES})
add_library(uplc::runtime_builtins ALIAS uplcrt_builtins)

target_link_libraries(uplcrt_builtins
    PUBLIC  uplcrt_core
    PRIVATE uplc::compile_options
)
set_target_properties(uplcrt_builtins PROPERTIES
    C_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
)

# Crypto libraries. PUBLIC because OBJECT libraries have no link step —
# consumers of the merge archive (or of this OBJECT lib directly) are the
# ones that actually link against sodium/openssl/secp256k1/blst. The PUBLIC
# keyword makes CMake propagate the link deps through.
if(SODIUM_FOUND)
    target_link_libraries(uplcrt_builtins PUBLIC PkgConfig::SODIUM)
    target_compile_definitions(uplcrt_builtins PRIVATE UPLCRT_HAVE_SODIUM=1)
endif()

if(OpenSSL_FOUND OR OPENSSL_FOUND)
    target_link_libraries(uplcrt_builtins PUBLIC OpenSSL::Crypto)
    target_compile_definitions(uplcrt_builtins PRIVATE UPLCRT_HAVE_OPENSSL=1)
endif()

if(SECP256K1_FOUND)
    target_link_libraries(uplcrt_builtins PUBLIC PkgConfig::SECP256K1)
    target_compile_definitions(uplcrt_builtins PRIVATE UPLCRT_HAVE_SECP256K1=1)
endif()

target_link_libraries(uplcrt_builtins PUBLIC blst)
target_compile_definitions(uplcrt_builtins PRIVATE UPLCRT_HAVE_BLST=1)

# ---------------------------------------------------------------------------
# 3. Compiled-mode helpers (JIT path)
# ---------------------------------------------------------------------------
set(UPLCRT_COMPILED_SOURCES
    compiled/closure.c
    compiled/apply.c
    compiled/force.c
    compiled/constr.c
    compiled/case.c
    compiled/builtin.c
    compiled/consts.c
    compiled/baked_const.c
    compiled/entry.c
)

add_library(uplcrt_compiled OBJECT ${UPLCRT_COMPILED_SOURCES})
add_library(uplc::runtime_compiled ALIAS uplcrt_compiled)

target_link_libraries(uplcrt_compiled
    PUBLIC  uplcrt_builtins
    PRIVATE uplc::compile_options
)
set_target_properties(uplcrt_compiled PROPERTIES
    C_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
)

# ---------------------------------------------------------------------------
# 4. Tree-walking CEK interpreter (scheduled for retirement in M-bc-9)
# ---------------------------------------------------------------------------
set(UPLCRT_CEK_SOURCES
    cek/env.c
    cek/closure.c
    cek/stack.c
    cek/cek.c
    cek/readback.c
)

add_library(uplcrt_cek OBJECT ${UPLCRT_CEK_SOURCES})
add_library(uplc::runtime_cek ALIAS uplcrt_cek)

target_link_libraries(uplcrt_cek
    PUBLIC  uplcrt_builtins
    PRIVATE uplc::compile_options
)
set_target_properties(uplcrt_cek PROPERTIES
    C_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
)

# ---------------------------------------------------------------------------
# 4.5. Bytecode VM (plan-bc.md — the uplci interpreter path)
#
# Lives behind a separate OBJECT library so the future `uplci` binary can
# link it without pulling in uplcrt_compiled (JIT helpers) or uplcrt_cek
# (tree-walker). No LLVM dependency.
# ---------------------------------------------------------------------------
set(UPLCRT_BC_SOURCES
    bytecode/vm.c
    bytecode/closure.c
    bytecode/ops_control.c
    bytecode/ops_abstraction.c
    bytecode/ops_apply.c
    bytecode/ops_force.c
    bytecode/ops_constr.c
    bytecode/ops_case.c
)

add_library(uplcrt_bc OBJECT ${UPLCRT_BC_SOURCES})
add_library(uplc::runtime_bc ALIAS uplcrt_bc)

target_link_libraries(uplcrt_bc
    PUBLIC  uplcrt_builtins
    PRIVATE uplc::compile_options
)
set_target_properties(uplcrt_bc PROPERTIES
    C_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
)

# The dispatch macro uses `[[clang::musttail]]` — a C23 attribute syntax
# clang accepts in C17 as an extension. Silence the diagnostic for this
# lib only; it's load-bearing for tail-call threaded dispatch.
target_compile_options(uplcrt_bc PRIVATE -Wno-c23-extensions)

# ---------------------------------------------------------------------------
# 5. Merge STATIC archive — what every existing consumer force-loads
# ---------------------------------------------------------------------------
add_library(uplcrt STATIC
    $<TARGET_OBJECTS:uplcrt_core>
    $<TARGET_OBJECTS:uplcrt_builtins>
    $<TARGET_OBJECTS:uplcrt_compiled>
    $<TARGET_OBJECTS:uplcrt_cek>
)
add_library(uplc::runtime ALIAS uplcrt)

target_link_libraries(uplcrt
    PUBLIC
        uplcrt_core
        uplcrt_builtins
        uplcrt_compiled
        uplcrt_cek
)
set_target_properties(uplcrt PROPERTIES
    C_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
)

# Note: the runner subdirectory is added from the top-level CMakeLists
# AFTER compiler/, because the runner depends on uplc::frontend.

# ---------------------------------------------------------------------------
# Runtime bitcode bundle for the JIT inliner.
#
# A focused subset of the runtime is compiled to LLVM bitcode and linked
# into a single uplcrt_runtime.bc file that the codegen embeds into every
# program module before optimization. With the runtime visible as IR,
# LLVM can inline uplcrt_apply / uplcrt_force / closure helpers / etc
# into the user's code, dropping the wire_ty marshalling and dispatch
# overhead we'd otherwise eat on every call.
#
# We deliberately exclude:
#   - builtins/*  (they're tag-dispatched function pointers — never inline)
#   - cek/*       (the CEK interpreter is a separate execution mode)
#   - core/exmem.c, core/costmodel.c, core/builtin_dispatch.c,
#     core/builtin_table.c
#                 (they reach into the per-builtin cost model and pull
#                  in the entire builtin tag space; not on the JIT hot path)
#   - core/readback.c, core/cbor_data.c (output-side, not steady-state)
#
# What remains is the core compiled-mode helpers + their immediate
# dependencies — small enough to inline aggressively, large enough to
# remove almost all wire_ty marshaling once LLVM is done.
# ---------------------------------------------------------------------------

set(UPLCRT_BITCODE_SOURCES
    core/arena.c
    core/value.c
    core/budget.c
    core/errors.c
    core/builtin_state.c
    core/builtin_dispatch.c     # uplcrt_run_builtin — exposes the dispatch chain
    core/builtin_table.c        # UPLC_BUILTIN_META[] — the const table the inliner needs
    core/costmodel.c            # cost-shape eval, folds when the shape is known
    core/exmem.c                # arg-size dispatch, switch on tag
    core/case_decompose.c
    compiled/closure.c
    compiled/apply.c
    compiled/force.c
    compiled/constr.c
    compiled/case.c
    compiled/builtin.c
    compiled/consts.c
    compiled/baked_const.c
    compiled/entry.c
)

# Locate clang, llvm-link and opt so we can drive the bitcode build directly.
# We require the brewed LLVM (already pinned via CMAKE_PREFIX_PATH) so the
# clang version matches the libLLVM we link against.
find_program(UPLC_BC_CLANG     clang     HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
find_program(UPLC_BC_LLVM_LINK llvm-link HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
find_program(UPLC_BC_OPT       opt       HINTS /opt/homebrew/opt/llvm/bin REQUIRED)
find_program(UPLC_BC_XXD       xxd       REQUIRED)
message(STATUS "llvm-uplc: bitcode toolchain clang=${UPLC_BC_CLANG} llvm-link=${UPLC_BC_LLVM_LINK} opt=${UPLC_BC_OPT}")

# Per-source bitcode build. Each .c becomes .bc in a parallel subdirectory
# of the build tree so we don't shadow the libuplcrt.a object outputs.
set(UPLCRT_BITCODE_OUTPUTS "")
foreach(src IN LISTS UPLCRT_BITCODE_SOURCES)
    get_filename_component(src_dir ${src} DIRECTORY)
    get_filename_component(src_name ${src} NAME_WE)
    set(bc_path "${CMAKE_CURRENT_BINARY_DIR}/bc/${src_dir}/${src_name}.bc")
    get_filename_component(bc_parent ${bc_path} DIRECTORY)
    file(MAKE_DIRECTORY ${bc_parent})

    add_custom_command(
        OUTPUT  ${bc_path}
        COMMAND ${UPLC_BC_CLANG}
                -emit-llvm -c
                -O3
                -fno-builtin
                -fvisibility=default
                -std=c17
                -DUPLCRT_BITCODE_BUILD=1
                -I${PROJECT_SOURCE_DIR}
                -I${PROJECT_SOURCE_DIR}/include
                -I/opt/homebrew/opt/gmp/include
                ${CMAKE_CURRENT_SOURCE_DIR}/${src}
                -o ${bc_path}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
        COMMENT "BC ${src}"
        VERBATIM
    )
    list(APPEND UPLCRT_BITCODE_OUTPUTS ${bc_path})
endforeach()

# Combined bitcode bundle (one big module containing every TU we want
# the per-program optimizer to be able to inline through).
set(UPLCRT_BITCODE_RAW "${CMAKE_CURRENT_BINARY_DIR}/uplcrt_runtime.raw.bc")
add_custom_command(
    OUTPUT  ${UPLCRT_BITCODE_RAW}
    COMMAND ${UPLC_BC_LLVM_LINK} ${UPLCRT_BITCODE_OUTPUTS}
            -o ${UPLCRT_BITCODE_RAW}
    DEPENDS ${UPLCRT_BITCODE_OUTPUTS}
    COMMENT "LINK uplcrt_runtime.raw.bc"
    VERBATIM
)

# We experimented with running an `opt` pass on the linked bundle here
# (both `-passes=default<O3>` and a leaner cleanup pipeline) — both
# slowed the per-program compile down by 20–30%. The per-program
# optimizer runs O3 anyway, and pre-running it on the runtime just
# inflates the IR (loop unrolling, instcombine duplication) and forces
# the per-program inliner to chase the already-inlined chains. So we
# embed the raw linked bundle directly.
set(UPLCRT_BITCODE_BLOB ${UPLCRT_BITCODE_RAW})

# Embed the bitcode blob as a C++ byte array. xxd -i is part of every
# macOS / *BSD / Linux base install, so no extra host tool to install.
set(UPLCRT_BITCODE_CC "${CMAKE_CURRENT_BINARY_DIR}/uplcrt_runtime_bc.c")
add_custom_command(
    OUTPUT  ${UPLCRT_BITCODE_CC}
    COMMAND ${UPLC_BC_XXD} -i -n uplcrt_runtime_bc
            < ${UPLCRT_BITCODE_BLOB}
            > ${UPLCRT_BITCODE_CC}
    DEPENDS ${UPLCRT_BITCODE_BLOB}
    COMMENT "EMBED uplcrt_runtime.bc → uplcrt_runtime_bc.c"
    VERBATIM
)

# Static lib whose only job is to expose `uplcrt_runtime_bc[]` and
# `uplcrt_runtime_bc_len` to the codegen. Built as a regular C library
# so consumers can link it without dragging in any additional headers.
add_library(uplcrt_bitcode STATIC ${UPLCRT_BITCODE_CC})
add_library(uplc::runtime_bitcode ALIAS uplcrt_bitcode)
target_include_directories(uplcrt_bitcode PRIVATE ${PROJECT_SOURCE_DIR})
set_target_properties(uplcrt_bitcode PROPERTIES
    C_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
)
