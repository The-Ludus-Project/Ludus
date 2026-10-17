#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/static_lighting.hpp>

#include "internal/preview_area_lights.h"
#include "internal/qt_numeric_stream.h"
#include "internal/render_scene.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>

namespace ludus::editor::preview_lighting
{
using namespace foundation;
namespace math = foundation::math;
namespace rr = graphics::renderer;
namespace rhi = graphics::rhi;
namespace format = content::scene_format;
constexpr usize MAX_INSTANCES = format::MAX_OBJECTS;
constexpr usize MAX_EMITTER_TRIANGLES = format::MAX_VERTICES / rr::AREA_TRIANGLE_CORNERS;
constexpr uint32 SHADOW_ATLAS_PIXELS = 2048;
constexpr uint32 SHADOW_COLUMNS = 9;
constexpr uint32 SHADOW_CELL_PIXELS = SHADOW_ATLAS_PIXELS / SHADOW_COLUMNS;
constexpr uint32 SHADOW_GUTTER_PIXELS = 2;
constexpr uint32 SHADOW_INNER_PIXELS = SHADOW_CELL_PIXELS - SHADOW_GUTTER_PIXELS * 2;

inline uint64 ReadRevision(const QByteArray& key, int32 start) noexcept
{
    uint64 result = 0;
    for (int32 i = 0; i < static_cast<int32>(sizeof(uint64)); ++i)
    {
        result |= uint64{static_cast<uint8>(key[start + i])} << (i * 8);
    }
    return result == 0 ? 1 : result;
}

// Camera state is absent. Typed geometry is tightly packed (asserted below);
// hash it directly without allocating a second scene-sized serialization buffer.
inline QByteArray MakeKey(const RenderScene& scene)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << scene.AssetsIdentity << scene.Lit << static_cast<uint32>(rr::LIGHTING_PROFILE_VERSION);
    for (const PreviewObject& object : scene.Objects)
    {
        stream << object.Id << object.Mesh << object.Surface << object.Texture;
        for (const math::Vector3& column : object.Transform.Columns)
        {
            stream << column.X << column.Y << column.Z;
        }
        stream << object.Transform.Translation.X << object.Transform.Translation.Y << object.Transform.Translation.Z;
        stream << object.Tint.X << object.Tint.Y << object.Tint.Z << object.Tint.W;
    }
    for (const PreviewSurface& surface : scene.Surfaces)
    {
        stream << surface.Texture << surface.Repeat << surface.Metallic << surface.Roughness;
        stream << surface.Tint.X << surface.Tint.Y << surface.Tint.Z << surface.Tint.W;
        stream << surface.Emission.X << surface.Emission.Y << surface.Emission.Z;
    }
    if (scene.Definition != nullptr)
    {
        for (const resources::SceneMaterial& material : scene.Definition->Materials)
        {
            WriteLittleEndianUint64(stream, material.AreaSourceId);
            stream << material.EmissionPower << material.HasEmissionPower << material.IsEmissionTwoSided;
        }
        for (const resources::SceneObject& object : scene.Definition->Objects)
        {
            WriteLittleEndianUint64(stream, object.SourceId);
        }
        for (const resources::SceneLight& light : scene.Definition->Lights)
        {
            WriteLittleEndianUint64(stream, light.SourceId);
            stream << static_cast<uint32>(light.Kind) << light.IsEnabled;
            stream << light.IntensityRgb.X << light.IntensityRgb.Y << light.IntensityRgb.Z;
            stream << light.PositionWorld.X << light.PositionWorld.Y << light.PositionWorld.Z;
            stream << light.DirectionWorld.X << light.DirectionWorld.Y << light.DirectionWorld.Z;
            stream << light.Range << light.InnerCos << light.OuterCos;
        }
        const resources::SceneEnvironment& environment = scene.Definition->Environment;
        stream << environment.Width << environment.Height << environment.Rgb.X << environment.Rgb.Y
               << environment.Rgb.Z;
        for (const math::Vector3& pixel : environment.Pixels)
        {
            stream << pixel.X << pixel.Y << pixel.Z;
        }
    }
    static_assert(sizeof(math::Vector3) == sizeof(float32) * 3);
    static_assert(sizeof(math::Vector2) == sizeof(float32) * 2);
    if (scene.Definition != nullptr)
    {
        WriteLittleEndianUint64(stream, static_cast<uint64>(scene.Definition->Meshes.GetSize()));
        WriteLittleEndianUint64(stream, static_cast<uint64>(scene.Definition->Textures.GetSize()));
        for (const resources::SceneMesh& mesh : scene.Definition->Meshes)
        {
            WriteLittleEndianUint64(stream, static_cast<uint64>(mesh.Positions.GetSize()));
            WriteLittleEndianUint64(stream, static_cast<uint64>(mesh.Normals.GetSize()));
            WriteLittleEndianUint64(stream, static_cast<uint64>(mesh.Texcoords.GetSize()));
            WriteLittleEndianUint64(stream, static_cast<uint64>(mesh.BakeTexcoords.GetSize()));
            WriteLittleEndianUint64(stream, static_cast<uint64>(mesh.Indices.GetSize()));
        }
        for (const resources::SceneTexture& texture : scene.Definition->Textures)
        {
            stream << texture.Width << texture.Height;
            WriteLittleEndianUint64(stream, static_cast<uint64>(texture.Pixels.GetSize()));
        }
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(bytes);
    const auto addBytes = [&hash](const void* dataOrNull, usize count) {
        hash.addData(QByteArrayView(static_cast<const char*>(dataOrNull), static_cast<isize>(count)));
    };
    if (scene.Definition != nullptr)
    {
        for (const resources::SceneMesh& mesh : scene.Definition->Meshes)
        {
            addBytes(mesh.Positions.GetData(), mesh.Positions.GetSize() * sizeof(math::Vector3));
            addBytes(mesh.Normals.GetData(), mesh.Normals.GetSize() * sizeof(math::Vector3));
            addBytes(mesh.Texcoords.GetData(), mesh.Texcoords.GetSize() * sizeof(math::Vector2));
            addBytes(mesh.BakeTexcoords.GetData(), mesh.BakeTexcoords.GetSize() * sizeof(math::Vector2));
            addBytes(mesh.Indices.GetData(), mesh.Indices.GetSize() * sizeof(uint32));
        }
        for (const resources::SceneTexture& texture : scene.Definition->Textures)
        {
            addBytes(texture.Pixels.GetData(), texture.Pixels.GetSize());
        }
    }
    return hash.result();
}

struct Inputs final
{
    rr::LightSelection Lights;
    rr::LightClusterDescription Clusters;
    StaticArray<rr::StaticLightingSurfaceDescription, MAX_INSTANCES> Surfaces{};
    StaticArray<rr::ShadowRegion, rr::SHADOW_REGION_CAPACITY> Shadows{};
    uint32 ShadowCount = 0;
    uint32 AreaCount = 0;
    uint64 SceneRevision = 0;
    uint64 DependencyRevision = 0;
};

inline rhi::RasterStatus BuildInputs(const RenderScene& scene, const QByteArray& key, Inputs& outInputs)
{
    if (scene.Definition == nullptr || key.size() < static_cast<isize>(sizeof(uint64) * 2) ||
        scene.Definition->Objects.GetSize() > MAX_INSTANCES ||
        scene.Definition->Objects.GetSize() != static_cast<usize>(scene.Objects.size()))
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    Inputs result;
    result.SceneRevision = ReadRevision(key, 0);
    result.DependencyRevision = ReadRevision(key, static_cast<int32>(sizeof(uint64)));
    const float32 radius = math::Max(scene.Radius, .1F);
    result.Clusters.Bounds = {scene.Center - math::Vector3{radius, radius, radius},
                              scene.Center + math::Vector3{radius, radius, radius}};
    for (const resources::SceneLight& source : scene.Definition->Lights)
    {
        if (source.IsEnabled == false)
        {
            continue;
        }
        if (result.Lights.Count == format::MAX_LIGHTS)
        {
            return rhi::RasterStatus::CapacityExceeded;
        }
        rr::DirectLight& light = result.Lights.Lights[result.Lights.Count++];
        light.SourceId = source.SourceId;
        light.Kind = static_cast<rr::LightKind>(source.Kind);
        light.Intensity = math::Max(source.IntensityRgb.X, math::Max(source.IntensityRgb.Y, source.IntensityRgb.Z));
        light.Color = light.Intensity > 0 ? source.IntensityRgb / light.Intensity : math::Vector3{};
        light.Position = source.PositionWorld;
        light.Direction = source.DirectionWorld;
        light.Range = source.Range;
        light.InnerCos = source.InnerCos;
        light.OuterCos = source.OuterCos;
    }
    for (usize objectIndex = 0; objectIndex < scene.Definition->Objects.GetSize(); ++objectIndex)
    {
        const PreviewObject& object = scene.Objects[static_cast<int32>(objectIndex)];
        const PreviewSurface& surface = scene.Surfaces[object.Surface];
        const resources::SceneObject& source = scene.Definition->Objects[objectIndex];
        const resources::SceneMaterial& material = scene.Definition->Materials[source.Material];
        const resources::SceneMesh& mesh = scene.Definition->Meshes[source.Mesh];
        rr::StaticLightingSurfaceDescription& overrideSurface = result.Surfaces[objectIndex];
        overrideSurface.Texture = surface.Texture >= 0 ? static_cast<uint32>(surface.Texture) : format::NO_TEXTURE;
        overrideSurface.Tint = {surface.Tint.X * object.Tint.X,
                                surface.Tint.Y * object.Tint.Y,
                                surface.Tint.Z * object.Tint.Z,
                                surface.Tint.W * object.Tint.W};
        overrideSurface.Metallic = surface.Metallic;
        overrideSurface.Roughness = surface.Roughness;
        overrideSurface.Emission = surface.Emission;
        overrideSurface.IsEmissionTwoSided = material.IsEmissionTwoSided;
        overrideSurface.ShouldRepeatTexture = surface.Repeat;
        if (surface.Emission.X == 0 && surface.Emission.Y == 0 && surface.Emission.Z == 0)
        {
            continue;
        }
        const usize triangles = mesh.Indices.GetSize() / rr::AREA_TRIANGLE_CORNERS;
        if (triangles > MAX_EMITTER_TRIANGLES)
        {
            return rhi::RasterStatus::CapacityExceeded;
        }
        float64 area = 0;
        for (usize i = 0; i < triangles; ++i)
        {
            const math::Vector3 a = math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[i * 3]]);
            const math::Vector3 b = math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[i * 3 + 1]]);
            const math::Vector3 c = math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[i * 3 + 2]]);
            area += static_cast<float64>(math::Length(math::Cross(b - a, c - a))) * .5;
        }
        if (area <= 0)
        {
            // Every triangle collapsed: no surface emits, so there is no proxy to add.
            overrideSurface.Emission = {};
            continue;
        }
        if (material.HasEmissionPower == true)
        {
            const float32 luminance = math::Dot(surface.Emission, math::Vector3{.2126F, .7152F, .0722F});
            if (luminance <= 0)
            {
                return rhi::RasterStatus::InvalidDescription;
            }
            overrideSurface.Emission =
                surface.Emission * static_cast<float32>(static_cast<float64>(material.EmissionPower) /
                                                        (static_cast<float64>(luminance) * math::kPi * area *
                                                         (material.IsEmissionTwoSided == true ? 2.0 : 1.0)));
        }
        uint32 added = 0;
        const rhi::RasterStatus status = AppendPreviewAreaLights(mesh,
                                                                 object.Transform,
                                                                 overrideSurface.Emission,
                                                                 material.IsEmissionTwoSided,
                                                                 source.SourceId,
                                                                 math::Min(1000000.0F, math::Max(10.0F, radius * 8)),
                                                                 result.Lights,
                                                                 added);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        result.AreaCount += added;
    }
    rr::LightSelection normalized;
    const rhi::RasterStatus normalizedStatus =
        rr::NormalizeDirectLights(result.Lights.Lights, result.Lights.Count, normalized);
    if (normalizedStatus != rhi::RasterStatus::Ready)
    {
        return normalizedStatus;
    }
    result.Lights = normalized;
    for (usize i = 0; i < result.Lights.Count; ++i)
    {
        const rr::DirectLight& light = result.Lights.Lights[i];
        const bool isCube = light.Kind == rr::LightKind::Point || light.Kind == rr::LightKind::AreaOneSided ||
                            light.Kind == rr::LightKind::AreaTwoSided;
        const uint32 count = isCube == true ? rr::POINT_SHADOW_FACES : 1;
        if (count > rr::SHADOW_REGION_CAPACITY - result.ShadowCount)
        {
            return rhi::RasterStatus::CapacityExceeded;
        }
        for (uint32 face = 0; face < count; ++face)
        {
            rr::ShadowRegion& region = result.Shadows[result.ShadowCount];
            region.Light = light;
            region.Interior = {(result.ShadowCount % SHADOW_COLUMNS) * SHADOW_CELL_PIXELS + SHADOW_GUTTER_PIXELS,
                               (result.ShadowCount / SHADOW_COLUMNS) * SHADOW_CELL_PIXELS + SHADOW_GUTTER_PIXELS,
                               SHADOW_INNER_PIXELS,
                               SHADOW_INNER_PIXELS};
            region.NearDistance = math::Min(.01F, light.Range * .01F);
            rhi::RasterStatus status = rhi::RasterStatus::Ready;
            if (isCube == true)
            {
                region.CubeFace = face;
                status = rr::BuildPointShadowMatrix(light.Position,
                                                    face,
                                                    region.NearDistance,
                                                    light.Range,
                                                    region.WorldToClip);
            }
            else if (light.Kind == rr::LightKind::Directional)
            {
                status = rr::BuildDirectionalShadowMatrix(result.Clusters.Bounds,
                                                          light.Direction,
                                                          SHADOW_INNER_PIXELS,
                                                          region.WorldToClip);
            }
            else
            {
                if (light.OuterCos <= 0)
                {
                    return rhi::RasterStatus::Unsupported;
                }
                math::Matrix4 worldToView;
                math::Matrix4 viewToClip;
                const math::Vector3 up =
                    math::Abs(light.Direction.Y) > .9F ? math::Vector3{1, 0, 0} : math::Vector3{0, 1, 0};
                const float32 fov = 2 * math::Atan2(math::Sqrt(1 - light.OuterCos * light.OuterCos), light.OuterCos);
                if (math::TryLookAt(light.Position, light.Position + light.Direction, up, worldToView) !=
                        math::MathStatus::Success ||
                    math::TryPerspectiveReverseZ(fov, 1, region.NearDistance, light.Range, viewToClip) !=
                        math::MathStatus::Success)
                {
                    return rhi::RasterStatus::InvalidDescription;
                }
                for (math::Vector4& column : viewToClip.Columns)
                {
                    column.Z = column.W - column.Z;
                }
                region.WorldToClip = viewToClip * worldToView;
            }
            if (status != rhi::RasterStatus::Ready)
            {
                return status;
            }
            ++result.ShadowCount;
        }
    }
    outInputs = result;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::editor::preview_lighting
