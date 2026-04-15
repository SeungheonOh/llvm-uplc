# uplcc — the UPLC -> .uplcx compiler.

# --- Static library: uplcfrontend ----------------------------------------
# AST + frontend components, shared by uplcc and tests. Kept separate from
# the main executable so unit tests can link directly against it.
set(UPLCFRONTEND_SOURCES
    ast/arena.cc
    ast/term.cc
    ast/builtin_table.cc
    ast/pretty.cc
    ast/cbor_data.cc
    driver.cc
    frontend/lexer.cc
    frontend/parser.cc
    frontend/name_to_debruijn.cc
    frontend/validate.cc
    frontend/flat.cc
    frontend/cbor_unwrap.cc
    lowering.cc
    analysis/compile_plan.cc
)

add_library(uplcfrontend STATIC ${UPLCFRONTEND_SOURCES})
add_library(uplc::frontend ALIAS uplcfrontend)

# The project uses root-rooted include paths like
#   #include "compiler/ast/arena.h"
# so expose ${PROJECT_SOURCE_DIR} as a public include dir.
target_include_directories(uplcfrontend PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)

target_link_libraries(uplcfrontend
    PUBLIC
        uplc::headers
        uplc::runtime
        GMP::GMP
    PRIVATE
        uplc::compile_options
)

# --- Executable: uplcc ---------------------------------------------------
set(UPLCC_SOURCES
    main.cc
)

add_executable(uplcc ${UPLCC_SOURCES})

target_link_libraries(uplcc
    PRIVATE
        uplc::codegen
        uplc::frontend
        uplc::headers
        uplc::runtime
        uplc::compile_options
        LLVM
)

# --- LLVM codegen library -------------------------------------------------
# Builds the M7 IR emitter as a static library that the compiler driver can
# link. We link against the monolithic LLVM dylib to avoid the brew-LLVM
# transitive SYSTEM-include issue (libffi / libxml2 dragging the macOS SDK
# /usr/include ahead of libc++'s own headers when component libs are used).
#
# The LLVM include dirs are added as regular (non-SYSTEM) includes so the
# compiler's own headers take precedence.

add_library(uplccodegen STATIC
    codegen/llvm_codegen.cc
    codegen/baked_const.cc
    jit/codegen_pipeline.cc
    jit/jit_runner.cc
    jit/runtime_bitcode.cc
)
add_library(uplc::codegen ALIAS uplccodegen)

target_include_directories(uplccodegen
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
    SYSTEM PRIVATE
        ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(uplccodegen PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(uplccodegen
    PUBLIC  uplc::frontend
    PRIVATE LLVM GMP::GMP uplc::runtime_bitcode
)
# The codegen TU includes LLVM headers that trigger -Wconversion /
# -Wsign-conversion in Clang 22's template instantiation diagnostics.
# Suppress only those two flags for this target; keep -Wall/-Wextra/-Werror.
target_compile_options(uplccodegen PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-conversion -Wno-sign-conversion
    -Wno-nested-anon-types -Wno-gnu-anonymous-struct
    -Wno-missing-field-initializers
    -fno-common -fvisibility=hidden
    $<$<CONFIG:Debug>:-O0 -g3>
    $<$<CONFIG:Release>:-O3 -g -DNDEBUG>
)

target_include_directories(uplcc
    SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS}
    PRIVATE ${PROJECT_SOURCE_DIR}
)
target_compile_options(uplcc PRIVATE
    -Wno-conversion -Wno-sign-conversion -Wno-missing-field-initializers
)
target_compile_definitions(uplcc PRIVATE
    UPLCC_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
    ${LLVM_DEFINITIONS}
)
# Force-include all uplcrt symbols so the LLJIT runtime linker can find them
# via DynamicLibrarySearchGenerator::GetForCurrentProcess() even for symbols
# not directly referenced from uplcc's own code.
if(APPLE)
    target_link_options(uplcc PRIVATE
        "LINKER:-force_load,$<TARGET_FILE:uplcrt>")
else()
    target_link_options(uplcc PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:uplcrt>,--no-whole-archive")
endif()
