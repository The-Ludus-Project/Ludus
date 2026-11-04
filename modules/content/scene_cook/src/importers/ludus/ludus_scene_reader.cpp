#include "ludus_scene_reader.h"

#include "../front_end_support.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::importers::ludus_scene
{
namespace
{
using scene_source::SceneSource;
using scene_source::SceneSourceError;
using scene_source::SceneSourceStatus;
using scene_source::SourceMaterial;
using scene_source::SourceMaterialModel;
using scene_source::SourceObject;

[[nodiscard]] std::string_view GetModelName(SourceMaterialModel model) noexcept
{
    if (model == SourceMaterialModel::Mix)
    {
        return "mix";
    }
    return model == SourceMaterialModel::Diffuse ? "diffuse" : "metallic_roughness";
}

void CopyRgb(math::Vector3d value, float64 (&outRgb)[IR_RGB_CHANNEL_COUNT]) noexcept
{
    outRgb[0] = value.X;
    outRgb[1] = value.Y;
    outRgb[2] = value.Z;
}

[[nodiscard]] bool IsEmitting(const SourceMaterial& material) noexcept
{
    return material.EmittedRadiance.X > 0.0 || material.EmittedRadiance.Y > 0.0 || material.EmittedRadiance.Z > 0.0;
}

[[nodiscard]] Status AddMaterial(const SourceMaterial& source, SceneIr& inoutScene) noexcept
{
    IrMaterial material;
    if (material.Name.TryAssign(source.Name.GetView()) != foundation::StringStatus::Ok ||
        material.SourceType.TryAssign(GetModelName(source.Model)) != foundation::StringStatus::Ok)
    {
        return Status::OutOfMemory;
    }
    CopyRgb(source.BaseColor, material.BaseColour);
    material.IsUntexturedReflectanceExact = true;
    material.Metallic = source.Metallic;
    material.Roughness = source.Roughness;
    material.Scattering.Model = source.Model == SourceMaterialModel::Diffuse
                                    ? scene_format::MaterialModel::Diffuse
                                    : scene_format::MaterialModel::MetallicRoughness;
    if (source.Model == SourceMaterialModel::Mix)
    {
        material.Metallic = source.MixWeight;
        material.Roughness = math::Sqrt(source.Conductor.Alpha);
        material.Scattering.Model = scene_format::MaterialModel::DiffuseConductorMix;
        material.Scattering.AlphaU = static_cast<float32>(source.Conductor.Alpha);
        material.Scattering.AlphaV = material.Scattering.AlphaU;
        material.Scattering.Eta = {{static_cast<float32>(source.Conductor.Eta.X),
                                    static_cast<float32>(source.Conductor.Eta.Y),
                                    static_cast<float32>(source.Conductor.Eta.Z)}};
        material.Scattering.K = {{static_cast<float32>(source.Conductor.Extinction.X),
                                  static_cast<float32>(source.Conductor.Extinction.Y),
                                  static_cast<float32>(source.Conductor.Extinction.Z)}};
    }
    material.SourceFile = 0;
    return inoutScene.Materials.TryAdd(static_cast<IrMaterial&&>(material)) == true ? Status::Ok : Status::OutOfMemory;
}

/// Append one polygon as a fan from its first corner, with its own vertices and flat normal.
[[nodiscard]] Status AddPolygon(const SourceObject& object, std::span<const uint32> corners, IrMesh& inoutMesh) noexcept
{
    // Newell's method gives the normal of the front face (counterclockwise corners).
    math::Vector3d normal{};
    for (usize i = 0; i < corners.size(); ++i)
    {
        normal = normal + math::Cross(object.PositionsWorld[corners[i]],
                                      object.PositionsWorld[corners[(i + 1) % corners.size()]]);
    }
    // SceneSource admits only polygons with nonzero area.
    normal = normal * (1.0 / math::Length(normal));
    const uint32 first = static_cast<uint32>(inoutMesh.GetVertexCount());
    for (const uint32 corner : corners)
    {
        const math::Vector3d position = object.PositionsWorld[corner];
        for (const float64 component : {position.X, position.Y, position.Z})
        {
            if (inoutMesh.Positions.TryAdd(static_cast<float32>(component)) == false)
            {
                return Status::OutOfMemory;
            }
        }
        for (const float64 component : {normal.X, normal.Y, normal.Z})
        {
            if (inoutMesh.Normals.TryAdd(static_cast<float32>(component)) == false)
            {
                return Status::OutOfMemory;
            }
        }
    }
    for (usize i = 1; i + 1 < corners.size(); ++i)
    {
        for (const uint32 index : {first, first + static_cast<uint32>(i), first + static_cast<uint32>(i + 1)})
        {
            if (inoutMesh.Indices.TryAdd(index) == false)
            {
                return Status::OutOfMemory;
            }
        }
    }
    return Status::Ok;
}

[[nodiscard]] Status AddObject(const SceneSource& source, const SourceObject& object, SceneIr& inoutScene) noexcept
{
    IrMesh mesh;
    usize first = 0;
    for (const uint32 cornerCount : object.PolygonVertexCounts)
    {
        const Status added =
            AddPolygon(object, std::span<const uint32>(object.PolygonIndices.GetData() + first, cornerCount), mesh);
        if (added != Status::Ok)
        {
            return added;
        }
        first += cornerCount;
    }
    IrShape shape;
    shape.Mesh = static_cast<uint32>(inoutScene.Meshes.GetSize());
    shape.Material = static_cast<uint32>(object.Material);
    shape.SourceFile = 0;
    const SourceMaterial& material = source.Materials[object.Material];
    if (IsEmitting(material) == true)
    {
        // A required area light, like a pbrt AreaLightSource: the planner keeps its exact
        // radiance (Emission over its luminance, times EmissionScale) or fails the import.
        CopyRgb(material.EmittedRadiance, shape.Emission);
        shape.EmissionScale = ComputeRec709Luminance(shape.Emission);
        shape.IsEmissionTwoSided = material.IsEmissionTwoSided;
        shape.IsEmissionOptional = false;
        shape.AreaLight = inoutScene.Lights.Area;
        ++inoutScene.Lights.Area;
    }
    if (inoutScene.Meshes.TryAdd(static_cast<IrMesh&&>(mesh)) == false ||
        inoutScene.WorldShapes.TryAdd(static_cast<uint32>(inoutScene.Shapes.GetSize())) == false ||
        inoutScene.Shapes.TryAdd(shape) == false)
    {
        return Status::OutOfMemory;
    }
    return Status::Ok;
}

/// Record the white-surface approximation the cooked format makes for emitters.
void ReportEmitterSurfaces(const SceneSource& source, DiagnosticLog& inoutLog) noexcept
{
    for (const SourceMaterial& material : source.Materials)
    {
        const bool isWhiteRough = material.Model == SourceMaterialModel::MetallicRoughness &&
                                  material.BaseColor == math::Vector3d{1.0, 1.0, 1.0} && material.Metallic == 0.0 &&
                                  material.Roughness == 1.0;
        if (IsEmitting(material) == true && isWhiteRough == false)
        {
            MessageBuilder message;
            message.Append("emitting material '")
                .Append(material.Name.GetView())
                .Append("' reflects as white rough metallic_roughness, as required LUSCN emitters do");
            (void)inoutLog.Add(DiagnosticSeverity::Warning, codes::MATERIAL_APPROXIMATED, {}, 0, message.GetView());
        }
    }
}
} // namespace

Status ReadScene(std::span<const uint8> entryBytes,
                 std::string_view entryName,
                 SceneIr& outScene,
                 DiagnosticLog& inoutLog,
                 const Cancellation* cancelOrNull) noexcept
{
    SceneIr freshScene;
    outScene = static_cast<SceneIr&&>(freshScene);
    if (AddSourceFile(outScene, entryName, Hash(entryBytes)) == false)
    {
        return Status::OutOfMemory;
    }
    SceneSource source;
    SceneSourceError error;
    const SceneSourceStatus parsed = scene_source::ParseSceneSource(
        std::string_view(reinterpret_cast<const char*>(entryBytes.data()), entryBytes.size()),
        source,
        error);
    if (parsed == SceneSourceStatus::OutOfMemory)
    {
        return Status::OutOfMemory;
    }
    if (parsed != SceneSourceStatus::Success)
    {
        (void)inoutLog.Add(DiagnosticSeverity::Error,
                           parsed == SceneSourceStatus::Limit ? codes::LIMIT_EXCEEDED : codes::MALFORMED_INPUT,
                           entryName,
                           0,
                           error.Message.View());
        return Status::Invalid;
    }
    for (const SourceMaterial& material : source.Materials)
    {
        const Status added = AddMaterial(material, outScene);
        if (added != Status::Ok)
        {
            return added;
        }
    }
    for (const SourceObject& object : source.Objects)
    {
        if (IsCancelled(cancelOrNull) == true)
        {
            return Status::Cancelled;
        }
        const Status added = AddObject(source, object, outScene);
        if (added != Status::Ok)
        {
            return added;
        }
    }
    if (source.Camera.IsPresent == true)
    {
        outScene.Camera.IsPresent = true;
        outScene.Camera.Eye = source.Camera.EyeWorld;
        outScene.Camera.Target = source.Camera.TargetWorld;
        outScene.Camera.Up = source.Camera.UpWorld;
        outScene.Camera.VerticalFov = source.Camera.VerticalFovRadians;
    }
    ReportEmitterSurfaces(source, inoutLog);
    return Status::Ok;
}
} // namespace ludus::content::scene_cook::importers::ludus_scene
