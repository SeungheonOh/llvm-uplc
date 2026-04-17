# uplcbench — llvm-uplc benchmark harness compatible with the output
# format of saib-inc/cardano-plutus-vm-benchmark.

add_executable(uplcbench main.cc)

target_link_libraries(uplcbench
    PRIVATE
        uplc::frontend
        uplc::codegen
        uplc::runtime
        uplc::lower_bc
        uplc::compile_options
        LLVM
)

target_include_directories(uplcbench PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(uplcbench SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(uplcbench PRIVATE ${LLVM_DEFINITIONS})

set_target_properties(uplcbench PROPERTIES
    CXX_STANDARD 20
)

target_compile_options(uplcbench PRIVATE
    -Wno-conversion -Wno-sign-conversion -Wno-missing-field-initializers
)

# Force-load libuplcrt symbols so LLJIT's JITDylib resolves the runtime
# ABI against this process regardless of -fvisibility=hidden.
if(APPLE)
    target_link_options(uplcbench PRIVATE
        "LINKER:-force_load,$<TARGET_FILE:uplcrt>")
else()
    target_link_options(uplcbench PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:uplcrt>,--no-whole-archive")
endif()
