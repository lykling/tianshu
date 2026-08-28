# =============================================================================
# TIANSHU library CMake build
# =============================================================================
#
# Phase 0: minimal library exposing only version info.
# Phase 1+: core / dsl / trace / compiler / lineage / sla / runtime subdirs.

# Public headers
add_library(tianshu SHARED
    src/version.cc
    src/transport_backend.cc
    src/intra_backend.cc
    src/shm_segment.cc
    src/shm_backend.cc
    src/data_dispatcher.cc
    src/data_notifier.cc
    src/component.cc
    src/launcher.cc
    src/monitor.cc
    src/field_table.cc
    src/lineage.cc
    src/dsl_runtime.cc
    src/hybrid_transport.cc
    src/node.cc
)

target_include_directories(tianshu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(tianshu PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# -----------------------------------------------------------------------------
# Feature flag target defines (mirror top-level)
# -----------------------------------------------------------------------------
target_compile_definitions(tianshu PUBLIC
    TIANSHU_VERSION="${PROJECT_VERSION}"
    TIANSHU_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    TIANSHU_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    TIANSHU_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# -----------------------------------------------------------------------------
# Install (Phase 2+: proper export)
# -----------------------------------------------------------------------------
install(TARGETS tianshu
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)
