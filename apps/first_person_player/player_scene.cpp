#include "player_scene.h"
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::first_person
{
using namespace foundation;
namespace rr = graphics::renderer;
namespace rhi = graphics::rhi;
namespace
{
rhi::TextureCpuContent GetTexture(const resources::SceneDefinition& scene, uint32 index) noexcept
{
    if (index == content::scene_format::NO_TEXTURE)
    {
        return {};
    }
    const resources::SceneTexture& texture = scene.Textures[index];
    return {texture.Pixels.GetData(),
            texture.Pixels.GetSize(),
            texture.Pixels.GetSize() / texture.Height,
            texture.Width,
            texture.Height,
            texture.IsLinear == true ? rhi::RasterFormat::Rgba8Unorm : rhi::RasterFormat::Rgba8Srgb};
}
} // namespace
bool PlayerScene::Build(const PlayableSnapshot& snapshot) noexcept
{
    const resources::SceneDefinition& scene = snapshot.Scene;
    for (const resources::SceneMesh& mesh : scene.Meshes)
    {
        rr::RenderGraphMesh record;
        record.Geometry.Positions = mesh.Positions.GetData();
        record.Geometry.VertexCount = mesh.Positions.GetSize();
        record.Geometry.Normals = mesh.Normals.GetData();
        record.Geometry.Texcoords = mesh.Texcoords.GetData();
        record.Geometry.Tangents = mesh.Tangents.AsView();
        record.Geometry.Indices = mesh.Indices.GetData();
        record.Geometry.IndexCount = mesh.Indices.GetSize();
        if (Meshes.TryAdd(record) == false)
        {
            return false;
        }
    }
    for (const resources::SceneMaterial& source : scene.Materials)
    {
        rr::SceneShaderMaterial material;
        material.BaseColor = source.Texture == content::scene_format::NO_TEXTURE && source.HasSourceMaterial == false
                                 ? math::Vector3{}
                                 : source.Reflectance;
        material.Metallic = source.Metallic;
        material.Roughness = source.Roughness;
        material.Scattering = source.Scattering;
        material.Program = source.Program;
        material.Emission = source.Emission;
        material.IsEmissionTwoSided = source.IsEmissionTwoSided;
        material.NormalScale = source.NormalScale;
        material.IsAlphaMasked = source.IsAlphaMasked;
        material.AlphaCutoff = source.AlphaCutoff;
        material.Sampler.Filter = rhi::RasterFilter::Linear;
        material.Sampler.U = source.Repeat == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
        material.Sampler.V = material.Sampler.U;
        material.ColorTexture = GetTexture(scene, source.Texture);
        material.SurfaceTexture = GetTexture(scene, source.SurfaceTexture);
        material.EmissionTexture = GetTexture(scene, source.EmissionTexture);
        if (Materials.TryAdd(material) == false)
        {
            return false;
        }
    }
    if (MaterialBindings.TryResize(scene.Objects.GetSize()) == false)
    {
        return false;
    }
    for (const resources::SceneObject& object : scene.Objects)
    {
        uint32& materialIndex = MaterialBindings[Items.GetSize()];
        materialIndex = object.Material;
        const resources::SceneMaterial& source = scene.Materials[object.Material];
        if (source.HasEmissionPower == true)
        {
            const resources::SceneMesh& mesh = scene.Meshes[object.Mesh];
            constexpr usize TRIANGLE_CORNERS = 3;
            float64 areaWorld = 0;
            for (usize index = 0; index < mesh.Indices.GetSize(); index += TRIANGLE_CORNERS)
            {
                const math::Vector3 a = math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[index]]);
                const math::Vector3 b = math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[index + 1]]);
                const math::Vector3 c = math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[index + 2]]);
                areaWorld += static_cast<float64>(math::Length(math::Cross(b - a, c - a))) * 0.5;
            }
            rr::SceneShaderMaterial instance = Materials[object.Material];
            if (areaWorld > 0)
            {
                if (rr::ResolveMeshEmissionPower(source.Emission,
                                                 source.EmissionPower,
                                                 areaWorld,
                                                 source.IsEmissionTwoSided,
                                                 instance.Emission) != rhi::RasterStatus::Ready)
                {
                    return false;
                }
            }
            else
            {
                instance.Emission = {};
            }
            materialIndex = static_cast<uint32>(Materials.GetSize());
            if (Materials.TryAdd(instance) == false)
            {
                return false;
            }
        }
        rr::RenderGraphItem item;
        item.SourceId = object.SourceId;
        item.MeshIndex = object.Mesh;
        item.MeshToWorld = object.Transform;
        // The clustered deferred pass reads alpha coverage from the scene material.
        item.MaterialOverrides = {&materialIndex, 1};
        if (Items.TryAdd(item) == false)
        {
            return false;
        }
    }
    for (const resources::SceneLight& source : scene.Lights)
    {
        if (source.IsEnabled == false)
        {
            continue;
        }
        rr::DirectLight light;
        light.Kind = source.Kind == resources::SceneLightKind::Point  ? rr::LightKind::Point
                     : source.Kind == resources::SceneLightKind::Spot ? rr::LightKind::Spot
                                                                      : rr::LightKind::Directional;
        light.SourceId = source.SourceId;
        light.Intensity = math::Max(source.IntensityRgb.X, math::Max(source.IntensityRgb.Y, source.IntensityRgb.Z));
        if (light.Intensity > 0)
        {
            light.Color = source.IntensityRgb / light.Intensity;
        }
        light.Direction = source.DirectionWorld;
        light.Position = source.PositionWorld;
        light.Range = source.Range;
        light.InnerCos = source.InnerCos;
        light.OuterCos = source.OuterCos;
        for (const uint64 unshadowedId : snapshot.RasterUnshadowedLightIds)
        {
            if (unshadowedId == light.SourceId)
            {
                light.ShouldCastRasterShadows = false;
                break;
            }
        }
        if (Lights.TryAdd(light) == false)
        {
            return false;
        }
    }
    Inputs.Materials = Materials.AsView();
    Inputs.Lights = Lights.AsView();
    Inputs.Environment = scene.Environment.Rgb;
    Inputs.EnvironmentWidth = scene.Environment.Width;
    Inputs.EnvironmentHeight = scene.Environment.Height;
    Inputs.EnvironmentPixels = scene.Environment.Pixels.AsView();
    return true;
}
WalkingController::StartStatus
StartWalking(const PlayableSnapshot& snapshot, const PlayerSpawn& spawn, WalkingController& outController) noexcept
{
    Array<StaticColliderInstance> instances;
    for (const usize index : snapshot.ColliderObjects)
    {
        const resources::SceneObject& object = snapshot.Scene.Objects[index];
        const resources::SceneMesh& mesh = snapshot.Scene.Meshes[object.Mesh];
        if (instances.TryAdd({mesh.Positions.AsView(), mesh.Indices.AsView(), object.Transform}) == false)
        {
            return WalkingController::StartStatus::GeometryUnavailable;
        }
    }
    return outController.Start(instances.AsView(), spawn.Pose.Position);
}
} // namespace ludus::first_person
