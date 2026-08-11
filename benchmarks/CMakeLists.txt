# =============================================================================
# TIANSHU benchmarks CMake build
# =============================================================================

add_executable(version_benchmark version_benchmark.cc)
target_link_libraries(version_benchmark PRIVATE tianshu benchmark::benchmark)
