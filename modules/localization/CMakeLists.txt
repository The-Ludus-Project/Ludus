add_library(ludus_localization STATIC src/catalog.cpp src/formatter.cpp)
add_library(Ludus::Localization ALIAS ludus_localization)
set_target_properties(ludus_localization PROPERTIES EXPORT_NAME Localization)
target_sources(ludus_localization PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/localization/catalog.hpp include/ludus/localization/formatter.hpp)
target_include_directories(ludus_localization PRIVATE src)
target_link_libraries(ludus_localization PUBLIC Ludus::FoundationBase Ludus::FoundationMemory Ludus::FoundationStrings
    PRIVATE Ludus::FoundationContainers)
# The formatter API is always present; only LUDUS_BUILD_UNICODE builds link the
# pinned ICU backend. Without it PrepareFormatterContext reports BackendUnavailable.
if(LUDUS_BUILD_UNICODE)
    target_sources(ludus_localization PRIVATE src/formatter_icu.cpp)
    target_link_libraries(ludus_localization PRIVATE Ludus::Unicode "$<BUILD_INTERFACE:ludus_unicode_internal>")
else()
    target_sources(ludus_localization PRIVATE src/formatter_unavailable.cpp)
endif()
ludus_apply_project_defaults(ludus_localization)
install(TARGETS ludus_localization EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS OR LUDUS_BUILD_UNICODE OR (EMSCRIPTEN AND LUDUS_BUILD_WEB_PROBES))
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
    add_executable(ludus_localization_tests tests/catalog_tests.cpp tests/release_tests.cpp tests/formatter_tests.cpp
        "${_localization_fixture}")
    target_include_directories(ludus_localization_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(ludus_localization_tests PRIVATE Ludus::Localization Ludus::FoundationContainers Catch2::Catch2WithMain)
    if(LUDUS_BUILD_UNICODE)
        target_compile_definitions(ludus_localization_tests PRIVATE LUDUS_LOCALIZATION_TEST_HAS_ICU=1)
        target_link_libraries(ludus_localization_tests PRIVATE Ludus::Unicode)
    else()
        target_compile_definitions(ludus_localization_tests PRIVATE LUDUS_LOCALIZATION_TEST_HAS_ICU=0)
    endif()
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

# L2 evidence on native and wasm32: corpus, measured cost and ICU fail-stop.
if(LUDUS_BUILD_UNICODE AND (LUDUS_BUILD_TESTS OR EMSCRIPTEN))
    add_executable(ludus_localization_format_probe tests/format_probe.cpp "${_localization_fixture}")
    target_include_directories(ludus_localization_format_probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(ludus_localization_format_probe PRIVATE Ludus::Localization Ludus::Unicode
        ludus_unicode_internal Ludus::FoundationContainers Ludus::FoundationHash Ludus::FoundationLogging
        Ludus::FoundationTime)
    ludus_apply_project_defaults(ludus_localization_format_probe)
    if(EMSCRIPTEN)
        # No DWARF, as for the L0 probe: full binaryen optimization and no postlink warning.
        target_compile_options(ludus_localization_format_probe PRIVATE -g0)
        target_link_options(ludus_localization_format_probe PRIVATE -fno-exceptions -g0 -sENVIRONMENT=web,node
            -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
    endif()
    if(LUDUS_COMPILED_LOG_LEVEL GREATER 2)
        message(STATUS "L2 format probe test needs Info diagnostics (LUDUS_COMPILED_LOG_LEVEL=2); not registered")
    else()
        enable_testing()
        add_test(NAME ludus_localization_format_probe
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_format_probe.py"
                "${CMAKE_CROSSCOMPILING_EMULATOR}" "$<TARGET_FILE:ludus_localization_format_probe>"
                "${CMAKE_CURRENT_BINARY_DIR}/format-evidence.json")
        set_tests_properties(ludus_localization_format_probe PROPERTIES LABELS "localization" TIMEOUT 900)
    endif()
endif()
