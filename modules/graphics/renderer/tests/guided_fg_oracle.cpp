#include "guided_fg_oracle.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::graphics::renderer::testing
{
using namespace foundation;
namespace
{
constexpr float64 RAY_OFFSET_WORLD = 1e-5;
constexpr float64 NORMAL_AGREEMENT = 0.9;
constexpr float64 PLANE_RADIUS_FRACTION = 0.1;
constexpr uint32 DIRECTION_CELLS = 16;
constexpr float32 SPECULAR_ROUGHNESS = 0.2F;
constexpr float32 REFLECTION_ONLY_METALLIC = 1;

math::Vector3 ConvertToFloat(math::Vector3d value) noexcept
{
    return {static_cast<float32>(value.X), static_cast<float32>(value.Y), static_cast<float32>(value.Z)};
}
math::Vector3d ConvertToDouble(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
bool IsDiffuse(const ReferenceBsdf& material) noexcept
{
    const bool isConductor = material.Scattering.Model == content::scene_format::MaterialModel::Conductor;
    const float32 roughness = isConductor == true ? math::Sqrt(material.Scattering.AlphaU) : material.Roughness;
    return (isConductor == false && material.Metallic < REFLECTION_ONLY_METALLIC) || roughness >= SPECULAR_ROUGHNESS;
}
// Ray origin and direction follow the intersection equation.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool IntersectTriangle(math::Vector3d originWorld,
                       math::Vector3d directionWorld,
                       const FgOracleTriangle& triangle,
                       float64& outDistanceWorld) noexcept
{
    const math::Vector3d edgeUWorld = triangle.BWorld - triangle.AWorld;
    const math::Vector3d edgeVWorld = triangle.CWorld - triangle.AWorld;
    const math::Vector3d crossWorld = math::Cross(directionWorld, edgeVWorld);
    const float64 determinant = math::Dot(edgeUWorld, crossWorld);
    if (determinant == 0)
    {
        return false;
    }
    const math::Vector3d offsetWorld = originWorld - triangle.AWorld;
    const float64 u = math::Dot(offsetWorld, crossWorld) / determinant;
    const math::Vector3d qWorld = math::Cross(offsetWorld, edgeUWorld);
    const float64 v = math::Dot(directionWorld, qWorld) / determinant;
    const float64 distanceWorld = math::Dot(edgeVWorld, qWorld) / determinant;
    if (u < 0 || v < 0 || u + v > 1 || distanceWorld <= RAY_OFFSET_WORLD)
    {
        return false;
    }
    outDistanceWorld = distanceWorld;
    return true;
}
} // namespace

bool TraceFgOraclePhotons(BorrowedView<const FgOracleTriangle> triangles,
                          math::Vector3d originWorld,
                          math::Vector3d directionWorld,
                          math::Vector3d flux,
                          const FgOracleSamples& samples,
                          FgOraclePhotons& outPhotons) noexcept
{
    FgOraclePhotons photons{};
    bool isCaustic = false;
    for (usize depth = 0; depth < FG_ORACLE_DEPTH; ++depth)
    {
        const FgOracleTriangle* closestOrNull = nullptr;
        float64 closestDistanceWorld = 0;
        for (usize triangleIndex = 0; triangleIndex < triangles.GetSize(); ++triangleIndex)
        {
            const FgOracleTriangle& triangle = triangles[triangleIndex];
            float64 distanceWorld = 0;
            if (IntersectTriangle(originWorld, directionWorld, triangle, distanceWorld) == true &&
                (closestOrNull == nullptr || distanceWorld < closestDistanceWorld))
            {
                closestOrNull = &triangle;
                closestDistanceWorld = distanceWorld;
            }
        }
        if (closestOrNull == nullptr)
        {
            break;
        }
        const FgOracleTriangle& triangle = *closestOrNull;
        if (triangle.Material.Scattering.Model == content::scene_format::MaterialModel::CoatedDiffuse ||
            triangle.Material.Scattering.Model == content::scene_format::MaterialModel::Dielectric ||
            triangle.Material.Scattering.Model == content::scene_format::MaterialModel::ThinDielectric)
        {
            return false;
        }
        math::Vector3d normalWorld = math::Cross(triangle.BWorld - triangle.AWorld, triangle.CWorld - triangle.AWorld);
        normalWorld = normalWorld * (1 / math::Length(normalWorld));
        const math::Vector3d positionWorld = originWorld + directionWorld * closestDistanceWorld;
        const bool isDiffuse = IsDiffuse(triangle.Material);
        if (isDiffuse == true)
        {
            photons[depth] = {positionWorld, normalWorld, directionWorld * -1, flux, true, isCaustic};
        }
        isCaustic = isDiffuse == false;
        ReferenceBsdfSample sampled;
        if (SampleReferenceBsdf(triangle.Material,
                                ConvertToFloat(normalWorld),
                                ConvertToFloat(directionWorld * -1),
                                samples[depth],
                                sampled) != rhi::RasterStatus::Ready)
        {
            return false;
        }
        if (sampled.Accepted == false || (sampled.Weight.X <= 0 && sampled.Weight.Y <= 0 && sampled.Weight.Z <= 0))
        {
            break;
        }
        directionWorld = ConvertToDouble(sampled.Direction);
        flux = math::Hadamard(flux, ConvertToDouble(sampled.Weight));
        originWorld = positionWorld + normalWorld * RAY_OFFSET_WORLD;
    }
    outPhotons = photons;
    return true;
}
math::Vector3d GatherFgOraclePhotons(BorrowedView<const FgOraclePhoton> photons,
                                     // Position, normal and view follow the surface contract.
                                     // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                     math::Vector3d positionWorld,
                                     math::Vector3d normalWorld,
                                     math::Vector3d viewDirectionWorld,
                                     const ReferenceBsdf& material,
                                     float64 radiusWorld,
                                     bool shouldGatherOnlyCaustics) noexcept
{
    math::Vector3d sum{};
    if (radiusWorld <= 0)
    {
        return sum;
    }
    for (usize photonIndex = 0; photonIndex < photons.GetSize(); ++photonIndex)
    {
        const FgOraclePhoton& photon = photons[photonIndex];
        const math::Vector3d offsetWorld = photon.PositionWorld - positionWorld;
        if (photon.IsValid == false || (shouldGatherOnlyCaustics == true && photon.IsCaustic == false) ||
            math::Dot(offsetWorld, offsetWorld) >= radiusWorld * radiusWorld ||
            math::Abs(math::Dot(offsetWorld, normalWorld)) > PLANE_RADIUS_FRACTION * radiusWorld ||
            math::Dot(photon.NormalWorld, normalWorld) < NORMAL_AGREEMENT ||
            math::Dot(normalWorld, photon.IncomingWorld) <= 0)
        {
            continue;
        }
        ReferenceBsdfEvaluation evaluated;
        if (EvaluateReferenceBsdf(material,
                                  ConvertToFloat(normalWorld),
                                  ConvertToFloat(viewDirectionWorld),
                                  ConvertToFloat(photon.IncomingWorld),
                                  evaluated) != rhi::RasterStatus::Ready)
        {
            continue;
        }
        sum = sum + math::Hadamard(ConvertToDouble(evaluated.Value), photon.Flux);
    }
    return sum * (1 / (math::kPi * radiusWorld * radiusWorld));
}
float64 ComputeFgOracleFluxScale(uint32 cellPaths, uint32 uniformPaths) noexcept
{
    const float64 denominator = static_cast<float64>(DIRECTION_CELLS) * cellPaths + uniformPaths;
    return denominator > 0 ? 1 / denominator : 0;
}
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — ordered old/new offsets define the direction of the Jacobian.
float64 ComputeFgOracleJacobian(math::Vector3d oldOffsetWorld,
                                math::Vector3d newOffsetWorld,
                                math::Vector3d normalWorld) noexcept
{
    const float64 oldSquared = math::Dot(oldOffsetWorld, oldOffsetWorld);
    const float64 newSquared = math::Dot(newOffsetWorld, newOffsetWorld);
    if (oldSquared <= 0 || newSquared <= 0)
    {
        return 0;
    }
    const float64 oldCosine = math::Abs(math::Dot(normalWorld, oldOffsetWorld)) / math::Sqrt(oldSquared);
    const float64 newCosine = math::Abs(math::Dot(normalWorld, newOffsetWorld)) / math::Sqrt(newSquared);
    return oldCosine > 0 ? newCosine * oldSquared / (oldCosine * newSquared) : 0;
}
float64
ComputeFgOraclePairWeight(float64 confidence, float64 otherConfidence, float64 target, float64 otherTarget) noexcept
{
    const float64 denominator = confidence * target + otherConfidence * otherTarget;
    return denominator > 0 ? confidence * target / denominator : 0;
}
} // namespace ludus::graphics::renderer::testing
