# M0 smoke tests: confirm both binaries produce a version line on --version.

add_executable(frontend_test unit/frontend_test.cc)
target_link_libraries(frontend_test PRIVATE uplc::frontend uplc::compile_options)
add_test(NAME frontend_test COMMAND frontend_test)

add_executable(round_trip_test conformance/round_trip_test.cc)
target_link_libraries(round_trip_test PRIVATE uplc::frontend uplc::compile_options)
target_compile_definitions(round_trip_test PRIVATE
    "UPLC_CONFORMANCE_ROOT=\"${CMAKE_SOURCE_DIR}/tests/conformance/fixtures\"")
add_test(NAME round_trip_test COMMAND round_trip_test)

add_executable(cek_test unit/cek_test.cc)
target_link_libraries(cek_test PRIVATE uplc::frontend uplc::runtime uplc::compile_options)
add_test(NAME cek_test COMMAND cek_test)

add_executable(compiled_abi_test unit/compiled_abi_test.cc)
target_link_libraries(compiled_abi_test PRIVATE uplc::runtime uplc::compile_options)
# The runtime static library keeps its root include path PRIVATE so consumers
# don't accidentally reach into runtime internals. This test deliberately
# does — it pokes runtime/arena.h and runtime/compiled/entry.h — so add the
# include root locally.
target_include_directories(compiled_abi_test PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME compiled_abi_test COMMAND compiled_abi_test)

add_executable(builtin_test unit/builtin_test.cc)
target_link_libraries(builtin_test PRIVATE uplc::frontend uplc::runtime uplc::compile_options)
target_include_directories(builtin_test PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME builtin_test COMMAND builtin_test)

add_executable(codegen_test unit/codegen_test.cc)
target_link_libraries(codegen_test PRIVATE uplc::codegen uplc::frontend)
target_include_directories(codegen_test PRIVATE ${PROJECT_SOURCE_DIR} ${LLVM_INCLUDE_DIRS})
target_compile_options(codegen_test PRIVATE
    -Wno-conversion -Wno-sign-conversion
    -Wno-nested-anon-types -Wno-gnu-anonymous-struct
    -Wno-missing-field-initializers
)
add_test(NAME codegen_test COMMAND codegen_test)

add_executable(compile_plan_test unit/compile_plan_test.cc)
target_link_libraries(compile_plan_test PRIVATE uplc::frontend uplc::compile_options)
add_test(NAME compile_plan_test COMMAND compile_plan_test)

add_executable(eval_conformance_test conformance/eval_conformance_test.cc)
target_link_libraries(eval_conformance_test PRIVATE uplc::frontend uplc::runtime uplc::compile_options)
target_include_directories(eval_conformance_test PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(eval_conformance_test PRIVATE
    "UPLC_CONFORMANCE_ROOT=\"${CMAKE_SOURCE_DIR}/tests/conformance/fixtures\"")
add_test(NAME eval_conformance_test COMMAND eval_conformance_test)

add_test(NAME smoke_uplcc_version
         COMMAND uplcc --version)
set_tests_properties(smoke_uplcc_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^uplcc [0-9]+\\.[0-9]+\\.[0-9]+"
)

add_test(NAME smoke_uplcr_version
         COMMAND uplcr --version)
set_tests_properties(smoke_uplcr_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^uplcr [0-9]+\\.[0-9]+\\.[0-9]+"
)

add_test(NAME smoke_uplcc_help
         COMMAND uplcc --help)
set_tests_properties(smoke_uplcc_help PROPERTIES
    PASS_REGULAR_EXPRESSION "usage: uplcc"
)

add_test(NAME smoke_uplcr_help
         COMMAND uplcr --help)
set_tests_properties(smoke_uplcr_help PROPERTIES
    PASS_REGULAR_EXPRESSION "usage: uplcr"
)

add_test(NAME smoke_uplcc_no_args_fails
         COMMAND uplcc)
set_tests_properties(smoke_uplcc_no_args_fails PROPERTIES
    WILL_FAIL TRUE
)

add_executable(jit_conformance_test conformance/jit_conformance_test.cc)
target_link_libraries(jit_conformance_test
    PRIVATE
        uplc::codegen
        uplc::frontend
        uplc::runtime
        uplc::compile_options
        LLVM
)
target_include_directories(jit_conformance_test
    PRIVATE ${PROJECT_SOURCE_DIR} ${LLVM_INCLUDE_DIRS})
target_compile_options(jit_conformance_test PRIVATE
    -Wno-conversion -Wno-sign-conversion
    -Wno-nested-anon-types -Wno-gnu-anonymous-struct
    -Wno-missing-field-initializers
)
target_compile_definitions(jit_conformance_test PRIVATE
    "UPLC_CONFORMANCE_ROOT=\"${CMAKE_SOURCE_DIR}/tests/conformance/fixtures\""
    ${LLVM_DEFINITIONS}
)
# Force-load uplcrt so the LLJIT runtime linker can find all ABI symbols
# even when the linker would otherwise dead-strip them.
if(APPLE)
    target_link_options(jit_conformance_test PRIVATE
        "LINKER:-force_load,$<TARGET_FILE:uplcrt>")
else()
    target_link_options(jit_conformance_test PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:uplcrt>,--no-whole-archive")
endif()
add_test(NAME jit_conformance_test COMMAND jit_conformance_test)
