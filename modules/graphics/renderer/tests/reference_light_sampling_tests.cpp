#include "../src/internal/reference_data.h"
#include "../src/internal/reference_light_sampling.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstring>

// Unit tests of RT1's resampled estimators, ResampledProjectedSolidAngle and ResampledFormFactor
// (reference_light_sampling.h): the MIS weights partition unity, the surrogate density's Z-hat = 0 rule, the empty
// LTC-space clip, emitters behind the receiver, invalid inputs, the bit-identical light density at both MIS sites,
// and the approximate target's support. Statistical agreement with UniformArea is checked on
// whole captures in reference_compute_tests.cpp. The estimator is
// docs/architecture/many-area-light-sampling-reference-review.md.
using namespace ludus::foundation;
namespace internal = ludus::graphics::renderer::internal;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace
{
const math::RandomKey TEST_KEY{0x52495331ULL};
constexpr uint32 DRAW_COUNT = 4096;
// Weights partition unity up to a few float32 roundings of the two divisions and the blend.
constexpr float32 PARTITION_TOLERANCE = 1e-6F;
constexpr usize EMITTER_CAPACITY = 2;

float32 Draw(uint32 index, uint16 dimension) noexcept
{
    return math::SampleFloat01(TEST_KEY, {index, 0, dimension});
}
// Zero for a quarter of the draws, otherwise log-uniform over 2^-12 to 2^12.
float32 DrawNonnegative(uint32 index, uint16 dimension) noexcept
{
    constexpr float32 ZERO_SHARE = 0.25F;
    constexpr float32 LOG2_RANGE = 24;
    return Draw(index, dimension) < ZERO_SHARE
               ? 0.0F
               : static_cast<float32>(
                     math::Pow(2.0, float64((Draw(index, static_cast<uint16>(dimension + 1)) - 0.5F) * LOG2_RANGE)));
}
// Symmetric in its arguments.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool HasSameBits(float32 a, float32 b) noexcept
{
    uint32 left = 0;
    uint32 right = 0;
    std::memcpy(&left, &a, sizeof(left));
    std::memcpy(&right, &b, sizeof(right));
    return left == right;
}
/// A receiver at the origin facing +z whose specular LTC is replaced by `inverseTransform`.
internal::ReferenceReceiverFrame MakeTestFrame(const math::Matrix3& inverseTransform)
{
    rr::ReferenceBsdf bsdf;
    bsdf.BaseColor = {0.5F, 0.5F, 0.5F};
    internal::ReferenceReceiverFrame frame = internal::MakeReferenceReceiverFrame({}, {0, 0, 1}, {0, 0, 1}, bsdf);
    frame.HasSpecular =
        math::TryMakeLinearlyTransformedCosine(inverseTransform, frame.Specular) == math::MathStatus::Success;
    return frame;
}

/// One emissive triangle per instance, every one an emitter in the power CDF.
struct EmitterScene final
{
    StaticArray<internal::ReferenceSurfaceData, EMITTER_CAPACITY> Surfaces;
    internal::ReferenceJob Job;
};
struct EmitterInput final
{
    StaticArray<math::Vector3, 3> PositionsWorld;
    math::Vector3 Emission{};
    bool IsTwoSided = true;
};
void BuildEmitterScene(const EmitterInput* inputs,
                       usize count,
                       EmitterScene& outScene,
                       rr::ReferenceLightSampling mode = rr::ReferenceLightSampling::ResampledProjectedSolidAngle)
{
    REQUIRE(count <= EMITTER_CAPACITY);
    internal::ReferenceJob& job = outScene.Job;
    job.IsCpuOnly = true;
    job.Description.LightSampling = mode;
    job.Key = TEST_KEY;
    REQUIRE(job.Scene.Instances.TryResize(count) == true);
    REQUIRE(job.Scene.Triangles.TryResize(count) == true);
    job.Scene.InstanceCount = count;
    for (usize i = 0; i < count; ++i)
    {
        outScene.Surfaces[i].Emission = inputs[i].Emission;
        outScene.Surfaces[i].IsEmissionTwoSided = inputs[i].IsTwoSided;
        job.Scene.Instances[i].Surface = &outScene.Surfaces[i];
        job.Scene.Instances[i].FirstTriangle = i;
        job.Scene.Instances[i].TriangleCount = 1;
        internal::ReferenceTriangle& triangle = job.Scene.Triangles[i];
        for (usize corner = 0; corner < 3; ++corner)
        {
            triangle.Positions[corner] = inputs[i].PositionsWorld[corner];
        }
        const math::Vector3 cross =
            math::Cross(triangle.Positions[1] - triangle.Positions[0], triangle.Positions[2] - triangle.Positions[0]);
        triangle.Area = 0.5F * math::Length(cross);
        triangle.GeometricNormal = internal::ReferenceNormalize(cross);
        triangle.Instance = static_cast<uint32>(i);
        REQUIRE(job.Scene.Emitters.TryAdd(static_cast<uint32>(i)) == true);
    }
    REQUIRE(internal::BuildReferenceEmitterSelection(job.Scene) == rhi::RasterStatus::Ready);
}
// A unit-area-ish triangle centred at `center` whose normal is +normalSign z.
EmitterInput MakeHorizontalEmitter(math::Vector3 center, float32 normalSign, math::Vector3 emission, bool isTwoSided)
{
    EmitterInput input;
    input.PositionsWorld[0] = center + math::Vector3{-0.5F, -0.5F, 0};
    input.PositionsWorld[1] = center + math::Vector3{normalSign > 0 ? 0.5F : -0.5F, normalSign > 0 ? -0.5F : 0.5F, 0};
    input.PositionsWorld[2] = center + math::Vector3{normalSign > 0 ? -0.5F : 0.5F, normalSign > 0 ? 0.5F : -0.5F, 0};
    input.Emission = emission;
    input.IsTwoSided = isTwoSided;
    return input;
}
} // namespace

TEST_CASE("Resampled light technique weights partition unity", "[renderer][reference][resampled-light]")
{
    for (uint32 i = 0; i < DRAW_COUNT; ++i)
    {
        CAPTURE(i);
        const math::Vector2 coefficients{DrawNonnegative(i, 0), DrawNonnegative(i, 2)};
        const math::Vector2 densities{DrawNonnegative(i, 4), DrawNonnegative(i, 6)};
        const float32 visibility = Draw(i, 8);
        const math::Vector2 weights =
            internal::ComputeReferenceLightTechniqueWeights(coefficients, densities, visibility);
        CHECK(weights.X >= 0);
        CHECK(weights.Y >= 0);
        if (densities.X + densities.Y > 0)
        {
            CHECK(math::Abs(weights.X + weights.Y - 1) <= PARTITION_TOLERANCE);
        }
        else
        {
            CHECK((weights.X == 0 && weights.Y == 0));
        }
        // A technique that cannot sample the direction never takes weight.
        CHECK((densities.X > 0 || weights.X == 0));
        CHECK((densities.Y > 0 || weights.Y == 0));

        // The light group and BSDF sampling split the remaining weight by the power heuristic.
        const float32 lightDensity = DrawNonnegative(i, 9);
        const float32 bsdfDensity = DrawNonnegative(i, 11);
        const float32 lightWeight = internal::ReferencePowerWeight(lightDensity, bsdfDensity);
        const float32 bsdfWeight = internal::ReferencePowerWeight(bsdfDensity, lightDensity);
        if (lightDensity > 0 || bsdfDensity > 0)
        {
            CHECK(math::Abs(lightWeight + bsdfWeight - 1) <= PARTITION_TOLERANCE);
        }
    }
    // Every c_j p_j zero falls back to the balance heuristic.
    const math::Vector2 balance = internal::ComputeReferenceLightTechniqueWeights({0, 0}, {1, 3}, 1);
    CHECK(balance.X == 0.25F);
    CHECK(balance.Y == 0.75F);
}

TEST_CASE("The surrogate light density applies the inclusion bound when Z-hat is zero",
          "[renderer][reference][resampled-light]")
{
    constexpr uint32 M = LUDUS_REFERENCE_RIS_CANDIDATE_COUNT;
    // Z-hat = 0 with a positive target: p-hat / Z-hat is infinite, so min(1, M p_src) applies.
    CHECK(internal::EvaluateReferenceSurrogateProbability(0.5F, 2, 0.01F, 0, M) == 0.5F * (M * 0.01F));
    CHECK(internal::EvaluateReferenceSurrogateProbability(0.5F, 2, 0.5F, 0, M) == 0.5F);
    // A tiny Z-hat never overflows the ratio.
    CHECK(internal::EvaluateReferenceSurrogateProbability(1, 1, 0.25F, 1e-38F, M) == 1);
    // Otherwise P-tilde = P_E p-hat / Z-hat below the bound.
    CHECK(internal::EvaluateReferenceSurrogateProbability(0.5F, 1, 0.5F, 4, M) == 0.5F * 0.25F);
    // A strategy that cannot propose the triangle has zero density.
    CHECK(internal::EvaluateReferenceSurrogateProbability(0.5F, 0, 0.5F, 0, M) == 0);
    CHECK(internal::EvaluateReferenceSurrogateProbability(0.5F, 1, 0, 1, M) == 0);
    CHECK(internal::EvaluateReferenceSurrogateProbability(0, 1, 0.5F, 1, M) == 0);
}

TEST_CASE("An empty LTC-space clip disables the specular technique", "[renderer][reference][resampled-light]")
{
    // M^-1 = [[1, 0, 0], [0, 1, 0], [1, 0, 0.2]]: the LTC is zero where x + 0.2 z <= 0, which holds for the whole
    // triangle although it lies above the shading horizon.
    const math::Matrix3 inverseTransform{{
        math::Vector3{1, 0, 1},
        math::Vector3{0, 1, 0},
        math::Vector3{0, 0, 0.2F},
    }};
    const internal::ReferenceReceiverFrame frame = MakeTestFrame(inverseTransform);
    REQUIRE(frame.HasSpecular == true);
    internal::ReferenceTriangleLight light;
    REQUIRE(internal::TryPrepareReferenceTriangleLight(frame,
                                                       {-2, -0.2F, 0.15F},
                                                       {-2, 0.2F, 0.15F},
                                                       {-2, 0, 0.35F},
                                                       light) == true);
    CHECK(light.DiffuseIntegral > 0);
    CHECK(light.SpecularIntegral == 0);
    math::Vector3 directionTangent{};
    REQUIRE(math::TrySampleProjectedSolidAnglePolygon(light.Diffuse, {0.3F, 0.6F}, directionTangent) ==
            math::MathStatus::Success);
    const math::Vector2 densities = internal::EvaluateReferenceLightTechniqueDensities(frame, light, directionTangent);
    CHECK(densities.X > 0);
    CHECK(densities.Y == 0);
    const math::Vector2 weights =
        internal::ComputeReferenceLightTechniqueWeights({light.DiffuseIntegral, 0},
                                                        densities,
                                                        internal::REFERENCE_LIGHT_TECHNIQUE_VISIBILITY);
    CHECK(weights.X == 1);
    CHECK(weights.Y == 0);
}

TEST_CASE("Resampled light geometry rejects invalid inputs without writing outputs",
          "[renderer][reference][resampled-light]")
{
    const internal::ReferenceReceiverFrame frame = MakeTestFrame(math::Matrix3::Identity());
    internal::ReferenceTriangleLight light;
    light.DiffuseIntegral = 7;
    CHECK(internal::TryPrepareReferenceTriangleLight(frame, {__builtin_nanf(""), 0, 1}, {1, 0, 1}, {0, 1, 1}, light) ==
          false);
    CHECK(light.DiffuseIntegral == 7);

    float32 distanceWorld = 3;
    math::Vector2 barycentric{4, 5};
    const math::Vector3 corner0{-1, -1, 2};
    const math::Vector3 corner1{1, -1, 2};
    const math::Vector3 corner2{-1, 1, 2};
    // Parallel to the plane, behind the origin and outside the triangle.
    CHECK(internal::TryIntersectReferenceTrianglePlane({},
                                                       {1, 0, 0},
                                                       corner0,
                                                       corner1,
                                                       corner2,
                                                       distanceWorld,
                                                       barycentric) == false);
    CHECK(internal::TryIntersectReferenceTrianglePlane({},
                                                       {0, 0, -1},
                                                       corner0,
                                                       corner1,
                                                       corner2,
                                                       distanceWorld,
                                                       barycentric) == false);
    CHECK(internal::TryIntersectReferenceTrianglePlane({},
                                                       internal::ReferenceNormalize({1, 1, 1}),
                                                       corner0,
                                                       corner1,
                                                       corner2,
                                                       distanceWorld,
                                                       barycentric) == false);
    CHECK(distanceWorld == 3);
    CHECK(barycentric.X == 4);
    REQUIRE(internal::TryIntersectReferenceTrianglePlane({},
                                                         internal::ReferenceNormalize({-0.5F, -0.5F, 2}),
                                                         corner0,
                                                         corner1,
                                                         corner2,
                                                         distanceWorld,
                                                         barycentric) == true);
    CHECK(math::Abs(barycentric.X - 0.25F) < 1e-6F);
    CHECK(math::Abs(barycentric.Y - 0.25F) < 1e-6F);
}

TEST_CASE("Resampled light sampling ignores emitters behind the receiver", "[renderer][reference][resampled-light]")
{
    // Both one-sided emitters face away from a receiver below them.
    const EmitterInput inputs[]{
        MakeHorizontalEmitter({0, 0, 2}, 1, {4, 4, 4}, false),
        MakeHorizontalEmitter({1, 0, 3}, 1, {9, 9, 9}, false),
    };
    const rr::ReferenceLightSampling mode = GENERATE(rr::ReferenceLightSampling::ResampledProjectedSolidAngle,
                                                     rr::ReferenceLightSampling::ResampledFormFactor);
    CAPTURE(static_cast<uint32>(mode));
    EmitterScene scene;
    BuildEmitterScene(inputs, 2, scene, mode);
    rr::ReferenceBsdf bsdf;
    const internal::ReferenceReceiverFrame frame =
        internal::MakeReferenceReceiverFrame({}, {0, 0, 1}, internal::ReferenceNormalize({0.3F, 0, 1}), bsdf);
    for (uint32 sample = 0; sample < 64; ++sample)
    {
        internal::ReferenceTargetTotal targetTotal;
        internal::ReferenceResampledLight light;
        REQUIRE(internal::SampleReferenceResampledLight(scene.Job, 0, sample, 0, frame, 1, targetTotal, light) ==
                rhi::RasterStatus::Ready);
        CHECK(light.HasSelection == false);
        float32 density = -1;
        REQUIRE(internal::EvaluateReferenceResampledLightDensity(scene.Job,
                                                                 0,
                                                                 sample,
                                                                 0,
                                                                 frame,
                                                                 1,
                                                                 0,
                                                                 {0, 0, 1},
                                                                 targetTotal,
                                                                 density) == rhi::RasterStatus::Ready);
        CHECK(density == 0);
    }
}

TEST_CASE("Resampled light densities agree bit for bit at both MIS sites", "[renderer][reference][resampled-light]")
{
    // A bright one-sided emitter facing away carries almost all power, so many vertices see Z-hat = 0 while the dim
    // emitter above them still has a positive target.
    const EmitterInput inputs[]{
        MakeHorizontalEmitter({0.2F, 0.1F, 1.5F}, -1, {50, 50, 50}, true),
        MakeHorizontalEmitter({0, 0, 3}, 1, {5000, 5000, 5000}, false),
    };
    const rr::ReferenceLightSampling mode = GENERATE(rr::ReferenceLightSampling::ResampledProjectedSolidAngle,
                                                     rr::ReferenceLightSampling::ResampledFormFactor);
    CAPTURE(static_cast<uint32>(mode));
    EmitterScene scene;
    BuildEmitterScene(inputs, 2, scene, mode);
    rr::ReferenceBsdf bsdf;
    bsdf.BaseColor = {0.8F, 0.6F, 0.4F};
    bsdf.Roughness = 0.3F;
    const internal::ReferenceReceiverFrame frame =
        internal::MakeReferenceReceiverFrame({}, {0, 0, 1}, internal::ReferenceNormalize({0.4F, 0.2F, 1}), bsdf);
    constexpr float32 EMISSIVE_PROBABILITY = 0.5F;
    const float32 dimPmf = internal::EvaluateReferenceEmissiveTriangleSelectionPmf(scene.Job.Scene, 0);
    REQUIRE(dimPmf > 0);
    uint32 zeroTotals = 0;
    uint32 lightSamples = 0;
    internal::ReferenceTriangleLight dimLight;
    REQUIRE(internal::TryPrepareReferenceTriangleLight(frame,
                                                       inputs[0].PositionsWorld[0],
                                                       inputs[0].PositionsWorld[1],
                                                       inputs[0].PositionsWorld[2],
                                                       dimLight) == true);
    for (uint32 sample = 0; sample < 1024; ++sample)
    {
        CAPTURE(sample);
        internal::ReferenceTargetTotal targetTotal;
        internal::ReferenceResampledLight light;
        REQUIRE(internal::SampleReferenceResampledLight(scene.Job,
                                                        3,
                                                        sample,
                                                        1,
                                                        frame,
                                                        EMISSIVE_PROBABILITY,
                                                        targetTotal,
                                                        light) == rhi::RasterStatus::Ready);
        // Only the dim emitter has a positive target, so it is selected whenever any candidate drew it.
        if (light.HasSelection == false)
        {
            continue;
        }
        CHECK(light.TriangleIndex == 0);
        CHECK(light.ContributionScale > 0);
        REQUIRE(targetTotal.IsReady == true);
        zeroTotals += targetTotal.Value == 0 ? 1U : 0U;
        for (const internal::ReferenceResampledLightSample& lightSample : light.Samples)
        {
            if (lightSample.HasSample == false)
            {
                continue;
            }
            ++lightSamples;
            CHECK(math::IsFinite(lightSample.LightDensity) == true);
            CHECK(lightSample.LightDensity > 0);
            CHECK((lightSample.TechniqueWeight >= 0 && lightSample.TechniqueWeight <= 1));
            // The BSDF-hit site recomputes Z-hat from the same slots and stored receiver.
            internal::ReferenceTargetTotal freshTotal;
            float32 density = 0;
            REQUIRE(internal::EvaluateReferenceResampledLightDensity(scene.Job,
                                                                     3,
                                                                     sample,
                                                                     1,
                                                                     frame,
                                                                     EMISSIVE_PROBABILITY,
                                                                     0,
                                                                     lightSample.DirectionWorld,
                                                                     freshTotal,
                                                                     density) == rhi::RasterStatus::Ready);
            CHECK(HasSameBits(freshTotal.Value, targetTotal.Value) == true);
            CHECK(HasSameBits(density, lightSample.LightDensity) == true);
            if (targetTotal.Value == 0)
            {
                // Z-hat = 0: the inclusion bound alone sets P-tilde.
                const float32 surrogate =
                    EMISSIVE_PROBABILITY *
                    math::Min(1.0F, static_cast<float32>(internal::GetReferenceRisCandidateCount(mode)) * dimPmf);
                const math::Vector2 densities =
                    internal::EvaluateReferenceLightTechniqueDensities(frame, dimLight, lightSample.DirectionWorld);
                CHECK(HasSameBits(density, surrogate * (densities.X + densities.Y)) == true);
            }
        }
    }
    CHECK(zeroTotals > 0);
    CHECK(lightSamples > 0);
}

TEST_CASE("Diffuse probes reject the resampled light-sampling mode", "[renderer][reference][resampled-light]")
{
    // A probe's first event is a virtual receiver with no stored frame for the BSDF-hit density.
    const EmitterInput inputs[]{MakeHorizontalEmitter({0, 0, 2}, -1, {4, 4, 4}, true)};
    const rr::ReferenceLightSampling mode = GENERATE(rr::ReferenceLightSampling::ResampledProjectedSolidAngle,
                                                     rr::ReferenceLightSampling::ResampledFormFactor);
    EmitterScene scene;
    BuildEmitterScene(inputs, 1, scene, mode);
    rr::ReferenceCaptureInfo info;
    math::Vector3 irradiance{1, 2, 3};
    CHECK(internal::EvaluateReferenceDiffuseProbe(scene.Job, info, {}, irradiance) == rhi::RasterStatus::Unsupported);
    CHECK(irradiance == math::Vector3{1, 2, 3});
    CHECK(info.InvalidContributions == 0);
}

TEST_CASE("The approximate resampling target is positive wherever the exact one is",
          "[renderer][reference][resampled-light]")
{
    // The support invariant of sampler version 3: E_0 > 0 whenever the triangle has area above the horizon, including
    // triangles that cross it, where Hill and Heitz's proxy sphere can lie below the horizon.
    const internal::ReferenceReceiverFrame frame = MakeTestFrame(math::Matrix3::Identity());
    constexpr uint32 TRIANGLE_COUNT = 20000;
    constexpr float32 RADIUS_WORLD = 2;
    uint32 crossingCount = 0;
    uint32 defendedCount = 0;
    uint32 belowCount = 0;
    for (uint32 index = 0; index < TRIANGLE_COUNT; ++index)
    {
        StaticArray<math::Vector3, 3> positions{};
        for (uint16 corner = 0; corner < 3; ++corner)
        {
            const uint16 dimension = static_cast<uint16>(corner * 3);
            positions[corner] = math::Vector3{Draw(index, dimension) * 2 - 1,
                                              Draw(index, static_cast<uint16>(dimension + 1)) * 2 - 1,
                                              Draw(index, static_cast<uint16>(dimension + 2)) * 2 - 1.2F} *
                                RADIUS_WORLD;
        }
        CAPTURE(index);
        internal::ReferenceTriangleLight exact;
        REQUIRE(internal::TryPrepareReferenceTriangleLight(frame, positions[0], positions[1], positions[2], exact) ==
                true);
        math::Vector2 approximate{-1, -1};
        REQUIRE(internal::TryApproximateReferenceTriangleIntegrals(frame,
                                                                   positions[0],
                                                                   positions[1],
                                                                   positions[2],
                                                                   approximate) == true);
        CHECK(approximate.X >= 0);
        CHECK(approximate.X <= 1);
        CHECK(approximate.Y >= 0);
        const bool hasVertexAbove = positions[0].Z > 0 || positions[1].Z > 0 || positions[2].Z > 0;
        const bool hasVertexBelow = positions[0].Z < 0 || positions[1].Z < 0 || positions[2].Z < 0;
        if (exact.DiffuseIntegral > 0)
        {
            REQUIRE(approximate.X > 0);
        }
        if (hasVertexAbove == false)
        {
            ++belowCount;
            CHECK(approximate.X == 0);
        }
        else if (hasVertexBelow == true)
        {
            ++crossingCount;
            // The defensive term is what keeps these positive.
            math::ProjectedPolygonInput polygon;
            polygon.VertexCount = 3;
            for (usize corner = 0; corner < 3; ++corner)
            {
                polygon.PositionsTangent[corner] = math::Vector3{math::Dot(positions[corner], frame.TangentWorld),
                                                                 math::Dot(positions[corner], frame.BitangentWorld),
                                                                 math::Dot(positions[corner], frame.NormalWorld)};
            }
            math::ApproximatePolygonIntegral sphere;
            REQUIRE(math::TryApproximateClampedCosinePolygonIntegral(polygon, sphere) == math::MathStatus::Success);
            defendedCount += sphere.Integral == 0 && exact.DiffuseIntegral > 0 ? 1U : 0U;
        }
    }
    CAPTURE(crossingCount, defendedCount, belowCount);
    CHECK(crossingCount > TRIANGLE_COUNT / 10);
    CHECK(belowCount > 0);
    CHECK(defendedCount > 0);
}
