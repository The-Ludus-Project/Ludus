// The golden route must reproduce the recorded digest; tools/web-weather-probe checks the same digest in Wasm.
#include "weather_golden_corpus.h"

#include <catch2/catch_test_macros.hpp>

namespace weather_testing = ludus::gameplay::weather::testing;

TEST_CASE("Every golden route agrees and reproduces the recorded digest", "[weather][golden]")
{
    ludus::foundation::Fingerprint128 digest;
    REQUIRE(weather_testing::TryComputeWeatherGoldenDigest(digest) == true);
    INFO("Low " << digest.Low << " High " << digest.High);
    CHECK(digest == weather_testing::EXPECTED_WEATHER_GOLDEN_DIGEST);
    CHECK(weather_testing::RunWeatherGoldenCorpus() == true);
}
