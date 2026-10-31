#include "internal/reference_coating.h"
#include "internal/render_graph_ray_scene.h"
#include "internal/scene_material_validation.h"
#include "internal/scene_memory.h"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/graphics/renderer/environment.hpp>

#include <new>

// Source/BSDF representation follows the existing Renderer reference profile.
// This freezes data and a BVH only; authored shaders own all transport decisions.
namespace ludus::graphics::renderer::internal
{
namespace
{
using Status = rhi::RasterStatus;
constexpr usize CORNERS = 3;
constexpr float32 MAX_EMISSION = 10000;
constexpr float32 MIN_ROUGHNESS = 0.045F;
constexpr float32 MAX_NORMAL_SCALE = 8;
constexpr float32 POSITION_ROUNDING = 2e-6F;
// Bounds of the emitter triangles scene.Emitters[firstEmitter, end), all of positive area:
// a sphere around their vertex AABB centre and the cone of their geometric normals.
ReferenceEmitterBounds
ComputeEmitterBounds(const ReferenceSceneData& scene, usize firstEmitter, bool isEmissionTwoSided) noexcept
{
    ReferenceEmitterBounds bounds;
    bounds.IsEmissionTwoSided = isEmissionTwoSided;
    math::Aabb3 boxWorld = math::EmptyAabb3();
    math::Vector3 normalSumWorld = {0, 0, 0};
    for (usize i = firstEmitter; i < scene.Emitters.GetSize(); ++i)
    {
        const ReferenceTriangle& triangle = scene.Triangles[scene.Emitters[i]];
        for (const math::Vector3 positionWorld : triangle.Positions)
        {
            boxWorld = math::Merge(boxWorld, {positionWorld, positionWorld});
        }
        normalSumWorld = normalSumWorld + triangle.GeometricNormal * triangle.Area;
    }
    bounds.SphereWorld.Center = (boxWorld.Min + boxWorld.Max) * 0.5F;
    const bool hasAxis = math::TryNormalize(normalSumWorld, bounds.NormalAxisWorld) == math::MathStatus::Success;
    bounds.CosConeHalfAngle = hasAxis == true ? 1.F : -1.F;
    for (usize i = firstEmitter; i < scene.Emitters.GetSize(); ++i)
    {
        const ReferenceTriangle& triangle = scene.Triangles[scene.Emitters[i]];
        for (const math::Vector3 positionWorld : triangle.Positions)
        {
            bounds.SphereWorld.Radius =
                math::Max(bounds.SphereWorld.Radius, math::Length(positionWorld - bounds.SphereWorld.Center));
        }
        if (hasAxis == true)
        {
            bounds.CosConeHalfAngle = math::Max(
                math::Min(bounds.CosConeHalfAngle, math::Dot(triangle.GeometricNormal, bounds.NormalAxisWorld)),
                -1.F);
        }
    }
    if (hasAxis == false)
    {
        bounds.NormalAxisWorld = {0, 0, 1};
    }
    return bounds;
}
bool TryAddSceneAllocation(usize count, usize stride, uint64& inoutBytes) noexcept
{
    uint64 bytes = 0;
    return TryMultiply(static_cast<uint64>(count), static_cast<uint64>(stride), bytes) == true &&
           TryAdd(inoutBytes, bytes, inoutBytes) == true;
}
SceneShaderMaterial SelectSceneMaterial(const SceneShaderMaterial& source, uint32 data) noexcept
{
    SceneShaderMaterial material = source;
    if ((data & ::ludus::content::render_graph_format::SCENE_DATA_MATERIALS) == 0)
    {
        material.BaseColor = {};
        material.Metallic = 0;
        material.Roughness = 1;
        material.ColorTexture = {};
        material.SurfaceTexture = {};
        material.IsAlphaMasked = false;
    }
    return material;
}
bool IsTextureValid(const rhi::TextureCpuContent& texture, bool isData) noexcept
{
    if (texture.Bytes == nullptr)
    {
        return texture.Size == 0 && texture.Width == 0 && texture.Height == 0 && texture.RowPitch == 0;
    }
    return texture.Width > 0 && texture.Height > 0 &&
           (texture.Format == rhi::RasterFormat::Rgba8Unorm ||
            (isData == false && texture.Format == rhi::RasterFormat::Rgba8Srgb)) &&
           uint64(texture.Width) * LUDUS_GRAPH_TEXTURE_CHANNELS <= texture.RowPitch &&
           texture.RowPitch <= texture.Size / texture.Height;
}
bool IsMaterialValid(const SceneShaderMaterial& material) noexcept
{
    return content::scene_format::IsMaterialScatteringValid(material.Scattering) == true &&
           internal::IsReferenceCoatingSupported(material.Scattering) == true &&
           (material.Scattering.Model != content::scene_format::MaterialModel::Conductor ||
            material.Scattering.AlphaU == material.Scattering.AlphaV) &&
           material.HasError == false && ReferenceColor(material.BaseColor, 1) &&
           ReferenceColor(material.Emission, MAX_EMISSION) && math::IsFinite(material.Metallic) &&
           material.Metallic >= 0 && material.Metallic <= 1 && math::IsFinite(material.Roughness) &&
           material.Roughness >= MIN_ROUGHNESS && material.Roughness <= 1 && math::IsFinite(material.NormalScale) &&
           material.NormalScale >= 0 && material.NormalScale <= MAX_NORMAL_SCALE &&
           math::IsFinite(material.AlphaCutoff) == true && material.AlphaCutoff >= 0 && material.AlphaCutoff <= 1 &&
           (material.Sampler.Filter == rhi::RasterFilter::Nearest ||
            material.Sampler.Filter == rhi::RasterFilter::Linear) &&
           (material.Sampler.U == rhi::RasterAddress::Clamp || material.Sampler.U == rhi::RasterAddress::Repeat) &&
           (material.Sampler.V == rhi::RasterAddress::Clamp || material.Sampler.V == rhi::RasterAddress::Repeat) &&
           math::IsFinite(material.UvTransform) && IsTextureValid(material.ColorTexture, false) &&
           IsTextureValid(material.SurfaceTexture, true) && IsTextureValid(material.EmissionTexture, false);
}
} // namespace
bool IsSceneShaderMaterialValid(const SceneShaderMaterial& material) noexcept
{
    return IsMaterialValid(material);
}
bool IsSceneRadianceValid(math::Vector3 radiance) noexcept
{
    return ReferenceColor(radiance, MAX_EMISSION);
}
Status BuildMaterialGraphRayScene(const RenderGraphSceneDescription& source,
                                  const ReferenceCaptureDescription& description,
                                  usize byteBudget,
                                  core::UniquePtr<ReferenceJob>& outJob) noexcept
{
    if ((source.SceneData & ~::ludus::content::render_graph_format::SCENE_DATA_ALL) != 0 ||
        (source.SceneData & ::ludus::content::render_graph_format::SCENE_DATA_GEOMETRY) == 0)
    {
        return Status::InvalidDescription;
    }
    RenderGraphSceneDescription input = source;
    const bool hasMaterials = (input.SceneData & ::ludus::content::render_graph_format::SCENE_DATA_MATERIALS) != 0;
    const bool hasLights = (input.SceneData & ::ludus::content::render_graph_format::SCENE_DATA_LIGHTS) != 0;
    if (hasMaterials == false && hasLights == false)
    {
        input.Inputs.Materials = {};
    }
    if (hasLights == false)
    {
        input.Inputs.Lights = {};
        input.Inputs.Environment = {};
        input.Inputs.EnvironmentPixels = {};
        input.Inputs.EnvironmentWidth = 0;
        input.Inputs.EnvironmentHeight = 0;
    }

    SceneShaderDiagnostics localDiagnostics;
    SceneShaderDiagnostics& diagnostics =
        input.DiagnosticsOrNull != nullptr ? *input.DiagnosticsOrNull : localDiagnostics;
    diagnostics = {};
    constexpr usize MAX_SLOTS = content::render_graph_format::MAX_ITEMS;
    if (outJob.Get() != nullptr || input.Meshes.IsValid() == false || input.Items.IsValid() == false ||
        input.Inputs.Materials.IsValid() == false || input.Inputs.Lights.IsValid() == false ||
        input.Inputs.EnvironmentPixels.IsValid() == false ||
        uint64(input.Inputs.EnvironmentWidth) * input.Inputs.EnvironmentHeight !=
            input.Inputs.EnvironmentPixels.GetSize() ||
        ((input.Inputs.EnvironmentWidth == 0) != (input.Inputs.EnvironmentHeight == 0)) ||
        input.Meshes.GetSize() > content::render_graph_format::MAX_MESHES ||
        input.Items.GetSize() > content::render_graph_format::MAX_ITEMS ||
        input.Inputs.Materials.GetSize() > MAX_SLOTS || ReferenceColor(input.Inputs.Environment, MAX_EMISSION) == false)
    {
        return Status::InvalidDescription;
    }
    usize triangleCapacity = 0;
    usize surfaceCapacity = 0;
    usize textureBytes = 0;
    for (usize i = 0; i < input.Inputs.Materials.GetSize(); ++i)
    {
        const SceneShaderMaterial material = SelectSceneMaterial(input.Inputs.Materials[i], input.SceneData);
        if (IsMaterialValid(material) == false)
        {
            if (input.AreErrorsAllowed == false)
            {
                diagnostics.ErrorBindings = 1;
                diagnostics.ErrorMaterial = static_cast<uint32>(i);
                return Status::InvalidDescription;
            }
            continue;
        }
        if (TryAdd(textureBytes, material.ColorTexture.Size, textureBytes) == false ||
            TryAdd(textureBytes, material.SurfaceTexture.Size, textureBytes) == false ||
            TryAdd(textureBytes, material.EmissionTexture.Size, textureBytes) == false)
        {
            return Status::CapacityExceeded;
        }
    }
    for (usize i = 0; i < input.Items.GetSize(); ++i)
    {
        const RenderGraphItem& item = input.Items[i];
        if (item.IsIntentionallyExcluded == true)
        {
            continue;
        }
        if (item.MeshIndex >= input.Meshes.GetSize() || item.SourceId == 0 || item.MaterialOverrides.IsValid() == false)
        {
            return Status::InvalidDescription;
        }
        const RenderGraphMesh& mesh = input.Meshes[item.MeshIndex];
        const MeshDescription& geometry = mesh.Geometry;
        if (mesh.MaterialSlots.IsValid() == false || mesh.MaterialBindings.IsValid() == false ||
            geometry.Positions == nullptr || geometry.Indices == nullptr || geometry.VertexCount == 0 ||
            geometry.IndexCount % CORNERS != 0 || geometry.Tangents.IsValid() == false ||
            geometry.Colors.IsValid() == false || (geometry.Tangents.GetSize() != 0 && geometry.Normals == nullptr) ||
            (geometry.Tangents.GetSize() != 0 && geometry.Tangents.GetSize() != geometry.VertexCount) ||
            (geometry.Colors.GetSize() != 0 && geometry.Colors.GetSize() != geometry.VertexCount) ||
            (mesh.MaterialSlots.GetSize() != 0 && mesh.MaterialSlots.GetSize() != geometry.IndexCount / CORNERS) ||
            math::Max(mesh.MaterialBindings.GetSize(), item.MaterialOverrides.GetSize()) > MAX_SLOTS)
        {
            return Status::InvalidDescription;
        }
        usize declaredSlots = 1;
        for (usize primitive = 0; primitive < mesh.MaterialSlots.GetSize(); ++primitive)
        {
            if (mesh.MaterialSlots[primitive] >= MAX_SLOTS)
            {
                return Status::InvalidDescription;
            }
            declaredSlots = math::Max(declaredSlots, usize(mesh.MaterialSlots[primitive]) + 1);
        }
        if (TryAdd(triangleCapacity, geometry.IndexCount / CORNERS, triangleCapacity) == false ||
            TryAdd(
                surfaceCapacity,
                math::Max(declaredSlots, math::Max(mesh.MaterialBindings.GetSize(), item.MaterialOverrides.GetSize())),
                surfaceCapacity) == false)
        {
            return Status::CapacityExceeded;
        }
    }
    uint64 peak = sizeof(ReferenceJob) + sizeof(EnvironmentData);
    if (TryAddSceneAllocation(textureBytes, 1, peak) == false ||
        TryAddSceneAllocation(input.Inputs.Materials.GetSize(), sizeof(SceneShaderMaterial), peak) == false ||
        TryAddSceneAllocation(input.Inputs.EnvironmentPixels.GetSize(),
                              sizeof(math::Vector4) + sizeof(float64),
                              peak) == false ||
        TryAddSceneAllocation(surfaceCapacity,
                              sizeof(ReferenceInstance) + sizeof(ReferenceSurfaceData) + sizeof(math::Vector4) +
                                  sizeof(ReferenceEmitterBounds),
                              peak) == false ||
        TryAddSceneAllocation(triangleCapacity,
                              sizeof(ReferenceTriangle) + 2 * sizeof(ReferenceBvhNode) +
                                  REFERENCE_TRIANGLE_INDEX_ARRAYS * sizeof(uint32),
                              peak) == false)
    {
        return Status::CapacityExceeded;
    }
    if (peak > byteBudget)
    {
        return RejectSceneMemory(&diagnostics, SceneMemoryBudget::CpuScene, peak, byteBudget);
    }
    if (triangleCapacity > (~uint32{0}) / 2 || surfaceCapacity > ~uint32{0})
    {
        return Status::CapacityExceeded;
    }
    ReferenceJob* allocationOrNull = new (std::nothrow) ReferenceJob;
    core::UniquePtr<ReferenceJob> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->SourceGeneration = input.SourceGeneration;
    candidate->SceneData = input.SceneData;
    candidate->Description = description;
    if (math::TryMakeRandomKey(description.Seed, REFERENCE_RANDOM_DOMAIN, candidate->Key) != math::MathStatus::Success)
    {
        return Status::InvalidDescription;
    }
    candidate->Description.Environment = input.Inputs.Environment;
    LightSelection lights;
    Status status = NormalizeDirectLights(input.Inputs.Lights.GetDataOrNull(), input.Inputs.Lights.GetSize(), lights);
    if (status != Status::Ready)
    {
        return status;
    }
    for (usize i = 0; i < lights.Count; ++i)
    {
        if (IsSceneShaderLightKindSupported(lights.Lights[i].Kind) == false)
        {
            return Status::Unsupported;
        }
        candidate->Lights[i] = lights.Lights[i];
    }
    candidate->Description.LightCount = lights.Count;
    candidate->Description.Lights = candidate->Lights;
    candidate->IsCpuOnly = true;
    if (input.Inputs.EnvironmentPixels.GetSize() != 0)
    {
        candidate->EnvironmentOrNull = new (std::nothrow) EnvironmentData;
        if (candidate->EnvironmentOrNull == nullptr)
        {
            return Status::OutOfMemory;
        }
        EnvironmentData& environment = *candidate->EnvironmentOrNull;
        environment.IsCpuOnly = true;
        environment.Width = input.Inputs.EnvironmentWidth;
        environment.Height = input.Inputs.EnvironmentHeight;
        if (environment.Pixels.TryResize(input.Inputs.EnvironmentPixels.GetSize()) == false)
        {
            return Status::OutOfMemory;
        }
        for (usize pixel = 0; pixel < environment.Pixels.GetSize(); ++pixel)
        {
            const math::Vector3 value = math::Hadamard(input.Inputs.EnvironmentPixels[pixel], input.Inputs.Environment);
            if (ReferenceColor(value, MAX_ENVIRONMENT_RADIANCE) == false)
            {
                return Status::InvalidDescription;
            }
            environment.Pixels[pixel] = {value.X, value.Y, value.Z, 0};
        }
        status = BuildEnvironmentDistribution(environment);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    if (candidate->FlatSurfaces.TryResize(surfaceCapacity) == false ||
        candidate->Scene.Instances.TryResize(surfaceCapacity) == false ||
        candidate->Scene.Triangles.TryEnsureCapacity(triangleCapacity) == false ||
        candidate->Scene.Emitters.TryEnsureCapacity(triangleCapacity) == false ||
        candidate->ShaderEmitters.TryEnsureCapacity(surfaceCapacity) == false ||
        candidate->ShaderEmitterBounds.TryEnsureCapacity(surfaceCapacity) == false ||
        candidate->ShaderTextureBytes.TryResize(textureBytes) == false)
    {
        return Status::OutOfMemory;
    }
    // Texture offsets are resolved before any instance borrows their stable owned allocation.
    Array<SceneShaderMaterial> materials;
    if (materials.TryResize(input.Inputs.Materials.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    usize writtenBytes = 0;
    for (usize i = 0; i < materials.GetSize(); ++i)
    {
        materials[i] = SelectSceneMaterial(input.Inputs.Materials[i], input.SceneData);
        if (IsMaterialValid(materials[i]) == false)
        {
            materials[i] = {};
            materials[i].HasError = true;
            continue;
        }
        const auto copyTexture = [&candidate, &writtenBytes](rhi::TextureCpuContent& inoutTexture) noexcept {
            if (inoutTexture.Bytes != nullptr)
            {
                uint8* destination = candidate->ShaderTextureBytes.GetData() + writtenBytes;
                for (usize byte = 0; byte < inoutTexture.Size; ++byte)
                {
                    destination[byte] = inoutTexture.Bytes[byte];
                }
                writtenBytes += inoutTexture.Size;
                inoutTexture.Bytes = destination;
            }
        };
        copyTexture(materials[i].ColorTexture);
        copyTexture(materials[i].SurfaceTexture);
        copyTexture(materials[i].EmissionTexture);
    }
    usize surfaceIndex = 0;
    for (usize itemIndex = 0; itemIndex < input.Items.GetSize(); ++itemIndex)
    {
        const RenderGraphItem& item = input.Items[itemIndex];
        if (item.IsIntentionallyExcluded == true)
        {
            continue;
        }
        const RenderGraphMesh& mesh = input.Meshes[item.MeshIndex];
        const MeshDescription& geometry = mesh.Geometry;
        math::Matrix3 normalMatrix;
        if (math::TryNormalMatrix(item.MeshToWorld, {}, normalMatrix) != math::MathStatus::Success)
        {
            return Status::InvalidDescription;
        }
        usize slots = math::Max(usize{1}, math::Max(mesh.MaterialBindings.GetSize(), item.MaterialOverrides.GetSize()));
        for (usize primitive = 0; primitive < mesh.MaterialSlots.GetSize(); ++primitive)
        {
            slots = math::Max(slots, usize(mesh.MaterialSlots[primitive]) + 1);
        }
        for (usize slot = 0; slot < slots; ++slot)
        {
            uint32 materialIndex =
                slot < mesh.MaterialBindings.GetSize() ? mesh.MaterialBindings[slot] : DEFAULT_SCENE_MATERIAL;
            if (slot < item.MaterialOverrides.GetSize() && item.MaterialOverrides[slot] != INHERIT_SCENE_MATERIAL)
            {
                materialIndex = item.MaterialOverrides[slot];
            }
            if (hasMaterials == false && hasLights == false)
            {
                materialIndex = DEFAULT_SCENE_MATERIAL;
            }
            SceneShaderMaterial material;
            uint32 materialKind = materialIndex == DEFAULT_SCENE_MATERIAL ? 1U : 0U;
            if (materialIndex != DEFAULT_SCENE_MATERIAL)
            {
                if (materialIndex >= materials.GetSize() || materials[materialIndex].HasError == true)
                {
                    if (diagnostics.ErrorSourceId == 0)
                    {
                        diagnostics.ErrorMaterial = materialIndex;
                        diagnostics.ErrorSourceId = item.SourceId;
                    }
                    if (input.AreErrorsAllowed == false)
                    {
                        diagnostics.ErrorBindings += 1;
                        return Status::InvalidDescription;
                    }
                    material.BaseColor = {1, 0, 1};
                    materialKind = 2;
                }
                else
                {
                    material = materials[materialIndex];
                }
            }
            if (input.HasMaterialInterface == false && hasMaterials == true)
            {
                material.BaseColor = {item.Material.Color.X, item.Material.Color.Y, item.Material.Color.Z};
                materialKind = 0;
            }
            ReferenceSurfaceData& surface = candidate->FlatSurfaces[surfaceIndex];
            if (math::IsFinite(item.SurfaceTint) == false || item.SurfaceTint.X < 0 || item.SurfaceTint.Y < 0 ||
                item.SurfaceTint.Z < 0 || item.SurfaceTint.X > 1 || item.SurfaceTint.Y > 1 || item.SurfaceTint.Z > 1 ||
                item.SurfaceTint.W != 1)
            {
                return Status::InvalidDescription;
            }
            surface.Bsdf = {
                materialKind == 2
                    ? material.BaseColor
                    : math::Hadamard(material.BaseColor,
                                     math::Vector3{item.SurfaceTint.X, item.SurfaceTint.Y, item.SurfaceTint.Z}),
                material.Metallic,
                material.Roughness,
                material.Scattering};
            surface.Emission = material.Emission;
            surface.IsEmissionTwoSided = material.IsEmissionTwoSided;
            surface.UvTransform = material.UvTransform;
            surface.NormalScale = material.NormalScale;
            surface.Alpha = material.IsAlphaMasked == true ? MaterialAlpha::Masked : MaterialAlpha::Opaque;
            surface.AlphaCutoff = material.AlphaCutoff;
            surface.Sampler = material.Sampler;
            surface.HasTexture = material.ColorTexture.Bytes != nullptr;
            surface.HasSurfaceMap = material.SurfaceTexture.Bytes != nullptr;
            surface.HasEmissionMap = material.EmissionTexture.Bytes != nullptr;
            ReferenceInstance& instance = candidate->Scene.Instances[surfaceIndex];
            instance.Surface = &surface;
            instance.Texture = material.ColorTexture;
            instance.SurfaceMap = material.SurfaceTexture;
            instance.EmissionMap = material.EmissionTexture;
            instance.SourceId = item.SourceId;
            instance.FirstTriangle = candidate->Scene.Triangles.GetSize();
            const bool isEmitter =
                hasLights == true && (material.Emission.X > 0 || material.Emission.Y > 0 || material.Emission.Z > 0);
            const usize firstEmitter = candidate->Scene.Emitters.GetSize();
            float32 totalArea = 0;
            for (usize primitive = 0; primitive < geometry.IndexCount / CORNERS; ++primitive)
            {
                const uint32 triangleSlot = mesh.MaterialSlots.GetSize() != 0 ? mesh.MaterialSlots[primitive] : 0;
                if (triangleSlot != slot)
                {
                    continue;
                }
                ReferenceTriangle triangle;
                triangle.Instance = static_cast<uint32>(surfaceIndex);
                triangle.Primitive = static_cast<uint32>(primitive);
                triangle.ShaderInstance = static_cast<uint32>(itemIndex);
                triangle.ShaderDraw = item.MeshIndex;
                triangle.ShaderMaterial = materialIndex;
                triangle.ShaderSlot = static_cast<uint32>(slot);
                triangle.ShaderMaterialKind = materialKind;
                triangle.ShaderSourceId = item.SourceId;
                for (usize corner = 0; corner < CORNERS; ++corner)
                {
                    const uint32 vertex = geometry.Indices[primitive * CORNERS + corner];
                    if (vertex >= geometry.VertexCount || math::IsFinite(geometry.Positions[vertex]) == false)
                    {
                        return Status::InvalidDescription;
                    }
                    triangle.Positions[corner] = math::TransformPoint(item.MeshToWorld, geometry.Positions[vertex]);
                    const math::Vector3 errorWorld =
                        ComputeReferencePositionError(item.MeshToWorld, geometry.Positions[vertex]);
                    triangle.Error = {math::Max(triangle.Error.X, errorWorld.X),
                                      math::Max(triangle.Error.Y, errorWorld.Y),
                                      math::Max(triangle.Error.Z, errorWorld.Z)};
                    triangle.Uvs[corner] = geometry.Texcoords != nullptr ? geometry.Texcoords[vertex] : math::Vector2{};
                    if (math::IsFinite(triangle.Positions[corner]) == false ||
                        math::IsFinite(triangle.Uvs[corner]) == false)
                    {
                        return Status::InvalidDescription;
                    }
                    triangle.Colors[corner] =
                        geometry.Colors.GetSize() != 0 ? geometry.Colors[vertex] : math::Vector4{1, 1, 1, 1};
                    if (math::IsFinite(triangle.Colors[corner]) == false ||
                        ReferenceColor(
                            {triangle.Colors[corner].X, triangle.Colors[corner].Y, triangle.Colors[corner].Z},
                            1) == false ||
                        triangle.Colors[corner].W < 0 || triangle.Colors[corner].W > 1)
                    {
                        return Status::InvalidDescription;
                    }
                    if (geometry.Tangents.GetSize() != 0)
                    {
                        const math::Vector4 tangent = geometry.Tangents[vertex];
                        math::Vector3 tangentWorld = ReferenceNormalize(
                            math::TransformVector(item.MeshToWorld, {tangent.X, tangent.Y, tangent.Z}));
                        if (math::IsFinite(tangent) == false || (tangent.W != 0 && tangent.W != 1 && tangent.W != -1))
                        {
                            return Status::InvalidDescription;
                        }
                        const float32 orientation =
                            math::Dot(math::Cross(item.MeshToWorld.Columns[0], item.MeshToWorld.Columns[1]),
                                      item.MeshToWorld.Columns[2]) < 0
                                ? -1.F
                                : 1.F;
                        triangle.Tangents[corner] = {tangentWorld.X,
                                                     tangentWorld.Y,
                                                     tangentWorld.Z,
                                                     tangent.W * orientation};
                    }
                    if (geometry.Normals != nullptr)
                    {
                        triangle.Normals[corner] = ReferenceNormalize(normalMatrix * geometry.Normals[vertex]);
                        if (math::Length(triangle.Normals[corner]) == 0)
                        {
                            return Status::InvalidDescription;
                        }
                    }
                }
                const math::Vector3 edgeWorld = triangle.Positions[1] - triangle.Positions[0];
                const math::Vector3 otherEdgeWorld = triangle.Positions[2] - triangle.Positions[0];
                const math::Vector3 crossWorld = math::Cross(edgeWorld, otherEdgeWorld);
                triangle.Area = math::Length(crossWorld) * 0.5F;
                if (math::IsFinite(triangle.Area) == false)
                {
                    return Status::InvalidDescription;
                }
                if (triangle.Area == 0)
                {
                    diagnostics.SkippedTriangles += 1;
                    continue;
                }
                const math::Vector3 positionMesh = geometry.Positions[geometry.Indices[primitive * CORNERS]];
                const math::Vector3 edgeMesh =
                    geometry.Positions[geometry.Indices[primitive * CORNERS + 1]] - positionMesh;
                const math::Vector3 otherEdgeMesh =
                    geometry.Positions[geometry.Indices[primitive * CORNERS + 2]] - positionMesh;
                // Match the existing reference renderer's authored outward orientation,
                // including reflected instances; world area remains unsigned.
                triangle.GeometricNormal = ReferenceNormalize(normalMatrix * math::Cross(edgeMesh, otherEdgeMesh));
                if (math::Length(triangle.GeometricNormal) == 0)
                {
                    return Status::Unsupported;
                }
                const float32 rounding = (math::Length(edgeWorld) + math::Length(otherEdgeWorld)) * POSITION_ROUNDING;
                triangle.Error = triangle.Error + math::Vector3{rounding, rounding, rounding};
                if (geometry.Normals == nullptr)
                {
                    for (usize corner = 0; corner < CORNERS; ++corner)
                    {
                        triangle.Normals[corner] = triangle.GeometricNormal;
                    }
                }
                if (isEmitter == true)
                {
                    triangle.ShaderEmitter = static_cast<uint32>(candidate->ShaderEmitters.GetSize());
                    totalArea += triangle.Area;
                    if (math::IsFinite(totalArea) == false || candidate->Scene.Emitters.TryAdd(static_cast<uint32>(
                                                                  candidate->Scene.Triangles.GetSize())) == false)
                    {
                        return Status::CapacityExceeded;
                    }
                }
                if (candidate->Scene.Triangles.TryAdd(triangle) == false)
                {
                    return Status::OutOfMemory;
                }
            }
            instance.TriangleCount = candidate->Scene.Triangles.GetSize() - instance.FirstTriangle;
            if (instance.TriangleCount != 0)
            {
                diagnostics.DefaultBindings += materialKind == 1 ? 1U : 0U;
                diagnostics.ErrorBindings += materialKind == 2 ? 1U : 0U;
            }
            if (isEmitter == true && totalArea > 0)
            {
                if (candidate->ShaderEmitters.TryAdd(
                        {static_cast<float32>(firstEmitter),
                         static_cast<float32>(candidate->Scene.Emitters.GetSize() - firstEmitter),
                         totalArea,
                         static_cast<float32>(surfaceIndex)}) == false ||
                    candidate->ShaderEmitterBounds.TryAdd(
                        ComputeEmitterBounds(candidate->Scene, firstEmitter, material.IsEmissionTwoSided)) == false)
                {
                    return Status::OutOfMemory;
                }
            }
            ++surfaceIndex;
        }
    }
    candidate->Scene.InstanceCount = surfaceIndex;
    status = BuildReferenceBvh(candidate->Scene);
    if (status != Status::Ready)
    {
        return status;
    }
    status = BuildReferenceEmitterSelection(candidate->Scene);
    if (status != Status::Ready)
    {
        return status;
    }
    status = PrepareReferenceCamera(*candidate);
    if (status != Status::Ready)
    {
        return status;
    }
    candidate->Info.CpuBytes = static_cast<usize>(peak);
    outJob.Swap(candidate);
    return Status::Ready;
}
} // namespace ludus::graphics::renderer::internal
