# Ludus::GameplayWeather: deterministic fixed-tick regional weather. Loads LUWTH
# climates, resolves schedules, feathered regions and overrides at World
# positions with integer codes, emits addressed events and saves its state.
# Foundation-only, so native and browser builds produce identical results
# (docs/modules/gameplay/weather.md). Installed in the SDK.
add_library(ludus_gameplay_weather STATIC
    src/climate.cpp
    src/weather_resolve.cpp
    src/weather_simulation.cpp
    src/weather_state_codec.cpp
)
add_library(Ludus::GameplayWeather ALIAS ludus_gameplay_weather)
set_target_properties(ludus_gameplay_weather PROPERTIES EXPORT_NAME GameplayWeather)
target_sources(ludus_gameplay_weather PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES
        include/ludus/gameplay/weather/climate.h
        include/ludus/gameplay/weather/weather_simulation.h
)
target_link_libraries(ludus_gameplay_weather
    PUBLIC Ludus::WeatherFormat Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationHash
        Ludus::FoundationMath)
ludus_apply_project_defaults(ludus_gameplay_weather)
# Position quantization is the only floating-point step on the authoritative path; keep it uncontracted.
target_compile_options(ludus_gameplay_weather PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)
install(TARGETS ludus_gameplay_weather EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The golden corpus lives with the installed SDK consumer, which runs it against the installed
# targets. The native test, tools/web-weather-probe and the Editor share this in-tree copy, so
# every build checks the same digest.
if(LUDUS_BUILD_TESTS OR LUDUS_BUILD_WEB_PROBES)
    add_library(ludus_weather_golden_corpus OBJECT "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/weather_golden_corpus.cpp")
    target_include_directories(ludus_weather_golden_corpus PUBLIC "${PROJECT_SOURCE_DIR}/tests/sdk_consumer")
    target_link_libraries(ludus_weather_golden_corpus PUBLIC Ludus::ContentWeather Ludus::GameplayWeather)
    ludus_apply_project_defaults(ludus_weather_golden_corpus)
endif()

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_gameplay_weather_tests
        tests/weather_simulation_tests.cpp
        tests/weather_state_tests.cpp
        tests/weather_session_tests.cpp
        tests/weather_golden_tests.cpp
    )
    target_link_libraries(ludus_gameplay_weather_tests
        PRIVATE ludus_weather_golden_corpus Ludus::ContentWeather Ludus::GameplayWeather Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_gameplay_weather_tests)
    ludus_enable_test_exceptions(ludus_gameplay_weather_tests)
    add_test(NAME ludus_gameplay_weather_tests COMMAND ludus_gameplay_weather_tests)
    set_tests_properties(ludus_gameplay_weather_tests PROPERTIES LABELS "unit;gameplay;weather" TIMEOUT 120)
endif()
