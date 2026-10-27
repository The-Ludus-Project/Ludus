#include "internal/reference_data.h"

#include <ludus/foundation/math/status.hpp>

// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, "Physically Based
// Rendering: From Theory to Implementation", 4th edition (2023), section 13.4,
// https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer:
// retain one light proposal/continuation, same-measure power MIS for both
// emitter/environment hits and next-event estimation, and compensated roulette.
// This RGB, fixed-mip, finite-range reference uses CPU transport with explicit
// asynchronous native query batches; it has no denoising, wavefront scheduler,
// path regularization, spectral/transmission model or hidden frame readback.
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
usize LightCount(const ReferenceJob& job) noexcept
{
    return job.Description.LightCount + job.Scene.Emitters.GetSize() +
           ((job.EnvironmentOrNull != nullptr || Positive(job.Description.Environment)) ? 1U : 0U);
}
math::Vector3 Position(const ReferenceTriangle& triangle, float32 u, float32 v) noexcept
{
    return triangle.Positions[0] * (1 - u - v) + triangle.Positions[1] * u + triangle.Positions[2] * v;
}
math::Vector2 Uv(const ReferenceTriangle& triangle, float32 u, float32 v) noexcept
{
    return triangle.Uvs[0] * (1 - u - v) + triangle.Uvs[1] * u + triangle.Uvs[2] * v;
}
// Solid-angle density of a point sampled uniformly on `triangle` after the light
// proposal chose that triangle with probability `selection`.
float32 AreaPdf(const ReferenceTriangle& triangle, math::Vector3 from, math::Vector3 point, float32 selection) noexcept
{
    const auto delta = point - from;
    const auto distanceSquared = math::Dot(delta, delta);
    const auto direction = ReferenceNormalize(delta);
    const auto cosine = math::Abs(math::Dot(triangle.GeometricNormal, -direction));
    if (selection <= 0 || triangle.Area <= 0 || cosine <= 0)
    {
        return 0;
    }
    return selection * distanceSquared / (triangle.Area * cosine);
}
// The uniform proposal picks the emitter category with probability emitters / lights;
// the power-weighted CDF then picks one emitter triangle. The GPU kernels use this order.
float32 ComputeEmitterSelection(const ReferenceJob& job, usize emitter, usize lights) noexcept
{
    if (lights == 0)
    {
        return 0;
    }
    return GetReferenceEmitterProbability(job.Scene, emitter) *
           (float32(job.Scene.Emitters.GetSize()) / float32(lights));
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
struct LightProposal final
{
    math::Vector3 Direction{}, Radiance{}, Point{}, Error{}, Normal{};
    float32 Distance = 0, Pdf = 0;
    bool Delta = false, FiniteEndpoint = false;
};
LightProposal SampleLight(const ReferenceJob& job, const ReferencePath& path, math::Vector3 point) noexcept
{
    LightProposal result;
    const auto count = LightCount(job);
    if (count == 0)
    {
        return result;
    }
    const uint16 dimension = ReferenceBounceDimension(path.Depth, 0);
    // Use the owning unbiased bounded mapper for discrete emitter selection;
    // float multiplication/truncation would give unequal selector probabilities.
    uint32 selected = 0;
    if (math::TrySampleBounded(job.Key, {path.Pixel, path.Sample, dimension}, static_cast<uint32>(count), selected) !=
        math::MathStatus::Success)
    {
        result.Pdf = -1;
        return result;
    }
    if (selected < job.Description.LightCount)
    {
        const auto& light = job.Lights[selected];
        result.Delta = true;
        result.Pdf = 1 / float32(count);
        result.Radiance = light.Color * light.Intensity;
        if (light.Kind == LightKind::Directional)
        {
            result.Direction = -light.Direction;
            result.Distance = job.Description.SecondaryDistance;
        }
        else
        {
            const auto delta = light.Position - point;
            const auto square = math::Dot(delta, delta);
            if (square <= 0)
            {
                result.Pdf = -1;
                return result;
            }
            result.Distance = math::Sqrt(square);
            result.Direction = delta / result.Distance;
            result.Point = light.Position;
            result.FiniteEndpoint = true;
            if (result.Distance >= light.Range || result.Distance > job.Description.SecondaryDistance)
            {
                result.Radiance = {};
                return result;
            }
            const auto ratio = result.Distance / light.Range;
            const auto window = math::Max(0.0F, 1 - ratio * ratio * ratio * ratio);
            float32 angular = 1;
            if (light.Kind == LightKind::Spot)
            {
                const auto cosine = math::Dot(light.Direction, -result.Direction);
                const auto t = math::Clamp((cosine - light.OuterCos) / (light.InnerCos - light.OuterCos), 0.0F, 1.0F);
                angular = t * t;
            }
            if (light.Kind == LightKind::AreaOneSided || light.Kind == LightKind::AreaTwoSided)
            {
                const float32 emitterCosine = math::Dot(light.Direction, -result.Direction);
                angular =
                    light.Kind == LightKind::AreaTwoSided ? math::Abs(emitterCosine) : math::Max(emitterCosine, 0.F);
            }
            result.Radiance = result.Radiance * (window * window * angular / math::Max(square, 1e-4F));
        }
        return result;
    }
    if (usize(selected) - job.Description.LightCount < job.Scene.Emitters.GetSize())
    {
        if (job.Scene.EmitterCdf.GetSize() != job.Scene.Emitters.GetSize())
        {
            result.Pdf = -1;
            return result;
        }
        const uint32 total = job.Scene.EmitterCdf[job.Scene.EmitterCdf.GetSize() - 1];
        if (total == 0)
        {
            return result; // No emitter carries power; the proposal contributes nothing.
        }
        uint32 value = 0;
        if (math::TrySampleBounded(
                job.Key,
                {path.Pixel, path.Sample, ReferenceBounceDimension(path.Depth, LUDUS_PATH_RANDOM_EMITTER_SLOT)},
                total,
                value) != math::MathStatus::Success)
        {
            result.Pdf = -1;
            return result;
        }
        const usize emitter = SelectReferenceEmitter(job.Scene, value);
        const auto& triangle = job.Scene.Triangles[job.Scene.Emitters[emitter]];
        const float32 root = math::Sqrt(ReferenceRandom(job, path, ReferenceBounceDimension(path.Depth, 1)));
        const float32 u = root * (1 - ReferenceRandom(job, path, ReferenceBounceDimension(path.Depth, 2)));
        const auto v = root - u;
        result.Point = Position(triangle, u, v);
        const auto delta = result.Point - point;
        result.Distance = math::Length(delta);
        if (result.Distance <= 0)
        {
            result.Pdf = -1;
            return result;
        }
        result.Direction = delta / result.Distance;
        result.Error = triangle.Error;
        result.Normal = triangle.GeometricNormal;
        result.FiniteEndpoint = true;
        result.Pdf = AreaPdf(triangle, point, result.Point, ComputeEmitterSelection(job, emitter, count));
        const auto& instance = job.Scene.Instances[triangle.Instance];
        if ((instance.Surface->IsEmissionTwoSided == true ||
             math::Dot(triangle.GeometricNormal, -result.Direction) > 0) &&
            result.Distance <= job.Description.SecondaryDistance &&
            ReferenceOpacity(instance, Uv(triangle, u, v), InterpolateReferenceColor(triangle, u, v).W))
        {
            result.Radiance = SampleReferenceEmission(instance, Uv(triangle, u, v));
        }
        return result;
    }
    if (job.EnvironmentOrNull != nullptr)
    {
        const EnvironmentSample sample =
            SampleEnvironment(*job.EnvironmentOrNull,
                              ReferenceRandom(job, path, ReferenceBounceDimension(path.Depth, 1)),
                              ReferenceRandom(job, path, ReferenceBounceDimension(path.Depth, 2)));
        result.Direction = sample.DirectionWorld;
        result.Radiance = sample.Radiance;
        result.Pdf = sample.Pdf / static_cast<float32>(count);
        result.Distance = job.Description.SecondaryDistance;
        return result;
    }
    const float32 z = 1 - 2 * ReferenceRandom(job, path, ReferenceBounceDimension(path.Depth, 1));
    const auto radius = math::Sqrt(math::Max(0.0F, 1 - z * z));
    const float32 angle = math::kTauF * ReferenceRandom(job, path, ReferenceBounceDimension(path.Depth, 2));
    result.Direction = {radius * math::Cos(angle), radius * math::Sin(angle), z};
    result.Distance = job.Description.SecondaryDistance;
    result.Radiance = job.Description.Environment;
    result.Pdf = 1 / (float32(count) * 4 * math::kPiF);
    return result;
}
} // namespace
rhi::RasterStatus PrepareReferenceVertex(const ReferenceJob& job,
                                         ReferenceCaptureInfo& inoutInfo,
                                         ReferencePath& inoutPath,
                                         const rhi::RayResult& hit) noexcept
{
    ReferencePath& path = inoutPath;
    path.HasShadow = path.HasNext = false;
    path.Active = false;
    const auto lights = LightCount(job);
    if (hit.Hit == 0)
    {
        const float32 environmentPdf =
            job.EnvironmentOrNull != nullptr
                ? EvaluateEnvironmentPdf(*job.EnvironmentOrNull, ReferenceDirection(path.Ray))
                : 1 / (4 * math::kPiF);
        const float32 lightPdf = lights == 0 ? 0 : environmentPdf / static_cast<float32>(lights);
        const auto weight = path.Depth == 0 ? 1 : ReferencePowerWeight(path.PreviousPdf, lightPdf);
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
    if (Positive(surface.Emission) == true &&
        (surface.IsEmissionTwoSided == true || math::Dot(triangle.GeometricNormal, view) > 0))
    {
        // An emitter missing from the proposal list cannot be light-sampled; BSDF sampling then owns it.
        usize emitter = 0;
        const float32 selection =
            FindReferenceEmitter(job.Scene, static_cast<uint32>(instance.FirstTriangle + hit.Primitive), emitter) ==
                    true
                ? ComputeEmitterSelection(job, emitter, lights)
                : 0;
        const float32 pdf = AreaPdf(triangle, path.PreviousPoint, point, selection);
        if (math::IsFinite(pdf) == false || pdf < 0)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidEmitterPdf);
        }
        const auto weight = path.Depth == 0 ? 1 : ReferencePowerWeight(path.PreviousPdf, pdf);
        AddTransport(path,
                     math::Hadamard(path.Throughput, SampleReferenceEmission(instance, Uv(triangle, hit.U, hit.V))) *
                         weight,
                     path.Depth,
                     true);
    }
    if (path.Depth >= job.Description.MaxDepth || math::Dot(triangle.GeometricNormal, view) <= 0)
    {
        return ValidContribution(path.Emission) && ValidContribution(path.Direct) && ValidContribution(path.Indirect)
                   ? rhi::RasterStatus::Ready
                   : FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidRadiance);
    }
    math::Vector3 normal = ReferenceNormalize(triangle.Normals[0] * (1 - hit.U - hit.V) + triangle.Normals[1] * hit.U +
                                              triangle.Normals[2] * hit.V);
    math::Vector4 texel = SampleReferenceTexture(instance, Uv(triangle, hit.U, hit.V));
    if (surface.AreAuthoredInputsEnabled == true)
    {
        const math::Vector4 color = InterpolateReferenceColor(triangle, hit.U, hit.V);
        texel = math::Hadamard(texel, color);
    }
    ReferenceBsdf bsdf = surface.Bsdf;
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
    const auto proposal = SampleLight(job, path, point);
    if (math::IsFinite(proposal.Pdf) == false || proposal.Pdf < 0 || ValidContribution(proposal.Radiance) == false)
    {
        return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidLightSample);
    }
    const auto cosine = math::Dot(normal, proposal.Direction);
    if (proposal.Pdf > 0 && Positive(proposal.Radiance) && cosine > 0 &&
        math::Dot(triangle.GeometricNormal, proposal.Direction) > 0)
    {
        ReferenceBsdfEvaluation evaluation;
        if (EvaluateReferenceBsdf(bsdf, normal, view, proposal.Direction, evaluation) != rhi::RasterStatus::Ready)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidBsdfEvaluation);
        }
        const auto weight = proposal.Delta ? 1 : ReferencePowerWeight(proposal.Pdf, evaluation.Pdf);
        path.PendingDirect = math::Hadamard(path.Throughput, math::Hadamard(evaluation.Value, proposal.Radiance)) *
                             (cosine * weight / proposal.Pdf);
        if (ValidContribution(path.PendingDirect) == false)
        {
            return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidDirectContribution);
        }
        if (Positive(path.PendingDirect))
        {
            const auto start = Offset(point, triangle, proposal.Direction);
            auto direction = proposal.Direction;
            auto distance = proposal.Distance;
            if (proposal.FiniteEndpoint)
            {
                auto end = proposal.Point;
                if (proposal.Delta == false &&
                    math::TryOffsetRayOrigin({proposal.Point, proposal.Error, proposal.Normal, -direction}, end) !=
                        math::MathStatus::Success)
                {
                    return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidShadowSegment);
                }
                const auto delta = end - start;
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
            path.Shadow = ReferenceRay(start, direction, distance);
            path.Shadow.VisibilityOnly = 1;
            path.DirectDepth = path.Depth;
            path.HasShadow = true;
        }
    }
    ReferenceBsdfSample sample;
    if (SampleReferenceBsdf(bsdf,
                            normal,
                            view,
                            {ReferenceRandom(job, path, ReferenceBounceDimension(path.Depth, 3)),
                             ReferenceRandom(job, path, ReferenceBounceDimension(path.Depth, 4)),
                             ReferenceRandom(job, path, ReferenceBounceDimension(path.Depth, 5))},
                            sample) != rhi::RasterStatus::Ready)
    {
        return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidBsdfSample);
    }
    if (sample.Accepted == false || math::Dot(triangle.GeometricNormal, sample.Direction) <= 0)
    {
        ++inoutInfo.NullBsdfSamples;
        return rhi::RasterStatus::Ready;
    }
    path.Throughput = math::Hadamard(path.Throughput, sample.Evaluation.Value) *
                      (math::Dot(normal, sample.Direction) / sample.Evaluation.Pdf);
    if (ValidContribution(path.Throughput) == false)
    {
        return FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidThroughput);
    }
    if (Positive(path.Throughput) == false)
    {
        return rhi::RasterStatus::Ready;
    }
    path.PreviousPoint = point;
    path.PreviousPdf = sample.Evaluation.Pdf;
    ++path.Depth;
    if (path.Depth >= job.Description.RouletteDepth)
    {
        const auto survival =
            math::Clamp(math::Max(math::Max(path.Throughput.X, path.Throughput.Y), path.Throughput.Z), 0.05F, 0.95F);
        if (ReferenceRandom(job, path, ReferenceBounceDimension(path.Depth, 6)) >= survival)
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
    path.HasNext = path.Active = true;
    return ValidContribution(path.Emission) && ValidContribution(path.Direct) && ValidContribution(path.Indirect)
               ? rhi::RasterStatus::Ready
               : FailReferencePath(inoutInfo, path, ReferenceFailure::InvalidRadiance);
}
rhi::RasterStatus EvaluateReferenceDiffuseProbe(const ReferenceJob& job,
                                                ReferenceCaptureInfo& inoutInfo,
                                                const ReferenceProbeAddress& address,
                                                math::Vector3& outIrradiance) noexcept
{
    const uint32 triangleIndex = address.TriangleIndex;
    const uint32 sampleIndex = address.SampleIndex;
    const ReferenceTriangle& triangle = job.Scene.Triangles[triangleIndex];
    const math::Vector3 pointWorld = Position(triangle, 1.F / 3.F, 1.F / 3.F);
    const math::Vector3 normalWorld = triangle.GeometricNormal;
    ReferencePath path;
    path.Pixel = triangleIndex;
    path.Sample = sampleIndex;
    // At a virtual white diffuse receiver, cosine sampling turns the irradiance
    // integral into PI times incoming radiance. Reuse the reference MIS/BSDF
    // transport after this first event; analytic direct remains the raster term.
    const LightProposal proposal = SampleLight(job, path, pointWorld);
    math::Vector3 directIrradiance;
    const float32 cosine = math::Max(math::Dot(normalWorld, proposal.Direction), 0.F);
    if (proposal.Delta == false && proposal.Pdf > 0 && cosine > 0)
    {
        math::Vector3 originWorld = Offset(pointWorld, triangle, proposal.Direction);
        math::Vector3 directionWorld = proposal.Direction;
        float32 distance = proposal.Distance;
        if (proposal.FiniteEndpoint == true)
        {
            math::Vector3 endpointWorld;
            if (math::TryOffsetRayOrigin({proposal.Point, proposal.Error, proposal.Normal, -directionWorld},
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
            directIrradiance =
                proposal.Radiance * (cosine / proposal.Pdf * ReferencePowerWeight(proposal.Pdf, cosine / math::kPiF));
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
    while (path.Active == true)
    {
        rhi::RayResult hit;
        if (TraceReferenceOracle(job.Scene, path.Ray, hit) != rhi::RasterStatus::Ready ||
            PrepareReferenceVertex(job, inoutInfo, path, hit) != rhi::RasterStatus::Ready)
        {
            return rhi::RasterStatus::Failed;
        }
        if (path.HasShadow == true)
        {
            rhi::RayResult visibility;
            if (TraceReferenceOracle(job.Scene, path.Shadow, visibility) != rhi::RasterStatus::Ready)
            {
                return rhi::RasterStatus::Failed;
            }
            if (visibility.Hit == 0)
            {
                path.Indirect = path.Indirect + path.PendingDirect;
            }
        }
        if (path.HasNext == false)
        {
            break;
        }
        path.Ray = path.Next;
    }
    const math::Vector3 irradiance = directIrradiance + path.Direct + path.Indirect;
    if (ValidContribution(irradiance) == false)
    {
        return rhi::RasterStatus::Failed;
    }
    outIrradiance = irradiance;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::graphics::renderer::internal
