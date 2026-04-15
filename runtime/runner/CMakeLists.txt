# libuplcrunner — the main() stub that `uplcc emit-exe` links into every
# fully-static executable.

add_library(uplcrunner STATIC main.cc)
add_library(uplc::runner ALIAS uplcrunner)

target_link_libraries(uplcrunner
    PRIVATE
        uplc::headers
        uplc::compile_options
        uplc::runtime
        uplc::frontend
)

# Allow root-rooted #includes ("runtime/...", "compiler/...").
target_include_directories(uplcrunner PRIVATE ${PROJECT_SOURCE_DIR})

set_target_properties(uplcrunner PROPERTIES
    CXX_STANDARD 20
    POSITION_INDEPENDENT_CODE ON
)
