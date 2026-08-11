# =============================================================================
# TIANSHU tests CMake build
# =============================================================================

add_executable(hello_test hello_test.cc)
target_link_libraries(hello_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(hello_test)

add_executable(object_pool_test object_pool_test.cc)
target_link_libraries(object_pool_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(object_pool_test)
