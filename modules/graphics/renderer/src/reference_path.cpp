#include "internal/reference_data.h"
#include "internal/reference_dielectric.h"
#include "internal/wave_connection.h"

#include <ludus/foundation/math/status.hpp>

// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, "Physically Based
// Rendering: From Theory to Implementation", 4th edition (2023), section 13.4,
// https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer:
// retain one light proposal/continuation, same-measure power MIS for both
// emitter/environment hits and next-event estimation, and compensated roulette.
// This RGB, fixed-mip, finite-range reference uses CPU transport with explicit
// asynchronous native query batches; it has no denoising, wavefront scheduler,
// path regularization, spectral or rough-transmission model or hidden frame readback.
// Smooth dielectrics follow pbrt-v4's specular bounces (section 13.4): they skip light
// sampling, and emission found through them takes full weight. Shadow rays stay opaque
// to them, as in pbrt-v4, so light reaches a surface through glass only by BSDF sampling.
namespace ludus::graphics::renderer::internal
{
namespace
{
bool Positive(math::Vector3 value) noexcept
{
    return value.X > 0 || value.Y > 0 || value.Z > 0;
}
bool ValidContribution(math::Vector3 value) noexcept
{
    return math::IsFinite(value) && value.X >= 0 && value.Y >= 0 && value.Z >= 0;
}
rhi::RasterStatus
FailReferencePath(ReferenceCaptureInfo& inoutInfo, const ReferencePath& path, ReferenceFailure failure) noexcept
{
    ++inoutInfo.InvalidContributions;
    inoutInfo.FailurePixel = path.Pixel;
    inoutInfo.FailureDepth = path.Depth;
    inoutInfo.Failure = failure;
    inoutInfo.FailureSample = path.Sample;
    return rhi::RasterStatus::Failed;
}
void AddTransport(ReferencePath& path, math::Vector3 contribution, uint32 depth, bool emission) noexcept
{
    if (emission && depth == 0)
    {
        path.Emission = path.Emission + contribution;
    }
    else if ((emission && depth == 1) || (emission == false && depth == 0))
    {
        path.Direct = path.Direct + contribution;
    }
    else
    {
        path.Indirect = path.Indirect + contribution;
    }
}
usize GetReferenceLightCount(const ReferenceJob& job) noexcept
{
    return job.Description.LightCount - job.DirectionalLightCount + job.Scene.Emitters.GetSize() +
           ((job.EnvironmentOrNull != nullptr || Positive(job.Description.Environment)) ? 1U : 0U);
}
math::Vector3 Position(const ReferenceTriangle& triangle, float32 u, float32 v) noexcept
{
    return InterpolateReferencePosition(triangle, u, v);
}
math::Vector2 Uv(const ReferenceTriangle& triangle, float32 u, float32 v) noexcept
{
    return InterpolateReferenceUv(triangle, u, v);
}
// Bounce slots of LUDUS_PATH_RANDOM_* shared by both sampler versions (renderer_reference.slang names the same slots).
constexpr uint32 BSDF_LOBE_SLOT = 3;
constexpr uint32 BSDF_FIRST_SLOT = 4;
constexpr uint32 BSDF_SECOND_SLOT = 5;
constexpr uint32 ROULETTE_SLOT = 6;
ReferenceBsdfRandomContext MakeMaterialRandomContext(const ReferenceJob& job, const ReferencePath& path) noexcept
{
    return {job.Key, {path.Pixel, path.Sample, static_cast<uint16>(path.Depth)}};
}
bool IsResampled(const ReferenceJob& job) noexcept
{
    return IsReferenceResampled(job.Description.LightSampling);
}
// Dimension `slot` of the path's current scattering event under the job's sampler version.
uint16 Dimension(const ReferenceJob& job, const ReferencePath& path, uint32 slot) noexcept
{
    return ReferenceBounceDimension(job.Description.LightSampling, path.Depth, slot);
}
// Combine the source PMF with the area-to-solid-angle conversion in the frozen operation order.
float32 EvaluateTriangleLightPdf(const ReferenceTriangle& triangle,
                                 math::Vector3 receiverWorld,
                                 math::Vector3 endpointWorld,
                                 float32 selectionPmf) noexcept
{
    const math::Vector3 deltaWorld = endpointWorld - receiverWorld;
    const float32 distanceSquared = math::Dot(deltaWorld, deltaWorld);
    const math::Vector3 directionWorld = ReferenceNormalize(deltaWorld);
    const float32 cosine = math::Abs(math::Dot(triangle.GeometricNormal, -directionWorld));
    if (selectionPmf <= 0 || triangle.Area <= 0 || cosine <= 0)
    {
        return 0;
    }
    return selectionPmf * distanceSquared / (triangle.Area * cosine);
}
// The uniform proposal picks the emitter category with probability emitters / lights;
// the power-weighted CDF then picks one emitter triangle. The GPU kernels use this order.
// ResampledProjectedSolidAngle keeps the category share P_E and replaces the CDF draw.
float32 ComputeEmissiveCategoryProbability(const ReferenceJob& job, usize lights) noexcept
{
    return lights == 0 ? 0 : float32(job.Scene.Emitters.GetSize()) / float32(lights);
}
float32 ComputeEmitterSelection(const ReferenceJob& job, usize emitter, usize lights) noexcept
{
    if (lights == 0)
    {
        return 0;
    }
    return GetReferenceEmitterProbability(job.Scene, emitter) * ComputeEmissiveCategoryProbability(job, lights);
}
math::Vector3 Offset(math::Vector3 point, const ReferenceTriangle& triangle, math::Vector3 direction) noexcept
{
    math::Vector3 result = point;
    if (math::TryOffsetRayOrigin({point, triangle.Error, triangle.GeometricNormal, direction}, result) !=
        math::MathStatus::Success)
    {
        return {__builtin_nanf(""), 0, 0};
    }
    return result;
}
// The reference selector keeps its historical category draw, including the
// triangle count as the weight of the emissive category.
enum class ReferenceLightChoiceKind : uint8
{
    None,
    Analytic,
    Triangle,
    ResampledTriangles,
    Environment,
    Invalid,
};
struct ReferenceLightChoice final
{
    ReferenceLightChoiceKind Kind = ReferenceLightChoiceKind::None;
    usize Index = 0;
    float32 SelectionPmf = 0;
};

ReferenceLightChoice SelectReferenceLight(const ReferenceJob& job, const ReferencePath& path, usize lightCount) noexcept
{
    if (lightCount == 0)
    {
        return {};
    }
    const uint16 dimension = Dimension(job, path, LUDUS_PATH_RANDOM_LIGHT_SELECTION_SLOT);
    uint32 selected = 0;
    if (math::TrySampleBounded(job.Key,
                               {path.Pixel, path.Sample, dimension},
                               static_cast<uint32>(lightCount),
                               selected) != math::MathStatus::Success)
    {
        return {ReferenceLightChoiceKind::Invalid};
    }
    const float32 uniformPmf = 1 / static_cast<float32>(lightCount);
    const usize remainingAnalyticLights = job.Description.LightCount - job.DirectionalLightCount;
    if (selected < remainingAnalyticLights)
    {
        return {ReferenceLightChoiceKind::Analytic, selected + job.DirectionalLightCount, uniformPmf};
    }
    if (usize(selected) - remainingAnalyticLights >= job.Scene.Emitters.GetSize())
    {
        return {ReferenceLightChoiceKind::Environment, 0, uniformPmf};
    }
    if (IsResampled(job) == true)
    {
        return {ReferenceLightChoiceKind::ResampledTriangles, 0, ComputeEmissiveCategoryProbability(job, lightCount)};
    }
    if (job.Scene.EmitterCdf.GetSize() != job.Scene.Emitters.GetSize())
    {
        return {ReferenceLightChoiceKind::Invalid};
    }
    const uint32 total = job.Scene.EmitterCdf[job.Scene.EmitterCdf.GetSize() - 1];
    if (total == 0)
    {
        return {}; // No emitter carries power.
    }
    uint32 value = 0;
    if (math::TrySampleBounded(job.Key,
                               {path.Pixel, path.Sample, Dimension(job, path, LUDUS_PATH_RANDOM_EMITTER_SLOT)},
                               total,
                               value) != math::MathStatus::Success)
    {
        return {ReferenceLightChoiceKind::Invalid};
    }
    const usize emitter = SelectReferenceEmitter(job.Scene, value);
    return {ReferenceLightChoiceKind::Triangle, emitter, ComputeEmitterSelection(job, emitter, lightCount)};
}

struct ReferenceIncidentLight final
{
    math::Vector3 DirectionWorld{};
    math::Vector3 Radiance{};
    math::Vector3 PositionWorld{};
    math::Vector3 ErrorWorld{};
    math::Vector3 NormalWorld{};
    float32 Distance = 0;
    float32 EnvironmentPdfSolidAngle = 0;
    bool IsDelta = false;
    bool HasFiniteEndpoint = false;
    bool HasFailed = false;
};

constexpr float32 NEAR_LIGHT_SQUARED_DISTANCE_FLOOR = 1e-4F;
constexpr float32 SPHERE_SOLID_ANGLE_FACTOR = 4;

ReferenceIncidentLight
EvaluateAnalyticIncidentLight(const ReferenceJob& job, const DirectLight& light, math::Vector3 receiverWorld) noexcept
{
    ReferenceIncidentLight result;
    result.IsDelta = true;
    result.Radiance = light.Color * light.Intensity;
    if (light.Kind == LightKind::Directional)
    {
        result.DirectionWorld = -light.Direction;
        result.Distance = job.Description.SecondaryDistance;
    }
    else
    {
        const math::Vector3 deltaWorld = light.Position - receiverWorld;
        const float32 distanceSquared = math::Dot(deltaWorld, deltaWorld);
        if (distanceSquared <= 0)
        {
            result.HasFailed = true;
            return result;
        }
        result.Distance = math::Sqrt(distanceSquared);
        result.DirectionWorld = deltaWorld / result.Distance;
        result.PositionWorld = light.Position;
        result.HasFiniteEndpoint = true;
        if (result.Distance >= light.Range || result.Distance > job.Description.SecondaryDistance)
        {
            result.Radiance = {};
            return result;
        }
        const float32 ratio = result.Distance / light.Range;
        const float32 window = math::Max(0.0F, 1 - ratio * ratio * ratio * ratio);
        float32 angular = 1;
        if (light.Kind == LightKind::Spot)
        {
            const float32 cosine = math::Dot(light.Direction, -result.DirectionWorld);
            const float32 t = math::Clamp((cosine - light.OuterCos) / (light.InnerCos - light.OuterCos), 0.0F, 1.0F);
            angular = t * t;
        }
        if (light.Kind == LightKind::AreaOneSided || light.Kind == LightKind::AreaTwoSided)
        {
            const float32 emitterCosine = math::Dot(light.Direction, -result.DirectionWorld);
            angular = light.Kind == LightKind::AreaTwoSided ? math::Abs(emitterCosine) : math::Max(emitterCosine, 0.F);
        }
        result.Radiance = result.Radiance *
                          (window * window * angular / math::Max(distanceSquared, NEAR_LIGHT_SQUARED_DISTANCE_FLOOR));
    }
    return result;
}
ReferenceIncidentLight SampleTriangleIncidentLight(const ReferenceJob& job,
                                                   const ReferencePath& path,
                                                   usize emitter,
                                                   math::Vector3 receiverWorld) noexcept
{
    ReferenceIncidentLight result;
    const ReferenceTriangle& triangle = job.Scene.Triangles[job.Scene.Emitters[emitter]];
    const float32 root =
        math::Sqrt(ReferenceRandom(job, path, Dimension(job, path, LUDUS_PATH_RANDOM_INCIDENT_FIRST_SLOT)));
    const float32 u =
        root * (1 - ReferenceRandom(job, path, Dimension(job, path, LUDUS_PATH_RANDOM_INCIDENT_SECOND_SLOT)));
    const float32 v = root - u;
    result.PositionWorld = Position(triangle, u, v);
    const math::Vector3 deltaWorld = result.PositionWorld - receiverWorld;
    result.Distance = math::Length(deltaWorld);
    if (result.Distance <= 0)
    {
        result.HasFailed = true;
        return result;
    }
    result.DirectionWorld = deltaWorld / result.Distance;
    result.ErrorWorld = triangle.Error;
    result.NormalWorld = OrientReferenceGeometricNormal(triangle, u, v);
    result.HasFiniteEndpoint = true;
    const ReferenceInstance& instance = job.Scene.Instances[triangle.Instance];
    if ((instance.Surface->IsEmissionTwoSided == true || math::Dot(result.NormalWorld, -result.DirectionWorld) > 0) &&
        result.Distance <= job.Description.SecondaryDistance &&
        ReferenceOpacity(instance, Uv(triangle, u, v), InterpolateReferenceColor(triangle, u, v).W) == true)
    {
        result.Radiance = SampleReferenceEmission(instance, Uv(triangle, u, v));
    }
    return result;
}
ReferenceIncidentLight SampleEnvironmentIncidentLight(const ReferenceJob& job, const ReferencePath& path) noexcept
{
    ReferenceIncidentLight result;
    result.Distance = job.Description.SecondaryDistance;
    const float32 first = ReferenceRandom(job, path, Dimension(job, path, LUDUS_PATH_RANDOM_INCIDENT_FIRST_SLOT));
    const float32 second = ReferenceRandom(job, path, Dimension(job, path, LUDUS_PATH_RANDOM_INCIDENT_SECOND_SLOT));
    if (job.EnvironmentOrNull != nullptr)
    {
        const EnvironmentSample sample = SampleEnvironment(*job.EnvironmentOrNull, first, second);
        result.DirectionWorld = sample.DirectionWorld;
        result.Radiance = sample.Radiance;
        result.EnvironmentPdfSolidAngle = sample.Pdf;
        return result;
    }
    const float32 z = 1 - 2 * first;
    const float32 radius = math::Sqrt(math::Max(0.0F, 1 - z * z));
    const float32 angle = math::kTauF * second;
    result.DirectionWorld = {radius * math::Cos(angle), radius * math::Sin(angle), z};
    result.Radiance = job.Description.Environment;
    result.EnvironmentPdfSolidAngle = 1 / (SPHERE_SOLID_ANGLE_FACTOR * math::kPiF);
    return result;
}
ReferenceIncidentLight SampleReferenceIncidentLight(const ReferenceJob& job,
                                                    const ReferencePath& path,
                                                    const ReferenceLightChoice& choice,
                                                    math::Vector3 receiverWorld) noexcept
{
    switch (choice.Kind)
    {
        case ReferenceLightChoiceKind::Analytic:
            return EvaluateAnalyticIncidentLight(job, GetOrderedReferenceLight(job, choice.Index), receiverWorld);
        case ReferenceLightChoiceKind::Triangle:
            return SampleTriangleIncidentLight(job, path, choice.Index, receiverWorld);
        case ReferenceLightChoiceKind::Environment:
            return SampleEnvironmentIncidentLight(job, path);
        default:
            return {};
    }
}
float32 ComputeEnvironmentLightPdf(float32 conditionalPdf, usize lightCount) noexcept
{
    return lightCount == 0 ? 0 : conditionalPdf / static_cast<float32>(lightCount);
}
float32 EvaluateSampledLightProbability(const ReferenceJob& job,
                                        const ReferenceLightChoice& choice,
                                        const ReferenceIncidentLight& incident,
                                        math::Vector3 receiverWorld,
                                        usize lightCount) noexcept
{
    if (choice.Kind == ReferenceLightChoiceKind::Invalid || incident.HasFailed == true)
    {
        return -1;
    }
    switch (choice.Kind)
    {
        case ReferenceLightChoiceKind::Analytic:
            return choice.SelectionPmf; // Unit conditional mass for a deterministic incident event.
        case ReferenceLightChoiceKind::Triangle:
            return EvaluateTriangleLightPdf(job.Scene.Triangles[job.Scene.Emitters[choice.Index]],
                                            receiverWorld,
                                            incident.PositionWorld,
                                            choice.SelectionPmf);
        case ReferenceLightChoiceKind::Environment:
            // Preserve the frozen CPU constant-sky evaluation order for sampled directions.
            return job.EnvironmentOrNull == nullptr
                       ? 1 / (static_cast<float32>(lightCount) * SPHERE_SOLID_ANGLE_FACTOR * math::kPiF)
                       : ComputeEnvironmentLightPdf(incident.EnvironmentPdfSolidAngle, lightCount);
        default:
            return 0;
    }
}
// Queue the visibility segment of one light contribution from `point` on `triangle`.
rhi::RasterStatus QueueShadow(const ReferenceJob& job,
                              ReferenceCaptureInfo& inoutInfo,
                              ReferencePath& inoutPath,
                              const ReferenceTriangle& triangle,
                              math::Vector3 point,
                              const ReferenceIncidentLight& proposal,
                              math::Vector3 contribution) noexcept
{
    ReferencePath& path = inoutPath;
    const math::Vector3 start = Offset(point, triangle, proposal.DirectionWorld);
    math::Vector3 direction = proposal.DirectionWorld;
    math::Vector3 endWorld = proposal.PositionWorld;
    float32 distance = proposal.Distance;
    if (proposal.HasFiniteEndpoint == true)
    {
        if (proposal.IsDelta == false &&
            math::TryOffsetRayOrigin({proposal.PositionWorld, proposal.ErrorWorld, proposal.NormalWorld, -direction},
                                     endWorld) != math::MathStatus::Success)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidShadowSegment);
        }
        const math::Vector3 delta = endWorld - start;
        distance = math::Length(delta);
        if (distance <= 0 || math::IsFinite(distance) == false)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidShadowSegment);
        }
        direction = delta / distance;
        // The target endpoint is excluded by one representable step.
        // Light geometry must not occlude its own sampled emission.
        distance = math::NextFloatDown(distance);
    }
    if (job.Description.OpticalWavelengthWorld > 0 && proposal.HasFiniteEndpoint == true)
    {
        // Retain the robustly offset light endpoint; reconstructing it from a rounded direction can land on the
        // emitter.
        const math::Vector3 sourceWorld = endWorld;
        WaveField field;
        const rhi::RasterStatus opticalStatus = EvaluateWaveConnectionToReceiver(
            job.Scene,
            {static_cast<float64>(sourceWorld.X),
             static_cast<float64>(sourceWorld.Y),
             static_cast<float64>(sourceWorld.Z)},
            {static_cast<float64>(start.X), static_cast<float64>(start.Y), static_cast<float64>(start.Z)},
            {-static_cast<float64>(direction.X),
             -static_cast<float64>(direction.Y),
             -static_cast<float64>(direction.Z)},
            static_cast<float64>(distance),
            static_cast<float64>(job.Description.OpticalWavelengthWorld),
            field);
        if (opticalStatus != rhi::RasterStatus::Ready)
        {
            return FailReferencePath(inoutInfo,
                                     path,
                                     opticalStatus == rhi::RasterStatus::Unsupported
                                         ? ReferenceFailure::OpticalPropagationCapacity
                                         : (opticalStatus == rhi::RasterStatus::CapacityExceeded
                                                ? ReferenceFailure::OpticalRegionCapacity
                                                : ReferenceFailure::InvalidShadowSegment));
        }
        const float32 transmission = static_cast<float32>(field.Real * field.Real + field.Imaginary * field.Imaginary);
        if (math::IsFinite(transmission) == false || transmission < 0)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidDirectContribution);
        }
        AddTransport(path, contribution * transmission, path.Depth, false);
        ++inoutInfo.VisibilityQueries;
        return rhi::RasterStatus::Ready;
    }
    LUDUS_ASSERT(path.ShadowCount < REFERENCE_MAX_SHADOWS_PER_VERTEX);
    rhi::RayInput& shadow = path.Shadows[path.ShadowCount];
    shadow = ReferenceRay(start, direction, distance);
    shadow.VisibilityOnly = 1;
    path.PendingDirects[path.ShadowCount] = contribution;
    ++path.ShadowCount;
    path.DirectDepth = path.Depth;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus QueueDirectContribution(const ReferenceJob& job,
                                          ReferenceCaptureInfo& inoutInfo,
                                          ReferencePath& inoutPath,
                                          const ReferenceTriangle& triangle,
                                          const ReferenceBsdf& bsdf,
                                          math::Vector3 positionWorld,
                                          math::Vector3 scatteringNormalWorld,
                                          math::Vector3 normalWorld,
                                          math::Vector3 viewWorld,
                                          const ReferenceIncidentLight& proposal,
                                          float32 lightProbability) noexcept
{
    const float32 cosine = math::Dot(normalWorld, proposal.DirectionWorld);
    if ((lightProbability > 0 && Positive(proposal.Radiance) == true && cosine > 0 &&
         math::Dot(scatteringNormalWorld, proposal.DirectionWorld) > 0) == false)
    {
        return rhi::RasterStatus::Ready;
    }
    ReferenceBsdfEvaluation evaluation;
    if (EvaluateReferenceBsdf(bsdf,
                              normalWorld,
                              viewWorld,
                              proposal.DirectionWorld,
                              evaluation,
                              MakeMaterialRandomContext(job, inoutPath)) != rhi::RasterStatus::Ready)
    {
        return FailReferencePath(inoutInfo, inoutPath, ReferenceFailure::InvalidBsdfEvaluation);
    }
    const float32 weight = proposal.IsDelta == true || job.Description.OpticalWavelengthWorld > 0
                               ? 1
                               : ReferencePowerWeight(lightProbability, evaluation.Pdf);
    const math::Vector3 contribution =
        math::Hadamard(inoutPath.Throughput, math::Hadamard(evaluation.Value, proposal.Radiance)) *
        (cosine * weight / lightProbability);
    if (ValidContribution(contribution) == false)
    {
        return FailReferencePath(inoutInfo, inoutPath, ReferenceFailure::InvalidDirectContribution);
    }
    return Positive(contribution) == true
               ? QueueShadow(job, inoutInfo, inoutPath, triangle, positionWorld, proposal, contribution)
               : rhi::RasterStatus::Ready;
}
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, Physically Based Rendering, 4th ed.,
// §§12.3 and 12.6, https://pbr-book.org/4ed/Light_Sources/Distant_Lights and
// https://pbr-book.org/4ed/Light_Sources/Light_Sampling. Connect to a distant light along its
// fixed incident direction. Estimate directional and remaining sources as disjoint sums.
ReferenceLightChoice SelectDirectionalLight(const ReferenceJob& job, const ReferencePath& path) noexcept
{
    uint32 selected = 0;
    const uint16 dimension = static_cast<uint16>(LUDUS_REFERENCE_DIRECTIONAL_FIRST_DIMENSION + path.Depth);
    if (job.DirectionalLightCount > 1 &&
        math::TrySampleBounded(job.Key, {path.Pixel, path.Sample, dimension}, job.DirectionalLightCount, selected) !=
            math::MathStatus::Success)
    {
        return {ReferenceLightChoiceKind::Invalid};
    }
    return {ReferenceLightChoiceKind::Analytic, selected, 1 / static_cast<float32>(job.DirectionalLightCount)};
}
// ResampledProjectedSolidAngle: the emissive branch of the light draw at `point`. Each technique's sample that the
// BSDF and the receiver's geometric side admit queues one shadow ray.
rhi::RasterStatus SampleResampledEmitter(const ReferenceJob& job,
                                         ReferenceCaptureInfo& inoutInfo,
                                         ReferencePath& inoutPath,
                                         const ReferenceTriangle& triangle,
                                         math::Vector3 geometricNormalWorld,
                                         const ReferenceBsdf& bsdf,
                                         math::Vector3 normalWorld,
                                         math::Vector3 viewWorld,
                                         const ReferenceReceiverFrame& frame,
                                         float32 emissiveCategoryProbability,
                                         ReferenceTargetTotal& inoutTargetTotal) noexcept
{
    ReferencePath& path = inoutPath;
    ReferenceResampledLight light;
    if (SampleReferenceResampledLight(job,
                                      path.Pixel,
                                      path.Sample,
                                      path.Depth,
                                      frame,
                                      emissiveCategoryProbability,
                                      inoutTargetTotal,
                                      light) != rhi::RasterStatus::Ready)
    {
        return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidLightSample);
    }
    if (light.HasSelection == false)
    {
        return rhi::RasterStatus::Ready;
    }
    const ReferenceTriangle& emitter = job.Scene.Triangles[light.TriangleIndex];
    for (const ReferenceResampledLightSample& sample : light.Samples)
    {
        if (ValidContribution(sample.Radiance) == false)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidLightSample);
        }
        const float32 cosine = math::Dot(normalWorld, sample.DirectionWorld);
        if (sample.HasSample == false || Positive(sample.Radiance) == false || cosine <= 0 ||
            math::Dot(geometricNormalWorld, sample.DirectionWorld) <= 0)
        {
            continue;
        }
        ReferenceBsdfEvaluation evaluation;
        if (EvaluateReferenceBsdf(bsdf,
                                  normalWorld,
                                  viewWorld,
                                  sample.DirectionWorld,
                                  evaluation,
                                  MakeMaterialRandomContext(job, path)) != rhi::RasterStatus::Ready)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidBsdfEvaluation);
        }
        const float32 weight = sample.TechniqueWeight * ReferencePowerWeight(sample.LightDensity, evaluation.Pdf);
        const math::Vector3 contribution =
            math::Hadamard(path.Throughput, math::Hadamard(evaluation.Value, sample.Radiance)) *
            (cosine * weight / sample.TechniqueDensity * light.ContributionScale);
        if (ValidContribution(contribution) == false)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidDirectContribution);
        }
        if (Positive(contribution) == false)
        {
            continue;
        }
        ReferenceIncidentLight proposal;
        proposal.DirectionWorld = sample.DirectionWorld;
        proposal.Radiance = sample.Radiance;
        proposal.PositionWorld = sample.PointWorld;
        proposal.ErrorWorld = emitter.Error;
        proposal.NormalWorld = emitter.GeometricNormal;
        proposal.Distance = sample.DistanceWorld;
        proposal.HasFiniteEndpoint = true;
        const rhi::RasterStatus queued =
            QueueShadow(job, inoutInfo, path, triangle, frame.PositionWorld, proposal, contribution);
        if (queued != rhi::RasterStatus::Ready)
        {
            return queued;
        }
    }
    return rhi::RasterStatus::Ready;
}
} // namespace
rhi::RasterStatus PrepareReferenceVertex(const ReferenceJob& job,
                                         ReferenceCaptureInfo& inoutInfo,
                                         ReferencePath& inoutPath,
                                         const rhi::RayResult& hit) noexcept
{
    ReferencePath& path = inoutPath;
    path.ShadowCount = 0;
    path.HasNext = false;
    path.IsActive = false;
    const usize lights = GetReferenceLightCount(job);
    if (hit.Hit == 0)
    {
        const float32 environmentPdf =
            job.EnvironmentOrNull != nullptr
                ? EvaluateEnvironmentPdf(*job.EnvironmentOrNull, ReferenceDirection(path.Ray))
                : 1 / (SPHERE_SOLID_ANGLE_FACTOR * math::kPiF);
        const float32 lightPdf = ComputeEnvironmentLightPdf(environmentPdf, lights);
        const float32 weight =
            path.Depth == 0 || path.IsPreviousSpecular == true
                ? 1
                : (job.Description.OpticalWavelengthWorld > 0 ? 0 : ReferencePowerWeight(path.PreviousPdf, lightPdf));
        AddTransport(path,
                     math::Hadamard(path.Throughput,
                                    job.EnvironmentOrNull != nullptr
                                        ? EvaluateEnvironment(*job.EnvironmentOrNull, ReferenceDirection(path.Ray))
                                        : job.Description.Environment) *
                         weight,
                     path.Depth,
                     true);
        return ValidContribution(path.Emission) && ValidContribution(path.Direct) && ValidContribution(path.Indirect)
                   ? rhi::RasterStatus::Ready
                   : FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidRadiance);
    }
    if (hit.Hit != 1 || hit.Instance >= job.Scene.InstanceCount || math::IsFinite(hit.Distance) == false ||
        math::IsFinite(hit.U) == false || math::IsFinite(hit.V) == false || hit.Distance < path.Ray.MinDistance ||
        hit.Distance > path.Ray.MaxDistance || hit.U < -1e-5F || hit.V < -1e-5F || hit.U + hit.V > 1.00001F)
    {
        return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidHit);
    }
    const auto& instance = job.Scene.Instances[hit.Instance];
    if (hit.Primitive >= instance.TriangleCount)
    {
        return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidHit);
    }
    const auto& triangle = job.Scene.Triangles[instance.FirstTriangle + hit.Primitive];
    const auto point = Position(triangle, hit.U, hit.V);
    const auto view = -ReferenceDirection(path.Ray);
    const auto& surface = *instance.Surface;
    const math::Vector3 geometricNormal = OrientReferenceGeometricNormal(triangle, hit.U, hit.V);
    if (Positive(surface.Emission) == true &&
        (surface.IsEmissionTwoSided == true || math::Dot(geometricNormal, view) > 0))
    {
        // An emitter missing from the proposal list cannot be light-sampled; BSDF sampling then owns it.
        const uint32 triangleIndex = static_cast<uint32>(instance.FirstTriangle + hit.Primitive);
        float32 pdf = 0;
        if (path.IsPreviousSpecular == true)
        {
            // No light sample reaches a delta lobe's direction; the emission keeps full weight.
        }
        else if (IsResampled(job) == false)
        {
            usize emitter = 0;
            const float32 selection = FindReferenceEmitter(job.Scene, triangleIndex, emitter) == true
                                          ? ComputeEmitterSelection(job, emitter, lights)
                                          : 0;
            pdf = EvaluateTriangleLightPdf(triangle, path.PreviousPoint, point, selection);
        }
        else if (path.Depth > 0 &&
                 EvaluateReferenceResampledLightDensity(job,
                                                        path.Pixel,
                                                        path.Sample,
                                                        path.Depth - 1,
                                                        path.PreviousReceiver,
                                                        ComputeEmissiveCategoryProbability(job, lights),
                                                        triangleIndex,
                                                        ReferenceDirection(path.Ray),
                                                        path.PreviousTargetTotal,
                                                        pdf) != rhi::RasterStatus::Ready)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidEmitterPdf);
        }
        if (math::IsFinite(pdf) == false || pdf < 0)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidEmitterPdf);
        }
        const float32 weight =
            path.Depth == 0 || path.IsPreviousSpecular == true
                ? 1
                : (job.Description.OpticalWavelengthWorld > 0 ? 0 : ReferencePowerWeight(path.PreviousPdf, pdf));
        AddTransport(path,
                     math::Hadamard(path.Throughput, SampleReferenceEmission(instance, Uv(triangle, hit.U, hit.V))) *
                         weight,
                     path.Depth,
                     true);
    }
    // Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, "Physically Based Rendering", 4th edition (2023),
    // sections 9.2 (Diffuse Reflection) and 14.3 (Scattering from Layered Materials): pbrt-v4's reflection models
    // scatter on whichever side of the surface the viewer is (SameHemisphere, LayeredBxDF twoSided), so a back face
    // reflects as its front does while emission above stays one-sided. Mirroring the geometric and shading normals
    // towards the viewer evaluates these models the same way.
    // https://pbr-book.org/4ed/Reflection_Models/Diffuse_Reflection
    const bool isBackFace = math::Dot(geometricNormal, view) < 0;
    const math::Vector3 scatteringNormal = isBackFace == true ? -geometricNormal : geometricNormal;
    if (path.Depth >= job.Description.MaxDepth || math::Dot(scatteringNormal, view) <= 0)
    {
        return ValidContribution(path.Emission) && ValidContribution(path.Direct) && ValidContribution(path.Indirect)
                   ? rhi::RasterStatus::Ready
                   : FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidRadiance);
    }
    // Corner normals on both sides of the face can cancel; pbrt-v4 then shades with the geometric normal.
    math::Vector3 normal = geometricNormal;
    static_cast<void>(math::TryNormalize(InterpolateReferenceShadingNormal(triangle, hit.U, hit.V), normal));
    math::Vector4 texel = SampleReferenceTexture(instance, Uv(triangle, hit.U, hit.V));
    if (surface.AreAuthoredInputsEnabled == true)
    {
        const math::Vector4 color = InterpolateReferenceColor(triangle, hit.U, hit.V);
        texel = math::Hadamard(texel, color);
    }
    ReferenceBsdf bsdf = surface.Bsdf;
    bsdf.ProgramOrNull = &surface.Program;
    bsdf.BaseColor = math::Hadamard({texel.X, texel.Y, texel.Z}, bsdf.BaseColor);
    if (surface.HasSurfaceMap)
    {
        SurfaceParameters parameters;
        if (DecodeSurfaceParameters(SampleReferenceSurfaceMap(instance, Uv(triangle, hit.U, hit.V)),
                                    bsdf.Metallic,
                                    bsdf.Roughness,
                                    surface.NormalScale,
                                    parameters) != rhi::RasterStatus::Ready)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidSurfaceParameters);
        }
        bsdf.Metallic = parameters.Metallic;
        bsdf.Roughness = parameters.Roughness;
        normal = ApplyReferenceNormal(triangle, surface, normal, parameters.NormalTangent, hit.U, hit.V);
    }
    if (isBackFace == true)
    {
        normal = -normal;
    }
    // A dielectric's medium lies behind its oriented front side; viewed from the back, the ray is inside.
    bsdf.IsViewerInside = isBackFace;
    const bool isSpecular = IsReferenceDielectric(bsdf.Scattering.Model);
    // ResampledProjectedSolidAngle keeps the receiver for this vertex's light sample and its continuation's BSDF-hit
    // density; Z-hat is computed once, on first use.
    ReferenceReceiverFrame frame;
    ReferenceTargetTotal targetTotal;
    if (IsResampled(job) == true && isSpecular == false)
    {
        frame = MakeReferenceReceiverFrame(point, normal, view, bsdf);
    }
    // A delta lobe is zero in every light proposal's direction, so specular vertices skip next-event estimation.
    const ReferenceLightChoice lightChoice =
        isSpecular == true ? ReferenceLightChoice{} : SelectReferenceLight(job, path, lights);
    const ReferenceIncidentLight proposal = SampleReferenceIncidentLight(job, path, lightChoice, point);
    const float32 lightProbability = EvaluateSampledLightProbability(job, lightChoice, proposal, point, lights);
    if (math::IsFinite(lightProbability) == false || lightProbability < 0 ||
        ValidContribution(proposal.Radiance) == false)
    {
        return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidLightSample);
    }
    if (lightChoice.Kind == ReferenceLightChoiceKind::ResampledTriangles)
    {
        const rhi::RasterStatus sampled = SampleResampledEmitter(job,
                                                                 inoutInfo,
                                                                 path,
                                                                 triangle,
                                                                 scatteringNormal,
                                                                 bsdf,
                                                                 normal,
                                                                 view,
                                                                 frame,
                                                                 lightChoice.SelectionPmf,
                                                                 targetTotal);
        if (sampled != rhi::RasterStatus::Ready)
        {
            return sampled;
        }
    }
    if (isSpecular == false && job.DirectionalLightCount != 0)
    {
        const ReferenceLightChoice directionalChoice = SelectDirectionalLight(job, path);
        const ReferenceIncidentLight directional = SampleReferenceIncidentLight(job, path, directionalChoice, point);
        if (directionalChoice.Kind == ReferenceLightChoiceKind::Invalid ||
            ValidContribution(directional.Radiance) == false)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidLightSample);
        }
        const rhi::RasterStatus queued = QueueDirectContribution(job,
                                                                 inoutInfo,
                                                                 path,
                                                                 triangle,
                                                                 bsdf,
                                                                 point,
                                                                 scatteringNormal,
                                                                 normal,
                                                                 view,
                                                                 directional,
                                                                 directionalChoice.SelectionPmf);
        if (queued != rhi::RasterStatus::Ready)
        {
            return queued;
        }
    }
    const rhi::RasterStatus queued = QueueDirectContribution(job,
                                                             inoutInfo,
                                                             path,
                                                             triangle,
                                                             bsdf,
                                                             point,
                                                             scatteringNormal,
                                                             normal,
                                                             view,
                                                             proposal,
                                                             lightProbability);
    if (queued != rhi::RasterStatus::Ready)
    {
        return queued;
    }
    ReferenceBsdfSample sample;
    if (SampleReferenceBsdf(bsdf,
                            normal,
                            view,
                            {ReferenceRandom(job, path, Dimension(job, path, BSDF_LOBE_SLOT)),
                             ReferenceRandom(job, path, Dimension(job, path, BSDF_FIRST_SLOT)),
                             ReferenceRandom(job, path, Dimension(job, path, BSDF_SECOND_SLOT))},
                            sample,
                            MakeMaterialRandomContext(job, path)) != rhi::RasterStatus::Ready)
    {
        return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidBsdfSample);
    }
    // Transmission continues on the far side of the geometric surface; every other lobe stays on the viewer's.
    const float32 side = math::Dot(scatteringNormal, sample.Direction);
    const bool isTransmitted = sample.IsSpecular == true && math::Dot(normal, sample.Direction) < 0;
    if (sample.Accepted == false || (isTransmitted == true ? side >= 0 : side <= 0))
    {
        ++inoutInfo.NullBsdfSamples;
        return rhi::RasterStatus::Ready;
    }
    if (sample.IsSpecular == true || bsdf.Scattering.Model == content::scene_format::MaterialModel::CoatedDiffuse)
    {
        path.Throughput = math::Hadamard(path.Throughput, sample.Weight);
    }
    else
    {
        path.Throughput = math::Hadamard(path.Throughput, sample.Evaluation.Value) *
                          (math::Dot(normal, sample.Direction) / sample.Evaluation.Pdf);
    }
    if (ValidContribution(path.Throughput) == false)
    {
        return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidThroughput);
    }
    if (Positive(path.Throughput) == false)
    {
        return rhi::RasterStatus::Ready;
    }
    path.PreviousPoint = point;
    path.PreviousPdf = sample.MisPdf;
    path.IsPreviousSpecular = sample.IsSpecular;
    path.PreviousReceiver = frame;
    path.PreviousTargetTotal = targetTotal;
    ++path.Depth;
    if (path.Depth >= job.Description.RouletteDepth)
    {
        const auto survival =
            math::Clamp(math::Max(math::Max(path.Throughput.X, path.Throughput.Y), path.Throughput.Z), 0.05F, 0.95F);
        if (ReferenceRandom(job, path, Dimension(job, path, ROULETTE_SLOT)) >= survival)
        {
            ++inoutInfo.RouletteTerminations;
            return rhi::RasterStatus::Ready;
        }
        path.Throughput = path.Throughput / survival;
    }
    const auto origin = Offset(point, triangle, sample.Direction);
    if (math::IsFinite(origin) == false)
    {
        return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidRayOrigin);
    }
    path.Next = ReferenceRay(origin, sample.Direction, job.Description.SecondaryDistance);
    path.HasNext = true;
    path.IsActive = true;
    return ValidContribution(path.Emission) && ValidContribution(path.Direct) && ValidContribution(path.Indirect)
               ? rhi::RasterStatus::Ready
               : FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidRadiance);
}
namespace
{
rhi::RasterStatus
TraceReferenceProbePath(const ReferenceJob& job, ReferenceCaptureInfo& inoutInfo, ReferencePath& inoutPath) noexcept
{
    ReferencePath& path = inoutPath;
    while (path.IsActive == true)
    {
        rhi::RayResult hit;
        if (TraceReferenceOracle(job.Scene, path.Ray, hit) != rhi::RasterStatus::Ready ||
            PrepareReferenceVertex(job, inoutInfo, path, hit) != rhi::RasterStatus::Ready)
        {
            return rhi::RasterStatus::Failed;
        }
        for (uint32 shadow = 0; shadow < path.ShadowCount; ++shadow)
        {
            rhi::RayResult visibility;
            if (TraceReferenceOracle(job.Scene, path.Shadows[shadow], visibility) != rhi::RasterStatus::Ready)
            {
                return rhi::RasterStatus::Failed;
            }
            if (visibility.Hit == 0)
            {
                path.Indirect = path.Indirect + path.PendingDirects[shadow];
            }
        }
        if (path.HasNext == false)
        {
            break;
        }
        path.Ray = path.Next;
    }
    return rhi::RasterStatus::Ready;
}
} // namespace

rhi::RasterStatus EvaluateReferenceDiffuseProbe(const ReferenceJob& job,
                                                ReferenceCaptureInfo& inoutInfo,
                                                const ReferenceProbeAddress& address,
                                                math::Vector3& outIrradiance) noexcept
{
    // The probe's first event is a virtual receiver without a stored frame for the resampled BSDF-hit density.
    if (IsResampled(job) == true)
    {
        return rhi::RasterStatus::Unsupported;
    }
    const uint32 triangleIndex = address.TriangleIndex;
    const uint32 sampleIndex = address.SampleIndex;
    const ReferenceTriangle& triangle = job.Scene.Triangles[triangleIndex];
    const math::Vector3 pointWorld = Position(triangle, 1.F / 3.F, 1.F / 3.F);
    const math::Vector3 normalWorld = OrientReferenceGeometricNormal(triangle, 1.F / 3.F, 1.F / 3.F);
    ReferencePath path;
    path.Pixel = triangleIndex;
    path.Sample = sampleIndex;
    // At a virtual white diffuse receiver, cosine sampling turns the irradiance
    // integral into PI times incoming radiance. Reuse the reference MIS/BSDF
    // transport after this first event. A selected-light bake includes analytic direct here.
    const usize lights = GetReferenceLightCount(job);
    const ReferenceLightChoice lightChoice = SelectReferenceLight(job, path, lights);
    const ReferenceIncidentLight proposal = SampleReferenceIncidentLight(job, path, lightChoice, pointWorld);
    if (address.ShouldIncludeAnalyticDirect == true && proposal.HasFailed == true)
    {
        return rhi::RasterStatus::Failed;
    }
    const float32 lightProbability = EvaluateSampledLightProbability(job, lightChoice, proposal, pointWorld, lights);
    math::Vector3 directIrradiance;
    const float32 cosine = math::Max(math::Dot(normalWorld, proposal.DirectionWorld), 0.F);
    if ((proposal.IsDelta == false || address.ShouldIncludeAnalyticDirect == true) && lightProbability > 0 &&
        cosine > 0)
    {
        math::Vector3 originWorld = Offset(pointWorld, triangle, proposal.DirectionWorld);
        math::Vector3 directionWorld = proposal.DirectionWorld;
        float32 distance = proposal.Distance;
        if (proposal.HasFiniteEndpoint == true)
        {
            math::Vector3 endpointWorld = proposal.PositionWorld;
            if (proposal.IsDelta == false &&
                math::TryOffsetRayOrigin(
                    {proposal.PositionWorld, proposal.ErrorWorld, proposal.NormalWorld, -directionWorld},
                    endpointWorld) != math::MathStatus::Success)
            {
                return rhi::RasterStatus::Failed;
            }
            const math::Vector3 deltaWorld = endpointWorld - originWorld;
            distance = math::Length(deltaWorld);
            if (distance <= 0)
            {
                return rhi::RasterStatus::Failed;
            }
            directionWorld = deltaWorld / distance;
            distance = math::NextFloatDown(distance);
        }
        rhi::RayInput shadow = ReferenceRay(originWorld, directionWorld, distance);
        shadow.VisibilityOnly = 1;
        rhi::RayResult visibility;
        if (TraceReferenceOracle(job.Scene, shadow, visibility) != rhi::RasterStatus::Ready)
        {
            return rhi::RasterStatus::Failed;
        }
        if (visibility.Hit == 0)
        {
            const float32 weight =
                proposal.IsDelta == true ? 1 : ReferencePowerWeight(lightProbability, cosine / math::kPiF);
            directIrradiance = proposal.Radiance * (cosine / lightProbability * weight);
        }
    }
    const float32 radius = math::Sqrt(ReferenceRandom(job, path, 0));
    const float32 angle = math::kTauF * ReferenceRandom(job, path, 1);
    const float32 cosineSample = math::Sqrt(math::Max(0.F, 1 - radius * radius));
    const math::Vector3 upWorld = math::Abs(normalWorld.Y) > .9F ? math::Vector3{1, 0, 0} : math::Vector3{0, 1, 0};
    const math::Vector3 tangentWorld = ReferenceNormalize(math::Cross(upWorld, normalWorld));
    const math::Vector3 bitangentWorld = math::Cross(normalWorld, tangentWorld);
    const math::Vector3 directionWorld =
        ReferenceNormalize(tangentWorld * (radius * math::Cos(angle)) + bitangentWorld * (radius * math::Sin(angle)) +
                           normalWorld * cosineSample);
    path.Depth = 1;
    path.Throughput = {math::kPiF, math::kPiF, math::kPiF};
    path.PreviousPoint = pointWorld;
    path.PreviousPdf = cosineSample / math::kPiF;
    path.Ray =
        ReferenceRay(Offset(pointWorld, triangle, directionWorld), directionWorld, job.Description.SecondaryDistance);
    if (TraceReferenceProbePath(job, inoutInfo, path) != rhi::RasterStatus::Ready)
    {
        return rhi::RasterStatus::Failed;
    }
    const math::Vector3 irradiance = directIrradiance + path.Direct + path.Indirect;
    if (ValidContribution(irradiance) == false)
    {
        return rhi::RasterStatus::Failed;
    }
    outIrradiance = irradiance;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus TraceReferenceGiProbe(const ReferenceJob& job,
                                        ReferenceCaptureInfo& inoutInfo,
                                        const ReferenceProbeAddress& address,
                                        math::Vector3 positionWorld,
                                        math::Vector3 directionWorld,
                                        math::Vector3& outRadiance) noexcept
{
    ReferencePath path;
    path.Pixel = address.TriangleIndex;
    path.Sample = address.SampleIndex;
    // The probe is a virtual receiver. At least one material vertex must scatter the
    // selected analytic sources before their energy can reach it.
    path.Depth = 1;
    path.PreviousPoint = positionWorld;
    path.Ray = ReferenceRay(positionWorld, directionWorld, job.Description.SecondaryDistance);
    const rhi::RasterStatus status = TraceReferenceProbePath(job, inoutInfo, path);
    const math::Vector3 radiance = path.Indirect;
    if (status != rhi::RasterStatus::Ready || ValidContribution(radiance) == false)
    {
        return rhi::RasterStatus::Failed;
    }
    outRadiance = radiance;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::graphics::renderer::internal
