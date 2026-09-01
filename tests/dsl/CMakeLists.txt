add_executable(dsl_test dsl_test.cc)
target_link_libraries(dsl_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(dsl_test)

add_executable(slice_state_test slice_state_test.cc)
target_link_libraries(slice_state_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(slice_state_test)
