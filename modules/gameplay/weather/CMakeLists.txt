# Ludus::GameplayWeather: deterministic fixed-tick regional weather. Loads LUWTH
# climates, resolves schedules, feathered regions and overrides at World
# positions with integer codes, emits addressed events and saves its state.
# Foundation-only, so native and browser builds produce identical results
# (docs/modules/gameplay/weather.md). In-tree until the installed SDK play
# fixture mounts cooked climates (#1704).
add_library(ludus_gameplay_weather STATIC
    src/climate.cpp
    src/weather_resolve.cpp
    src/weather_simulation.cpp
    src/weather_state_codec.cpp
)
add_library(Ludus::GameplayWeather ALIAS ludus_gameplay_weather)
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

# The golden corpus is shared with tools/web-weather-probe, so the native test and the
# browser run under Node check the same digest.
if(LUDUS_BUILD_TESTS OR LUDUS_BUILD_WEB_PROBES)
    add_library(ludus_weather_golden_corpus OBJECT tests/weather_golden_corpus.cpp)
    target_include_directories(ludus_weather_golden_corpus PUBLIC tests)
    target_link_libraries(ludus_weather_golden_corpus PUBLIC Ludus::ContentWeather Ludus::GameplayWeather)
    ludus_apply_project_defaults(ludus_weather_golden_corpus)
endif()

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_gameplay_weather_tests
        tests/weather_simulation_tests.cpp
        tests/weather_state_tests.cpp
        tests/weather_golden_tests.cpp
    )
    target_link_libraries(ludus_gameplay_weather_tests
        PRIVATE ludus_weather_golden_corpus Ludus::ContentWeather Ludus::GameplayWeather Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_gameplay_weather_tests)
    ludus_enable_test_exceptions(ludus_gameplay_weather_tests)
    add_test(NAME ludus_gameplay_weather_tests COMMAND ludus_gameplay_weather_tests)
    set_tests_properties(ludus_gameplay_weather_tests PROPERTIES LABELS "unit;gameplay;weather" TIMEOUT 120)
endif()
