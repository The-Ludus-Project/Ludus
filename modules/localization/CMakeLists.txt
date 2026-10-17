add_library(ludus_localization STATIC src/catalog.cpp)
add_library(Ludus::Localization ALIAS ludus_localization)
set_target_properties(ludus_localization PROPERTIES EXPORT_NAME Localization)
target_sources(ludus_localization PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/localization/catalog.hpp)
target_link_libraries(ludus_localization PUBLIC Ludus::FoundationBase Ludus::FoundationMemory Ludus::FoundationStrings)
ludus_apply_project_defaults(ludus_localization)
install(TARGETS ludus_localization EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS OR (EMSCRIPTEN AND LUDUS_BUILD_WEB_PROBES))
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(_localization_fixture "${CMAKE_CURRENT_BINARY_DIR}/generated/fixture.hpp")
    # Version 2 fixtures replay recorded pinned-ICU verdicts; see tests/generate_fixture.py.
    file(GLOB _localization_release_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/*.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/release/*.json" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.json")
    add_custom_command(OUTPUT "${_localization_fixture}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_fixture.py" "${_localization_fixture}"
        DEPENDS tests/generate_fixture.py tests/source.json "${PROJECT_SOURCE_DIR}/scripts/python/localization.py"
            "${PROJECT_SOURCE_DIR}/scripts/python/localization_corpus.py"
            "${PROJECT_SOURCE_DIR}/scripts/python/localization_release.py" ${_localization_release_inputs}
        VERBATIM)
    add_custom_target(ludus_localization_fixture DEPENDS "${_localization_fixture}")
endif()
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_localization_tests tests/catalog_tests.cpp tests/release_tests.cpp "${_localization_fixture}")
    target_include_directories(ludus_localization_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(ludus_localization_tests PRIVATE Ludus::Localization Ludus::FoundationContainers Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_localization_tests)
    ludus_enable_test_exceptions(ludus_localization_tests)
    add_test(NAME ludus_localization_tests COMMAND ludus_localization_tests)
    add_test(NAME ludus_localization_cooker
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/scripts/python"
            -p test_localization.py -v)
    add_test(NAME ludus_localization_release
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/scripts/python"
            -p test_localization_release.py -v)
    set_tests_properties(ludus_localization_tests ludus_localization_cooker ludus_localization_release
        PROPERTIES LABELS "unit;localization")
endif()
