# Test-only host filesystem operations (tests/support/test_files.hpp). Fixtures
# call them so <filesystem> is parsed in one translation unit instead of every
# test source including a fixture (docs/development/build-profiling.md,
# "Filesystem in test fixtures"). Link it only from test targets; it is never
# installed. It compiles without exceptions like engine code.
if(LUDUS_BUILD_TESTS)
    add_library(ludus_test_files STATIC tests/support/test_files.cpp)
    target_include_directories(ludus_test_files PUBLIC "${PROJECT_SOURCE_DIR}/tests/support")
    target_link_libraries(ludus_test_files PUBLIC Ludus::FoundationBase Ludus::FoundationStrings)
    ludus_apply_project_defaults(ludus_test_files)
endif()
