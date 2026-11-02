#include "internal/metropolis.h"

#include <ludus/foundation/math/status.hpp>

// Thanks to Sascha Holl, Gurprit Singh and Hans-Peter Seidel, "Rao-Blackwellized
// Markov chain Monte Carlo Light Transport", SIGGRAPH 2026, section 5.4,
// Algorithm 5.2, https://arxiv.org/abs/2605.09117 . CPU oracle for the template's
// primary-space chain. Both numerator and denominator use the selected estimator;
// the paper's practical holding-time truncation and our ratio have finite-sample bias.
namespace ludus::graphics::renderer::internal
{
namespace
{
constexpr uint16 ACCEPTANCE_DIMENSION = METROPOLIS_PRIMARY_DIMENSIONS;
constexpr uint16 LARGE_STEP_DIMENSION = ACCEPTANCE_DIMENSION + 1;
constexpr uint16 HOLDING_TIME_DIMENSION = LARGE_STEP_DIMENSION + 1;
constexpr uint32 INITIAL_SCOPE = 1;
constexpr uint32 PROPOSAL_SCOPE = 2;
constexpr uint32 RESTORE_INITIAL_SCOPE = 3;
constexpr uint32 RESTORE_PROPOSAL_SCOPE = 4;
constexpr uint32 SCOPE_HIGH_SHIFT = 32;
constexpr math::Vector3 LUMINANCE_WEIGHTS{0.2126F, 0.7152F, 0.0722F};
constexpr float32 MAXIMUM_DISTANCE_WORLD = 0x1.fffffeP+127F;
// Squared unit-direction tolerance of the scene ray interface (SCENE_UNIT_TOLERANCE).
constexpr float32 RAY_UNIT_SQUARED_LENGTH_TOLERANCE = 0.00001F;
float32 Sample(math::RandomKey key, uint32 pixel, uint32 scope, uint32 iteration, uint16 dimension) noexcept
{
    return math::SampleFloat01(key, {uint64{pixel} | (uint64{scope} << SCOPE_HIGH_SHIFT), iteration, dimension});
}
bool IsValidRadiance(math::Vector3 value) noexcept
{
    return math::IsFinite(value) == true && value.X >= 0 && value.Y >= 0 && value.Z >= 0;
}
float32 ComputeTarget(const MetropolisOptions& options, math::Vector3 radiance) noexcept
{
    return options.TargetFloor + math::Dot(radiance, LUMINANCE_WEIGHTS);
}
void AddPathAov(math::Vector3 contribution, uint32 depth, MetropolisPathAovs& inoutAovs) noexcept
{
    if (depth == 0)
        inoutAovs.Emission = inoutAovs.Emission + contribution;
    else if (depth == 1)
        inoutAovs.Direct = inoutAovs.Direct + contribution;
    else
        inoutAovs.Indirect = inoutAovs.Indirect + contribution;
}
rhi::RasterStatus AdvanceChain(const MetropolisOptions& options,
                               MetropolisEstimator estimator,
                               math::RandomKey key,
                               uint32 pixel,
                               MetropolisEvaluator evaluate,
                               const void* context,
                               MetropolisChain& inoutChain,
                               math::Vector3& outRadiance,
                               bool isJumpRestore) noexcept
{
    if (evaluate == nullptr || context == nullptr || math::IsFinite(options.LargeStepProbability) == false ||
        options.LargeStepProbability <= 0 || options.LargeStepProbability > 1 ||
        math::IsFinite(options.TargetFloor) == false || options.TargetFloor <= 0 ||
        static_cast<uint32>(estimator) > static_cast<uint32>(MetropolisEstimator::RaoBlackwell))
        return rhi::RasterStatus::InvalidDescription;
    if (inoutChain.Iteration == ~uint32{0})
        return rhi::RasterStatus::CapacityExceeded;
    MetropolisChain chain = inoutChain;
    const uint32 initialScope = isJumpRestore == true ? RESTORE_INITIAL_SCOPE : INITIAL_SCOPE;
    const uint32 proposalScope = isJumpRestore == true ? RESTORE_PROPOSAL_SCOPE : PROPOSAL_SCOPE;
    if (chain.IsInitialized == false)
    {
        for (usize dimension = 0; dimension < METROPOLIS_PRIMARY_DIMENSIONS; ++dimension)
            chain.Primary[dimension] =
                Sample(key, pixel, initialScope, chain.Iteration, static_cast<uint16>(dimension));
        const rhi::RasterStatus status = evaluate(context, chain.Primary, chain.Radiance);
        if (status != rhi::RasterStatus::Ready)
            return status;
        chain.Target = ComputeTarget(options, chain.Radiance);
        chain.IsInitialized = true;
    }
    math::JumpRestoreRates rates{0, 1};
    float32 holdingTime = 1;
    bool isRestoration = false;
    if (isJumpRestore == true)
    {
        if (math::TryComputeJumpRestoreRates(chain.Target, options.RestorationScale, rates) !=
                math::MathStatus::Success ||
            math::TrySampleJumpRestoreHoldingTime(
                Sample(key, pixel, proposalScope, chain.Iteration, HOLDING_TIME_DIMENSION),
                rates.MeanHoldingTime,
                holdingTime) != math::MathStatus::Success)
            return rhi::RasterStatus::InvalidDescription;
        isRestoration =
            Sample(key, pixel, proposalScope, chain.Iteration, LARGE_STEP_DIMENSION) < rates.RestoreProbability;
    }
    const bool isLargeStep =
        isJumpRestore == true
            ? isRestoration
            : Sample(key, pixel, proposalScope, chain.Iteration, LARGE_STEP_DIMENSION) < options.LargeStepProbability;
    MetropolisPrimary proposal;
    for (usize dimension = 0; dimension < METROPOLIS_PRIMARY_DIMENSIONS; ++dimension)
    {
        if (math::TryMutateMetropolisPrimary(
                chain.Primary[dimension],
                Sample(key, pixel, proposalScope, chain.Iteration, static_cast<uint16>(dimension)),
                options.SmallStepRadius,
                isLargeStep,
                proposal[dimension]) != math::MathStatus::Success)
            return rhi::RasterStatus::InvalidDescription;
    }
    math::Vector3 proposedRadiance;
    const rhi::RasterStatus evaluated = evaluate(context, proposal, proposedRadiance);
    if (evaluated != rhi::RasterStatus::Ready)
        return evaluated;
    if (IsValidRadiance(chain.Radiance) == false || IsValidRadiance(proposedRadiance) == false)
        return rhi::RasterStatus::Failed;
    const float32 proposedTarget = ComputeTarget(options, proposedRadiance);
    float32 acceptance = 0;
    if (math::TryComputeSymmetricMetropolisAcceptance(chain.Target, proposedTarget, acceptance) !=
        math::MathStatus::Success)
        return rhi::RasterStatus::Failed;
    const bool isAccepted =
        isRestoration == true || Sample(key, pixel, proposalScope, chain.Iteration, ACCEPTANCE_DIMENSION) < acceptance;
    if (chain.Iteration >= options.BurnIn)
    {
        math::MathStatus status = math::MathStatus::Success;
        if (estimator == MetropolisEstimator::Standard)
            status = math::TryAddWeightedRadiance(chain.Radiance, holdingTime / chain.Target, chain.Accumulation);
        else if (estimator == MetropolisEstimator::WasteRecycling)
        {
            const float32 currentWeight = isRestoration == true ? 1 : 1 - acceptance;
            status = math::TryAddWeightedRadiance(chain.Radiance,
                                                  holdingTime * currentWeight / chain.Target,
                                                  chain.Accumulation);
            if (status == math::MathStatus::Success && isRestoration == false)
                status = math::TryAddWeightedRadiance(proposedRadiance,
                                                      holdingTime * acceptance / proposedTarget,
                                                      chain.Accumulation);
        }
        else
        {
            if (isRestoration == false)
                status = math::TryAdvanceMetropolisTour(acceptance, chain.Tour);
            if (status == math::MathStatus::Success && isAccepted == true)
                status = math::TryAddWeightedRadiance(chain.Radiance,
                                                      chain.Tour.HoldingWeight * rates.MeanHoldingTime / chain.Target,
                                                      chain.Accumulation);
        }
        if (status != math::MathStatus::Success)
            return rhi::RasterStatus::Failed;
    }
    if (isAccepted == true)
    {
        chain.Primary = proposal;
        chain.Radiance = proposedRadiance;
        chain.Target = proposedTarget;
        chain.Tour = {};
    }
    math::Vector3 radiance;
    if (math::TryResolveWeightedRadiance(chain.Accumulation, radiance) != math::MathStatus::Success)
        return rhi::RasterStatus::Failed;
    ++chain.Iteration;
    inoutChain = chain;
    outRadiance = radiance;
    return rhi::RasterStatus::Ready;
}
} // namespace

rhi::RasterStatus AdvanceMetropolisChain(const MetropolisOptions& options,
                                         MetropolisEstimator estimator,
                                         math::RandomKey key,
                                         uint32 pixel,
                                         MetropolisEvaluator evaluate,
                                         const void* context,
                                         MetropolisChain& inoutChain,
                                         math::Vector3& outRadiance) noexcept
{
    return AdvanceChain(options, estimator, key, pixel, evaluate, context, inoutChain, outRadiance, false);
}

// Thanks to Holl, Singh and Seidel, "Rao-Blackwellized Markov chain Monte Carlo
// Light Transport", SIGGRAPH 2026, section 6, Algorithm 6.2,
// https://arxiv.org/abs/2605.09117 . Unit-rate MH local moves and uniform
// restorations; time-weighted ratios retain the template's normalization contract.
rhi::RasterStatus AdvanceJumpRestoreChain(const MetropolisOptions& options,
                                          MetropolisEstimator estimator,
                                          math::RandomKey key,
                                          uint32 pixel,
                                          MetropolisEvaluator evaluate,
                                          const void* context,
                                          MetropolisChain& inoutChain,
                                          math::Vector3& outRadiance) noexcept
{
    return AdvanceChain(options, estimator, key, pixel, evaluate, context, inoutChain, outRadiance, true);
}

rhi::RasterStatus EvaluateMetropolisPath(const ReferenceSceneData& scene,
                                         rhi::RayInput ray,
                                         const MetropolisPrimary& primary,
                                         uint32 scatteringDepth,
                                         math::Vector3 environment,
                                         math::Vector3& outRadiance,
                                         const EnvironmentData* environmentOrNull,
                                         MetropolisPathAovs* outAovsOrNull) noexcept
{
    if (scatteringDepth == 0 || scatteringDepth > METROPOLIS_MAXIMUM_DEPTH || IsValidRadiance(environment) == false)
        return rhi::RasterStatus::InvalidDescription;
    const math::Vector3 directionWorld = ReferenceDirection(ray);
    if (math::IsFinite(ReferenceOrigin(ray)) == false || math::IsFinite(directionWorld) == false ||
        math::IsFinite(ray.MinDistance) == false || math::IsFinite(ray.MaxDistance) == false || ray.MinDistance < 0 ||
        ray.MaxDistance < ray.MinDistance ||
        math::Abs(math::Dot(directionWorld, directionWorld) - 1) > RAY_UNIT_SQUARED_LENGTH_TOLERANCE)
        return rhi::RasterStatus::InvalidDescription;
    math::Vector3 radiance{};
    MetropolisPathAovs aovs;
    math::Vector3 throughput{1, 1, 1};
    for (uint32 depth = 0; depth <= scatteringDepth; ++depth)
    {
        rhi::RayResult hit;
        const rhi::RasterStatus status = TraceReferenceOracle(scene, ray, hit);
        if (status != rhi::RasterStatus::Ready)
            return status;
        if (hit.Hit == 0)
        {
            const math::Vector3 contribution = math::Hadamard(
                throughput,
                environmentOrNull != nullptr ? EvaluateEnvironment(*environmentOrNull, ReferenceDirection(ray))
                                             : environment);
            radiance = radiance + contribution;
            AddPathAov(contribution, depth, aovs);
            break;
        }
        if (hit.Instance >= scene.Instances.GetSize())
            return rhi::RasterStatus::Failed;
        const ReferenceInstance& instance = scene.Instances[hit.Instance];
        const ReferenceTriangle& triangle = scene.Triangles[instance.FirstTriangle + hit.Primitive];
        const ReferenceSurfaceData& surface = *instance.Surface;
        const math::Vector3 viewDirectionWorld = -ReferenceDirection(ray);
        const math::Vector2 uv = InterpolateReferenceUv(triangle, hit.U, hit.V);
        if (surface.IsEmissionTwoSided == true || math::Dot(triangle.GeometricNormal, viewDirectionWorld) > 0)
        {
            const math::Vector3 contribution = math::Hadamard(throughput, SampleReferenceEmission(instance, uv));
            radiance = radiance + contribution;
            AddPathAov(contribution, depth, aovs);
        }
        if (depth == scatteringDepth || math::Dot(triangle.GeometricNormal, viewDirectionWorld) <= 0)
            break;
        if (surface.Bsdf.Scattering.Model == content::scene_format::MaterialModel::CoatedDiffuse)
            return rhi::RasterStatus::Unsupported;
        ReferenceBsdf bsdf = surface.Bsdf;
        math::Vector4 texel = SampleReferenceTexture(instance, uv);
        if (surface.AreAuthoredInputsEnabled == true)
            texel = math::Hadamard(texel, InterpolateReferenceColor(triangle, hit.U, hit.V));
        bsdf.BaseColor = math::Hadamard(bsdf.BaseColor, {texel.X, texel.Y, texel.Z});
        math::Vector3 normalWorld = ReferenceNormalize(triangle.Normals[0] * (1 - hit.U - hit.V) +
                                                       triangle.Normals[1] * hit.U + triangle.Normals[2] * hit.V);
        if (surface.HasSurfaceMap == true)
        {
            SurfaceParameters parameters;
            if (DecodeSurfaceParameters(SampleReferenceSurfaceMap(instance, uv),
                                        bsdf.Metallic,
                                        bsdf.Roughness,
                                        surface.NormalScale,
                                        parameters) != rhi::RasterStatus::Ready)
                return rhi::RasterStatus::Failed;
            bsdf.Metallic = parameters.Metallic;
            bsdf.Roughness = parameters.Roughness;
            normalWorld = ApplyReferenceNormal(triangle, surface, normalWorld, parameters.NormalTangent, hit.U, hit.V);
        }
        const usize dimension = METROPOLIS_CAMERA_DIMENSIONS + depth * METROPOLIS_BSDF_DIMENSIONS;
        ReferenceBsdfSample sample;
        if (SampleReferenceBsdf(bsdf,
                                normalWorld,
                                viewDirectionWorld,
                                {primary[dimension], primary[dimension + 1], primary[dimension + 2]},
                                sample) != rhi::RasterStatus::Ready)
            return rhi::RasterStatus::Failed;
        if (sample.Accepted == false || math::Dot(triangle.GeometricNormal, sample.Direction) <= 0)
            break;
        throughput = math::Hadamard(throughput, sample.Weight);
        const math::Vector3 positionWorld = InterpolateReferencePosition(triangle, hit.U, hit.V);
        math::Vector3 originWorld;
        if (math::TryOffsetRayOrigin({positionWorld, triangle.Error, triangle.GeometricNormal, sample.Direction},
                                     originWorld) != math::MathStatus::Success)
            return rhi::RasterStatus::Failed;
        ray = ReferenceRay(originWorld, sample.Direction, MAXIMUM_DISTANCE_WORLD);
    }
    if (IsValidRadiance(radiance) == false)
        return rhi::RasterStatus::Failed;
    outRadiance = radiance;
    if (outAovsOrNull != nullptr)
        *outAovsOrNull = aovs;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::graphics::renderer::internal
