# Ludus::ContentWeather: headless climate cooker. Validates an authored climate,
# quantizes it to Ludus::WeatherFormat codes and writes one canonical LUWTH blob.
# Foundation-only, so it builds for native and browser targets. It never links
# the runtime; its tests load the cooked bytes with Ludus::GameplayWeather.
# In-tree until the installed SDK play fixture mounts cooked climates (#1704).
add_library(ludus_content_weather STATIC src/climate_cook.cpp)
add_library(Ludus::ContentWeather ALIAS ludus_content_weather)
target_sources(ludus_content_weather PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/weather/climate_cook.h)
target_link_libraries(ludus_content_weather
    PUBLIC Ludus::WeatherFormat Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationMath)
ludus_apply_project_defaults(ludus_content_weather)
# Quantization is the cooker's only floating-point step; keep it uncontracted so every host cooks identical bytes.
target_compile_options(ludus_content_weather PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_content_weather_tests tests/climate_cook_tests.cpp)
    target_link_libraries(ludus_content_weather_tests
        PRIVATE Ludus::ContentWeather Ludus::GameplayWeather Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_content_weather_tests)
    ludus_enable_test_exceptions(ludus_content_weather_tests)
    add_test(NAME ludus_content_weather_tests COMMAND ludus_content_weather_tests)
    set_tests_properties(ludus_content_weather_tests PROPERTIES LABELS "unit;content;weather" TIMEOUT 120)
endif()
