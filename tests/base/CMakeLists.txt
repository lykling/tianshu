# =============================================================================
# TIANSHU tests/base CMake build
# =============================================================================

add_executable(object_pool_test object_pool_test.cc)
target_link_libraries(object_pool_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(object_pool_test)

add_executable(cache_buffer_test cache_buffer_test.cc)
target_link_libraries(cache_buffer_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(cache_buffer_test)
