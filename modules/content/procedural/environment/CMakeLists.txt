# Ludus::ProceduralEnvironment: headless serial reference scatter kernel for
# procedural content generation slice P0
# (docs/architecture/procedural-content-generation.md#p0-room-scoped-scatter).
# Foundation-only, so it builds for native and browser targets. In-tree only
# until an SDK consumer needs it.
add_library(ludus_procedural_environment STATIC src/scatter.cpp)
add_library(Ludus::ProceduralEnvironment ALIAS ludus_procedural_environment)
target_sources(ludus_procedural_environment PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/procedural/environment/scatter.h)
target_link_libraries(ludus_procedural_environment
    PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationMath)
ludus_apply_project_defaults(ludus_procedural_environment)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_procedural_environment_tests tests/scatter_tests.cpp)
    target_link_libraries(ludus_procedural_environment_tests
        PRIVATE Ludus::ProceduralEnvironment Ludus::FoundationThreading Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_procedural_environment_tests)
    ludus_enable_test_exceptions(ludus_procedural_environment_tests)
    add_test(NAME ludus_procedural_environment_tests COMMAND ludus_procedural_environment_tests)
    set_tests_properties(ludus_procedural_environment_tests PROPERTIES LABELS "unit;content;procedural" TIMEOUT 120)
endif()
