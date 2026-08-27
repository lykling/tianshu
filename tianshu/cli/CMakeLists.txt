# =============================================================================
# TIANSHU CLI tools (ti dispatcher + ti-* tools)
# =============================================================================

add_executable(ti ti_main.cc)
target_link_libraries(ti PRIVATE tianshu)

add_executable(ti-launch ti_launch_main.cc)
target_link_libraries(ti-launch PRIVATE tianshu)

add_executable(ti-monitor ti_monitor_main.cc)
target_link_libraries(ti-monitor PRIVATE tianshu)
