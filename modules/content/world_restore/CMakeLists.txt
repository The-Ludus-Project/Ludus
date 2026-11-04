# Stage 4 two-pass World restore (docs/architecture/object-handles-uuid.md).
# In-tree bridge between the version-2 authoring records (Ludus::ContentIdentity)
# and the runtime World boundary (Ludus::GameplayWorld): it creates entities and
# publishes durable bindings. Keeping it in its own library lets content and
# gameplay/world stay free of each other. Tool-side and tests link it; not
# installed until an SDK consumer needs it.
add_library(ludus_content_world_restore STATIC src/world_restore.cpp)
add_library(Ludus::WorldRestore ALIAS ludus_content_world_restore)
target_sources(ludus_content_world_restore PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/world_restore/world_restore.h)
target_link_libraries(ludus_content_world_restore
    PUBLIC Ludus::Content Ludus::FoundationContainers Ludus::FoundationIdentity Ludus::GameplayWorld
    PRIVATE Ludus::ContentIdentity Ludus::FoundationParsingJson)
ludus_apply_project_defaults(ludus_content_world_restore)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_content_world_restore_tests tests/world_restore_tests.cpp)
    target_link_libraries(ludus_content_world_restore_tests PRIVATE
        Ludus::WorldRestore Ludus::GameplayWorld Ludus::FoundationIdentity Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_content_world_restore_tests)
    ludus_enable_test_exceptions(ludus_content_world_restore_tests)
    add_test(NAME ludus_content_world_restore_tests COMMAND ludus_content_world_restore_tests)
    set_tests_properties(ludus_content_world_restore_tests PROPERTIES LABELS "unit;content;restore" TIMEOUT 60)
endif()
