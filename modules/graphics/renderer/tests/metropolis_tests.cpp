#include "../src/internal/metropolis.h"

#include <ludus/foundation/math/metropolis.hpp>

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace
{
using namespace ludus::foundation;
namespace internal = ludus::graphics::renderer::internal;
namespace rhi = ludus::graphics::rhi;
using AdvanceChainFunction = decltype(&internal::AdvanceMetropolisChain);
struct IntegrandContext final
{
    bool ShouldFail = false;
};
rhi::RasterStatus
EvaluateStepIntegrand(const void*, const internal::MetropolisPrimary& primary, math::Vector3& outRadiance) noexcept
{
    outRadiance = primary[0] >= 0.5F ? math::Vector3{1, 1, 1} : math::Vector3{};
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus
EvaluateIntegrand(const void* context, const internal::MetropolisPrimary& primary, math::Vector3& outRadiance) noexcept
{
    const IntegrandContext& integrand = *static_cast<const IntegrandContext*>(context);
    if (integrand.ShouldFail == true)
        return rhi::RasterStatus::Failed;
    // An analytically integrable RGB function over the unit cube: (1, 1/3, 1/2).
    outRadiance = {2 * primary[0], primary[1] * primary[1], primary[2]};
    return rhi::RasterStatus::Ready;
}
} // namespace

TEST_CASE("Jump Restore unconditionally restores and flushes its unfinished RB holding weight",
          "[renderer][metropolis]")
{
    internal::MetropolisOptions options;
    options.BurnIn = 0;
    options.RestorationScale = 64;
    internal::MetropolisChain chain;
    chain.IsInitialized = true;
    chain.Radiance = {1, 1, 1};
    chain.Target = 2;
    chain.Tour = {0.25F, 1.75F};
    for (float32& sample : chain.Primary)
        sample = 0.5F;
    math::RandomKey key;
    constexpr uint32 PROPOSAL_SCOPE_SHIFT = 32;
    constexpr uint64 PROPOSAL_SCOPE = uint64{4} << PROPOSAL_SCOPE_SHIFT;
    constexpr uint16 EVENT_DIMENSION = 12;
    constexpr uint16 ACCEPTANCE_DIMENSION = 11;
    constexpr uint64 SEED_COUNT = 256;
    math::JumpRestoreRates rates;
    REQUIRE(math::TryComputeJumpRestoreRates(chain.Target, options.RestorationScale, rates) ==
            math::MathStatus::Success);
    bool hasRestoreTicket = false;
    for (uint64 seed = 0; seed < SEED_COUNT; ++seed)
    {
        key.Value = seed;
        if (math::SampleFloat01(key, {PROPOSAL_SCOPE, 0, 0}) < 0.5F &&
            math::SampleFloat01(key, {PROPOSAL_SCOPE, 0, ACCEPTANCE_DIMENSION}) > 0.5F &&
            math::SampleFloat01(key, {PROPOSAL_SCOPE, 0, EVENT_DIMENSION}) < rates.RestoreProbability)
        {
            hasRestoreTicket = true;
            break;
        }
    }
    REQUIRE(hasRestoreTicket == true);
    const IntegrandContext context;
    math::Vector3 radiance;
    REQUIRE(internal::AdvanceJumpRestoreChain(options,
                                              internal::MetropolisEstimator::RaoBlackwell,
                                              key,
                                              0,
                                              EvaluateStepIntegrand,
                                              &context,
                                              chain,
                                              radiance) == rhi::RasterStatus::Ready);
    CHECK(chain.Primary[0] == math::SampleFloat01(key, {PROPOSAL_SCOPE, 0, 0}));
    // This lower-target proposal would fail the corresponding MH acceptance test.
    CHECK(chain.Radiance.X == 0);
    CHECK(chain.Target == 1);
    CHECK(chain.Tour.RejectionProduct == 1);
    CHECK(chain.Tour.HoldingWeight == 1);
    CHECK(math::Abs(chain.Accumulation.Sum.W - 1.75F * rates.MeanHoldingTime / 2) < 0.000001F);
    CHECK(math::Abs(radiance.X - 1) < 0.000001F);
    const internal::MetropolisChain previous = chain;
    options.RestorationScale = 0;
    radiance = {2, 2, 2};
    CHECK(internal::AdvanceJumpRestoreChain(options,
                                            internal::MetropolisEstimator::RaoBlackwell,
                                            key,
                                            0,
                                            EvaluateStepIntegrand,
                                            &context,
                                            chain,
                                            radiance) == rhi::RasterStatus::InvalidDescription);
    CHECK(chain.Iteration == previous.Iteration);
    CHECK(chain.Accumulation.Sum.W == previous.Accumulation.Sum.W);
    CHECK(radiance.X == 2);
}

TEST_CASE("CPU MH estimators integrate a known primary-space function over multiple seeds", "[renderer][metropolis]")
{
    bool isJumpRestore = false;
    SECTION("Metropolis-Hastings") {}
    SECTION("Jump Restore")
    {
        isJumpRestore = true;
    }
    const AdvanceChainFunction advance =
        isJumpRestore == true ? internal::AdvanceJumpRestoreChain : internal::AdvanceMetropolisChain;
    constexpr uint32 SEED_COUNT = 16;
    constexpr uint32 ITERATION_COUNT = 8192;
    constexpr float32 MEAN_TOLERANCE = 0.03F;
    constexpr uint32 ESTIMATOR_COUNT = 3;
    constexpr float32 GREEN_EXPECTATION = 1.0F / 3;
    const IntegrandContext context;
    const internal::MetropolisOptions options;
    for (uint32 estimator = 0; estimator < ESTIMATOR_COUNT; ++estimator)
    {
        math::Vector3 mean{};
        for (uint32 seed = 0; seed < SEED_COUNT; ++seed)
        {
            internal::MetropolisChain chain;
            math::Vector3 radiance;
            for (uint32 iteration = 0; iteration < ITERATION_COUNT; ++iteration)
                REQUIRE(advance(options,
                                static_cast<internal::MetropolisEstimator>(estimator),
                                {seed},
                                0,
                                EvaluateIntegrand,
                                &context,
                                chain,
                                radiance) == rhi::RasterStatus::Ready);
            mean = mean + radiance / static_cast<float32>(SEED_COUNT);
        }
        CAPTURE(estimator, mean.X, mean.Y, mean.Z);
        CHECK(math::Abs(mean.X - 1) < MEAN_TOLERANCE);
        CHECK(math::Abs(mean.Y - GREEN_EXPECTATION) < MEAN_TOLERANCE);
        CHECK(math::Abs(mean.Z - 0.5F) < MEAN_TOLERANCE);
    }
}

TEST_CASE("CPU MH burn-in, failures and reset preserve accepted weighted history", "[renderer][metropolis]")
{
    bool isJumpRestore = false;
    SECTION("Metropolis-Hastings") {}
    SECTION("Jump Restore")
    {
        isJumpRestore = true;
    }
    const AdvanceChainFunction advance =
        isJumpRestore == true ? internal::AdvanceJumpRestoreChain : internal::AdvanceMetropolisChain;
    internal::MetropolisOptions options;
    options.BurnIn = 2;
    internal::MetropolisChain chain;
    IntegrandContext context;
    math::Vector3 radiance{1, 1, 1};
    REQUIRE(advance(options,
                    internal::MetropolisEstimator::RaoBlackwell,
                    {1},
                    0,
                    EvaluateIntegrand,
                    &context,
                    chain,
                    radiance) == rhi::RasterStatus::Ready);
    CHECK(chain.Accumulation.Sum.W == 0);
    CHECK(radiance.X == 0);
    const internal::MetropolisChain previous = chain;
    context.ShouldFail = true;
    radiance = {1, 1, 1};
    CHECK(advance(options,
                  internal::MetropolisEstimator::RaoBlackwell,
                  {1},
                  0,
                  EvaluateIntegrand,
                  &context,
                  chain,
                  radiance) == rhi::RasterStatus::Failed);
    CHECK(chain.Iteration == previous.Iteration);
    CHECK(chain.Target == previous.Target);
    CHECK(chain.Tour.HoldingWeight == previous.Tour.HoldingWeight);
    CHECK(radiance.X == 1);
    context.ShouldFail = false;
    options.SmallStepRadius = 0;
    CHECK(advance(options,
                  internal::MetropolisEstimator::RaoBlackwell,
                  {1},
                  0,
                  EvaluateIntegrand,
                  &context,
                  chain,
                  radiance) == rhi::RasterStatus::InvalidDescription);
    CHECK(chain.Iteration == previous.Iteration);
    chain = {};
    CHECK(chain.Iteration == 0);
    CHECK(chain.Accumulation.Sum.W == 0);
}

TEST_CASE("CPU BSDF path oracle retains emission and reflection and rejects unsupported coating",
          "[renderer][metropolis]")
{
    internal::ReferenceSceneData scene;
    internal::ReferenceSurfaceData surface;
    surface.Bsdf.Scattering.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    surface.Bsdf.BaseColor = {0.5F, 0.5F, 0.5F};
    surface.Emission = {1, 1, 1};
    REQUIRE(scene.Instances.TryResize(1) == true);
    scene.InstanceCount = 1;
    scene.Instances[0].Surface = &surface;
    scene.Instances[0].TriangleCount = 1;
    REQUIRE(scene.Triangles.TryResize(1) == true);
    internal::ReferenceTriangle& triangle = scene.Triangles[0];
    triangle.Positions[0] = {-10, -10, 0.5F};
    triangle.Positions[1] = {0, 10, 0.5F};
    triangle.Positions[2] = {10, -10, 0.5F};
    triangle.Area = 0.5F * math::Length(math::Cross(triangle.Positions[1] - triangle.Positions[0],
                                                    triangle.Positions[2] - triangle.Positions[0]));
    triangle.GeometricNormal = {0, 0, -1};
    triangle.Error = {0, 0, 0.00001F};
    for (math::Vector3& normalWorld : triangle.Normals)
        normalWorld = triangle.GeometricNormal;
    REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
    internal::MetropolisPrimary primary;
    for (float32& sample : primary)
        sample = 0.5F;
    math::Vector3 radiance;
    internal::MetropolisPathAovs aovs;
    REQUIRE(internal::EvaluateMetropolisPath(scene,
                                             internal::ReferenceRay({0, 0, 0}, {0, 0, 1}, 1),
                                             primary,
                                             1,
                                             {2, 2, 2},
                                             radiance,
                                             nullptr,
                                             &aovs) == rhi::RasterStatus::Ready);
    CHECK(math::Abs(radiance.X - 2) < 0.0001F);
    CHECK(math::Abs(aovs.Emission.X - 1) < 0.0001F);
    CHECK(math::Abs(aovs.Direct.X - 1) < 0.0001F);
    CHECK(aovs.Indirect.X == 0);
    surface.Bsdf.Scattering.Model = ludus::content::scene_format::MaterialModel::CoatedDiffuse;
    radiance = {1, 1, 1};
    CHECK(internal::EvaluateMetropolisPath(scene,
                                           internal::ReferenceRay({0, 0, 0}, {0, 0, 1}, 1),
                                           primary,
                                           1,
                                           {2, 2, 2},
                                           radiance) == rhi::RasterStatus::Unsupported);
    CHECK(radiance.X == 1);
}
