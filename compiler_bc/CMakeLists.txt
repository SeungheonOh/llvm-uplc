# compiler_bc — frontend + lowerer + uplci binary for the bytecode VM
# (plan-bc.md).
#
# Deliberately NO LLVM dependency: the bytecode VM is pure C and the
# lowerer works off the existing compiler's AST / rterm pipeline.

# --- Static library: uplclower_bc ---------------------------------------
# AST / rterm → uplc_bc_program lowering. Shared by uplci and tests.
add_library(uplclower_bc STATIC
    lower_to_bc.cc
    disasm.cc
)
add_library(uplc::lower_bc ALIAS uplclower_bc)

target_link_libraries(uplclower_bc
    PUBLIC
        uplc::frontend       # AST types, lower_to_runtime
        uplc::runtime_bc     # bytecode ISA + VM
        uplc::runtime_core
        uplc::runtime_builtins
    PRIVATE
        uplc::compile_options
)
target_include_directories(uplclower_bc PUBLIC ${PROJECT_SOURCE_DIR})
set_target_properties(uplclower_bc PROPERTIES
    CXX_STANDARD 20
    POSITION_INDEPENDENT_CODE ON
)

# --- Executable: uplci --------------------------------------------------
add_executable(uplci main.cc)
target_link_libraries(uplci
    PRIVATE
        uplc::lower_bc
        uplc::frontend
        uplc::runtime
        uplc::compile_options
)
target_include_directories(uplci PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_options(uplci PRIVATE
    -Wno-conversion -Wno-sign-conversion -Wno-missing-field-initializers
)
