# uplcr — the .uplcx loader/runner.

set(UPLCR_SOURCES
    main.cc
)

add_executable(uplcr ${UPLCR_SOURCES})

# uplc::frontend gives us arg parsing (parser, flat, cbor_unwrap,
# name_to_debruijn, lowering) and result pretty-printing (ast/pretty).
# It transitively pulls in uplc::runtime and uplc::headers.
target_link_libraries(uplcr
    PRIVATE
        uplc::frontend
        uplc::compile_options
        LLVM
)

# LLVM include dirs (non-SYSTEM to match the compiler target's convention).
target_include_directories(uplcr SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(uplcr PRIVATE ${LLVM_DEFINITIONS})

target_compile_options(uplcr PRIVATE
    -Wno-conversion -Wno-sign-conversion -Wno-missing-field-initializers
)

# Force-export all uplcrt symbols into the uplcr process image so the LLJIT
# linker can resolve them when loading the compiled object file.
if(APPLE)
    target_link_options(uplcr PRIVATE
        "LINKER:-force_load,$<TARGET_FILE:uplcrt>")
else()
    target_link_options(uplcr PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:uplcrt>,--no-whole-archive")
endif()
