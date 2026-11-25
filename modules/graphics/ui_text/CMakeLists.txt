# U1 UiText: shaped labels on Ui elements as GraphicsText coverage quads (docs/architecture/ui.md).
add_library(ludus_ui_text STATIC src/ui_text.cpp)
add_library(Ludus::UiText ALIAS ludus_ui_text)
set_target_properties(ludus_ui_text PROPERTIES EXPORT_NAME UiText)
target_sources(ludus_ui_text PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
    include/ludus/graphics/ui_text/ui_text.hpp)
target_link_libraries(ludus_ui_text PUBLIC Ludus::UiRender Ludus::GraphicsText PRIVATE Ludus::FoundationMath)
if(NOT EMSCRIPTEN)
    # The label cache shapes through GraphicsText's native-only PrepareText.
    target_sources(ludus_ui_text PRIVATE src/label_cache.cpp)
    target_sources(ludus_ui_text PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
        include/ludus/graphics/ui_text/label_cache.hpp)
endif()
ludus_apply_project_defaults(ludus_ui_text)
install(TARGETS ludus_ui_text EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_ui_text_tests tests/ui_text_tests.cpp)
    if(NOT EMSCRIPTEN)
        target_sources(ludus_ui_text_tests PRIVATE tests/label_cache_tests.cpp)
        target_include_directories(ludus_ui_text_tests PRIVATE "${PROJECT_SOURCE_DIR}/modules/text/tests")
        target_compile_definitions(ludus_ui_text_tests PRIVATE
            LUDUS_TEXT_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/modules/text/tests/fixtures")
    endif()
    target_link_libraries(ludus_ui_text_tests PRIVATE Ludus::UiText Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_ui_text_tests)
    ludus_enable_test_exceptions(ludus_ui_text_tests)
    add_test(NAME ludus_ui_text_tests COMMAND ludus_ui_text_tests)
    set_tests_properties(ludus_ui_text_tests PROPERTIES LABELS "unit;ui;renderer;text")
endif()
