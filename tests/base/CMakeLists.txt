# =============================================================================
# TIANSHU tests/base CMake build
# =============================================================================

add_executable(object_pool_test object_pool_test.cc)
target_link_libraries(object_pool_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(object_pool_test)

add_executable(cache_buffer_test cache_buffer_test.cc)
target_link_libraries(cache_buffer_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(cache_buffer_test)

add_executable(spin_lock_test spin_lock_test.cc)
target_link_libraries(spin_lock_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(spin_lock_test)

add_executable(rw_lock_test rw_lock_test.cc)
target_link_libraries(rw_lock_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(rw_lock_test)

add_executable(sync_test sync_test.cc)
target_link_libraries(sync_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(sync_test)

add_executable(atomic_hash_map_test atomic_hash_map_test.cc)
target_link_libraries(atomic_hash_map_test PRIVATE tianshu GTest::gtest_main)
gtest_discover_tests(atomic_hash_map_test)
