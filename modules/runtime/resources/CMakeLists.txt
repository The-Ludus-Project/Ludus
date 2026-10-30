add_library(ludus_resources STATIC src/manager.cpp src/mesh.cpp src/scene.cpp)
add_library(Ludus::Resources ALIAS ludus_resources)
set_target_properties(ludus_resources PROPERTIES EXPORT_NAME Resources)
target_sources(ludus_resources PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/runtime/resources/manager.h include/ludus/runtime/resources/mesh.h include/ludus/runtime/resources/scene.h)
target_link_libraries(ludus_resources PUBLIC Ludus::Content Ludus::FoundationMath Ludus::FoundationContainers Ludus::SceneFormat
    PRIVATE Ludus::FoundationParsingJson)
ludus_apply_project_defaults(ludus_resources)
if(EMSCRIPTEN)
    # Leave room for callers and bounded DAG recursion in the default 64 KiB stack.
    # Catch accidental whole-snapshot temporaries during compilation.
    target_compile_options(ludus_resources PRIVATE -Wframe-larger-than=16384)
endif()
install(TARGETS ludus_resources EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_resources_tests tests/manager_tests.cpp tests/mesh_cases.hpp tests/mesh_allocation_cases.hpp tests/dag_cases.hpp tests/scene_cases.hpp src/mesh.cpp)
    # Compile the exact production mesh implementation with a private allocation
    # injector. The production archive has no hook; no global allocator is replaced.
    target_compile_definitions(ludus_resources_tests PRIVATE LUDUS_RESOURCE_MESH_TEST_ALLOCATIONS=1)
    target_include_directories(ludus_resources_tests PRIVATE src)
    target_link_libraries(ludus_resources_tests PRIVATE Ludus::Resources Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_resources_tests)
    ludus_enable_test_exceptions(ludus_resources_tests)
    add_test(NAME ludus_resources_tests COMMAND ludus_resources_tests)
    set_tests_properties(ludus_resources_tests PROPERTIES LABELS "unit;resources" TIMEOUT 30)
endif()

if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" AND NOT EMSCRIPTEN)
    add_executable(ludus_resources_audio_tests tests/audio_tests.cpp
        "${PROJECT_SOURCE_DIR}/examples/audio-content/resource_adapter.cpp")
    target_include_directories(ludus_resources_audio_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/examples/audio-content"
        "${PROJECT_SOURCE_DIR}/modules/audio/content/src"
        "${PROJECT_SOURCE_DIR}/modules/audio/tests")
    target_link_libraries(ludus_resources_audio_tests PRIVATE Ludus::Resources
        ludus_audio_content_worker_test_backend Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_resources_audio_tests)
    ludus_enable_test_exceptions(ludus_resources_audio_tests)
    add_test(NAME ludus_resources_audio_tests COMMAND ludus_resources_audio_tests)
    set_tests_properties(ludus_resources_audio_tests PROPERTIES LABELS "unit;resources;audio;concurrency" TIMEOUT 30)
endif()
