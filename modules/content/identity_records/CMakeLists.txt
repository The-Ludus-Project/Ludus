# Ludus::ContentIdentity: versioned UUID authoring records and the one-time
# migration manifest that maps version-1 World IDs to committed UUIDs
# (docs/architecture/object-handles-uuid.md, stage 3). Tool-side and in-tree
# only: ludus-cook and tests link it; runtime loaders read version-2 records
# through their own game-owned codecs. Not installed until an SDK consumer needs it.
add_library(ludus_content_identity STATIC src/migration_manifest.cpp src/record_json.cpp src/world_records.cpp)
add_library(Ludus::ContentIdentity ALIAS ludus_content_identity)
target_sources(ludus_content_identity PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/identity_records/migration_manifest.h
          include/ludus/content/identity_records/world_records.h)
target_link_libraries(ludus_content_identity
    PUBLIC Ludus::Content Ludus::FoundationContainers Ludus::FoundationIdentity
    PRIVATE Ludus::FoundationParsingJson)
ludus_apply_project_defaults(ludus_content_identity)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_content_identity_tests
        tests/migration_manifest_tests.cpp tests/world_records_tests.cpp tests/duplicate_tests.cpp)
    if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" AND NOT EMSCRIPTEN)
        target_sources(ludus_content_identity_tests PRIVATE tests/publish_tests.cpp)
    endif()
    target_link_libraries(ludus_content_identity_tests PRIVATE Ludus::ContentIdentity Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_content_identity_tests)
    ludus_enable_test_exceptions(ludus_content_identity_tests)
    add_test(NAME ludus_content_identity_tests COMMAND ludus_content_identity_tests)
    set_tests_properties(ludus_content_identity_tests PROPERTIES LABELS "unit;content;identity" TIMEOUT 60)
endif()
