# =============================================================================
# TIANSHU library CMake build
# =============================================================================
#
# Phase 0: minimal library exposing only version info.
# Phase 1+: core / dsl / trace / compiler / lineage / sla / runtime subdirs.

# Public headers
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

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
    src/schema_sidecar.cc
    src/lineage.cc
    src/record.cc
    src/record_v2.cc
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

target_link_libraries(tianshu PRIVATE PkgConfig::LZ4 PkgConfig::ZSTD)

install(DIRECTORY include/
    DESTINATION include
)
