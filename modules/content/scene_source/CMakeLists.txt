# Ludus::SceneSource: the hand-readable Ludus scene source format (.luscene.json)
# and its validating reader (docs/architecture/scene-source-format.md). Owned by
# neither tools nor runtime: SceneCook's ludus-scene-v1 importer cooks it, and
# renderer tests read the engine's test scenes through it.
add_library(ludus_scene_source STATIC src/scene_source.cpp src/material_program_compiler.cpp)
add_library(Ludus::SceneSource ALIAS ludus_scene_source)
set_target_properties(ludus_scene_source PROPERTIES EXPORT_NAME SceneSource)
target_sources(ludus_scene_source PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/scene_source/scene_source.h)
target_link_libraries(ludus_scene_source
    PUBLIC Ludus::Content Ludus::FoundationContainers Ludus::FoundationMath Ludus::FoundationStrings
        Ludus::SceneFormat
    PRIVATE Ludus::FoundationParsingJson)
ludus_apply_project_defaults(ludus_scene_source)
install(TARGETS ludus_scene_source EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_scene_source_tests tests/scene_source_tests.cpp)
    target_link_libraries(ludus_scene_source_tests PRIVATE Ludus::SceneSource Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_scene_source_tests)
    ludus_enable_test_exceptions(ludus_scene_source_tests)
    add_test(NAME ludus_scene_source_tests COMMAND ludus_scene_source_tests)
    set_tests_properties(ludus_scene_source_tests PROPERTIES LABELS "unit;content" TIMEOUT 60)
endif()
