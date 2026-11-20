# Module-owned audio configuration schema, plain options adapters and the first
# Live apply-group owner (docs/architecture/engine-configuration.md#audio-configuration).
add_library(ludus_audio_configuration STATIC src/audio_configuration.cpp)
add_library(Ludus::AudioConfiguration ALIAS ludus_audio_configuration)
set_target_properties(ludus_audio_configuration PROPERTIES EXPORT_NAME AudioConfiguration)
target_sources(ludus_audio_configuration PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/audio/configuration/audio_configuration.hpp)
target_link_libraries(ludus_audio_configuration PUBLIC Ludus::Audio Ludus::FoundationConfig
    PRIVATE Ludus::FoundationContainers)
ludus_apply_project_defaults(ludus_audio_configuration)
install(TARGETS ludus_audio_configuration EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_audio_configuration_tests tests/audio_configuration_tests.cpp)
    target_link_libraries(ludus_audio_configuration_tests PRIVATE Ludus::AudioConfiguration Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_audio_configuration_tests)
    ludus_enable_test_exceptions(ludus_audio_configuration_tests)
    add_test(NAME ludus_audio_configuration_tests COMMAND ludus_audio_configuration_tests)
    set_tests_properties(ludus_audio_configuration_tests PROPERTIES LABELS "unit;audio;configuration" TIMEOUT 60)
endif()
