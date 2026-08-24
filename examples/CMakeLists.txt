add_executable(hello_world hello_world.cc)
target_link_libraries(hello_world PRIVATE tianshu)

add_executable(intra_example intra_example.cc)
target_link_libraries(intra_example PRIVATE tianshu)

add_executable(shm_talker shm_talker.cc)
target_link_libraries(shm_talker PRIVATE tianshu)

add_executable(shm_listener shm_listener.cc)
target_link_libraries(shm_listener PRIVATE tianshu)
