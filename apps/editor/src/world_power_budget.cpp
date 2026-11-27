#include "internal/world_power_budget.h"

#include "internal/render_scene.h"

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/rhi/raster.h>

#include <QVector>

namespace ludus::editor
{
using namespace foundation;
namespace
{
namespace renderer = graphics::renderer;
namespace format = content::scene_format;

constexpr qsizetype MAX_TRIANGLES = 256;
constexpr int32 COARSE_SAMPLES_PER_SIDE = 128;
constexpr int32 FINE_SAMPLES_PER_SIDE = 512;
constexpr int32 MAX_BOUNCE_STEPS = 64;
constexpr qsizetype SIDES_PER_TRIANGLE = 2;
constexpr float64 REMAINING_FRACTION = 0.0001;
constexpr math::Vector3d UNIT_REFLECTANCE{1, 1, 1};

struct PowerTriangle final
{
    StaticArray<math::Vector3, renderer::AREA_TRIANGLE_CORNERS> CornersWorld;
    math::Vector3 NormalWorld;
    math::Vector3d Reflectance;
    math::Vector3d Emission;
    float64 AreaWorld = 0;
    bool IsEmissionTwoSided = false;
};

struct PowerTransfer final
{
    qsizetype DestinationSide = 0;
    float64 Fraction = 0;
};

struct PowerTransferRow final
{
    QVector<PowerTransfer> Hits;
    float64 EscapeFraction = 0;
};

bool IsNonnegative(math::Vector3 value) noexcept
{
    return math::IsFinite(value) == true && value.X >= 0 && value.Y >= 0 && value.Z >= 0;
}

math::Vector3d WidenColor(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

QString BuildPowerTriangles(const RenderScene& scene, QVector<PowerTriangle>& outTriangles)
{
    if (scene.Identity.isEmpty() == true)
    {
        return QStringLiteral("Load a World in the viewport, then measure again.");
    }
    if (scene.Atmosphere.IsEnabled == true ||
        (scene.Definition != nullptr && scene.Definition->Environment.Rgb != math::Vector3{}))
    {
        return QStringLiteral("Use a black environment and disable the atmosphere for this budget.");
    }
    if (scene.AuthoredLights.Count != 0 || scene.Luminaires.Count != 0)
    {
        return QStringLiteral(
            "Disable analytic lights and panel luminaires. This budget takes its emission from meshes.");
    }
    if (scene.Medium.IsPresent() == true)
    {
        return QStringLiteral("Disable the absorbing medium. This budget measures absorption at opaque surfaces.");
    }

    for (const PreviewObject& object : scene.Objects)
    {
        const QString objectName = object.Name.isEmpty() == true ? object.Id : object.Name;
        if (object.Surface < 0 || object.Surface >= scene.Surfaces.size())
        {
            return QStringLiteral("Assign an opaque Diffuse material to %1, then measure again.").arg(objectName);
        }
        const PreviewSurface& surface = scene.Surfaces[object.Surface];
        const math::Vector3 reflectance{surface.Tint.X * object.Tint.X,
                                        surface.Tint.Y * object.Tint.Y,
                                        surface.Tint.Z * object.Tint.Z};
        if (surface.HasUnresolvedMaterial == true || surface.Scattering.Model != format::MaterialModel::Diffuse ||
            surface.Texture >= 0 || object.Texture >= 0 || surface.SurfaceTexture >= 0 ||
            surface.EmissionTexture >= 0 || surface.IsAlphaMasked == true || IsNonnegative(reflectance) == false ||
            reflectance.X > 1 || reflectance.Y > 1 || reflectance.Z > 1 || IsNonnegative(surface.Emission) == false)
        {
            return QStringLiteral("Use an untextured, opaque Diffuse material on %1 with reflectance in [0, 1].")
                .arg(objectName);
        }

        if (object.Mesh < 0 || object.Mesh >= scene.Meshes.size() ||
            (scene.Definition != nullptr && static_cast<usize>(object.Mesh) >= scene.Definition->Meshes.GetSize()))
        {
            return QStringLiteral("Assign a valid mesh to %1, then measure again.").arg(objectName);
        }
        const BorrowedView<const math::Vector3> positions = GetRenderSceneMeshPositions(scene, object.Mesh);
        const BorrowedView<const uint32> indices = GetRenderSceneMeshIndices(scene, object.Mesh);
        if (indices.GetSize() % renderer::AREA_TRIANGLE_CORNERS != 0)
        {
            return QStringLiteral("Replace the incomplete triangle list of %1, then measure again.").arg(objectName);
        }
        const qsizetype first = outTriangles.size();
        float64 totalAreaWorld = 0;
        for (usize index = 0; index < indices.GetSize(); index += renderer::AREA_TRIANGLE_CORNERS)
        {
            PowerTriangle triangle;
            for (usize corner = 0; corner < renderer::AREA_TRIANGLE_CORNERS; ++corner)
            {
                const uint32 vertex = indices[index + corner];
                if (vertex >= positions.GetSize())
                {
                    return QStringLiteral("Repair the out-of-range mesh index of %1, then measure again.")
                        .arg(objectName);
                }
                triangle.CornersWorld[corner] = math::TransformPoint(object.Transform, positions[vertex]);
            }
            const math::Vector3 areaVectorWorld = math::Cross(triangle.CornersWorld[1] - triangle.CornersWorld[0],
                                                              triangle.CornersWorld[2] - triangle.CornersWorld[0]);
            triangle.AreaWorld = static_cast<float64>(math::Length(areaVectorWorld)) * 0.5;
            if (triangle.AreaWorld == 0)
            {
                continue;
            }
            const math::Vector3 localA = positions[indices[index]];
            const math::Vector3 localB = positions[indices[index + 1]];
            const math::Vector3 localC = positions[indices[index + 2]];
            if (math::IsFinite(triangle.AreaWorld) == false ||
                math::TryTransformNormal(object.Transform,
                                         math::Cross(localB - localA, localC - localA),
                                         triangle.NormalWorld) != math::MathStatus::Success)
            {
                return QStringLiteral("Repair invalid triangle geometry or the transform of %1, then measure again.")
                    .arg(objectName);
            }
            if (outTriangles.size() == MAX_TRIANGLES)
            {
                return QStringLiteral(
                           "The World exceeds %1 nondegenerate triangles. Use a simplified fixture and measure again.")
                    .arg(MAX_TRIANGLES);
            }
            triangle.Reflectance = WidenColor(reflectance);
            triangle.IsEmissionTwoSided = surface.IsEmissionTwoSided;
            outTriangles.append(triangle);
            totalAreaWorld += triangle.AreaWorld;
        }
        if (totalAreaWorld == 0)
        {
            if (surface.HasEmissionPower == true && surface.EmissionPower > 0)
            {
                return QStringLiteral("Give the emitter %1 a nonzero world-space area, then measure again.")
                    .arg(objectName);
            }
            continue;
        }
        math::Vector3 radiance = surface.Emission;
        if (surface.HasEmissionPower == true && surface.EmissionPower == 0)
        {
            radiance = {};
        }
        else if (surface.HasEmissionPower == true &&
                 renderer::ResolveMeshEmissionPower(surface.Emission,
                                                    surface.EmissionPower,
                                                    totalAreaWorld,
                                                    surface.IsEmissionTwoSided,
                                                    radiance) != graphics::rhi::RasterStatus::Ready)
        {
            return QStringLiteral("Check the emission color, flux and area of %1, then measure again.").arg(objectName);
        }
        for (qsizetype triangleIndex = first; triangleIndex < outTriangles.size(); ++triangleIndex)
        {
            PowerTriangle& triangle = outTriangles[triangleIndex];
            triangle.Emission = WidenColor(radiance) * (math::kPi * triangle.AreaWorld);
        }
    }
    return {};
}

qsizetype
TraceTransfer(const QVector<PowerTriangle>& triangles, qsizetype sourceTriangle, const math::Ray3& rayWorld) noexcept
{
    qsizetype destination = -1;
    float64 nearestWorld = 0;
    for (qsizetype i = 0; i < triangles.size(); ++i)
    {
        if (i == sourceTriangle)
        {
            continue;
        }
        const PowerTriangle& triangle = triangles[i];
        float64 distanceWorld = 0;
        if (IntersectRenderSceneTriangle(rayWorld,
                                         triangle.CornersWorld[0],
                                         triangle.CornersWorld[1],
                                         triangle.CornersWorld[2],
                                         distanceWorld) == true &&
            distanceWorld > 0 && (destination < 0 || distanceWorld < nearestWorld))
        {
            const bool isBack = math::Dot(triangle.NormalWorld, rayWorld.Direction) > 0;
            destination = SIDES_PER_TRIANGLE * i + (isBack == true ? 1 : 0);
            nearestWorld = distanceWorld;
        }
    }
    return destination;
}

PowerTransferRow MeasureTransferRow(const QVector<PowerTriangle>& triangles, qsizetype side, int32 samples, uint64 seed)
{
    const qsizetype sourceTriangle = side / SIDES_PER_TRIANGLE;
    const PowerTriangle& source = triangles[sourceTriangle];
    const math::Vector3 normalWorld = side % SIDES_PER_TRIANGLE == 0 ? source.NormalWorld : -source.NormalWorld;
    const math::Vector3 axisWorld =
        math::Abs(normalWorld.X) < math::Abs(normalWorld.Z) ? math::Vector3{1, 0, 0} : math::Vector3{0, 0, 1};
    math::Vector3 tangentWorld;
    static_cast<void>(math::TryNormalize(math::Cross(normalWorld, axisWorld), tangentWorld));
    const math::Vector3 bitangentWorld = math::Cross(normalWorld, tangentWorld);
    QVector<int32> hitCounts(triangles.size() * SIDES_PER_TRIANGLE, 0);
    int32 escapeCount = 0;
    math::RandomStream stream;
    static_cast<void>(stream.TryReseed(seed, static_cast<uint64>(side)));
    for (int32 sample = 0; sample < samples; ++sample)
    {
        // Square-root warps map uniform draws to triangle area and to the projected hemisphere disk.
        const float32 root = math::Sqrt(stream.NextFloat01());
        const float32 alongEdge = stream.NextFloat01();
        const math::Vector3 cornerWorld = source.CornersWorld[0];
        const math::Vector3 originWorld = cornerWorld +
                                          (source.CornersWorld[1] - cornerWorld) * (root * (1 - alongEdge)) +
                                          (source.CornersWorld[2] - cornerWorld) * (root * alongEdge);
        const float32 radiusSquared = stream.NextFloat01();
        const float32 radius = math::Sqrt(radiusSquared);
        const float32 azimuth = math::kTauF * stream.NextFloat01();
        const math::Vector3 directionWorld = tangentWorld * (radius * math::Cos(azimuth)) +
                                             bitangentWorld * (radius * math::Sin(azimuth)) +
                                             normalWorld * math::Sqrt(1 - radiusSquared);
        const qsizetype destination = TraceTransfer(triangles, sourceTriangle, {originWorld, directionWorld});
        if (destination < 0)
        {
            ++escapeCount;
        }
        else
        {
            ++hitCounts[destination];
        }
    }
    const float64 inverseSamples = 1.0 / static_cast<float64>(samples);
    PowerTransferRow result;
    result.EscapeFraction = static_cast<float64>(escapeCount) * inverseSamples;
    for (qsizetype destination = 0; destination < hitCounts.size(); ++destination)
    {
        if (hitCounts[destination] > 0)
        {
            result.Hits.append({destination, static_cast<float64>(hitCounts[destination]) * inverseSamples});
        }
    }
    return result;
}

float64 GetLargestFraction(math::Vector3d flux, math::Vector3d emitted) noexcept
{
    return math::Max(emitted.X > 0 ? math::Abs(flux.X) / emitted.X : 0,
                     math::Max(emitted.Y > 0 ? math::Abs(flux.Y) / emitted.Y : 0,
                               emitted.Z > 0 ? math::Abs(flux.Z) / emitted.Z : 0));
}

// Thanks to Michael F. Cohen, Shenchang Eric Chen, John R. Wallace and Donald P. Greenberg,
// "A Progressive Refinement Approach to Fast Radiosity Image Generation", SIGGRAPH 1988, §3,
// https://doi.org/10.1145/378456.378487. We distribute unshot diffuse flux with form factors.
// Single-segment visibility samples replace the hemicube. Cache a row per side and update by bounce.
WorldPowerFlux
SolvePower(const QVector<PowerTriangle>& triangles, const QVector<math::Vector3d>& emission, int32 samples, uint64 seed)
{
    WorldPowerFlux result;
    for (const math::Vector3d flux : emission)
    {
        result.Emitted = result.Emitted + flux;
    }
    QVector<math::Vector3d> outgoing = emission;
    QVector<math::Vector3d> incoming(emission.size());
    QVector<PowerTransferRow> transfer(emission.size());
    QVector<bool> hasTransfer(emission.size(), false);
    for (int32 bounce = 0; bounce < MAX_BOUNCE_STEPS; ++bounce)
    {
        incoming.fill({});
        for (qsizetype side = 0; side < outgoing.size(); ++side)
        {
            const math::Vector3d flux = outgoing[side];
            if (flux == math::Vector3d{})
            {
                continue;
            }
            if (hasTransfer[side] == false)
            {
                transfer[side] = MeasureTransferRow(triangles, side, samples, seed);
                hasTransfer[side] = true;
            }
            const PowerTransferRow& row = transfer[side];
            result.Escaped = result.Escaped + flux * row.EscapeFraction;
            for (const PowerTransfer& hit : row.Hits)
            {
                incoming[hit.DestinationSide] = incoming[hit.DestinationSide] + flux * hit.Fraction;
            }
        }
        result.Unresolved = {};
        for (qsizetype side = 0; side < incoming.size(); ++side)
        {
            const math::Vector3d reflectance = triangles[side / SIDES_PER_TRIANGLE].Reflectance;
            const math::Vector3d flux = incoming[side];
            result.Absorbed = result.Absorbed + math::Hadamard(flux, UNIT_REFLECTANCE - reflectance);
            outgoing[side] = math::Hadamard(flux, reflectance);
            result.Unresolved = result.Unresolved + outgoing[side];
        }
        if (GetLargestFraction(result.Unresolved, result.Emitted) <= REMAINING_FRACTION)
        {
            break;
        }
    }
    return result;
}
} // namespace

WorldPowerBudget MeasureWorldPower(const RenderScene& scene)
{
    QVector<PowerTriangle> triangles;
    WorldPowerBudget result;
    result.Error = BuildPowerTriangles(scene, triangles);
    if (result.Error.isEmpty() == false)
    {
        return result;
    }
    QVector<math::Vector3d> emission(triangles.size() * SIDES_PER_TRIANGLE);
    for (qsizetype i = 0; i < triangles.size(); ++i)
    {
        const PowerTriangle& triangle = triangles[i];
        emission[SIDES_PER_TRIANGLE * i] = triangle.Emission;
        if (triangle.IsEmissionTwoSided == true)
        {
            emission[SIDES_PER_TRIANGLE * i + 1] = triangle.Emission;
        }
    }
    const WorldPowerFlux coarse = SolvePower(triangles, emission, COARSE_SAMPLES_PER_SIDE, 0);
    result.Flux = SolvePower(triangles, emission, FINE_SAMPLES_PER_SIDE, 1);
    result.SamplingChange =
        math::Max(GetLargestFraction(result.Flux.Absorbed - coarse.Absorbed, result.Flux.Emitted),
                  math::Max(GetLargestFraction(result.Flux.Escaped - coarse.Escaped, result.Flux.Emitted),
                            GetLargestFraction(result.Flux.Unresolved - coarse.Unresolved, result.Flux.Emitted)));
    return result;
}
} // namespace ludus::editor
