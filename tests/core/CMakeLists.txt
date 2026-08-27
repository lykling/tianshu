add_executable(node_test node_test.cc)
target_link_libraries(node_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(node_test)

add_executable(typed_message_test typed_message_test.cc)
target_link_libraries(typed_message_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(typed_message_test)

add_executable(data_visitor_test data_visitor_test.cc)
target_link_libraries(data_visitor_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(data_visitor_test)

add_executable(component_test component_test.cc)
target_link_libraries(component_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(component_test)

add_executable(launcher_test launcher_test.cc)
target_link_libraries(launcher_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(launcher_test PROPERTIES ENVIRONMENT "TI_BIN_DIR=${CMAKE_BINARY_DIR}/bin")

add_executable(monitor_test monitor_test.cc)
target_link_libraries(monitor_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(monitor_test)
