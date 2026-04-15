cmake_minimum_required(VERSION 3.22)

project(llvm_uplc
    VERSION 0.0.1
    DESCRIPTION "UPLC to LLVM AOT compiler with CEK-preserving semantics"
    LANGUAGES C CXX ASM
)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Homebrew paths on macOS — make find_package locate brewed LLVM/GMP/libsodium.
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH
        /opt/homebrew/opt/llvm
        /opt/homebrew/opt/gmp
        /opt/homebrew/opt/libsodium
        /opt/homebrew/opt/secp256k1
    )
endif()

include(UplcCompileOptions)
include(CTest)
enable_testing()

# ---------------------------------------------------------------------------
# Required dependencies
# ---------------------------------------------------------------------------

# LLVM — pinned to the brewed copy on macOS. We target the latest stable and
# do not enforce a minimum via find_package (the range syntax is finicky);
# instead we assert on the discovered version below.
find_package(LLVM CONFIG REQUIRED)
if(LLVM_PACKAGE_VERSION VERSION_LESS "19.0")
    message(FATAL_ERROR
        "llvm-uplc requires LLVM >= 19.0, found ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
endif()
message(STATUS "llvm-uplc: found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

# GMP — required as soon as runtime bigint lands in M2, detected now so CMake
# fails fast if the user hasn't installed it.
find_package(GMP REQUIRED)
message(STATUS "llvm-uplc: found GMP ${GMP_VERSION} (${GMP_LIBRARIES})")

# ---------------------------------------------------------------------------
# Optional dependencies (will become required in later milestones)
# ---------------------------------------------------------------------------

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SODIUM IMPORTED_TARGET libsodium)
    pkg_check_modules(SECP256K1 IMPORTED_TARGET libsecp256k1)
endif()

if(SODIUM_FOUND)
    message(STATUS "llvm-uplc: found libsodium ${SODIUM_VERSION}")
else()
    message(FATAL_ERROR "llvm-uplc: libsodium not found — install with `brew install libsodium`")
endif()

if(SECP256K1_FOUND)
    message(STATUS "llvm-uplc: found libsecp256k1 ${SECP256K1_VERSION}")
else()
    message(STATUS "llvm-uplc: libsecp256k1 NOT found (secp256k1 builtins remain stubs; `brew install secp256k1`)")
endif()

# OpenSSL 3.x — required for sha3_256, keccak_256, ripemd_160.
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/openssl@3)
endif()
find_package(OpenSSL REQUIRED)
message(STATUS "llvm-uplc: found OpenSSL ${OPENSSL_VERSION} (${OPENSSL_LIBRARIES})")

# BLST — BLS12-381 pairing library vendored via FetchContent.
# BLST ships without a top-level CMakeLists.txt, so we build it as a manual
# static target: a single amalgamated C file plus the asm trampoline.
include(FetchContent)
FetchContent_Declare(
    blst
    GIT_REPOSITORY https://github.com/supranational/blst.git
    GIT_TAG        v0.3.13
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(blst)

set(BLST_ASM "${blst_SOURCE_DIR}/build/assembly.S")
# Compiling as ASM keeps the clang preprocessor enabled (uppercase .S) while
# picking the correct toolchain invocation for an assembly file.
add_library(blst STATIC
    "${blst_SOURCE_DIR}/src/server.c"
    "${BLST_ASM}"
)
target_include_directories(blst PUBLIC "${blst_SOURCE_DIR}/bindings")
# BLST includes its asm fragments via the C preprocessor, so the asm files
# must be reachable relative to the assembly.S directory.
target_include_directories(blst PRIVATE "${blst_SOURCE_DIR}/build")
set_target_properties(blst PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
)
message(STATUS "llvm-uplc: BLST vendored from supranational/blst v0.3.13")

# ---------------------------------------------------------------------------
# Static-library paths for `uplcc emit-exe`.
#
# emit-exe shells out to the system C++ compiler to link a fully-static
# executable. We pre-locate every dependency's .a archive at configure
# time and bake the absolute paths into uplc/build_paths.h so the
# compiled `uplcc` binary has zero runtime dependency on CMake or this
# directory tree.
# ---------------------------------------------------------------------------

find_library(UPLC_GMP_STATIC NAMES libgmp.a HINTS
    /opt/homebrew/opt/gmp/lib
    /usr/local/opt/gmp/lib
    /usr/lib
    /usr/local/lib)
find_library(UPLC_SODIUM_STATIC NAMES libsodium.a HINTS
    /opt/homebrew/opt/libsodium/lib
    /usr/local/opt/libsodium/lib
    /usr/lib
    /usr/local/lib)
find_library(UPLC_CRYPTO_STATIC NAMES libcrypto.a HINTS
    /opt/homebrew/opt/openssl@3/lib
    /usr/local/opt/openssl@3/lib
    /usr/lib
    /usr/local/lib)
find_library(UPLC_SECP256K1_STATIC NAMES libsecp256k1.a HINTS
    /opt/homebrew/opt/secp256k1/lib
    /usr/local/opt/secp256k1/lib
    /usr/lib
    /usr/local/lib)

if(NOT UPLC_GMP_STATIC)
    message(WARNING "llvm-uplc: libgmp.a not found — `uplcc emit-exe` will fail")
endif()
if(NOT UPLC_SODIUM_STATIC)
    message(WARNING "llvm-uplc: libsodium.a not found — `uplcc emit-exe` will fail")
endif()
if(NOT UPLC_CRYPTO_STATIC)
    message(WARNING "llvm-uplc: libcrypto.a not found — `uplcc emit-exe` will fail")
endif()
if(NOT UPLC_SECP256K1_STATIC)
    message(WARNING "llvm-uplc: libsecp256k1.a not found — `uplcc emit-exe` will fail")
endif()

message(STATUS "llvm-uplc: static libgmp        = ${UPLC_GMP_STATIC}")
message(STATUS "llvm-uplc: static libsodium     = ${UPLC_SODIUM_STATIC}")
message(STATUS "llvm-uplc: static libcrypto     = ${UPLC_CRYPTO_STATIC}")
message(STATUS "llvm-uplc: static libsecp256k1  = ${UPLC_SECP256K1_STATIC}")

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------

add_subdirectory(include)
add_subdirectory(runtime)
add_subdirectory(compiler)
# The static-runner stub depends on uplc::frontend (added by compiler/),
# so it must come after the compiler subdirectory.
add_subdirectory(runtime/runner)
add_subdirectory(driver)
add_subdirectory(tools/bench)
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Generated build-paths header for `uplcc emit-exe`
#
# The static-library locations are computed by appending the canonical
# CMake archive output paths to the build tree root. We resolve them via
# generator expressions through file(GENERATE), which runs after the
# targets are fully defined.
# ---------------------------------------------------------------------------
set(UPLC_RUNTIME_LIB_PATH  "$<TARGET_FILE:uplcrt>")
set(UPLC_RUNNER_LIB_PATH   "$<TARGET_FILE:uplcrunner>")
set(UPLC_FRONTEND_LIB_PATH "$<TARGET_FILE:uplcfrontend>")
set(UPLC_BLST_LIB_PATH     "$<TARGET_FILE:blst>")

# Stage 1: configure_file expands @VAR@ tokens but leaves $<...> untouched.
configure_file(
    ${PROJECT_SOURCE_DIR}/include/uplc/build_paths.h.in
    ${CMAKE_BINARY_DIR}/generated/uplc/build_paths.h.tpl
    @ONLY
)
# Stage 2: file(GENERATE) expands the remaining $<TARGET_FILE:...> entries
# at build time, when the targets have known on-disk locations.
file(GENERATE
    OUTPUT  ${CMAKE_BINARY_DIR}/generated/uplc/build_paths.h
    INPUT   ${CMAKE_BINARY_DIR}/generated/uplc/build_paths.h.tpl
)
target_include_directories(uplcc PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(uplcc uplcrt uplcrunner uplcfrontend blst)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "----------------------------------------------------------")
message(STATUS "llvm-uplc ${PROJECT_VERSION}")
message(STATUS "  build type       : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C compiler       : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  LLVM             : ${LLVM_PACKAGE_VERSION}")
message(STATUS "  GMP              : ${GMP_VERSION}")
message(STATUS "  libsodium        : ${SODIUM_VERSION}")
message(STATUS "  libsecp256k1     : ${SECP256K1_VERSION}")
message(STATUS "  BLST             : v0.3.13 (vendored)")
message(STATUS "----------------------------------------------------------")
