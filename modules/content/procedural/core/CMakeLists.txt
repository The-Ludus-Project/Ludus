# Ludus::ProceduralCore: canonical bytes, content digests, the validated stage
# product cache, bounded stage traces and the demand-driven stage coordinator
# shared by procedural generators (slice P1,
# docs/architecture/procedural-content-generation.md#module-boundaries).
# Depends on Foundation and Content's SHA-256 digest only, so it builds for native
# and browser targets alike.
add_library(ludus_procedural_core STATIC src/generation_bytes.cpp src/generation_cache.cpp src/generation_executor.cpp
    src/generation_trace.cpp)
add_library(Ludus::ProceduralCore ALIAS ludus_procedural_core)
set_target_properties(ludus_procedural_core PROPERTIES EXPORT_NAME ProceduralCore)
target_sources(ludus_procedural_core PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES
        include/ludus/content/procedural/core/generation_bytes.h
        include/ludus/content/procedural/core/generation_cache.h
        include/ludus/content/procedural/core/generation_executor.h
        include/ludus/content/procedural/core/generation_trace.h)
target_link_libraries(ludus_procedural_core
    PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::Content)
ludus_apply_project_defaults(ludus_procedural_core)

install(TARGETS ludus_procedural_core EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_procedural_core_tests tests/generation_core_tests.cpp tests/generation_executor_tests.cpp)
    target_link_libraries(ludus_procedural_core_tests PRIVATE Ludus::ProceduralCore Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_procedural_core_tests)
    ludus_enable_test_exceptions(ludus_procedural_core_tests)
    add_test(NAME ludus_procedural_core_tests COMMAND ludus_procedural_core_tests)
    set_tests_properties(ludus_procedural_core_tests PROPERTIES LABELS "unit;content;procedural" TIMEOUT 120)
endif()
