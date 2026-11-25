# Ludus::ProceduralEnvironment: headless serial reference scatter kernel for
# procedural content generation slice P0
# (docs/architecture/procedural-content-generation.md#p0-room-scoped-scatter)
# and its staged, cached generation coordinator (P1, on Ludus::ProceduralCore).
# Foundation-only, shared by the in-tree and installed-SDK World Demo.
add_library(ludus_procedural_environment STATIC src/scatter.cpp src/scatter_generation.cpp)
add_library(Ludus::ProceduralEnvironment ALIAS ludus_procedural_environment)
set_target_properties(ludus_procedural_environment PROPERTIES EXPORT_NAME ProceduralEnvironment)
target_sources(ludus_procedural_environment PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES
        include/ludus/content/procedural/environment/scatter.h
        include/ludus/content/procedural/environment/scatter_generation.h)
target_link_libraries(ludus_procedural_environment
    PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationMath Ludus::ProceduralCore)
ludus_apply_project_defaults(ludus_procedural_environment)

install(TARGETS ludus_procedural_environment EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_procedural_environment_tests tests/scatter_tests.cpp tests/scatter_generation_tests.cpp)
    # The demand fixture composes scatter with modular assembly over one shared catalog.
    target_link_libraries(ludus_procedural_environment_tests
        PRIVATE Ludus::ProceduralEnvironment Ludus::ProceduralArchitecture Ludus::FoundationThreading
            Catch2::Catch2WithMain)
    # Cache recovery runs over the persistent store create temporary native directories.
    if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" AND NOT EMSCRIPTEN)
        target_sources(ludus_procedural_environment_tests PRIVATE tests/generation_storage_tests.cpp)
        target_link_libraries(ludus_procedural_environment_tests PRIVATE Ludus::ProceduralStorage)
    endif()
    ludus_apply_project_defaults(ludus_procedural_environment_tests)
    ludus_enable_test_exceptions(ludus_procedural_environment_tests)
    add_test(NAME ludus_procedural_environment_tests COMMAND ludus_procedural_environment_tests)
    set_tests_properties(ludus_procedural_environment_tests PROPERTIES LABELS "unit;content;procedural" TIMEOUT 120)
endif()
