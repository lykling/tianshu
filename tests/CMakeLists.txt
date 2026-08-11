# =============================================================================
# TIANSHU tests CMake build (top-level)
# =============================================================================

add_executable(hello_test hello_test.cc)
target_link_libraries(hello_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(hello_test)

add_subdirectory(base)
add_subdirectory(sched)
