# Ludus::GeometryContent (curves and surfaces architecture, G5; ADR 0024 boundary
# table): source codecs, validation, cooking, loading and publication over
# Ludus::FoundationGeometry through the shared Ludus::GeometryFormat cooked layout.
# A cooker never links the renderer; a runtime loader never links a cooker.
add_library(ludus_geometry_content STATIC
    src/geometry_source.cpp src/geometry_cook.cpp src/geometry_load.cpp)
add_library(Ludus::GeometryContent ALIAS ludus_geometry_content)
set_target_properties(ludus_geometry_content PROPERTIES EXPORT_NAME GeometryContent)
target_sources(ludus_geometry_content PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/geometry/content/geometry_content.h)
target_include_directories(ludus_geometry_content PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(ludus_geometry_content
    PUBLIC Ludus::Content Ludus::FoundationContainers Ludus::FoundationGeometry Ludus::GeometryFormat
    PRIVATE Ludus::FoundationMath Ludus::FoundationParsingJson)
# Certified bounds from FoundationGeometry require unfused IEEE arithmetic.
target_compile_options(ludus_geometry_content PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
ludus_apply_project_defaults(ludus_geometry_content)
install(TARGETS ludus_geometry_content EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_geometry_content_tests tests/geometry_content_tests.cpp)
    target_link_libraries(ludus_geometry_content_tests
        PRIVATE Ludus::GeometryContent Ludus::GeometryFormat Ludus::FoundationGeometry Ludus::FoundationMath
                Catch2::Catch2WithMain ludus_geometry_reference_fixtures)
    target_compile_options(ludus_geometry_content_tests PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
    ludus_apply_project_defaults(ludus_geometry_content_tests)
    ludus_enable_test_exceptions(ludus_geometry_content_tests)
    add_test(NAME ludus_geometry_content_tests COMMAND ludus_geometry_content_tests)
    set_tests_properties(ludus_geometry_content_tests PROPERTIES LABELS "unit;content;geometry")

    add_executable(ludus_geometry_content_alloc_tests tests/geometry_content_alloc_tests.cpp)
    target_link_libraries(ludus_geometry_content_alloc_tests
        PRIVATE Ludus::GeometryContent Ludus::GeometryFormat Ludus::FoundationGeometry Ludus::FoundationMath
                Catch2::Catch2WithMain)
    target_compile_options(ludus_geometry_content_alloc_tests PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>)
    ludus_apply_project_defaults(ludus_geometry_content_alloc_tests)
    ludus_enable_test_exceptions(ludus_geometry_content_alloc_tests)
    add_test(NAME ludus_geometry_content_alloc_tests COMMAND ludus_geometry_content_alloc_tests)
    set_tests_properties(ludus_geometry_content_alloc_tests PROPERTIES LABELS "unit;content;geometry")
endif()
