# libuplcrt — the pure-C runtime linked into both uplcr and every .uplcx.

# Sources are split into two groups:
#
#   UPLCRT_SHARED_SOURCES
#       Pieces used by both execution modes — direct CEK interpretation and
#       the (later) LLVM-compiled entry path. Stays directly in runtime/.
#
#   UPLCRT_CEK_SOURCES
#       CEK-interpreter-specific state and control flow. Lives in
#       runtime/cek/ so it is obvious which files go away (or are
#       re-purposed) for compiled execution.
set(UPLCRT_SHARED_SOURCES
    version.c
    builtin_table.c
    arena.c
    value.c
    budget.c
    errors.c
    costmodel.c
    exmem.c
    builtin_state.c
    builtin_dispatch.c
    cbor_data.c
    readback.c
    case_decompose.c
)

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

set(UPLCRT_CEK_SOURCES
    cek/rterm.c
    cek/env.c
    cek/closure.c
    cek/stack.c
    cek/cek.c
)

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

set(UPLCRT_SOURCES
    ${UPLCRT_SHARED_SOURCES}
    ${UPLCRT_BUILTIN_SOURCES}
    ${UPLCRT_CEK_SOURCES}
    ${UPLCRT_COMPILED_SOURCES}
)

add_library(uplcrt STATIC ${UPLCRT_SOURCES})
add_library(uplc::runtime ALIAS uplcrt)

target_link_libraries(uplcrt
    PUBLIC  uplc::headers
    PRIVATE uplc::compile_options
)

# Allow runtime .c files to use root-rooted includes like
# "runtime/cek/cek.h" or "runtime/value.h".
target_include_directories(uplcrt PRIVATE ${PROJECT_SOURCE_DIR})

set_target_properties(uplcrt PROPERTIES
    C_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
)

# GMP is linked transitively by any consumer of the runtime (compiler, driver,
# .uplcx). The runtime itself doesn't yet reference GMP symbols in M0, but we
# wire the dependency now so downstream targets don't have to repeat it.
target_link_libraries(uplcrt PUBLIC GMP::GMP)

# Crypto libraries — libsodium (sha2/blake2b/ed25519) + OpenSSL (sha3/keccak/ripemd).
if(SODIUM_FOUND)
    target_link_libraries(uplcrt PRIVATE PkgConfig::SODIUM)
    target_compile_definitions(uplcrt PRIVATE UPLCRT_HAVE_SODIUM=1)
endif()

if(OpenSSL_FOUND OR OPENSSL_FOUND)
    target_link_libraries(uplcrt PRIVATE OpenSSL::Crypto)
    target_compile_definitions(uplcrt PRIVATE UPLCRT_HAVE_OPENSSL=1)
endif()

if(SECP256K1_FOUND)
    target_link_libraries(uplcrt PRIVATE PkgConfig::SECP256K1)
    target_compile_definitions(uplcrt PRIVATE UPLCRT_HAVE_SECP256K1=1)
endif()

# BLST — always available (vendored).
target_link_libraries(uplcrt PRIVATE blst)
target_compile_definitions(uplcrt PRIVATE UPLCRT_HAVE_BLST=1)

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
#   - exmem.c, costmodel.c, builtin_dispatch.c, builtin_table.c
#                 (they reach into the per-builtin cost model and pull
#                  in the entire builtin tag space; not on the JIT hot path)
#   - readback.c, cbor_data.c (output-side, not steady-state)
#
# What remains is the core compiled-mode helpers + their immediate
# dependencies — small enough to inline aggressively, large enough to
# remove almost all wire_ty marshaling once LLVM is done.
# ---------------------------------------------------------------------------

set(UPLCRT_BITCODE_SOURCES
    arena.c
    value.c
    budget.c
    errors.c
    builtin_state.c
    builtin_dispatch.c     # uplcrt_run_builtin — exposes the dispatch chain
    builtin_table.c        # UPLC_BUILTIN_META[] — the const table the inliner needs
    costmodel.c            # cost-shape eval, folds when the shape is known
    exmem.c                # arg-size dispatch, switch on tag
    case_decompose.c
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
