# =============================================================================
# TIANSHU compiler tests CMake build
# =============================================================================

add_executable(ir_test ir_test.cc)
target_link_libraries(ir_test PRIVATE tianshu tianshu_compiler GTest::gtest_main)
gtest_discover_tests(ir_test)
