# Ludus::ProceduralStorage: the persistent generation cache store, a Content file
# adapter behind Ludus::ProceduralCore's GenerationCacheStore (procedural content
# generation slice P1,
# docs/architecture/procedural-content-generation.md#incremental-cooking-and-cache-keys).
# Uses Content's ReadFile and atomic SaveFile, so it builds wherever Content does.
add_library(ludus_procedural_storage STATIC src/file_generation_cache.cpp)
add_library(Ludus::ProceduralStorage ALIAS ludus_procedural_storage)
set_target_properties(ludus_procedural_storage PROPERTIES EXPORT_NAME ProceduralStorage)
target_sources(ludus_procedural_storage PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES
        include/ludus/content/procedural/storage/file_generation_cache.h)
target_link_libraries(ludus_procedural_storage
    PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationStrings Ludus::Content
        Ludus::ProceduralCore)
ludus_apply_project_defaults(ludus_procedural_storage)

install(TARGETS ludus_procedural_storage EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The tests create temporary native directories.
if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" AND NOT EMSCRIPTEN)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_procedural_storage_tests tests/file_generation_cache_tests.cpp)
    target_link_libraries(ludus_procedural_storage_tests PRIVATE Ludus::ProceduralStorage Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_procedural_storage_tests)
    ludus_enable_test_exceptions(ludus_procedural_storage_tests)
    add_test(NAME ludus_procedural_storage_tests COMMAND ludus_procedural_storage_tests)
    set_tests_properties(ludus_procedural_storage_tests PROPERTIES LABELS "unit;content;procedural" TIMEOUT 120)
endif()
