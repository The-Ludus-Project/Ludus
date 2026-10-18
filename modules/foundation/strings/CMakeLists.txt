add_library(ludus_foundation_strings STATIC)
add_library(Ludus::FoundationStrings ALIAS ludus_foundation_strings)
set_target_properties(ludus_foundation_strings PROPERTIES OUTPUT_NAME ludus_foundation_strings EXPORT_NAME FoundationStrings)
target_sources(ludus_foundation_strings
    PRIVATE
        src/string.cpp
        src/shared_string.cpp
        src/utf8.cpp
        src/string_table.cpp
    PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
        include/ludus/foundation/strings/status.hpp
        include/ludus/foundation/strings/static_string.hpp
        include/ludus/foundation/strings/string.hpp
        include/ludus/foundation/strings/shared_string.hpp
        include/ludus/foundation/strings/utf8.hpp
        include/ludus/foundation/strings/string_table.hpp
)
target_link_libraries(ludus_foundation_strings PUBLIC Ludus::FoundationBase Ludus::FoundationMemory Ludus::FoundationHash)
ludus_apply_project_defaults(ludus_foundation_strings)
install(TARGETS ludus_foundation_strings EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    find_package(Threads REQUIRED)
    add_executable(ludus_foundation_strings_tests tests/strings_tests.cpp)
    target_include_directories(ludus_foundation_strings_tests PRIVATE src)
    target_link_libraries(ludus_foundation_strings_tests
        PRIVATE Ludus::FoundationStrings Ludus::FoundationContainers Catch2::Catch2WithMain Threads::Threads)
    ludus_apply_project_defaults(ludus_foundation_strings_tests)
    ludus_enable_test_exceptions(ludus_foundation_strings_tests)
    add_test(NAME ludus_foundation_strings_tests COMMAND ludus_foundation_strings_tests)
    set_tests_properties(ludus_foundation_strings_tests PROPERTIES LABELS "unit;foundation;strings" TIMEOUT 60)
endif()

option(LUDUS_BUILD_STRING_BENCHMARKS "Build opt-in string corpus microbenchmarks" OFF)
if(LUDUS_BUILD_STRING_BENCHMARKS AND NOT EMSCRIPTEN)
    add_executable(ludus_strings_benchmark "${PROJECT_SOURCE_DIR}/tests/strings/benchmark.cpp")
    target_link_libraries(ludus_strings_benchmark PRIVATE Ludus::FoundationStrings)
    ludus_apply_project_defaults(ludus_strings_benchmark)
endif()
