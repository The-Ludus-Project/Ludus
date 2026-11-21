add_executable(ludus_audio_content_demo "${PROJECT_SOURCE_DIR}/examples/audio-content/main.cpp"
    "${PROJECT_SOURCE_DIR}/examples/audio-content/resource_adapter.cpp")
target_link_libraries(ludus_audio_content_demo PRIVATE Ludus::AudioContent Ludus::Resources Ludus::FoundationLogging Ludus::FoundationMath)
ludus_apply_project_defaults(ludus_audio_content_demo)
if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_content_fixture "${CMAKE_CURRENT_BINARY_DIR}/content")
    add_test(NAME ludus_audio_content_fixture COMMAND Python3::Interpreter
        "${PROJECT_SOURCE_DIR}/examples/audio-content/generate_fixtures.py" "${_content_fixture}")
    set_tests_properties(ludus_audio_content_fixture PROPERTIES FIXTURES_SETUP audio_content LABELS "audio;content")
    add_test(NAME ludus_audio_content_demo COMMAND ludus_audio_content_demo --offline "${_content_fixture}")
    set_tests_properties(ludus_audio_content_demo PROPERTIES FIXTURES_REQUIRED audio_content LABELS "audio;content")
    add_test(NAME ludus_audio_content_package COMMAND Python3::Interpreter
        "${PROJECT_SOURCE_DIR}/tests/audio_content_package_tests.py" "$<TARGET_FILE:ludus_content_validate>")
    set_tests_properties(ludus_audio_content_package PROPERTIES LABELS "audio;content;packaging")
endif()

if(LUDUS_BUILD_TESTS AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_music_sample "${PROJECT_SOURCE_DIR}/examples/audio-content")
    set(_music_content "${CMAKE_CURRENT_BINARY_DIR}/transition-content")
    add_executable(ludus_audio_transition_timing "${_music_sample}/transition_timing.cpp")
    target_link_libraries(ludus_audio_transition_timing PRIVATE Ludus::AudioContent Ludus::FoundationMath)
    ludus_apply_project_defaults(ludus_audio_transition_timing)
    add_test(NAME ludus_audio_transition_fixture COMMAND Python3::Interpreter
        "${_music_sample}/generate_transition_fixture.py" "${_music_content}")
    set_tests_properties(ludus_audio_transition_fixture PROPERTIES FIXTURES_SETUP music_transition LABELS "audio;content")
    add_test(NAME ludus_audio_transition_timing COMMAND Python3::Interpreter
        "${_music_sample}/verify_transition_capture.py" "$<TARGET_FILE:ludus_audio_transition_timing>"
        "${_music_content}" "${CMAKE_CURRENT_BINARY_DIR}/transition-replacement.wav")
    set_tests_properties(ludus_audio_transition_timing PROPERTIES FIXTURES_REQUIRED music_transition LABELS "audio;content;concurrency")
endif()
