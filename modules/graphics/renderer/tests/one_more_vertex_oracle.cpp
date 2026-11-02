#include "one_more_vertex_oracle.h"

#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/random_domains.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>

// Thanks to Garcia-Pueyo, Monzon, Jarabo and Muñoz, "One-more-vertex Next-Event Estimation with Hierarchical Geometry
// Sampling", CGF 2026, https://doi.org/10.1111/cgf.70539, equations 2, 8-9, 14, 17, 19-21 and Table 1. Independent
// CPU implementation of the same Cholesky/quadrature specialization documented beside the template's shaders.
namespace ludus::graphics::renderer::testing
{
namespace
{
using namespace foundation;
constexpr uint32 NORMAL_QUADRATURE_COUNT = 12;
constexpr float32 AZIMUTH_STEP = 2.399963229728653F;
constexpr float32 DEFENSIVE_WEIGHT = 0.000001F;
constexpr float32 MAXIMUM_CONCENTRATION = 10000;
constexpr float32 GAUSSIAN_OFFSET_WEIGHT = 0.606530659712633F;
constexpr float32 FAR_SOLID_ANGLE = 0.1F;
constexpr float32 MAXIMUM_UNIT_SAMPLE = 0.9999999403953552F;
constexpr float32 MAXIMUM_IMPORTANCE = 1e30F;
constexpr float32 MAXIMUM_DISTANCE_WORLD = 3.402823466e38F;
constexpr math::Vector3 LUMINANCE{0.2126F, 0.7152F, 0.0722F};
float32 Exp32(float32 value) noexcept
{
    return static_cast<float32>(math::Exp(static_cast<float64>(value)));
}
float32 Log32(float32 value) noexcept
{
    return static_cast<float32>(math::Log(static_cast<float64>(value)));
}
math::Vector3 Normalize(math::Vector3 value) noexcept
{
    const float32 length = math::Length(value);
    return length > 0 ? value / length : math::Vector3{};
}
math::Vector3 CrossMoment(math::Vector3 value) noexcept
{
    return {value.X * value.Y, value.X * value.Z, value.Y * value.Z};
}
math::Vector3 EvaluateEmission(math::Vector3 emission,
                               math::Vector3 normalWorld,
                               math::Vector3 directionWorld,
                               bool isTwoSided) noexcept
{
    return isTwoSided == true || math::Dot(normalWorld, directionWorld) > 0 ? emission : math::Vector3{};
}
float32 ComputeConcentration(float32 meanLength) noexcept
{
    const float32 r = math::Min(meanLength, 1 - DEFENSIVE_WEIGHT);
    return math::Min(MAXIMUM_CONCENTRATION, r * (3 - r * r) / math::Max(1 - r * r, DEFENSIVE_WEIGHT));
}
float32 EvaluateVmf(float32 cosine, float32 concentration) noexcept
{
    return concentration < DEFENSIVE_WEIGHT ? 1 / (4 * math::kPiF)
                                            : concentration * Exp32(concentration * (cosine - 1)) /
                                                  (2 * math::kPiF * (1 - Exp32(-2 * concentration)));
}
float32 EvaluateMasking(float32 cosine, float32 alphaSquared) noexcept
{
    cosine = math::Abs(cosine);
    return 2 * cosine /
           math::Max(cosine + math::Sqrt(alphaSquared + (1 - alphaSquared) * cosine * cosine), DEFENSIVE_WEIGHT);
}
float32 EvaluateNormalIntegral(math::Vector3 normalWorld,
                               float32 concentration,
                               math::Vector3 sensorWorld,
                               math::Vector3 emitterWorld) noexcept
{
    const math::Vector3 tangentWorld = Normalize(
        math::Cross(math::Abs(normalWorld.Z) < 0.5F ? math::Vector3{0, 0, 1} : math::Vector3{0, 1, 0}, normalWorld));
    const math::Vector3 bitangentWorld = math::Cross(normalWorld, tangentWorld);
    float32 result = 0;
    for (uint32 index = 0; index < NORMAL_QUADRATURE_COUNT; ++index)
    {
        const float32 u = (static_cast<float32>(index) + 0.5F) / NORMAL_QUADRATURE_COUNT;
        const float32 z = concentration < DEFENSIVE_WEIGHT
                              ? 2 * u - 1
                              : 1 + Log32(u + (1 - u) * Exp32(-2 * concentration)) / concentration;
        const float32 radius = math::Sqrt(math::Max(0.0F, 1 - z * z));
        const float32 angle = static_cast<float32>(index) * AZIMUTH_STEP;
        const math::Vector3 sampleWorld =
            normalWorld * z + (tangentWorld * math::Cos(angle) + bitangentWorld * math::Sin(angle)) * radius;
        result += math::Max(0.0F, math::Dot(sampleWorld, sensorWorld)) *
                  math::Max(0.0F, math::Dot(sampleWorld, emitterWorld));
    }
    return result / NORMAL_QUADRATURE_COUNT;
}
rhi::RayInput MakeRay(math::Vector3 originWorld, math::Vector3 directionWorld, float32 maximumDistanceWorld) noexcept
{
    rhi::RayInput result;
    result.Origin[0] = originWorld.X;
    result.Origin[1] = originWorld.Y;
    result.Origin[2] = originWorld.Z;
    result.Direction[0] = directionWorld.X;
    result.Direction[1] = directionWorld.Y;
    result.Direction[2] = directionWorld.Z;
    result.MaxDistance = maximumDistanceWorld;
    return result;
}
uint32 FindTriangle(const internal::ReferenceSceneData& scene, const rhi::RayResult& hit) noexcept
{
    for (uint32 index = 0; index < scene.Triangles.GetSize(); ++index)
    {
        const internal::ReferenceTriangle& triangle = scene.Triangles[index];
        if (triangle.Instance == hit.Instance && triangle.Primitive == hit.Primitive)
        {
            return index;
        }
    }
    LUDUS_ASSERT(false && "A successful oracle hit must name an admitted triangle");
    return 0;
}
} // namespace

OneMoreVertexOracle::Point OneMoreVertexOracle::loadPoint(const internal::ReferenceSceneData& scene,
                                                          uint32 triangle,
                                                          float32 u,
                                                          float32 v) const noexcept
{
    const internal::ReferenceTriangle& geometry = scene.Triangles[triangle];
    const internal::ReferenceSurfaceData& surface = *scene.Instances[geometry.Instance].Surface;
    Point result;
    result.Triangle = triangle;
    result.PositionWorld = geometry.Positions[0] * (1 - u - v) + geometry.Positions[1] * u + geometry.Positions[2] * v;
    result.NormalWorld =
        Normalize(geometry.Normals[0] * (1 - u - v) + geometry.Normals[1] * u + geometry.Normals[2] * v);
    result.GeometricNormalWorld = geometry.GeometricNormal;
    const internal::ReferenceInstance& instance = scene.Instances[geometry.Instance];
    const math::Vector2 texcoordUv = geometry.Uvs[0] * (1 - u - v) + geometry.Uvs[1] * u + geometry.Uvs[2] * v;
    const math::Vector4 color = internal::InterpolateReferenceColor(geometry, u, v);
    const math::Vector4 texel = math::Hadamard(internal::SampleReferenceTexture(instance, texcoordUv), color);
    result.Bsdf = surface.Bsdf;
    result.Bsdf.BaseColor = math::Hadamard(result.Bsdf.BaseColor, math::Vector3{texel.X, texel.Y, texel.Z});
    if (surface.HasSurfaceMap == true)
    {
        SurfaceParameters parameters;
        if (DecodeSurfaceParameters(internal::SampleReferenceSurfaceMap(instance, texcoordUv),
                                    result.Bsdf.Metallic,
                                    result.Bsdf.Roughness,
                                    surface.NormalScale,
                                    parameters) == rhi::RasterStatus::Ready)
        {
            result.Bsdf.Metallic = parameters.Metallic;
            result.Bsdf.Roughness = parameters.Roughness;
            result.NormalWorld =
                internal::ApplyReferenceNormal(geometry, surface, result.NormalWorld, parameters.NormalTangent, u, v);
        }
    }
    result.Emission = internal::SampleReferenceEmission(instance, texcoordUv);
    result.HasCoverage = internal::ReferenceOpacity(instance, texcoordUv, color.W);
    result.IsEmissionTwoSided = surface.IsEmissionTwoSided;
    return result;
}
bool OneMoreVertexOracle::Build(const internal::ReferenceSceneData& scene) noexcept
{
    if (mStatistics.TryResize(scene.Nodes.GetSize()) == false ||
        mLeafIndices.TryResize(scene.Triangles.GetSize()) == false)
    {
        return false;
    }
    mOriginWorld = (scene.Bounds.Min + scene.Bounds.Max) * 0.5F;
    for (usize remaining = scene.Nodes.GetSize(); remaining > 0; --remaining)
    {
        const usize index = remaining - 1;
        const internal::ReferenceBvhNode& node = scene.Nodes[index];
        Statistics sum;
        if (node.Count > 0)
        {
            for (uint32 primitive = 0; primitive < node.Count; ++primitive)
            {
                const uint32 triangleIndex = scene.Order[node.First + primitive];
                const internal::ReferenceTriangle& triangle = scene.Triangles[triangleIndex];
                const Point point = loadPoint(scene, triangleIndex, 1.0F / 3, 1.0F / 3);
                const math::Vector3 aWorld = triangle.Positions[0] - mOriginWorld;
                const math::Vector3 bWorld = triangle.Positions[1] - mOriginWorld;
                const math::Vector3 cWorld = triangle.Positions[2] - mOriginWorld;
                const math::Vector3 positionsWorld = aWorld + bWorld + cWorld;
                sum.AreaWorld += triangle.Area;
                sum.PositionSumWorld = sum.PositionSumWorld + positionsWorld * (triangle.Area / 3);
                sum.DiagonalSumWorld =
                    sum.DiagonalSumWorld +
                    (math::Hadamard(aWorld, aWorld) + math::Hadamard(bWorld, bWorld) + math::Hadamard(cWorld, cWorld) +
                     math::Hadamard(positionsWorld, positionsWorld)) *
                        (triangle.Area / 12);
                sum.CrossSumWorld = sum.CrossSumWorld + (CrossMoment(aWorld) + CrossMoment(bWorld) +
                                                         CrossMoment(cWorld) + CrossMoment(positionsWorld)) *
                                                            (triangle.Area / 12);
                sum.NormalSumWorld = sum.NormalSumWorld + triangle.GeometricNormal * triangle.Area;
                sum.ColorSum = sum.ColorSum + point.Bsdf.BaseColor * triangle.Area;
                const content::scene_format::MaterialModel model = point.Bsdf.Scattering.Model;
                sum.MetallicSum += triangle.Area *
                                   (model == content::scene_format::MaterialModel::Conductor ? 1 : point.Bsdf.Metallic);
                const float32 alpha = model == content::scene_format::MaterialModel::MetallicRoughness
                                          ? point.Bsdf.Roughness * point.Bsdf.Roughness
                                          : point.Bsdf.Scattering.AlphaU;
                sum.AlphaSquaredSum += triangle.Area * alpha * alpha;
                sum.SpecularitySum +=
                    triangle.Area * (model == content::scene_format::MaterialModel::Diffuse ? 0 : 0.04F);
                mLeafIndices[triangleIndex] = static_cast<uint32>(index);
            }
        }
        else
        {
            const Statistics& left = mStatistics[node.Left];
            const Statistics& right = mStatistics[node.Right];
            sum.AreaWorld = left.AreaWorld + right.AreaWorld;
            sum.PositionSumWorld = left.PositionSumWorld + right.PositionSumWorld;
            sum.DiagonalSumWorld = left.DiagonalSumWorld + right.DiagonalSumWorld;
            sum.CrossSumWorld = left.CrossSumWorld + right.CrossSumWorld;
            sum.NormalSumWorld = left.NormalSumWorld + right.NormalSumWorld;
            sum.ColorSum = left.ColorSum + right.ColorSum;
            sum.MetallicSum = left.MetallicSum + right.MetallicSum;
            sum.AlphaSquaredSum = left.AlphaSquaredSum + right.AlphaSquaredSum;
            sum.SpecularitySum = left.SpecularitySum + right.SpecularitySum;
        }
        if (sum.AreaWorld <= 0)
        {
            return false;
        }
        const math::Vector3 meanWorld = sum.PositionSumWorld / sum.AreaWorld;
        const math::Vector3 diagonalWorld = sum.DiagonalSumWorld / sum.AreaWorld - math::Hadamard(meanWorld, meanWorld);
        const math::Vector3 crossWorld = sum.CrossSumWorld / sum.AreaWorld - CrossMoment(meanWorld);
        if (math::TryComputeGaussianAxes(diagonalWorld, crossWorld, sum.AxesWorld) == false)
        {
            sum.AxesWorld.Axis0 = {math::Sqrt(math::Max(diagonalWorld.X, 0.0F)), 0, 0};
            sum.AxesWorld.Axis1 = {0, math::Sqrt(math::Max(diagonalWorld.Y, 0.0F)), 0};
            sum.AxesWorld.Axis2 = {0, 0, math::Sqrt(math::Max(diagonalWorld.Z, 0.0F))};
        }
        mStatistics[index] = sum;
    }
    return true;
}

float32 OneMoreVertexOracle::evaluateImportance(const internal::ReferenceSceneData& scene,
                                                uint32 nodeIndex,
                                                const Context& context) const noexcept
{
    const Statistics& statistics = mStatistics[nodeIndex];
    const math::Vector3 meanWorld = mOriginWorld + statistics.PositionSumWorld / statistics.AreaWorld;
    const math::Vector3 color = statistics.ColorSum / statistics.AreaWorld;
    const float32 metallic = statistics.MetallicSum / statistics.AreaWorld;
    const float32 normalLength = math::Length(statistics.NormalSumWorld);
    const math::Vector3 normalWorld =
        normalLength > 0 ? statistics.NormalSumWorld / normalLength : math::Vector3{0, 0, 1};
    const float32 concentration = ComputeConcentration(normalLength / statistics.AreaWorld);
    const float32 alphaSquared = math::Max(statistics.AlphaSquaredSum / statistics.AreaWorld, DEFENSIVE_WEIGHT);
    const float32 materialConcentration = 2 / alphaSquared;
    const float32 convolvedConcentration =
        concentration * materialConcentration / (concentration + materialConcentration);
    const math::Vector3 reflectance =
        color * metallic + math::Vector3{1, 1, 1} * ((1 - metallic) * statistics.SpecularitySum / statistics.AreaWorld);
    const auto evaluate = [&](math::Vector3 positionWorld) noexcept -> math::Vector3 {
        const math::Vector3 sensorOffsetWorld = context.Receiver.PositionWorld - positionWorld;
        const math::Vector3 emitterOffsetWorld = context.Emitter.PositionWorld - positionWorld;
        const float32 sensorSquaredWorld = math::Dot(sensorOffsetWorld, sensorOffsetWorld);
        const float32 emitterSquaredWorld = math::Dot(emitterOffsetWorld, emitterOffsetWorld);
        if (sensorSquaredWorld <= 0 || emitterSquaredWorld <= 0)
        {
            return {};
        }
        const math::Vector3 sensorWorld = Normalize(sensorOffsetWorld);
        const math::Vector3 emitterWorld = Normalize(emitterOffsetWorld);
        const math::Vector3 halfWorld = Normalize(sensorWorld + emitterWorld);
        const float32 diffuse = EvaluateNormalIntegral(normalWorld, concentration, sensorWorld, emitterWorld);
        math::Vector3 directional = color * ((1 - metallic) * diffuse / math::kPiF);
        const float32 fresnel = static_cast<float32>(
            math::Pow(static_cast<float64>(1 - math::Max(0.0F, math::Dot(halfWorld, sensorWorld))), 5.0));
        if (statistics.SpecularitySum > 0 || metallic > 0)
        {
            directional =
                directional + (reflectance * (1 - fresnel) + math::Vector3{fresnel, fresnel, fresnel}) *
                                  (0.25F * EvaluateMasking(math::Dot(normalWorld, sensorWorld), alphaSquared) *
                                   EvaluateMasking(math::Dot(normalWorld, emitterWorld), alphaSquared) *
                                   EvaluateVmf(math::Dot(normalWorld, halfWorld), convolvedConcentration));
        }
        ReferenceBsdfEvaluation value;
        if (EvaluateReferenceBsdf(context.Receiver.Bsdf,
                                  context.Receiver.NormalWorld,
                                  context.ViewWorld,
                                  -sensorWorld,
                                  value) != rhi::RasterStatus::Ready)
        {
            return {};
        }
        const math::Vector3 emission = EvaluateEmission(context.Emitter.Emission,
                                                        context.Emitter.GeometricNormalWorld,
                                                        -emitterWorld,
                                                        context.Emitter.IsEmissionTwoSided);
        return math::Hadamard(math::Hadamard(value.Value, directional), emission) *
               (math::Max(0.0F, math::Dot(context.Receiver.NormalWorld, -sensorWorld)) *
                math::Abs(math::Dot(context.Emitter.GeometricNormalWorld, -emitterWorld)) / sensorSquaredWorld /
                emitterSquaredWorld);
    };
    const internal::ReferenceBvhNode& node = scene.Nodes[nodeIndex];
    const math::Vector3 extentWorld = node.Bounds.Max - node.Bounds.Min;
    const float32 radiusSquaredWorld = math::Dot(extentWorld, extentWorld) * 0.25F;
    const math::Vector3 sensorOffsetWorld = context.Receiver.PositionWorld - meanWorld;
    const math::Vector3 emitterOffsetWorld = context.Emitter.PositionWorld - meanWorld;
    const float32 minimumSquaredWorld =
        math::Min(math::Dot(sensorOffsetWorld, sensorOffsetWorld), math::Dot(emitterOffsetWorld, emitterOffsetWorld));
    math::Vector3 value = evaluate(meanWorld);
    if (minimumSquaredWorld <= 0 || math::kPiF * radiusSquaredWorld >= FAR_SOLID_ANGLE * minimumSquaredWorld)
    {
        constexpr usize SPATIAL_AXIS_COUNT = 3;
        const StaticArray<math::Vector3, SPATIAL_AXIS_COUNT> axes{
            {statistics.AxesWorld.Axis0, statistics.AxesWorld.Axis1, statistics.AxesWorld.Axis2}};
        for (const math::Vector3 axisWorld : axes)
        {
            value =
                value + (evaluate(meanWorld - axisWorld) + evaluate(meanWorld + axisWorld)) * GAUSSIAN_OFFSET_WEIGHT;
        }
        value = value / (1 + 6 * GAUSSIAN_OFFSET_WEIGHT);
    }
    float32 importance = math::Dot(value, LUMINANCE);
    importance =
        math::IsFinite(importance) == true ? math::Clamp(importance, 0.0F, MAXIMUM_IMPORTANCE) : MAXIMUM_IMPORTANCE;
    return math::Min(statistics.AreaWorld * (importance + DEFENSIVE_WEIGHT), MAXIMUM_IMPORTANCE);
}
float32 OneMoreVertexOracle::computeLeftProbability(const internal::ReferenceSceneData& scene,
                                                    uint32 node,
                                                    const Context& context) const noexcept
{
    const float32 left = evaluateImportance(scene, scene.Nodes[node].Left, context);
    const float32 right = evaluateImportance(scene, scene.Nodes[node].Right, context);
    const float32 scale = math::Max(left, right);
    const float32 probability = scale > 0 ? (left / scale) / (left / scale + right / scale) : 0.5F;
    return math::Clamp(probability, DEFENSIVE_WEIGHT, 1 - DEFENSIVE_WEIGHT);
}
float32 OneMoreVertexOracle::evaluatePdf(const internal::ReferenceSceneData& scene,
                                         uint32 triangle,
                                         const Context& context) const noexcept
{
    uint32 node = 0;
    float32 probability = 1;
    const uint32 leaf = mLeafIndices[triangle];
    while (scene.Nodes[node].Count == 0)
    {
        const float32 left = computeLeftProbability(scene, node, context);
        const bool isLeft = leaf < scene.Nodes[node].Right;
        probability *= isLeft == true ? left : 1 - left;
        node = isLeft == true ? scene.Nodes[node].Left : scene.Nodes[node].Right;
    }
    return node == leaf ? probability / mStatistics[node].AreaWorld : 0;
}
OneMoreVertexOracle::Point OneMoreVertexOracle::sampleGeometry(const internal::ReferenceSceneData& scene,
                                                               const Context& context,
                                                               math::Vector3 random,
                                                               float32& outPdf) const noexcept
{
    uint32 node = 0;
    float32 probability = 1;
    while (scene.Nodes[node].Count == 0)
    {
        const float32 left = computeLeftProbability(scene, node, context);
        const bool isLeft = random.X < left;
        probability *= isLeft == true ? left : 1 - left;
        random.X = isLeft == true ? random.X / left : (random.X - left) / (1 - left);
        node = isLeft == true ? scene.Nodes[node].Left : scene.Nodes[node].Right;
    }
    const internal::ReferenceBvhNode& leaf = scene.Nodes[node];
    float32 targetArea = math::Min(random.X, MAXIMUM_UNIT_SAMPLE) * mStatistics[node].AreaWorld;
    uint32 triangle = scene.Order[leaf.First];
    for (uint32 primitive = 0; primitive < leaf.Count; ++primitive)
    {
        triangle = scene.Order[leaf.First + primitive];
        targetArea -= scene.Triangles[triangle].Area;
        if (targetArea <= 0)
        {
            break;
        }
    }
    outPdf = probability / mStatistics[node].AreaWorld;
    const float32 root = math::Sqrt(random.Y);
    return loadPoint(scene, triangle, root * (1 - random.Z), root * random.Z);
}
bool OneMoreVertexOracle::isVisible(const internal::ReferenceSceneData& scene,
                                    const Point& from,
                                    const Point& to) const noexcept
{
    const math::Vector3 directionWorld = Normalize(to.PositionWorld - from.PositionWorld);
    math::Vector3 originWorld;
    math::Vector3 endpointWorld;
    if (math::TryOffsetRayOrigin(
            {from.PositionWorld, scene.Triangles[from.Triangle].Error, from.GeometricNormalWorld, directionWorld},
            originWorld) != math::MathStatus::Success ||
        math::TryOffsetRayOrigin(
            {to.PositionWorld, scene.Triangles[to.Triangle].Error, to.GeometricNormalWorld, -directionWorld},
            endpointWorld) != math::MathStatus::Success)
    {
        return false;
    }
    const math::Vector3 offsetWorld = endpointWorld - originWorld;
    const float32 distanceWorld = math::Length(offsetWorld);
    rhi::RayResult hit;
    const rhi::RayInput ray = MakeRay(originWorld, Normalize(offsetWorld), math::NextFloatDown(distanceWorld));
    return internal::TraceReferenceOracle(scene, ray, hit) == rhi::RasterStatus::Ready && hit.Hit == 0;
}
math::Vector3 OneMoreVertexOracle::evaluateConnection(const internal::ReferenceSceneData& scene,
                                                      const Context& context,
                                                      const Point& middle,
                                                      bool shouldTestFirst,
                                                      float32& outBsdfPdf) const noexcept
{
    outBsdfPdf = 0;
    if (middle.HasCoverage == false)
    {
        return {};
    }
    const math::Vector3 firstOffsetWorld = middle.PositionWorld - context.Receiver.PositionWorld;
    const math::Vector3 secondOffsetWorld = context.Emitter.PositionWorld - middle.PositionWorld;
    const float32 firstSquaredWorld = math::Dot(firstOffsetWorld, firstOffsetWorld);
    const float32 secondSquaredWorld = math::Dot(secondOffsetWorld, secondOffsetWorld);
    const math::Vector3 firstWorld = Normalize(firstOffsetWorld);
    const math::Vector3 secondWorld = Normalize(secondOffsetWorld);
    if (firstSquaredWorld <= 0 || secondSquaredWorld <= 0 ||
        math::Dot(context.Receiver.GeometricNormalWorld, firstWorld) <= 0 ||
        math::Dot(middle.GeometricNormalWorld, -firstWorld) <= 0 ||
        math::Dot(middle.GeometricNormalWorld, secondWorld) <= 0)
    {
        return {};
    }
    ReferenceBsdfEvaluation first;
    ReferenceBsdfEvaluation second;
    if (EvaluateReferenceBsdf(context.Receiver.Bsdf,
                              context.Receiver.NormalWorld,
                              context.ViewWorld,
                              firstWorld,
                              first) != rhi::RasterStatus::Ready ||
        EvaluateReferenceBsdf(middle.Bsdf, middle.NormalWorld, -firstWorld, secondWorld, second) !=
            rhi::RasterStatus::Ready)
    {
        return {};
    }
    const float32 jacobian = math::Abs(math::Dot(middle.GeometricNormalWorld, -firstWorld)) / firstSquaredWorld;
    outBsdfPdf = first.Pdf * jacobian;
    if ((shouldTestFirst == true && isVisible(scene, context.Receiver, middle) == false) ||
        isVisible(scene, middle, context.Emitter) == false)
    {
        return {};
    }
    const math::Vector3 emission = EvaluateEmission(context.Emitter.Emission,
                                                    context.Emitter.GeometricNormalWorld,
                                                    -secondWorld,
                                                    context.Emitter.IsEmissionTwoSided);
    return math::Hadamard(math::Hadamard(first.Value, second.Value), emission) *
           (math::Max(0.0F, math::Dot(context.Receiver.NormalWorld, firstWorld)) * jacobian *
            math::Max(0.0F, math::Dot(middle.NormalWorld, secondWorld)) *
            math::Abs(math::Dot(context.Emitter.GeometricNormalWorld, -secondWorld)) / secondSquaredWorld);
}

math::Vector3 OneMoreVertexOracle::Evaluate(const internal::ReferenceSceneData& scene,
                                            const rhi::RayInput& cameraRay,
                                            math::RandomKey key,
                                            uint32 pixel,
                                            uint32 sample,
                                            uint32 estimator,
                                            math::Vector3& outEmission,
                                            math::Vector3& outDirect,
                                            math::Vector3& outIndirect) const noexcept
{
    outEmission = {};
    outDirect = {};
    outIndirect = {};
    rhi::RayResult primary;
    if (internal::TraceReferenceOracle(scene, cameraRay, primary) != rhi::RasterStatus::Ready || primary.Hit == 0)
    {
        return {};
    }
    Context context;
    context.Receiver = loadPoint(scene, FindTriangle(scene, primary), primary.U, primary.V);
    context.ViewWorld = -math::Vector3{cameraRay.Direction[0], cameraRay.Direction[1], cameraRay.Direction[2]};
    outEmission = EvaluateEmission(context.Receiver.Emission,
                                   context.Receiver.GeometricNormalWorld,
                                   context.ViewWorld,
                                   context.Receiver.IsEmissionTwoSided);
    if (scene.Emitters.IsEmpty() == true || math::Dot(context.Receiver.GeometricNormalWorld, context.ViewWorld) <= 0)
    {
        return outEmission;
    }
    const auto random = [&](uint32 dimension) noexcept {
        return math::SampleFloat01(key, {pixel, sample, static_cast<uint16>(dimension)});
    };
    // This oracle samples emitter triangles by area (one-emitter fixtures); the template first chooses emitter groups.
    float32 emitterAreaWorld = 0;
    for (const uint32 triangle : scene.Emitters)
    {
        emitterAreaWorld += scene.Triangles[triangle].Area;
    }
    float32 targetAreaWorld = random(3) * emitterAreaWorld;
    uint32 emitterTriangle = scene.Emitters[0];
    for (const uint32 triangle : scene.Emitters)
    {
        emitterTriangle = triangle;
        targetAreaWorld -= scene.Triangles[triangle].Area;
        if (targetAreaWorld <= 0)
        {
            break;
        }
    }
    const float32 root = math::Sqrt(random(4));
    context.Emitter = loadPoint(scene, emitterTriangle, root * (1 - random(5)), root * random(5));
    if (context.Emitter.HasCoverage == false)
    {
        return outEmission;
    }
    const math::Vector3 directOffsetWorld = context.Emitter.PositionWorld - context.Receiver.PositionWorld;
    const math::Vector3 directWorld = Normalize(directOffsetWorld);
    const float32 directSquaredWorld = math::Dot(directOffsetWorld, directOffsetWorld);
    ReferenceBsdfEvaluation direct;
    if (directSquaredWorld > 0 && math::Dot(context.Receiver.GeometricNormalWorld, directWorld) > 0 &&
        EvaluateReferenceBsdf(context.Receiver.Bsdf,
                              context.Receiver.NormalWorld,
                              context.ViewWorld,
                              directWorld,
                              direct) == rhi::RasterStatus::Ready &&
        isVisible(scene, context.Receiver, context.Emitter) == true)
    {
        outDirect = math::Hadamard(direct.Value,
                                   EvaluateEmission(context.Emitter.Emission,
                                                    context.Emitter.GeometricNormalWorld,
                                                    -directWorld,
                                                    context.Emitter.IsEmissionTwoSided)) *
                    (math::Max(0.0F, math::Dot(context.Receiver.NormalWorld, directWorld)) *
                     math::Abs(math::Dot(context.Emitter.GeometricNormalWorld, -directWorld)) * emitterAreaWorld /
                     directSquaredWorld);
    }
    if (estimator != 2)
    {
        float32 geometryPdf;
        const Point middle = sampleGeometry(scene, context, {random(6), random(7), random(8)}, geometryPdf);
        float32 bsdfPdf;
        const math::Vector3 value = evaluateConnection(scene, context, middle, true, bsdfPdf);
        outIndirect = outIndirect + value * (emitterAreaWorld / (geometryPdf + (estimator == 0 ? bsdfPdf : 0)));
    }
    if (estimator != 1)
    {
        ReferenceBsdfSample proposal;
        if (SampleReferenceBsdf(context.Receiver.Bsdf,
                                context.Receiver.NormalWorld,
                                context.ViewWorld,
                                {random(9), random(10), random(11)},
                                proposal) == rhi::RasterStatus::Ready &&
            proposal.Accepted == true && math::Dot(context.Receiver.GeometricNormalWorld, proposal.Direction) > 0)
        {
            math::Vector3 originWorld;
            if (math::TryOffsetRayOrigin({context.Receiver.PositionWorld,
                                          scene.Triangles[context.Receiver.Triangle].Error,
                                          context.Receiver.GeometricNormalWorld,
                                          proposal.Direction},
                                         originWorld) == math::MathStatus::Success)
            {
                rhi::RayResult secondary;
                const rhi::RayInput ray = MakeRay(originWorld, proposal.Direction, MAXIMUM_DISTANCE_WORLD);
                if (internal::TraceReferenceOracle(scene, ray, secondary) == rhi::RasterStatus::Ready &&
                    secondary.Hit != 0)
                {
                    const Point middle = loadPoint(scene, FindTriangle(scene, secondary), secondary.U, secondary.V);
                    float32 bsdfPdf;
                    const math::Vector3 value = evaluateConnection(scene, context, middle, false, bsdfPdf);
                    if (bsdfPdf > 0)
                    {
                        outIndirect =
                            outIndirect +
                            value * (emitterAreaWorld /
                                     (bsdfPdf + (estimator == 0 ? evaluatePdf(scene, middle.Triangle, context) : 0)));
                    }
                }
            }
        }
    }
    return outEmission + outDirect + outIndirect;
}
float32 OneMoreVertexOracle::IntegratePdf(const internal::ReferenceSceneData& scene,
                                          uint32 receiverTriangle,
                                          uint32 emitterTriangle) const noexcept
{
    Context context;
    context.Receiver = loadPoint(scene, receiverTriangle, 1.0F / 3, 1.0F / 3);
    context.Emitter = loadPoint(scene, emitterTriangle, 1.0F / 3, 1.0F / 3);
    context.ViewWorld = context.Receiver.NormalWorld;
    float32 sum = 0;
    for (uint32 triangle = 0; triangle < scene.Triangles.GetSize(); ++triangle)
    {
        sum += evaluatePdf(scene, triangle, context) * scene.Triangles[triangle].Area;
    }
    return sum;
}
} // namespace ludus::graphics::renderer::testing
