add_executable(node_test node_test.cc)
target_link_libraries(node_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(node_test)

add_executable(typed_message_test typed_message_test.cc)
target_link_libraries(typed_message_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(typed_message_test)
