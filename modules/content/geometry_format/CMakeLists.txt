# Ludus::GeometryFormat: header-only LUGEO cooked geometry contract shared by the
# GeometryContent cooker, the Editor composer and the GeometryContent loader
# (owned by neither tools nor runtime). Curves and surfaces architecture, G5;
# ADR 0024 boundary table. Depends only on FoundationBase.
add_library(ludus_geometry_format INTERFACE)
add_library(Ludus::GeometryFormat ALIAS ludus_geometry_format)
set_target_properties(ludus_geometry_format PROPERTIES EXPORT_NAME GeometryFormat)
target_sources(ludus_geometry_format INTERFACE FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/geometry_format/geometry_format.h include/ludus/content/geometry_format/limits.h)
target_link_libraries(ludus_geometry_format INTERFACE Ludus::FoundationBase)
install(TARGETS ludus_geometry_format EXPORT LudusTargets
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_geometry_format_tests tests/geometry_format_tests.cpp)
    target_link_libraries(ludus_geometry_format_tests PRIVATE Ludus::GeometryFormat Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_geometry_format_tests)
    ludus_enable_test_exceptions(ludus_geometry_format_tests)
    add_test(NAME ludus_geometry_format_tests COMMAND ludus_geometry_format_tests)
    set_tests_properties(ludus_geometry_format_tests PROPERTIES LABELS "unit;content;geometry")
endif()
