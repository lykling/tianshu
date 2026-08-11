# =============================================================================
# TIANSHU tests/sched CMake build
# =============================================================================

add_executable(scheduler_test scheduler_test.cc)
target_link_libraries(scheduler_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(scheduler_test)
