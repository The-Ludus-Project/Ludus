#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/tone_mapping.hpp>

#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace math = ludus::foundation::math;

namespace
{
constexpr float32 ROUND_TRIP_TOLERANCE = 2.0e-5F;
constexpr float32 MONOTONE_STEP = 0.01F;
constexpr uint32 MONOTONE_STEPS = 1000;
constexpr float32 HUGE_RADIANCE = 1.0e30F;

void CheckColour(math::Vector3 actual, math::Vector3 expected, float32 tolerance)
{
    CHECK(actual.X == Catch::Approx(expected.X).margin(tolerance));
    CHECK(actual.Y == Catch::Approx(expected.Y).margin(tolerance));
    CHECK(actual.Z == Catch::Approx(expected.Z).margin(tolerance));
}
} // namespace

TEST_CASE("PBR Neutral matches the Khronos reference for greys", "[renderer][tone-mapping]")
{
    // Below twice the Fresnel term the toe removes x * x / 0.16: 0.02 -> 0.0025.
    CheckColour(renderer::ApplyPbrNeutral({0.02F, 0.02F, 0.02F}), {0.0025F, 0.0025F, 0.0025F}, ROUND_TRIP_TOLERANCE);
    // Between the toe and the compression start only the 0.04 offset applies.
    CheckColour(renderer::ApplyPbrNeutral({0.3F, 0.3F, 0.3F}), {0.26F, 0.26F, 0.26F}, ROUND_TRIP_TOLERANCE);
    CheckColour(renderer::ApplyPbrNeutral({0.8F, 0.8F, 0.8F}), {0.76F, 0.76F, 0.76F}, ROUND_TRIP_TOLERANCE);
    // Peak 0.96 compresses to 1 - 0.0576 / 0.44.
    const float32 expected = 1 - 0.0576F / 0.44F;
    CheckColour(renderer::ApplyPbrNeutral({1, 1, 1}), {expected, expected, expected}, ROUND_TRIP_TOLERANCE);
    CheckColour(renderer::ApplyPbrNeutral({0, 0, 0}), {0, 0, 0}, 0);
}

TEST_CASE("PBR Neutral keeps hue below the compression start and never exceeds one", "[renderer][tone-mapping]")
{
    const math::Vector3 mapped = renderer::ApplyPbrNeutral({0.5F, 0.25F, 0.125F});
    // The offset is per-colour, so channel differences survive exactly.
    CHECK(mapped.X - mapped.Y == Catch::Approx(0.25F).margin(ROUND_TRIP_TOLERANCE));
    CHECK(mapped.Y - mapped.Z == Catch::Approx(0.125F).margin(ROUND_TRIP_TOLERANCE));
    const math::Vector3 bright = renderer::ApplyPbrNeutral({HUGE_RADIANCE, 1, 0});
    CHECK(bright.X <= 1);
    CHECK(bright.X >= bright.Y);
    CHECK(bright.Y >= bright.Z);
    CheckColour(renderer::ApplyPbrNeutral({-1, 0.5F, -1}), renderer::ApplyPbrNeutral({0, 0.5F, 0}), 0);
    CheckColour(renderer::ApplyPbrNeutral({std::numeric_limits<float32>::infinity(), 0, 0}), {1, 1, 1}, 0);
    CheckColour(renderer::ApplyPbrNeutral({std::numeric_limits<float32>::quiet_NaN(), 0, 0}), {1, 1, 1}, 0);
}

TEST_CASE("PBR Neutral is monotone in every channel", "[renderer][tone-mapping]")
{
    math::Vector3 previous = renderer::ApplyPbrNeutral({0, 0.1F, 0.4F});
    for (uint32 step = 1; step <= MONOTONE_STEPS; ++step)
    {
        const float32 lift = static_cast<float32>(step) * MONOTONE_STEP;
        const math::Vector3 current = renderer::ApplyPbrNeutral({lift, 0.1F + lift, 0.4F + lift});
        CHECK(current.X >= previous.X - ROUND_TRIP_TOLERANCE);
        CHECK(current.Y >= previous.Y - ROUND_TRIP_TOLERANCE);
        CHECK(current.Z >= previous.Z - ROUND_TRIP_TOLERANCE);
        previous = current;
    }
}

TEST_CASE("InvertPbrNeutral reproduces the colour the viewport would display", "[renderer][tone-mapping]")
{
    const math::Vector3 wanted[] = {{0, 0, 0},
                                    {0.001F, 0.001F, 0.001F},
                                    {0.02F, 0.05F, 0.01F},
                                    {0.2F, 0.5F, 0.7F},
                                    {0.75F, 0.1F, 0.1F},
                                    {0.9F, 0.4F, 0.1F},
                                    {0.95F, 0.95F, 0.95F},
                                    {0.95F, 0.3F, 0.8F},
                                    {0.5F, 0.5F, 0.5F}};
    for (const math::Vector3& display : wanted)
    {
        const math::Vector3 scene = renderer::InvertPbrNeutral(display);
        CHECK(scene.X >= 0);
        CHECK(scene.Y >= 0);
        CHECK(scene.Z >= 0);
        // Colours with a high peak and a dark channel lose precision in single precision, so allow a little more.
        CheckColour(renderer::ApplyPbrNeutral(scene), display, 1.0e-3F);
    }
}

TEST_CASE("InvertPbrNeutral clamps a saturated near-white display to the nearest reachable colour",
          "[renderer][tone-mapping]")
{
    // Compression desaturates toward white, so a 0.99 peak cannot keep a 0.2 channel; the dark channel rises.
    const math::Vector3 displayed = renderer::ApplyPbrNeutral(renderer::InvertPbrNeutral({0.99F, 0.2F, 0.8F}));
    CHECK(displayed.X == Catch::Approx(0.99F).margin(1.0e-3F));
    CHECK(displayed.Y > 0.2F);
    CHECK(displayed.Z == Catch::Approx(0.8F).margin(1.0e-2F));
}

TEST_CASE("InvertPbrNeutral stays finite and clamps unreachable displays", "[renderer][tone-mapping]")
{
    const math::Vector3 scene = renderer::InvertPbrNeutral({1, 1, 1});
    CHECK(math::IsFinite(scene.X) == true);
    CHECK(math::IsFinite(scene.Y) == true);
    CHECK(math::IsFinite(scene.Z) == true);
    const math::Vector3 displayed = renderer::ApplyPbrNeutral(scene);
    CHECK(displayed.X == Catch::Approx(renderer::PBR_NEUTRAL_INVERSE_LIMIT).margin(1.0e-3F));
    CheckColour(renderer::InvertPbrNeutral({-1, -1, -1}), {0, 0, 0}, 0);
}

TEST_CASE("Reinhard and the curve selector keep the existing behaviour", "[renderer][tone-mapping]")
{
    CheckColour(renderer::ApplyReinhard({1, 3, 0}), {0.5F, 0.75F, 0}, ROUND_TRIP_TOLERANCE);
    CheckColour(renderer::ApplyReinhard({std::numeric_limits<float32>::infinity(), -2, 0}), {1, 0, 0}, 0);
    CheckColour(renderer::ApplyToneCurve(renderer::ToneCurve::Reinhard, {1, 1, 1}),
                {0.5F, 0.5F, 0.5F},
                ROUND_TRIP_TOLERANCE);
    CheckColour(renderer::ApplyToneCurve(renderer::ToneCurve::PbrNeutral, {0.3F, 0.3F, 0.3F}),
                renderer::ApplyPbrNeutral({0.3F, 0.3F, 0.3F}),
                0);
}

TEST_CASE("Physical exposure follows the saturation-based camera model", "[renderer][tone-mapping]")
{
    // 1 / (1.2 * 2^EV100): EV100 0 maps 1.2 nits to one, and each value halves the multiplier.
    CHECK(renderer::ComputeExposureMultiplier(0) == Catch::Approx(1 / 1.2));
    CHECK(renderer::ComputeExposureMultiplier(1) == Catch::Approx(1 / 2.4));
    constexpr float32 SUNNY_EXPOSURE_VALUE = 15;
    CHECK(renderer::ComputeExposureMultiplier(SUNNY_EXPOSURE_VALUE) == Catch::Approx(1 / (1.2 * 32768)));
    // -log2(1.2) reproduces the graph's default multiplier of one.
    CHECK(renderer::ComputeExposureMultiplier(static_cast<float32>(-std::log2(1.2))) == Catch::Approx(1));
    CHECK(renderer::ComputeExposureMultiplier(renderer::MIN_EXPOSURE_VALUE_100) > 0);
    CHECK(renderer::ComputeExposureMultiplier(renderer::MAX_EXPOSURE_VALUE_100) > 0);
    CHECK(renderer::ComputeExposureMultiplier(renderer::MIN_EXPOSURE_VALUE_100 - 1) == 0);
    CHECK(renderer::ComputeExposureMultiplier(renderer::MAX_EXPOSURE_VALUE_100 + 1) == 0);
    CHECK(renderer::ComputeExposureMultiplier(std::numeric_limits<float32>::quiet_NaN()) == 0);
    CHECK(renderer::ComputeExposureMultiplier(std::numeric_limits<float32>::infinity()) == 0);
}

TEST_CASE("Incident metering reads daylight, moonlight and a natural night", "[renderer][tone-mapping]")
{
    // EV = log2(E * 100 / 250) after adding the 0.002 lx night sky.
    constexpr float64 SUNLIGHT_LUX = 100'000;
    CHECK(renderer::ComputeIncidentExposureValue100(SUNLIGHT_LUX) ==
          Catch::Approx(std::log2((SUNLIGHT_LUX + 0.002) * 0.4)));
    // Sunny 16 at ISO 100 is EV 14.6; a flat meter under clear sun reads within a stop of it.
    CHECK(renderer::ComputeIncidentExposureValue100(SUNLIGHT_LUX) > 14);
    CHECK(renderer::ComputeIncidentExposureValue100(SUNLIGHT_LUX) < 16);
    constexpr float64 FULL_MOON_LUX = 0.25;
    CHECK(renderer::ComputeIncidentExposureValue100(FULL_MOON_LUX) == Catch::Approx(std::log2(0.252 * 0.4)));
    // No light at all reads the natural night sky, not an unbounded exposure.
    CHECK(renderer::ComputeIncidentExposureValue100(0) == Catch::Approx(std::log2(0.002 * 0.4)));
    CHECK(renderer::ComputeIncidentExposureValue100(-1) == renderer::ComputeIncidentExposureValue100(0));
    CHECK(renderer::ComputeIncidentExposureValue100(std::numeric_limits<float64>::quiet_NaN()) ==
          renderer::ComputeIncidentExposureValue100(0));
    CHECK(renderer::ComputeIncidentExposureValue100(1e30) == renderer::MAX_EXPOSURE_VALUE_100);
    // Metered values are always admissible physical exposures.
    CHECK(renderer::ComputeExposureMultiplier(renderer::ComputeIncidentExposureValue100(0)) > 0);
}
