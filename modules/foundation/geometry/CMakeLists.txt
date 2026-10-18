add_library(ludus_foundation_geometry STATIC)
add_library(Ludus::FoundationGeometry ALIAS ludus_foundation_geometry)

set_target_properties(
    ludus_foundation_geometry
    PROPERTIES
        OUTPUT_NAME ludus_foundation_geometry
        EXPORT_NAME FoundationGeometry
)

target_sources(
    ludus_foundation_geometry
    PRIVATE
        src/status.cpp
        src/arc_length.cpp
        src/path.cpp
        src/conversion.cpp
        src/distance.cpp
        src/track.cpp
        src/span_math.cpp
        src/frame.cpp
        src/query.cpp
        src/sweep.cpp
        src/control_box.cpp
        src/surface.cpp
        src/tessellation.cpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS
            include
        FILES
            include/ludus/foundation/geometry/status.hpp
            include/ludus/foundation/geometry/path.hpp
            include/ludus/foundation/geometry/distance.hpp
            include/ludus/foundation/geometry/track.hpp
            include/ludus/foundation/geometry/frame.hpp
            include/ludus/foundation/geometry/query.hpp
            include/ludus/foundation/geometry/sweep.hpp
            include/ludus/foundation/geometry/surface.hpp
            include/ludus/foundation/geometry/tessellation.hpp
)
target_include_directories(ludus_foundation_geometry PRIVATE src)

# Dependency direction (ADR 0024): Geometry may use Base, Math and Containers.
# It never links Content, RHI, Logging or Threading; consumers log and profile.
target_link_libraries(
    ludus_foundation_geometry
    PUBLIC
        Ludus::FoundationBase
        Ludus::FoundationMath
        Ludus::FoundationContainers
)

# Certified bounds rely on ordinary precise IEEE arithmetic, as in FoundationMath.
target_compile_options(
    ludus_foundation_geometry
    PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>
)

ludus_apply_project_defaults(ludus_foundation_geometry)

install(
    TARGETS ludus_foundation_geometry
    EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)

    add_executable(ludus_foundation_geometry_tests)
    target_sources(
        ludus_foundation_geometry_tests
        PRIVATE
            tests/status_tests.cpp
            tests/path_tests.cpp
            tests/distance_tests.cpp
            tests/track_tests.cpp
            tests/frame_tests.cpp
            tests/query_tests.cpp
            tests/sweep_tests.cpp
            tests/surface_tests.cpp
            tests/tessellation_tests.cpp
    )
    target_link_libraries(
        ludus_foundation_geometry_tests
        PRIVATE
            Ludus::FoundationGeometry
            Catch2::Catch2WithMain
            # Test-only G0 reference corpus (tests/fixtures/curves_surfaces).
            ludus_geometry_reference_fixtures
    )
    target_compile_options(
        ludus_foundation_geometry_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>
    )
    ludus_apply_project_defaults(ludus_foundation_geometry_tests)
    ludus_enable_test_exceptions(ludus_foundation_geometry_tests)
    add_test(NAME ludus_foundation_geometry_tests COMMAND ludus_foundation_geometry_tests)
    set_tests_properties(ludus_foundation_geometry_tests PROPERTIES LABELS "unit;foundation;geometry")

    # Allocation probe: injects every allocation failure during preparation and
    # checks that sampling and queries never allocate. Sanitizers intercept the
    # allocator themselves, so the probe runs only without them.
    if(NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
        add_executable(ludus_geometry_alloc_tests)
        target_sources(ludus_geometry_alloc_tests PRIVATE tests/allocation_tests.cpp)
        target_link_libraries(
            ludus_geometry_alloc_tests
            PRIVATE
                Ludus::FoundationGeometry
                Catch2::Catch2WithMain
        )
        ludus_apply_project_defaults(ludus_geometry_alloc_tests)
        ludus_enable_test_exceptions(ludus_geometry_alloc_tests)
        add_test(NAME ludus_geometry_alloc_tests COMMAND ludus_geometry_alloc_tests)
        set_tests_properties(ludus_geometry_alloc_tests PROPERTIES LABELS "unit;foundation;geometry;alloc")
    endif()
endif()
