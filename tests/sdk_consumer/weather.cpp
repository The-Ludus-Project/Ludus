// Installed-SDK evidence for regional weather: the installed Ludus::ContentWeather and Ludus::GameplayWeather
// reproduce the golden corpus digest that the in-tree tests, the browser probe and the Editor share. The Editor's
// World climate cooks to the corpus climate bytes (apps/editor/tests/world_climate_tests.cpp), so the installed
// SDK mounts the same cooked input as Editor Play. See docs/modules/gameplay/weather.md#determinism-evidence.
#include "weather_golden_corpus.h"

bool ExerciseInstalledWeather() noexcept
{
    return ludus::gameplay::weather::testing::RunWeatherGoldenCorpus() == true;
}
