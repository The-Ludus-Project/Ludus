// Browser determinism evidence for Ludus::GameplayWeather: the golden corpus must reproduce the digest
// recorded from the native build. See docs/modules/gameplay/weather.md#determinism-evidence.
#include <weather_golden_corpus.h>

int main()
{
    return ludus::gameplay::weather::testing::RunWeatherGoldenCorpus() == true ? 0 : 1;
}
