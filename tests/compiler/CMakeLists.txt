# =============================================================================
# TIANSHU compiler tests CMake build
# =============================================================================

add_executable(ir_test ir_test.cc)
target_link_libraries(ir_test PRIVATE tianshu tianshu_compiler GTest::gtest_main)
gtest_discover_tests(ir_test)

add_executable(pipeline_test pipeline_test.cc)
target_link_libraries(pipeline_test PRIVATE tianshu tianshu_compiler GTest::gtest_main)
gtest_discover_tests(pipeline_test)

add_executable(pipeline_validation_test pipeline_validation_test.cc)
target_link_libraries(pipeline_validation_test PRIVATE tianshu tianshu_compiler GTest::gtest_main)
gtest_discover_tests(pipeline_validation_test)
