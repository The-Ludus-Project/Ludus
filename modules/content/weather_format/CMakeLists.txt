# Ludus::WeatherFormat: header-only LUWTH cooked climate contract shared by the
# ContentWeather cooker and the GameplayWeather runtime (owned by neither).
# docs/modules/gameplay/weather.md; World environment architecture,
# "Authored climate and resolved weather". Depends only on FoundationBase.
add_library(ludus_weather_format INTERFACE)
add_library(Ludus::WeatherFormat ALIAS ludus_weather_format)
set_target_properties(ludus_weather_format PROPERTIES EXPORT_NAME WeatherFormat)
target_sources(ludus_weather_format INTERFACE FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/weather_format/weather_format.h)
target_link_libraries(ludus_weather_format INTERFACE Ludus::FoundationBase)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_weather_format_tests tests/weather_format_tests.cpp)
    target_link_libraries(ludus_weather_format_tests PRIVATE Ludus::WeatherFormat Ludus::FoundationContainers Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_weather_format_tests)
    ludus_enable_test_exceptions(ludus_weather_format_tests)
    add_test(NAME ludus_weather_format_tests COMMAND ludus_weather_format_tests)
    set_tests_properties(ludus_weather_format_tests PROPERTIES LABELS "unit;content;weather" TIMEOUT 120)
endif()
