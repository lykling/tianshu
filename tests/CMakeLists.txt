# =============================================================================
# TIANSHU tests CMake build
# =============================================================================

add_executable(hello_test hello_test.cc)
target_link_libraries(hello_test PRIVATE tianshu GTest::gtest_main)

# Register with CTest (gtest_main discovers TEST() automatically)
gtest_discover_tests(hello_test)
