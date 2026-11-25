# U1 bridge from Ludus::Ui paint commands to the renderer overlay list (docs/architecture/ui.md).
add_library(ludus_ui_render STATIC src/ui_render.cpp)
add_library(Ludus::UiRender ALIAS ludus_ui_render)
set_target_properties(ludus_ui_render PROPERTIES EXPORT_NAME UiRender)
target_sources(ludus_ui_render PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/graphics/ui_render/ui_render.hpp)
target_link_libraries(ludus_ui_render PUBLIC Ludus::Ui Ludus::GraphicsRenderer PRIVATE Ludus::FoundationMath)
ludus_apply_project_defaults(ludus_ui_render)
install(TARGETS ludus_ui_render EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_ui_render_tests tests/ui_render_tests.cpp)
    target_link_libraries(ludus_ui_render_tests PRIVATE Ludus::UiRender Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_ui_render_tests)
    ludus_enable_test_exceptions(ludus_ui_render_tests)
    add_test(NAME ludus_ui_render_tests COMMAND ludus_ui_render_tests)
    set_tests_properties(ludus_ui_render_tests PROPERTIES LABELS "unit;ui;renderer")
endif()
