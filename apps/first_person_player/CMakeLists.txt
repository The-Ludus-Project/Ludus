# Shared application code for the editor's walk preview and the built-in player.
add_library(ludus_first_person_walking STATIC walking_controller.cpp)
target_include_directories(ludus_first_person_walking PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ludus_first_person_walking PUBLIC Ludus::PhysicsCollision)
ludus_apply_project_defaults(ludus_first_person_walking)

add_library(ludus_playable_snapshot STATIC playable_snapshot.cpp)
target_include_directories(ludus_playable_snapshot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ludus_playable_snapshot
    PUBLIC Ludus::GameplayCamera Ludus::FoundationParsingJson Ludus::Resources
           "$<COMPILE_ONLY:Ludus::GraphicsRenderer>"
    PRIVATE Ludus::SceneFormat Ludus::Content)
ludus_apply_project_defaults(ludus_playable_snapshot)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_first_person_walking_tests tests/walking_controller_tests.cpp)
    target_link_libraries(ludus_first_person_walking_tests PRIVATE ludus_first_person_walking Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_first_person_walking_tests)
    ludus_enable_test_exceptions(ludus_first_person_walking_tests)
    add_test(NAME ludus_first_person_walking_tests COMMAND ludus_first_person_walking_tests)
    set_tests_properties(ludus_first_person_walking_tests PROPERTIES LABELS "unit;collision")
endif()

if(APPLE AND NOT EMSCRIPTEN AND LUDUS_BUILD_SHADER_WORKER)
    add_executable(ludus_first_person_player main.cpp player_scene.cpp player_renderer.cpp)
    target_link_libraries(ludus_first_person_player PRIVATE ludus_first_person_walking ludus_playable_snapshot
        Ludus::GraphicsRenderer Ludus::Platform Ludus::FoundationFilesystem Ludus::FoundationTime
        Ludus::FoundationLogging Ludus::SceneFormat)
    ludus_apply_project_defaults(ludus_first_person_player)
    ludus_compile_renderer_shaders(TARGET ludus_first_person_player SHADERS renderer_flat renderer_composite)
    get_property(player_graph GLOBAL PROPERTY LUDUS_DEFAULT_RENDER_GRAPH_DIR)
    ludus_compile_render_graph(TARGET ludus_first_person_player NAME player_raster
        MANIFEST "${player_graph}/main.lushader.json")
endif()
