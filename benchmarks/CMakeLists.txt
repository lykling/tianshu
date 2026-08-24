# =============================================================================
# TIANSHU benchmarks CMake build
# =============================================================================

add_executable(version_benchmark version_benchmark.cc)
target_link_libraries(version_benchmark PRIVATE tianshu benchmark::benchmark)
target_compile_options(version_benchmark PRIVATE -Wno-c2y-extensions)

add_executable(object_pool_benchmark object_pool_benchmark.cc)
target_link_libraries(object_pool_benchmark PRIVATE tianshu benchmark::benchmark)
target_compile_options(object_pool_benchmark PRIVATE -Wno-c2y-extensions)

add_executable(shm_transport_benchmark shm_transport_benchmark.cc)
target_link_libraries(shm_transport_benchmark PRIVATE tianshu pthread)
