# Ludus::ProceduralArchitecture: modular assembly stages (module catalog and span
# assembly) on Ludus::ProceduralCore's executor, the second consumer of procedural
# content generation slice P1
# (docs/architecture/procedural-content-generation.md#p1-staged-scatter-generation).
# Foundation and ProceduralCore only, so it builds for native and browser targets.
add_library(ludus_procedural_architecture STATIC src/modular_assembly.cpp)
add_library(Ludus::ProceduralArchitecture ALIAS ludus_procedural_architecture)
set_target_properties(ludus_procedural_architecture PROPERTIES EXPORT_NAME ProceduralArchitecture)
target_sources(ludus_procedural_architecture PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES
        include/ludus/content/procedural/architecture/modular_assembly.h)
target_link_libraries(ludus_procedural_architecture
    PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::ProceduralCore)
ludus_apply_project_defaults(ludus_procedural_architecture)

install(TARGETS ludus_procedural_architecture EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_procedural_architecture_tests tests/modular_assembly_tests.cpp)
    target_link_libraries(ludus_procedural_architecture_tests
        PRIVATE Ludus::ProceduralArchitecture Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_procedural_architecture_tests)
    ludus_enable_test_exceptions(ludus_procedural_architecture_tests)
    add_test(NAME ludus_procedural_architecture_tests COMMAND ludus_procedural_architecture_tests)
    set_tests_properties(ludus_procedural_architecture_tests PROPERTIES LABELS "unit;content;procedural" TIMEOUT 120)
endif()
