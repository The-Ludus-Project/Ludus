# Ludus::SceneFormat: header-only LUSCN cooked scene contract shared by the
# cooker, the Editor and the runtime decoder (owned by neither tools nor runtime).
add_library(ludus_scene_format INTERFACE)
add_library(Ludus::SceneFormat ALIAS ludus_scene_format)
set_target_properties(ludus_scene_format PROPERTIES EXPORT_NAME SceneFormat)
target_sources(ludus_scene_format INTERFACE FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/scene_format/material.h include/ludus/content/scene_format/luscn.h include/ludus/content/scene_format/limits.h)
target_link_libraries(ludus_scene_format INTERFACE Ludus::FoundationBase Ludus::FoundationContainers)
install(TARGETS ludus_scene_format EXPORT LudusTargets
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_scene_format_tests tests/scene_format_tests.cpp)
    target_link_libraries(ludus_scene_format_tests PRIVATE Ludus::SceneFormat Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_scene_format_tests)
    ludus_enable_test_exceptions(ludus_scene_format_tests)
    add_test(NAME ludus_scene_format_tests COMMAND ludus_scene_format_tests)
    set_tests_properties(ludus_scene_format_tests PROPERTIES LABELS "unit;content")
endif()
