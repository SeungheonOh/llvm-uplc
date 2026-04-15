# Public headers shared between compiler, runtime and driver. INTERFACE-only.

add_library(uplc_headers INTERFACE)
add_library(uplc::headers ALIAS uplc_headers)

target_include_directories(uplc_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
