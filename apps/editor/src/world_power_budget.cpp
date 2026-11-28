#include "internal/world_power_budget.h"

#include "internal/render_scene.h"

#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/queries.hpp>
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
constexpr uint32 ESCAPE_RAY_COUNT = 8192;
constexpr uint32 ABSORPTION_RAYS_PER_SIDE = 128;
constexpr uint32 PROBE_ROW_WIDTH = 128;
constexpr float32 SPHERE_PADDING_SCALE = 1.025F;
constexpr float32 ORIGIN_ROUNDING_SCALE = 0.000001F;
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

// Thanks to Pharr, Jakob and Humphreys, "Physically Based Rendering", 4th ed. §§4.1 and A.5.3,
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions.
// Cosine-weighted rays estimate flux as area times pi times mean directional radiance.
math::Vector3 SamplePowerDirection(math::Vector3 normalWorld, math::RandomStream& inoutStream) noexcept
{
    const math::Vector3 axisWorld =
        math::Abs(normalWorld.X) < math::Abs(normalWorld.Z) ? math::Vector3{1, 0, 0} : math::Vector3{0, 0, 1};
    math::Vector3 tangentWorld;
    static_cast<void>(math::TryNormalize(math::Cross(normalWorld, axisWorld), tangentWorld));
    const math::Vector3 bitangentWorld = math::Cross(normalWorld, tangentWorld);
    const float32 radiusSquared = inoutStream.NextFloat01();
    const float32 radius = math::Sqrt(radiusSquared);
    const float32 azimuth = math::kTauF * inoutStream.NextFloat01();
    return tangentWorld * (radius * math::Cos(azimuth)) + bitangentWorld * (radius * math::Sin(azimuth)) +
           normalWorld * math::Sqrt(1 - radiusSquared);
}

bool AppendEscapeProbes(const math::Aabb3& boundsWorld, uint32 count, WorldPowerProbes& inoutProbes)
{
    constexpr float64 SPHERE_AREA_SCALE = 4;
    const math::Vector3 halfExtentWorld = (boundsWorld.Max - boundsWorld.Min) * 0.5F;
    const math::Vector3 centerWorld = boundsWorld.Min + halfExtentWorld;
    const float32 radiusWorld = math::Length(halfExtentWorld) * SPHERE_PADDING_SCALE;
    const float64 radius = static_cast<float64>(radiusWorld);
    const float64 weight = SPHERE_AREA_SCALE * math::kPi * radius * radius * math::kPi / count;
    math::RandomStream stream;
    for (uint32 index = 0; index < count; ++index)
    {
        const float32 z = 2 * stream.NextFloat01() - 1;
        const float32 azimuth = math::kTauF * stream.NextFloat01();
        const float32 radial = math::Sqrt(1 - z * z);
        const math::Vector3 outwardWorld{radial * math::Cos(azimuth), radial * math::Sin(azimuth), z};
        const math::Vector3 originWorld = centerWorld + outwardWorld * radiusWorld;
        if (inoutProbes.Rays.TryAdd({originWorld, SamplePowerDirection(-outwardWorld, stream)}) == false ||
            inoutProbes.Weights.TryAdd({weight, weight, weight}) == false)
        {
            return false;
        }
    }
    inoutProbes.EscapeRayCount = inoutProbes.Rays.GetSize();
    return true;
}

bool AppendAbsorptionProbes(const PowerTriangle& triangle,
                            uint32 countPerSide,
                            math::RandomStream& inoutStream,
                            WorldPowerProbes& inoutProbes)
{
    const math::Vector3d weight =
        (UNIT_REFLECTANCE - triangle.Reflectance) * (math::kPi * triangle.AreaWorld / countPerSide);
    if (weight == math::Vector3d{})
    {
        return true;
    }
    math::Vector3 errorWorld;
    for (const math::Vector3 cornerWorld : triangle.CornersWorld)
    {
        errorWorld.X += math::Abs(cornerWorld.X) * ORIGIN_ROUNDING_SCALE;
        errorWorld.Y += math::Abs(cornerWorld.Y) * ORIGIN_ROUNDING_SCALE;
        errorWorld.Z += math::Abs(cornerWorld.Z) * ORIGIN_ROUNDING_SCALE;
    }
    const math::Vector3 cornerWorld = triangle.CornersWorld[0];
    for (const math::Vector3 normalWorld : {triangle.NormalWorld, -triangle.NormalWorld})
    {
        for (uint32 index = 0; index < countPerSide; ++index)
        {
            const float32 root = math::Sqrt(inoutStream.NextFloat01());
            const float32 alongEdge = inoutStream.NextFloat01();
            const math::Vector3 pointWorld = cornerWorld * (1 - root) +
                                             triangle.CornersWorld[1] * (root * (1 - alongEdge)) +
                                             triangle.CornersWorld[2] * (root * alongEdge);
            const math::Vector3 directionWorld = SamplePowerDirection(normalWorld, inoutStream);
            math::Vector3 originWorld;
            if (math::TryOffsetRayOrigin({pointWorld, errorWorld, normalWorld, directionWorld}, originWorld) !=
                    math::MathStatus::Success ||
                inoutProbes.Rays.TryAdd({originWorld, directionWorld}) == false ||
                inoutProbes.Weights.TryAdd(weight) == false)
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace

QString BuildWorldPowerProbes(const RenderScene& scene, uint32 probeDensity, WorldPowerProbes& outProbes)
{
    if (probeDensity == 0 || probeDensity > MAX_WORLD_POWER_PROBE_DENSITY)
    {
        return QStringLiteral("Choose a supported probe density and measure again.");
    }
    QVector<PowerTriangle> triangles;
    const QString error = BuildPowerTriangles(scene, triangles);
    if (error.isEmpty() == false)
    {
        return error;
    }
    math::Aabb3 boundsWorld;
    if (triangles.isEmpty() == true || scene.TryGetBounds({}, boundsWorld) == false)
    {
        return QStringLiteral("Add a nondegenerate diffuse mesh with emission, then measure again.");
    }
    WorldPowerProbes probes;
    if (AppendEscapeProbes(boundsWorld, ESCAPE_RAY_COUNT * probeDensity, probes) == false)
    {
        return QStringLiteral("Could not allocate reference escape probes. Close other captures and try again.");
    }
    math::RandomStream stream;
    static_cast<void>(stream.TryReseed(1, 0));
    for (const PowerTriangle& triangle : triangles)
    {
        probes.Emitted = probes.Emitted + triangle.Emission * (triangle.IsEmissionTwoSided == true ? 2 : 1);
        if (AppendAbsorptionProbes(triangle, ABSORPTION_RAYS_PER_SIDE * probeDensity, stream, probes) == false)
        {
            return QStringLiteral("Could not place reference absorption probes. Check World bounds and try again.");
        }
    }
    probes.RowWidth = PROBE_ROW_WIDTH;
    outProbes = foundation::Move(probes);
    return {};
}

bool IntegrateWorldPower(const WorldPowerProbes& probes,
                         BorrowedView<const renderer::ReferencePixel> pixels,
                         WorldPowerFlux& outFlux) noexcept
{
    if (pixels.IsValid() == false || pixels.GetSize() != probes.Weights.GetSize() ||
        probes.EscapeRayCount > pixels.GetSize())
    {
        return false;
    }
    WorldPowerFlux flux;
    flux.Emitted = probes.Emitted;
    for (usize index = 0; index < pixels.GetSize(); ++index)
    {
        const renderer::ReferencePixel& pixel = pixels[index];
        if (pixel.Samples == 0 || IsNonnegative(pixel.Radiance) == false)
        {
            return false;
        }
        const math::Vector3d power = math::Hadamard(probes.Weights[index], WidenColor(pixel.Radiance));
        math::Vector3d& bucket = index < probes.EscapeRayCount ? flux.Escaped : flux.Absorbed;
        bucket = bucket + power;
    }
    outFlux = flux;
    return true;
}
} // namespace ludus::editor
