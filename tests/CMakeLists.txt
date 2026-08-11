# =============================================================================
# TIANSHU tests CMake build
# =============================================================================
#
# Phase 0: minimal smoke test (validates CMake + Bazel both work end-to-end).
# Phase 1+: use GoogleTest via FetchContent / bazel http_archive.

enable_testing()

add_executable(hello_test hello_test.cc)
target_link_libraries(hello_test PRIVATE tianshu)

# Phase 0: register as a CTest without gtest
add_test(NAME hello_test COMMAND hello_test)
