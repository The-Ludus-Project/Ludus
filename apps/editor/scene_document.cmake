# Private portable scene document; deliberately no Qt, Platform, RHI or game runtime.
#
# Included by apps/editor/CMakeLists.txt when the Editor is enabled, and by the
# top-level CMakeLists.txt for native Editor-OFF builds. The second include keeps
# the document's size budgets (static_asserts in internal/scene_document.h) and
# its unit tests in the default pull-request build, which never configures Qt.
add_library(ludus_editor_scene STATIC "${CMAKE_CURRENT_LIST_DIR}/src/scene_document.cpp")
target_include_directories(ludus_editor_scene PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(ludus_editor_scene PUBLIC ludus_world_level PRIVATE Ludus::FoundationMath)
ludus_apply_project_defaults(ludus_editor_scene)

if(LUDUS_BUILD_TESTS AND NOT EMSCRIPTEN)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_editor_scene_tests "${CMAKE_CURRENT_LIST_DIR}/tests/scene_document_tests.cpp")
    target_link_libraries(ludus_editor_scene_tests PRIVATE ludus_editor_scene Ludus::FoundationMath Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_editor_scene_tests)
    ludus_enable_test_exceptions(ludus_editor_scene_tests)
    add_test(NAME ludus_editor_scene_tests COMMAND ludus_editor_scene_tests)
    set_tests_properties(ludus_editor_scene_tests PROPERTIES LABELS "unit;editor;scene")
endif()
