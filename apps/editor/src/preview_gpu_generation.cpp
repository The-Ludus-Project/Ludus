#include "internal/preview_gpu_generation.h"
#include "internal/preview_lighting.h"
#include "internal/preview_shader_set.h"
#include "internal/preview_shading.h"
#include "internal/render_scene.h"
#include "internal/render_scene_limits.h"
#include "internal/static_lighting_job.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/content/scene_format/luscn.h>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

#include <ludus/graphics/renderer/environment.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/materials.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/static_lighting.hpp>

#include <ludus/runtime/resources/scene.h>

#include <QImage>
#include <QString>
#include <QVector>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace renderer = graphics::renderer;
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
/// Indices per triangle in a preview mesh's triangle list.
constexpr qsizetype TRIANGLE_INDEX_COUNT = 3;
/// Lowest perceptual roughness CreateLitMaterial admits (its GGX alpha floor).
constexpr float32 MINIMUM_LIT_ROUGHNESS = 0.045F;
/// Live material versions per material slot: shadow caster, lit, and the replacement
/// generation's while it prepares.
constexpr uint32 MATERIAL_VERSIONS_PER_SLOT = 3;
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
/// Area-weighted vertex normals for unlit authored meshes, which carry none. Debug views need
/// normals for the lit program; degenerate fans fall back to +Y so every normal is nonzero.
QVector<math::Vector3> ComputeVertexNormals(const PreviewMesh& mesh)
{
    QVector<math::Vector3> normals(mesh.Positions.size(), math::Vector3{});
    for (qsizetype i = 0; i + 2 < mesh.Indices.size(); i += TRIANGLE_INDEX_COUNT)
    {
        const uint32 first = mesh.Indices[i];
        const uint32 second = mesh.Indices[i + 1];
        const uint32 third = mesh.Indices[i + 2];
        const uint32 vertexCount = static_cast<uint32>(normals.size());
        if (first >= vertexCount || second >= vertexCount || third >= vertexCount)
        {
            continue; // CreateMesh rejects the mesh; never read outside its positions here.
        }
        const math::Vector3 faceMesh =
            math::Cross(mesh.Positions[second] - mesh.Positions[first], mesh.Positions[third] - mesh.Positions[first]);
        normals[first] = normals[first] + faceMesh;
        normals[second] = normals[second] + faceMesh;
        normals[third] = normals[third] + faceMesh;
    }
    for (math::Vector3& normal : normals)
    {
        if (math::TryNormalize(normal, 0.0F, normal) != math::MathStatus::Success)
        {
            normal = {0, 1, 0};
        }
    }
    return normals;
}
/// Indices of the triangles with a nonzero mesh-space area. RT1 admission rejects a zero-area
/// triangle, while the raster viewport draws it as nothing; it has no area to hit or emit from,
/// so omitting it leaves the reference integral unchanged.
QVector<uint32> CollectNondegenerateIndices(const resources::SceneMesh& mesh)
{
    QVector<uint32> indices;
    indices.reserve(static_cast<qsizetype>(mesh.Indices.GetSize()));
    for (usize i = 0; i + 2 < mesh.Indices.GetSize(); i += static_cast<usize>(TRIANGLE_INDEX_COUNT))
    {
        const uint32 first = mesh.Indices[i];
        const uint32 second = mesh.Indices[i + 1];
        const uint32 third = mesh.Indices[i + 2];
        const usize vertexCount = mesh.Positions.GetSize();
        if (first < vertexCount && second < vertexCount && third < vertexCount)
        {
            const math::Vector3 areaMesh = math::Cross(mesh.Positions[second] - mesh.Positions[first],
                                                       mesh.Positions[third] - mesh.Positions[first]);
            if (areaMesh.X == 0 && areaMesh.Y == 0 && areaMesh.Z == 0)
            {
                continue;
            }
        }
        // Out-of-range corners stay so CreateMesh rejects the mesh rather than this filter hiding it.
        indices.append(first);
        indices.append(second);
        indices.append(third);
    }
    return indices;
}
#endif
} // namespace

PreviewGpuGeneration::PreviewGpuGeneration(rhi::DeviceHandle device,
                                           const RenderScene& source,
                                           PreviewShading shading,
                                           const PreviewShaderSet& shaders,
                                           const PreviewReferenceSetup* referenceOrNull)
    : Device(device), Source(source), Shading(shading), Shaders(shaders),
      LightingKey(preview_lighting::MakeKey(source)), IsReference(referenceOrNull != nullptr)
{
    if (referenceOrNull != nullptr)
    {
        mReference = *referenceOrNull;
    }
}
PreviewGpuGeneration::~PreviewGpuGeneration() noexcept
{
    Viewport.Reset();
    Renderer.Reset();
    Environment.Reset();
    for (rhi::TextureHandle& texture : Textures)
    {
        static_cast<void>(rhi::Destroy(Device, texture));
    }
}

PreviewPreparationStatus PreviewGpuGeneration::Prepare(rhi::SurfaceHandle surface, QString& outError) noexcept
{
    if (mHasFailed == true)
    {
        return PreviewPreparationStatus::Failed;
    }
    if (prepareResources(surface, outError) == false)
    {
        mHasFailed = true;
        if (outError.isEmpty() == true)
        {
            switch (mStage)
            {
                case PreparationStage::Renderer:
                    outError = QStringLiteral("GPU renderer initialization failed.");
                    break;
                case PreparationStage::Views:
                    outError = QStringLiteral("GPU viewport composition setup failed.");
                    break;
                case PreparationStage::Materials:
                    outError = QStringLiteral("GPU material pipeline preparation failed.");
                    break;
                case PreparationStage::Assets:
                    outError = QStringLiteral("GPU scene mesh, texture or material upload failed.");
                    break;
                case PreparationStage::Snapshot:
                    outError = QStringLiteral("GPU scene snapshot preparation failed.");
                    break;
                case PreparationStage::Ready:
                    break;
            }
        }
        return PreviewPreparationStatus::Failed;
    }
    return IsPrepared() == true ? PreviewPreparationStatus::Ready : PreviewPreparationStatus::Pending;
}

bool PreviewGpuGeneration::acceptStage(rhi::RasterStatus status, PreparationStage next) noexcept
{
    if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
    {
        return false;
    }
    mStage = next;
    return true;
}

bool PreviewGpuGeneration::prepareResources(rhi::SurfaceHandle surface, QString& outError) noexcept
{
    switch (mStage)
    {
        case PreparationStage::Renderer:
            return prepareRenderer();
        case PreparationStage::Views:
            return prepareViews();
        case PreparationStage::Materials:
            return prepareMaterials();
        case PreparationStage::Assets:
            return prepareAssets(outError);
        case PreparationStage::Snapshot:
            return prepareSnapshot(surface, outError);
        case PreparationStage::Ready:
            return true;
    }
    return false;
}

bool PreviewGpuGeneration::prepareRenderer() noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    renderer::Limits limits;
    limits.Views = renderer::SHADOW_REGION_CAPACITY * 2 + 8;
    // Version 5 (import profile) scenes: every mesh, object and material version.
    limits.Meshes = static_cast<uint32>(PREVIEW_MESH_CAPACITY);
    limits.Objects = static_cast<uint32>(MAX_PREVIEW_OBJECTS);
    limits.Materials = static_cast<uint32>(PREVIEW_MATERIAL_CAPACITY) * MATERIAL_VERSIONS_PER_SLOT;
    limits.Snapshots = 8;
    limits.HostBytes = usize{128} * 1024 * 1024;
    return acceptStage(Renderer.Initialize(Device,
                                           Shaders.Get(PreviewShaderProgram::Flat, PreviewShaderStage::Vertex),
                                           Shaders.Get(PreviewShaderProgram::Flat, PreviewShaderStage::Fragment),
                                           limits),
                       PreparationStage::Views);
#else
    return false;
#endif
}

bool PreviewGpuGeneration::prepareViews() noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    const rhi::RasterStatus status = Renderer.Poll();
    return status == rhi::RasterStatus::Pending ||
           (status == rhi::RasterStatus::Ready &&
            acceptStage(
                Renderer.InitializeViews(Shaders.Get(PreviewShaderProgram::Composite, PreviewShaderStage::Vertex),
                                         Shaders.Get(PreviewShaderProgram::Composite, PreviewShaderStage::Fragment)),
                PreparationStage::Materials));
#else
    return false;
#endif
}

bool PreviewGpuGeneration::prepareMaterials() noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    const rhi::RasterStatus status = Renderer.PollViews();
    if (status == rhi::RasterStatus::Ready)
    {
        if (mIsLightingInitialized == false)
        {
            const rhi::RasterStatus lighting =
                Renderer.InitializeLighting(Shaders.Get(PreviewShaderProgram::Output, PreviewShaderStage::Vertex),
                                            Shaders.Get(PreviewShaderProgram::Output, PreviewShaderStage::Fragment),
                                            Shaders.Get(PreviewShaderProgram::Shadow, PreviewShaderStage::Fragment));
            if (IsAccepted(lighting) == false)
            {
                return false;
            }
            mIsLightingInitialized = true;
        }
        const rhi::RasterStatus lighting = Renderer.PollLighting();
        if (lighting == rhi::RasterStatus::Pending)
        {
            return true;
        }
        if (lighting != rhi::RasterStatus::Ready)
        {
            return false;
        }
        if (shouldUseAuthoredLighting() == true && IsPreviewDebugShading(Shading) == true)
        {
            return acceptStage(
                Renderer.PrewarmLitProgram(
                    Shaders.Get(PreviewShaderProgram::ClusteredAuthoredDebug, PreviewShaderStage::Vertex),
                    Shaders.Get(PreviewShaderProgram::ClusteredAuthoredDebug, PreviewShaderStage::Fragment),
                    Shaders.Get(PreviewShaderProgram::ClusteredAuthoredDebugShadow, PreviewShaderStage::Fragment),
                    mProgram),
                PreparationStage::Assets);
        }
        if (shouldUseAuthoredLighting() == true)
        {
            return acceptStage(
                Renderer.PrewarmLitProgram(
                    Shaders.Get(PreviewShaderProgram::ClusteredAuthored, PreviewShaderStage::Vertex),
                    Shaders.Get(PreviewShaderProgram::ClusteredAuthored, PreviewShaderStage::Fragment),
                    Shaders.Get(PreviewShaderProgram::ClusteredAuthoredShadow, PreviewShaderStage::Fragment),
                    mProgram),
                PreparationStage::Assets);
        }
        if (shouldUseUnlitDebugView() == true)
        {
            return acceptStage(Renderer.PrewarmLitProgram(
                                   Shaders.Get(PreviewShaderProgram::LitDebug, PreviewShaderStage::Vertex),
                                   Shaders.Get(PreviewShaderProgram::LitDebug, PreviewShaderStage::Fragment),
                                   Shaders.Get(PreviewShaderProgram::LitDebugShadow, PreviewShaderStage::Fragment),
                                   mProgram),
                               PreparationStage::Assets);
        }
        return acceptStage(
            shouldUseLitMaterials() == true
                ? Renderer.PrewarmLitProgram(Shaders.Get(PreviewShaderProgram::Lit, PreviewShaderStage::Vertex),
                                             Shaders.Get(PreviewShaderProgram::Lit, PreviewShaderStage::Fragment),
                                             Shaders.Get(PreviewShaderProgram::LitShadow, PreviewShaderStage::Fragment),
                                             mProgram)
                : Renderer.PrewarmMaterialProgram(
                      Shaders.Get(PreviewShaderProgram::Material, PreviewShaderStage::Vertex),
                      Shaders.Get(PreviewShaderProgram::Material, PreviewShaderStage::Fragment),
                      mProgram),
            PreparationStage::Assets);
    }
    return status == rhi::RasterStatus::Pending ||
           (status == rhi::RasterStatus::Ready &&
            acceptStage(Renderer.PrewarmMaterialProgram(
                            Shaders.Get(PreviewShaderProgram::Material, PreviewShaderStage::Vertex),
                            Shaders.Get(PreviewShaderProgram::Material, PreviewShaderStage::Fragment),
                            mProgram),
                        PreparationStage::Assets));
#else
    return false;
#endif
}

bool PreviewGpuGeneration::enableReferenceRendering(QString& outError) noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    if (shouldUseAuthoredLighting() == false || Shading != PreviewShading::Lit)
    {
        outError = QStringLiteral("Reference capture needs a lit scene: add a light or an emissive surface.");
        return false;
    }
    // Must precede every mesh and material version, so RT1 retains their CPU copies.
    const rhi::RasterStatus status =
        mReference.Options.Traversal == graphics::renderer::ReferenceTraversal::GpuCompute
            ? Renderer.EnableReferenceRendering(mReference.TraceShader, mReference.ReduceShader, mReference.Options)
            : Renderer.EnableReferenceRendering(mReference.QueryShader, mReference.Options);
    if (status != rhi::RasterStatus::Ready)
    {
        outError = QStringLiteral("Reference rendering could not be enabled on this device (status %1).")
                       .arg(static_cast<uint32>(status));
        return false;
    }
    return true;
#else
    static_cast<void>(outError);
    return false;
#endif
}

bool PreviewGpuGeneration::prepareAssets(QString& outError) noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    const rhi::RasterStatus programStatus = Renderer.GetStatus(mProgram);
    if (programStatus == rhi::RasterStatus::Pending)
    {
        return true;
    }
    if (programStatus != rhi::RasterStatus::Ready)
    {
        return false;
    }
    if (mAreAssetsCreated == false)
    {
        if (IsReference == true && enableReferenceRendering(outError) == false)
        {
            return false;
        }
        for (int32 i = 0; i < Source.Meshes.size(); ++i)
        {
            const PreviewMesh& mesh = Source.Meshes[i];
            renderer::MeshDescription description;
            if (Source.Lit == true)
            {
                const resources::SceneMesh& source = Source.Definition->Meshes[static_cast<usize>(i)];
                description.Positions = source.Positions.GetData();
                description.VertexCount = source.Positions.GetSize();
                description.Indices = source.Indices.GetData();
                description.IndexCount = source.Indices.GetSize();
                description.Texcoords = source.Texcoords.GetData();
                description.Normals = source.Normals.GetData();
                // Cooked v6 tangent frames; vertices without one (W = 0) use the UV-derived frame.
                description.Tangents = {source.Tangents.GetData(), source.Tangents.GetSize()};
            }
            // CreateMesh copies normals and indices, so derived storage only lives for this call.
            QVector<math::Vector3> derivedNormals;
            QVector<uint32> referenceIndices;
            if (Source.Lit == true && IsReference == true)
            {
                referenceIndices = CollectNondegenerateIndices(Source.Definition->Meshes[static_cast<usize>(i)]);
                description.Indices = referenceIndices.constData();
                description.IndexCount = static_cast<usize>(referenceIndices.size());
            }
            if (Source.Lit == false)
            {
                description.Positions = mesh.Positions.constData();
                description.VertexCount = static_cast<usize>(mesh.Positions.size());
                description.Indices = mesh.Indices.constData();
                description.IndexCount = static_cast<usize>(mesh.Indices.size());
                description.Texcoords = mesh.Uvs.constData();
                description.Normals = mesh.Normals.isEmpty() == true ? nullptr : mesh.Normals.constData();
                if (description.Normals == nullptr && shouldUseUnlitDebugView() == true)
                {
                    derivedNormals = ComputeVertexNormals(mesh);
                    description.Normals = derivedNormals.constData();
                }
            }
            if (IsAccepted(Renderer.CreateMesh(description, mMeshes[i])) == false)
            {
                return false;
            }
        }
        for (int32 i = 0; i < Source.Textures.size(); ++i)
        {
            const QImage& image = Source.Textures[i].Pixels;
            // Surface maps are linear data; colour textures decode sRGB.
            const rhi::RasterFormat format =
                Source.Textures[i].IsLinear == true ? rhi::RasterFormat::Rgba8Unorm : rhi::RasterFormat::Rgba8Srgb;
            if (IsAccepted(rhi::CreateTexture(Device,
                                              {
                                                  .Width = static_cast<uint32>(image.width()),
                                                  .Height = static_cast<uint32>(image.height()),
                                                  .Format = format,
                                                  .RetainCpuContent = IsReference,
                                              },
                                              {
                                                  .Bytes = {image.constBits(), static_cast<usize>(image.sizeInBytes())},
                                                  .RowPitch = static_cast<usize>(image.bytesPerLine()),
                                              },
                                              Textures[i])) == false)
            {
                return false;
            }
        }
        mAreAssetsCreated = true;
    }
    for (int32 i = 0; i < Source.Meshes.size(); ++i)
    {
        const rhi::RasterStatus status = Renderer.GetStatus(mMeshes[i]);
        if (IsAccepted(status) == false)
        {
            return false;
        }
        if (status == rhi::RasterStatus::Pending)
        {
            return true;
        }
    }
    for (int32 i = 0; i < Source.Textures.size(); ++i)
    {
        const rhi::RasterStatus status = rhi::GetStatus(Device, Textures[i]);
        if (IsAccepted(status) == false)
        {
            return false;
        }
        if (status == rhi::RasterStatus::Pending)
        {
            return true;
        }
    }
    if (shouldUseAuthoredLighting() == true)
    {
        mStage = PreparationStage::Snapshot;
        return true;
    }
    if (shouldUseUnlitDebugView() == true)
    {
        if (createUnlitDebugMaterials() == false)
        {
            return false;
        }
        mStage = PreparationStage::Snapshot;
        return true;
    }
    for (int32 i = 0; i < Source.Surfaces.size(); ++i)
    {
        const PreviewSurface& surface = Source.Surfaces[i];
        renderer::MaterialDescription description;
        if (surface.Texture >= 0)
        {
            description.Texture = Textures[surface.Texture];
        }
        description.Tint = surface.Tint;
        if (surface.IsAlphaMasked == true)
        {
            description.Alpha = renderer::MaterialAlpha::Masked;
            description.AlphaCutoff = surface.AlphaCutoff;
        }
        if (Shading == PreviewShading::Solid)
        {
            // Solid shading samples no texture, so masked surfaces draw fully covered.
            description.Texture = {};
            description.Tint = {.6F, .6F, .6F, 1};
            description.Alpha = renderer::MaterialAlpha::Opaque;
        }
        description.Sampler.Filter = rhi::RasterFilter::Linear;
        description.Sampler.U = description.Sampler.V =
            surface.Repeat == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
        renderer::LitMaterialDescription lit;
        lit.Surface = description;
        lit.Surface.Sampler.Filter = rhi::RasterFilter::Linear;
        lit.Surface.Sampler.U = lit.Surface.Sampler.V =
            surface.Repeat == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
        lit.Metallic = surface.Metallic;
        lit.Roughness = surface.Roughness;
        lit.Emission = surface.Emission;
        renderer::DirectLight previewLight;
        if (Shading == PreviewShading::Solid)
        {
            // Explicit editor visualization; these values never enter the World.
            previewLight.SourceId = 1;
            previewLight.Intensity = 3;
            previewLight.Direction = {.4F, -.8F, -.5F};
            lit.Lights = &previewLight;
            lit.LightCount = 1;
            lit.Metallic = 0;
            lit.Roughness = 1;
            lit.Emission = {};
            lit.EnvironmentIrradiance = {.5F, .5F, .5F};
        }
        renderer::LightSelection selection;
        if (IsAccepted(shouldUseLitMaterials() == true
                           ? Renderer.CreateLitMaterial(mProgram, lit, mMaterials[i], selection)
                           : Renderer.CreateMaterial(mProgram, description, mMaterials[i])) == false)
        {
            return false;
        }
    }
    mStage = PreparationStage::Snapshot;
    return true;
#else
    static_cast<void>(outError);
    return false;
#endif
}

bool PreviewGpuGeneration::createUnlitDebugMaterials() noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    // One material per authored surface plus a default for untextured objects. No lights are
    // invented: unlit Worlds have none, so Light count truthfully shows black.
    for (int32 i = 0; i <= Source.Surfaces.size(); ++i)
    {
        renderer::LitMaterialDescription material;
        if (i < Source.Surfaces.size())
        {
            const PreviewSurface& surface = Source.Surfaces[i];
            if (surface.Texture >= 0)
            {
                material.Surface.Texture = Textures[surface.Texture];
            }
            material.Surface.Sampler.U = material.Surface.Sampler.V =
                surface.Repeat == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
            material.Metallic = surface.Metallic;
            material.Roughness = math::Clamp(surface.Roughness, MINIMUM_LIT_ROUGHNESS, 1.0F);
        }
        material.Surface.Sampler.Filter = rhi::RasterFilter::Linear;
        material.DebugView = GetMaterialDebugView(Shading);
        renderer::LightSelection selection;
        if (IsAccepted(Renderer.CreateLitMaterial(mProgram, material, mMaterials[i], selection)) == false)
        {
            return false;
        }
    }
    return true;
#else
    return false;
#endif
}

bool PreviewGpuGeneration::prepareSnapshot(rhi::SurfaceHandle surface, QString& outError) noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    if (shouldUseAuthoredLighting() == true)
    {
        return prepareLighting(surface, outError);
    }
    Array<renderer::SceneItem> items;
    if (items.TryResize(static_cast<usize>(Source.Objects.size())) == false)
    {
        return false;
    }
    for (int32 i = 0; i < Source.Objects.size(); ++i)
    {
        const PreviewObject& object = Source.Objects[i];
        renderer::SceneItem& item = items[i];
        item.Geometry = mMeshes[object.Mesh];
        item.Transform = object.Transform;
        item.Material.Color = Shading == PreviewShading::Solid ? math::Vector4{1, 1, 1, 1} : object.Tint;
        item.SourceId = static_cast<uint64>(i) + 1;
        const bool isDebugView = shouldUseUnlitDebugView();
        if (Source.Lit == true || object.Texture >= 0 || isDebugView == true)
        {
            // Debug views need a lit material on every object; untextured ones share the default slot.
            const int32 slot = object.Surface >= 0 ? object.Surface : getDefaultDebugMaterial();
            const rhi::RasterStatus status = Renderer.GetStatus(mMaterials[slot]);
            if (IsAccepted(status) == false)
            {
                return false;
            }
            if (status == rhi::RasterStatus::Pending)
            {
                return true;
            }
            item.Surface = mMaterials[slot];
            if (isDebugView == true)
            {
                item.Material.Color = {1, 1, 1, 1};
            }
        }
    }
    if (IsAccepted(Renderer.CreateSnapshot(items.GetData(), static_cast<usize>(Source.Objects.size()), mSnapshot)) ==
        false)
    {
        return false;
    }
    mStage = PreparationStage::Ready;
    return true;
#else
    static_cast<void>(surface);
    static_cast<void>(outError);
    return false;
#endif
}
rhi::RasterStatus PreviewGpuGeneration::CreateLightingMaterial(usize objectIndex,
                                                               bool hasShadows,
                                                               renderer::MaterialVersion& outMaterial) noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    PreviewGpuGeneration& resources = *this;
    const renderer::StaticLightingSurfaceDescription& surface = resources.Lighting.Surfaces[objectIndex];
    renderer::LitMaterialDescription material;
    if (surface.Texture != content::scene_format::NO_TEXTURE)
    {
        material.Surface.Texture = resources.Textures[surface.Texture];
    }
    material.Surface.Tint = surface.Tint;
    material.Surface.UvTransform = surface.UvTransform;
    material.Surface.Alpha = surface.Alpha;
    material.Surface.AlphaCutoff = surface.AlphaCutoff;
    material.Surface.Sampler.Filter = rhi::RasterFilter::Linear;
    material.Surface.Sampler.U = material.Surface.Sampler.V =
        surface.ShouldRepeatTexture == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
    material.Metallic = surface.Metallic;
    material.Roughness = surface.Roughness;
    material.Emission = surface.Emission;
    material.IsEmissionTwoSided = surface.IsEmissionTwoSided;
    // Cooked v6 maps share the base texture's coordinates and sampler.
    const PreviewSurface& maps =
        resources.Source.Surfaces[resources.Source.Objects[static_cast<int32>(objectIndex)].Surface];
    if (maps.SurfaceTexture >= 0)
    {
        material.SurfaceMap = resources.Textures[maps.SurfaceTexture];
        material.NormalScale = maps.NormalScale;
    }
    if (maps.EmissionTexture >= 0)
    {
        material.EmissionMap = resources.Textures[maps.EmissionTexture];
    }
    material.AreAuthoredInputsEnabled = true;
    material.Clusters = &resources.Lighting.Clusters;
    material.Lights = resources.Lighting.Lights.Lights;
    material.LightCount = resources.Lighting.Lights.Count;
    if (hasShadows == true && resources.Lighting.ShadowCount != 0)
    {
        material.Shadows = resources.Shadows;
    }
    material.DebugView = GetMaterialDebugView(Shading);
    if (resources.HasEnvironment == true)
    {
        material.EnvironmentOrNull = &resources.Environment;
    }
    else
    {
        const math::Vector3 radiance = resources.Source.Definition->Environment.Rgb;
        material.EnvironmentIrradiance = radiance * math::kPiF;
        material.EnvironmentRadiance = radiance;
    }
    renderer::LightSelection report;
    return resources.Renderer.CreateLitMaterial(mProgram, material, outMaterial, report);
#else
    static_cast<void>(objectIndex);
    static_cast<void>(hasShadows);
    static_cast<void>(outMaterial);
    return rhi::RasterStatus::Unsupported;
#endif
}

bool PreviewGpuGeneration::prepareLighting(rhi::SurfaceHandle surface, QString& outError) noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    PreviewGpuGeneration& resources = *this;
    const usize count = resources.Source.Definition->Objects.GetSize();
    if (resources.LightingStage == PreviewLightingStage::Inputs)
    {
        const rhi::RasterStatus status = preview_lighting::BuildInputs(resources.Source,
                                                                       resources.LightingKey,
                                                                       resources.Lighting,
                                                                       IsReference == false);
        if (status != rhi::RasterStatus::Ready)
        {
            outError = QStringLiteral("Authored lighting admission failed (%1); no lights were dropped.")
                           .arg(static_cast<uint32>(status));
            return false;
        }
        const resources::SceneEnvironment& environment = resources.Source.Definition->Environment;
        if (environment.Width != 0)
        {
            Array<math::Vector4> pixels;
            if (pixels.TryResize(environment.Pixels.GetSize()) == false)
            {
                return false;
            }
            for (usize i = 0; i < pixels.GetSize(); ++i)
            {
                const math::Vector3 rgb{environment.Pixels[i].X * environment.Rgb.X,
                                        environment.Pixels[i].Y * environment.Rgb.Y,
                                        environment.Pixels[i].Z * environment.Rgb.Z};
                pixels[i] = {rgb.X, rgb.Y, rgb.Z, 1};
            }
            renderer::EnvironmentDescription description;
            description.PixelsOrNull = pixels.GetData();
            description.Width = environment.Width;
            description.Height = environment.Height;
            description.FilterWidth = 64;
            description.FilterSamples = 16;
            description.CpuByteBudget = usize{256} * 1024 * 1024;
            if (IsAccepted(resources.Environment.Initialize(Device, description)) == false)
            {
                return false;
            }
            resources.HasEnvironment = true;
        }
        // Reference transport traces its own visibility, so it needs no casters or shadow atlas.
        resources.LightingStage = IsReference == true ? PreviewLightingStage::Materials : PreviewLightingStage::Casters;
    }
    if (resources.HasEnvironment == true)
    {
        const rhi::RasterStatus status = resources.Environment.GetStatus();
        if (status != rhi::RasterStatus::Ready)
        {
            return status == rhi::RasterStatus::Pending;
        }
    }
    if (resources.LightingStage == PreviewLightingStage::Casters)
    {
        while (resources.MaterialCursor < count)
        {
            if (IsAccepted(CreateLightingMaterial(resources.MaterialCursor,
                                                  false,
                                                  resources.CasterMaterials[resources.MaterialCursor])) == false)
            {
                return false;
            }
            ++resources.MaterialCursor;
        }
        Array<renderer::SceneItem> items;
        if (items.TryResize(count) == false)
        {
            return false;
        }
        for (usize i = 0; i < count; ++i)
        {
            const rhi::RasterStatus ready = resources.Renderer.GetStatus(resources.CasterMaterials[i]);
            if (ready != rhi::RasterStatus::Ready)
            {
                return ready == rhi::RasterStatus::Pending;
            }
            const resources::SceneObject& source = resources.Source.Definition->Objects[i];
            items[i].Geometry = mMeshes[source.Mesh];
            items[i].Transform = source.Transform;
            items[i].SourceId = source.SourceId;
            items[i].Surface = resources.CasterMaterials[i];
            items[i].Material.Color = {1, 1, 1, 1};
        }
        if (IsAccepted(resources.Renderer.CreateSnapshot(items.GetData(), count, resources.Casters)) == false)
        {
            return false;
        }
        resources.MaterialCursor = 0;
        resources.LightingStage = PreviewLightingStage::Shadows;
    }
    if (resources.LightingStage == PreviewLightingStage::Shadows)
    {
        if (resources.Lighting.ShadowCount == 0)
        {
            resources.LightingStage = PreviewLightingStage::Materials;
        }
        else
        {
            renderer::ShadowAtlasDescription description;
            description.Width = description.Height = preview_lighting::SHADOW_ATLAS_PIXELS;
            description.Regions = resources.Lighting.Shadows.GetData();
            description.Count = resources.Lighting.ShadowCount;
            if (IsAccepted(resources.Renderer.PrepareShadowAtlas(resources.Casters, description, resources.Shadows)) ==
                false)
            {
                return false;
            }
            resources.LightingStage = PreviewLightingStage::Submission;
        }
    }
    if (resources.LightingStage == PreviewLightingStage::Submission)
    {
        const rhi::RasterStatus ready = resources.Renderer.GetStatus(resources.Shadows);
        if (ready != rhi::RasterStatus::Ready)
        {
            return ready == rhi::RasterStatus::Pending;
        }
#    if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
        constexpr uint32 SHADOW_FRAME_EXTENT = 1;
        if (rhi::SetFrameTarget(Device,
                                surface,
                                {
                                    .Width = SHADOW_FRAME_EXTENT,
                                    .Height = SHADOW_FRAME_EXTENT,
                                    .ShouldPresentToSurface = false,
                                }) != rhi::DeviceStatus::Ready)
        {
            outError = QStringLiteral("The selected GPU backend cannot prepare shadows without presenting.");
            return false;
        }
#    endif
        if (rhi::BeginFrame(Device, surface) != rhi::DeviceStatus::Ready)
        {
            return false;
        }
        const rhi::RasterStatus drawn = resources.Renderer.DrawShadowAtlas(resources.Shadows);
        const rhi::RasterStatus ended = rhi::EndFrame(Device, resources.ShadowProducer);
        if (drawn != rhi::RasterStatus::Ready)
        {
            return false;
        }
        if (ended == rhi::RasterStatus::Pending)
        {
            resources.IsShadowFrameOpen = true;
            return true;
        }
        if (ended != rhi::RasterStatus::Ready)
        {
            return false;
        }
        resources.LightingStage = PreviewLightingStage::Materials;
    }
    if (resources.LightingStage == PreviewLightingStage::Materials)
    {
        if (resources.Lighting.ShadowCount != 0)
        {
            const rhi::RasterStatus status = rhi::GetStatus(Device, resources.ShadowProducer);
            if (status != rhi::RasterStatus::Ready)
            {
                return status == rhi::RasterStatus::Pending;
            }
        }
        while (resources.MaterialCursor < count)
        {
            if (IsAccepted(
                    CreateLightingMaterial(resources.MaterialCursor, true, mMaterials[resources.MaterialCursor])) ==
                false)
            {
                return false;
            }
            ++resources.MaterialCursor;
        }
        Array<renderer::SceneItem> items;
        if (items.TryResize(count) == false)
        {
            return false;
        }
        for (usize i = 0; i < count; ++i)
        {
            const rhi::RasterStatus ready = resources.Renderer.GetStatus(mMaterials[i]);
            if (ready != rhi::RasterStatus::Ready)
            {
                return ready == rhi::RasterStatus::Pending;
            }
            const resources::SceneObject& source = resources.Source.Definition->Objects[i];
            items[i].Geometry = mMeshes[source.Mesh];
            items[i].Transform = source.Transform;
            items[i].SourceId = source.SourceId;
            items[i].Surface = mMaterials[i];
            items[i].Material.Color = {1, 1, 1, 1};
        }
        if (IsAccepted(resources.Renderer.CreateSnapshot(items.GetData(), count, mSnapshot)) == false)
        {
            return false;
        }
        resources.LightingStage = PreviewLightingStage::Ready;
    }
    mStage = PreparationStage::Ready;
    return true;
#else
    static_cast<void>(surface);
    static_cast<void>(outError);
    return false;
#endif
}

} // namespace ludus::editor
