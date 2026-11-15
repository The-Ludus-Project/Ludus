#include <ludus/graphics/renderer/hair_card.hpp>

#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
namespace format = ludus::content::scene_format;
namespace math = ludus::foundation::math;
namespace renderer = ludus::graphics::renderer;
using ludus::foundation::float32;
using ludus::foundation::int32;

constexpr float32 DEGREE = math::kPiF / 180.0F; // ludus-style: magic-number degrees in a half turn
constexpr float32 ROUGHNESS = 0.35F;
const math::Vector3 BASE_COLOR{0.35F, 0.22F, 0.12F};
const math::Vector3 NORMAL{0, 0, 1};
const math::Vector3 STRAND{0, 1, 0};

/// A direction in the plane spanned by the normal (+Z) and the strand (+Y), at angle degrees from +Z toward +Y.
math::Vector3 MakeLongitudinalDirection(float32 degrees)
{
    return {0, math::Sin(degrees * DEGREE), math::Cos(degrees * DEGREE)};
}

/// Rotate value about +Z by degrees.
math::Vector3 RotateAboutNormal(math::Vector3 value, float32 degrees)
{
    const float32 cosine = math::Cos(degrees * DEGREE);
    const float32 sine = math::Sin(degrees * DEGREE);
    return {value.X * cosine - value.Y * sine, value.X * sine + value.Y * cosine, value.Z};
}

renderer::HairCardShading MakePrimaryOnly(float32 shiftDegrees)
{
    format::HairCardParameters parameters;
    parameters.PrimaryShiftDegrees = shiftDegrees;
    parameters.PrimaryWeight = 1;
    parameters.SecondaryWeight = 0;
    return renderer::MakeHairCardShading(parameters);
}
} // namespace

TEST_CASE("Hair-card highlight peaks where the shifted strand is perpendicular to the half vector", "[hair-card]")
{
    // In the longitudinal plane the half vector lies halfway between the light at thetaI and the view at thetaO, and
    // the strand tilted by s toward the normal is perpendicular to it when (thetaI + thetaO) / 2 = -s.
    for (const float32 shiftDegrees : {-5.0F, 0.0F, 8.0F})
    {
        const renderer::HairCardShading shading = MakePrimaryOnly(shiftDegrees);
        for (const float32 incidentDegrees : {30.0F, 50.0F})
        {
            const math::Vector3 light = MakeLongitudinalDirection(incidentDegrees);
            const float32 predictedDegrees = -2 * shiftDegrees - incidentDegrees;
            constexpr float32 SWEEP_STEP_DEGREES = 0.05F;
            float32 peakDegrees = 0;
            float32 peakValue = -1;
            constexpr int32 SWEEP_STEPS = 1200; // Thirty degrees on each side of the prediction.
            for (int32 step = -SWEEP_STEPS; step <= SWEEP_STEPS; ++step)
            {
                const float32 viewDegrees = predictedDegrees + static_cast<float32>(step) * SWEEP_STEP_DEGREES;
                const math::Vector3 value = renderer::EvaluateHairCardReflectance(
                    BASE_COLOR,
                    ROUGHNESS,
                    shading,
                    {NORMAL, STRAND, MakeLongitudinalDirection(viewDegrees), light});
                REQUIRE(math::IsFinite(value) == true);
                if (value.X > peakValue)
                {
                    peakValue = value.X;
                    peakDegrees = viewDegrees;
                }
            }
            INFO("shift " << shiftDegrees << " incidence " << incidentDegrees);
            CHECK(peakDegrees == Catch::Approx(predictedDegrees).margin(SWEEP_STEP_DEGREES));
            // At the peak the sine is one: the value is the cosine times the diffuse term plus the normalization.
            const float32 exponent = renderer::ComputeHairCardExponent(ROUGHNESS);
            const float32 cosine = math::Cos(incidentDegrees * DEGREE);
            const float32 expected = cosine * (BASE_COLOR.X / math::kPiF + (exponent + 2) / (2 * math::kPiF));
            CHECK(peakValue == Catch::Approx(expected).epsilon(1e-4));
            // The lobe falls off symmetrically: twenty degrees of view move the half vector by ten, and sin^p(80
            // degrees) is about 0.14 at this roughness.
            constexpr float32 FALLOFF_DEGREES = 20;
            const math::Vector3 below = renderer::EvaluateHairCardReflectance(
                BASE_COLOR,
                ROUGHNESS,
                shading,
                {NORMAL, STRAND, MakeLongitudinalDirection(predictedDegrees - FALLOFF_DEGREES), light});
            const math::Vector3 above = renderer::EvaluateHairCardReflectance(
                BASE_COLOR,
                ROUGHNESS,
                shading,
                {NORMAL, STRAND, MakeLongitudinalDirection(predictedDegrees + FALLOFF_DEGREES), light});
            CHECK(below.X < peakValue * 0.5F);
            CHECK(below.X == Catch::Approx(above.X).epsilon(1e-3));
        }
    }
}

TEST_CASE("Rotating the authored strand direction rotates the hair-card highlight", "[hair-card]")
{
    format::HairCardParameters parameters;
    parameters.StrandDirectionDegrees = 90;
    const math::Vector3 tangent{1, 0, 0};
    const math::Vector3 vertical =
        renderer::ComputeHairCardStrand(NORMAL, math::Vector4{tangent.X, tangent.Y, tangent.Z, 1}, 90 * DEGREE);
    CHECK(vertical.X == Catch::Approx(0).margin(1e-6));
    CHECK(vertical.Y == Catch::Approx(1));
    // A negative handedness mirrors the bitangent.
    CHECK(renderer::ComputeHairCardStrand(NORMAL, math::Vector4{tangent.X, tangent.Y, tangent.Z, -1}, 90 * DEGREE).Y ==
          Catch::Approx(-1));
    // A tangent with a normal component is projected into the card's plane first.
    const math::Vector3 tilted = renderer::ComputeHairCardStrand(NORMAL, math::Vector4{1, 0, 1, 1}, 0);
    CHECK(tilted.X == Catch::Approx(1));
    CHECK(tilted.Z == Catch::Approx(0).margin(1e-6));
    // The half vector lies thirty degrees from the normal, where a thirty degree primary shift puts the vertical
    // strand's highlight; turning the strand about the normal then visibly moves the highlight.
    parameters.PrimaryShiftDegrees = -format::MAX_HAIR_CARD_SHIFT_DEGREES;
    parameters.SecondaryWeight = 0;
    const math::Vector3 light = MakeLongitudinalDirection(60);
    const math::Vector3 view = MakeLongitudinalDirection(0);
    for (const float32 rotationDegrees : {15.0F, 45.0F, 90.0F})
    {
        parameters.StrandDirectionDegrees = 90 + rotationDegrees;
        const renderer::HairCardShading shading = renderer::MakeHairCardShading(parameters);
        parameters.StrandDirectionDegrees = 90;
        const renderer::HairCardShading reference = renderer::MakeHairCardShading(parameters);
        const math::Vector3 rotatedStrand =
            renderer::ComputeHairCardStrand(NORMAL,
                                            math::Vector4{tangent.X, tangent.Y, tangent.Z, 1},
                                            shading.StrandRadians);
        const math::Vector3 strand = renderer::ComputeHairCardStrand(NORMAL,
                                                                     math::Vector4{tangent.X, tangent.Y, tangent.Z, 1},
                                                                     reference.StrandRadians);
        // Rotating the strand and both directions together about the normal preserves the reflectance ...
        const math::Vector3 rotated =
            renderer::EvaluateHairCardReflectance(BASE_COLOR,
                                                  ROUGHNESS,
                                                  shading,
                                                  {NORMAL,
                                                   rotatedStrand,
                                                   RotateAboutNormal(view, rotationDegrees),
                                                   RotateAboutNormal(light, rotationDegrees)});
        const math::Vector3 original =
            renderer::EvaluateHairCardReflectance(BASE_COLOR, ROUGHNESS, reference, {NORMAL, strand, view, light});
        INFO("rotation " << rotationDegrees);
        CHECK(rotated.X == Catch::Approx(original.X).epsilon(1e-4));
        CHECK(rotated.Z == Catch::Approx(original.Z).epsilon(1e-4));
        // ... while rotating the strand alone moves the highlight away from the fixed directions.
        const math::Vector3 strandOnly =
            renderer::EvaluateHairCardReflectance(BASE_COLOR, ROUGHNESS, shading, {NORMAL, rotatedStrand, view, light});
        CHECK(strandOnly.X < original.X);
        if (rotationDegrees >= 45)
        {
            CHECK(strandOnly.X < original.X * 0.5F);
        }
    }
}

TEST_CASE("Hair cards shade both sides and gate lights behind the facing side", "[hair-card]")
{
    const renderer::HairCardShading shading = renderer::MakeHairCardShading({});
    const math::Vector3 light = MakeLongitudinalDirection(35);
    const math::Vector3 view = MakeLongitudinalDirection(-25);
    const math::Vector3 front =
        renderer::EvaluateHairCardReflectance(BASE_COLOR, ROUGHNESS, shading, {NORMAL, STRAND, view, light});
    CHECK(front.X > 0);
    // The back side faces the viewer with the opposite normal and keeps the physical strand: mirroring every
    // direction through the card's plane reproduces the front side's value.
    const math::Vector3 mirroredLight{light.X, light.Y, -light.Z};
    const math::Vector3 mirroredView{view.X, view.Y, -view.Z};
    const math::Vector3 back = renderer::EvaluateHairCardReflectance(BASE_COLOR,
                                                                     ROUGHNESS,
                                                                     shading,
                                                                     {-NORMAL, STRAND, mirroredView, mirroredLight});
    CHECK(back.X == Catch::Approx(front.X).epsilon(1e-5));
    CHECK(back.Y == Catch::Approx(front.Y).epsilon(1e-5));
    // A light on the far side of the card, or a viewer behind the facing normal, contributes nothing.
    CHECK(renderer::EvaluateHairCardReflectance(BASE_COLOR, ROUGHNESS, shading, {NORMAL, STRAND, view, mirroredLight})
              .X == 0);
    CHECK(renderer::EvaluateHairCardReflectance(BASE_COLOR, ROUGHNESS, shading, {NORMAL, STRAND, mirroredView, light})
              .X == 0);
    // Without highlights the card is Lambertian about the facing normal.
    format::HairCardParameters matte;
    matte.PrimaryWeight = 0;
    matte.SecondaryWeight = 0;
    const math::Vector3 diffuse = renderer::EvaluateHairCardReflectance(BASE_COLOR,
                                                                        ROUGHNESS,
                                                                        renderer::MakeHairCardShading(matte),
                                                                        {NORMAL, STRAND, view, light});
    CHECK(diffuse.Z == Catch::Approx(BASE_COLOR.Z / math::kPiF * light.Z));
}

TEST_CASE("Hair-card shading stays finite near tangent-aligned directions", "[hair-card]")
{
    const renderer::HairCardShading shading = renderer::MakeHairCardShading({});
    constexpr float32 GRAZING_DEGREES = 89.999F;
    const math::Vector3 alongStrand = MakeLongitudinalDirection(GRAZING_DEGREES);
    const math::Vector3 againstStrand = MakeLongitudinalDirection(-GRAZING_DEGREES);
    for (const math::Vector3 light : {alongStrand, againstStrand, MakeLongitudinalDirection(30)})
    {
        for (const math::Vector3 view : {alongStrand, againstStrand, MakeLongitudinalDirection(-30)})
        {
            for (const float32 roughness : {0.045F, ROUGHNESS, 1.0F})
            {
                const math::Vector3 value = renderer::EvaluateHairCardReflectance(BASE_COLOR,
                                                                                  roughness,
                                                                                  shading,
                                                                                  {NORMAL, STRAND, view, light});
                CHECK(math::IsFinite(value) == true);
                CHECK(value.X >= 0);
            }
        }
    }
    // Opposite light and view leave no half vector; only the diffuse term remains, and it is gated to zero.
    const math::Vector3 value = renderer::EvaluateHairCardReflectance(BASE_COLOR,
                                                                      ROUGHNESS,
                                                                      shading,
                                                                      {NORMAL, STRAND, againstStrand, alongStrand});
    CHECK(math::IsFinite(value) == true);
}

TEST_CASE("Hair-card exponents and fallback frames are bounded and deterministic", "[hair-card]")
{
    CHECK(renderer::ComputeHairCardExponent(1) == renderer::MIN_HAIR_CARD_EXPONENT);
    CHECK(renderer::ComputeHairCardExponent(0.045F) == renderer::MAX_HAIR_CARD_EXPONENT);
    CHECK(renderer::ComputeHairCardExponent(__builtin_nanf("")) == renderer::MIN_HAIR_CARD_EXPONENT);
    const float32 alpha = ROUGHNESS * ROUGHNESS;
    CHECK(renderer::ComputeHairCardExponent(ROUGHNESS) == Catch::Approx(2 / (alpha * alpha) - 2));
    // A vertical card facing +Z takes +X as its tangent, so ninety degrees runs up the card.
    const math::Vector3 tangent = renderer::ComputeHairCardFallbackTangent(NORMAL);
    CHECK(tangent.X == Catch::Approx(1));
    CHECK(renderer::ComputeHairCardStrand(NORMAL, math::Vector4{0, 0, 0, 1}, 90 * DEGREE).Y == Catch::Approx(1));
    // Horizontal cards use +Z instead of the vanishing horizontal cross product.
    const math::Vector3 floorTangent = renderer::ComputeHairCardFallbackTangent({0, 1, 0});
    CHECK(math::Dot(floorTangent, floorTangent) == Catch::Approx(1));
    CHECK(math::Dot(floorTangent, math::Vector3{0, 1, 0}) == Catch::Approx(0).margin(1e-6));
    const renderer::HairCardShading defaults = renderer::MakeHairCardShading({});
    CHECK(defaults.PrimaryShiftRadians == Catch::Approx(format::DEFAULT_HAIR_CARD_PRIMARY_SHIFT_DEGREES * DEGREE));
    CHECK(defaults.SecondaryWeight == format::DEFAULT_HAIR_CARD_SECONDARY_WEIGHT);
}
