# Shared warning / hardening flags for every target in the project.
# Targets opt in by linking against the INTERFACE target `uplc::compile_options`.

add_library(uplc_compile_options INTERFACE)
add_library(uplc::compile_options ALIAS uplc_compile_options)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(uplc_compile_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wno-unused-parameter
        -fno-common
        -fvisibility=hidden
        $<$<CONFIG:Debug>:-O0 -g3>
        $<$<CONFIG:Release>:-O3 -g -DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
    )
    # Apply C++-only hardening that doesn't belong on the C TU.
    target_compile_options(uplc_compile_options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(uplc_compile_options INTERFACE
        -Wno-c99-extensions
        # Nested anonymous struct/union in anonymous union is used intentionally
        # for the Term / Constant / PlutusData tagged unions.
        -Wno-nested-anon-types
        -Wno-gnu-anonymous-struct
    )
endif()
