add_executable(node_test node_test.cc)
target_link_libraries(node_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(node_test)
