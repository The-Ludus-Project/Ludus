#include "internal/preview_gpu_generation.h"
#include "internal/preview_lighting.h"
#include "internal/preview_shading.h"
#include "internal/render_scene.h"
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

#if defined(LUDUS_EDITOR_GPU_PREVIEW)
#    include "renderer_clustered_authored.h"
#    include "renderer_clustered_authored_shadow.h"
#    include "renderer_composite.h"
#    include "renderer_flat.h"
#    include "renderer_lit.h"
#    include "renderer_lit_shadow.h"
#    include "renderer_material.h"
#    include "renderer_output.h"
#    include "renderer_shadow.h"
#endif

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rhi = graphics::rhi;
namespace renderer = graphics::renderer;
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
#endif
} // namespace

PreviewGpuGeneration::PreviewGpuGeneration(rhi::DeviceHandle device, const RenderScene& source, PreviewShading shading)
    : Device(device), Source(source), Shading(shading), LightingKey(preview_lighting::MakeKey(source))
{
}
PreviewGpuGeneration::~PreviewGpuGeneration() noexcept
{
    // Cancels and drains an in-flight bake before the borrowed scene is released.
    BakeWorker.Reset();
    Bake.Reset();
    Viewport.Reset();
    Renderer.Reset();
    Environment.Reset();
    for (rhi::TextureHandle& texture : BakeTextures)
    {
        static_cast<void>(rhi::Destroy(Device, texture));
    }
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

void PreviewGpuGeneration::PublishBakedSnapshot() noexcept
{
    static_cast<void>(Renderer.Release(mSnapshot));
    mSnapshot = BakedSnapshot;
    IsBakePublished = true;
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
            return prepareAssets();
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
    limits.Materials = content::scene_format::MAX_OBJECTS * 3;
    limits.Snapshots = 8;
    limits.HostBytes = usize{128} * 1024 * 1024;
    return acceptStage(Renderer.Initialize(Device,
                                           ludus::shaders::renderer_flat::Vertex(),
                                           ludus::shaders::renderer_flat::Fragment(),
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
            acceptStage(Renderer.InitializeViews(ludus::shaders::renderer_composite::Vertex(),
                                                 ludus::shaders::renderer_composite::Fragment()),
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
            const rhi::RasterStatus lighting = Renderer.InitializeLighting(ludus::shaders::renderer_output::Vertex(),
                                                                           ludus::shaders::renderer_output::Fragment(),
                                                                           ludus::shaders::renderer_shadow::Fragment());
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
        if (Source.Lit == true && Shading == PreviewShading::Lit)
        {
            return acceptStage(
                Renderer.PrewarmLitProgram(ludus::shaders::renderer_clustered_authored::Vertex(),
                                           ludus::shaders::renderer_clustered_authored::Fragment(),
                                           ludus::shaders::renderer_clustered_authored_shadow::Fragment(),
                                           mProgram),
                PreparationStage::Assets);
        }
        return acceptStage(shouldUseLitMaterials() == true
                               ? Renderer.PrewarmLitProgram(ludus::shaders::renderer_lit::Vertex(),
                                                            ludus::shaders::renderer_lit::Fragment(),
                                                            ludus::shaders::renderer_lit_shadow::Fragment(),
                                                            mProgram)
                               : Renderer.PrewarmMaterialProgram(ludus::shaders::renderer_material::Vertex(),
                                                                 ludus::shaders::renderer_material::Fragment(),
                                                                 mProgram),
                           PreparationStage::Assets);
    }
    return status == rhi::RasterStatus::Pending ||
           (status == rhi::RasterStatus::Ready &&
            acceptStage(Renderer.PrewarmMaterialProgram(ludus::shaders::renderer_material::Vertex(),
                                                        ludus::shaders::renderer_material::Fragment(),
                                                        mProgram),
                        PreparationStage::Assets));
#else
    return false;
#endif
}

bool PreviewGpuGeneration::prepareAssets() noexcept
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
        for (int32 i = 0; i < Source.Meshes.size(); ++i)
        {
            const PreviewMesh& mesh = Source.Meshes[i];
            renderer::MeshDescription description;
            Array<math::Vector3> positionsMesh;
            Array<math::Vector3> normalsMesh;
            Array<math::Vector2> textureCoordinatesMaterial;
            Array<math::Vector2> textureCoordinatesBakeAtlas;
            Array<uint32> indices;
            if (Source.Lit == true)
            {
                const resources::SceneMesh& source = Source.Definition->Meshes[static_cast<usize>(i)];
#    if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
                // Browser direct lighting does not need the native bake's expanded chart vertices.
                description.Positions = source.Positions.GetData();
                description.VertexCount = source.Positions.GetSize();
                description.Indices = source.Indices.GetData();
                description.IndexCount = source.Indices.GetSize();
                description.Texcoords = source.Texcoords.GetData();
                description.Normals = source.Normals.GetData();
#    else
                // Expand every triangle corner and derive its bake chart coordinate, so
                // indexed (LUSCN v4) and stored-chart (v3) meshes share one layout.
                const usize count = source.Indices.GetSize();
                content::scene_format::BakeChartLayout layout;
                if (count > content::scene_format::MAX_INDICES ||
                    content::scene_format::TryMakeBakeChartLayout(static_cast<uint32>(count / 3), layout) == false ||
                    positionsMesh.TryResize(count) == false || normalsMesh.TryResize(count) == false ||
                    textureCoordinatesMaterial.TryResize(count) == false ||
                    textureCoordinatesBakeAtlas.TryResize(count) == false || indices.TryResize(count) == false)
                {
                    return false;
                }
                for (usize vertex = 0; vertex < count; ++vertex)
                {
                    const uint32 index = source.Indices[vertex];
                    positionsMesh[vertex] = source.Positions[index];
                    normalsMesh[vertex] = source.Normals[index];
                    textureCoordinatesMaterial[vertex] = source.Texcoords[index];
                    indices[vertex] = static_cast<uint32>(vertex);
                    if (content::scene_format::TryGetBakeChartUv(layout,
                                                                 static_cast<uint32>(vertex / 3),
                                                                 static_cast<uint32>(vertex % 3),
                                                                 textureCoordinatesBakeAtlas[vertex].X,
                                                                 textureCoordinatesBakeAtlas[vertex].Y) == false)
                    {
                        return false;
                    }
                }
                description.Positions = positionsMesh.GetData();
                description.VertexCount = count;
                description.Indices = indices.GetData();
                description.IndexCount = count;
                description.Texcoords = textureCoordinatesMaterial.GetData();
                description.Normals = normalsMesh.GetData();
                description.BakeTexcoords = textureCoordinatesBakeAtlas.GetData();
#    endif
            }
            else
            {
                description.Positions = mesh.Positions.constData();
                description.VertexCount = static_cast<usize>(mesh.Positions.size());
                description.Indices = mesh.Indices.constData();
                description.IndexCount = static_cast<usize>(mesh.Indices.size());
                description.Texcoords = mesh.Uvs.constData();
                description.Normals = mesh.Normals.isEmpty() == true ? nullptr : mesh.Normals.constData();
            }
            if (IsAccepted(Renderer.CreateMesh(description, mMeshes[i])) == false)
            {
                return false;
            }
        }
        for (int32 i = 0; i < Source.Textures.size(); ++i)
        {
            const QImage& image = Source.Textures[i].Pixels;
            if (IsAccepted(rhi::CreateTexture(Device,
                                              {
                                                  .Width = static_cast<uint32>(image.width()),
                                                  .Height = static_cast<uint32>(image.height()),
                                                  .Format = rhi::RasterFormat::Rgba8Srgb,
                                                  .RetainCpuContent = false,
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
    if (Source.Lit == true && Shading == PreviewShading::Lit)
    {
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
        if (Shading == PreviewShading::Solid)
        {
            description.Texture = {};
            description.Tint = {.6F, .6F, .6F, 1};
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
    return false;
#endif
}

bool PreviewGpuGeneration::prepareSnapshot(rhi::SurfaceHandle surface, QString& outError) noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    if (Source.Lit == true && Shading == PreviewShading::Lit)
    {
        return prepareLighting(surface, outError);
    }
    StaticArray<renderer::SceneItem, content::scene_format::MAX_OBJECTS> items{};
    for (int32 i = 0; i < Source.Objects.size(); ++i)
    {
        const PreviewObject& object = Source.Objects[i];
        renderer::SceneItem& item = items[i];
        item.Geometry = mMeshes[object.Mesh];
        item.Transform = object.Transform;
        item.Material.Color = Shading == PreviewShading::Solid ? math::Vector4{1, 1, 1, 1} : object.Tint;
        item.SourceId = static_cast<uint64>(i) + 1;
        if (Source.Lit == true || object.Texture >= 0)
        {
            const rhi::RasterStatus status = Renderer.GetStatus(mMaterials[object.Surface]);
            if (IsAccepted(status) == false)
            {
                return false;
            }
            if (status == rhi::RasterStatus::Pending)
            {
                return true;
            }
            item.Surface = mMaterials[object.Surface];
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
                                                               const renderer::BakedLighting* bakeOrNull,
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
    material.AreAuthoredInputsEnabled = true;
    material.Clusters = &resources.Lighting.Clusters;
    material.Lights = resources.Lighting.Lights.Lights;
    material.LightCount = resources.Lighting.Lights.Count;
    if (hasShadows == true && resources.Lighting.ShadowCount != 0)
    {
        material.Shadows = resources.Shadows;
    }
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
    if (bakeOrNull != nullptr)
    {
        material.Bake = *bakeOrNull;
    }
    renderer::LightSelection report;
    return resources.Renderer.CreateLitMaterial(mProgram, material, outMaterial, report);
#else
    static_cast<void>(objectIndex);
    static_cast<void>(bakeOrNull);
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
#    if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
                                                                       false
#    else
                                                                       true
#    endif
        );
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
#    if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
        // No shadow submission may clear the browser surface during candidate preparation.
        resources.Lighting.ShadowCount = 0;
        resources.LightingStage = PreviewLightingStage::Materials;
#    else
        resources.LightingStage = PreviewLightingStage::Casters;
#    endif
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
                                                  nullptr,
                                                  false,
                                                  resources.CasterMaterials[resources.MaterialCursor])) == false)
            {
                return false;
            }
            ++resources.MaterialCursor;
        }
        StaticArray<renderer::SceneItem, content::scene_format::MAX_OBJECTS> items{};
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
            if (IsAccepted(CreateLightingMaterial(resources.MaterialCursor,
                                                  nullptr,
                                                  true,
                                                  mMaterials[resources.MaterialCursor])) == false)
            {
                return false;
            }
            ++resources.MaterialCursor;
        }
        StaticArray<renderer::SceneItem, content::scene_format::MAX_OBJECTS> items{};
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
