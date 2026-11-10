add_library(ludus_audio_content STATIC src/definitions.cpp src/loader.cpp)
add_library(Ludus::AudioContent ALIAS ludus_audio_content)
set_target_properties(ludus_audio_content PROPERTIES EXPORT_NAME AudioContent)
target_sources(ludus_audio_content PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/audio/content/definitions.h include/ludus/audio/content/loader.h)
target_link_libraries(ludus_audio_content PUBLIC Ludus::Content Ludus::Audio PRIVATE Ludus::FoundationMath)
ludus_apply_project_defaults(ludus_audio_content)
install(TARGETS ludus_audio_content EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
# The file-backed fixture uses POSIX temporary directories.
if(LUDUS_BUILD_TESTS AND NOT WIN32)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_audio_content_tests tests/content_tests.cpp)
    target_include_directories(ludus_audio_content_tests PRIVATE "${PROJECT_SOURCE_DIR}/modules/audio/tests" "${PROJECT_SOURCE_DIR}/examples/audio-content")
    target_link_libraries(ludus_audio_content_tests PRIVATE Ludus::AudioContent Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_audio_content_tests)
    ludus_enable_test_exceptions(ludus_audio_content_tests)
    add_test(NAME ludus_audio_content_tests COMMAND ludus_audio_content_tests)
    set_tests_properties(ludus_audio_content_tests PROPERTIES LABELS "unit;audio;content")
endif()

# These native owners call pthread directly; keep the system dependency explicit.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_package(Threads REQUIRED)
    target_link_libraries(ludus_audio_content PRIVATE Threads::Threads)
endif()

if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" AND NOT EMSCRIPTEN)
    # Exercise the production worker with deterministic stalls. The seam stays
    # in this private backend; the installed AudioContent library has no hooks.
    add_library(ludus_audio_content_worker_test_backend STATIC src/loader.cpp)
    target_compile_definitions(ludus_audio_content_worker_test_backend PRIVATE LUDUS_AUDIO_CONTENT_WORKER_TESTING=1)
    target_link_libraries(ludus_audio_content_worker_test_backend PUBLIC Ludus::AudioContent PRIVATE Threads::Threads)
    ludus_apply_project_defaults(ludus_audio_content_worker_test_backend)
    add_executable(ludus_audio_content_worker_tests tests/worker_tests.cpp)
    target_include_directories(ludus_audio_content_worker_tests PRIVATE src "${PROJECT_SOURCE_DIR}/modules/audio/tests")
    target_link_libraries(ludus_audio_content_worker_tests PRIVATE Ludus::FoundationThreading ludus_audio_content_worker_test_backend Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_audio_content_worker_tests)
    ludus_enable_test_exceptions(ludus_audio_content_worker_tests)
    add_test(NAME ludus_audio_content_worker_tests COMMAND ludus_audio_content_worker_tests)
    set_tests_properties(ludus_audio_content_worker_tests PROPERTIES LABELS "unit;audio;content;concurrency" TIMEOUT 30)
endif()
