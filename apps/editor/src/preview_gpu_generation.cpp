#include "internal/preview_gpu_generation.h"
#include "internal/preview_lighting.h"
#include "internal/preview_shader_set.h"
#include "internal/preview_shading.h"
#include "internal/render_scene.h"
#include "internal/render_scene_limits.h"
#include "internal/world_surface_bake.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <ludus/content/scene_format/luscn.h>

#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

#include <ludus/graphics/renderer/environment.hpp>
#include <ludus/graphics/renderer/light_selection.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/materials.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/static_lighting.hpp>

#include <ludus/runtime/resources/scene.h>

#include <QImage>
#include <QString>
#include <QVector>

#include <new>

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
/// Irradiance of an upward-facing Solid surface, and the share of it that a downward-facing one receives. The
/// sphere average, (1 + scale) / 2 of the sky value, stays at the previous constant 0.5.
constexpr float32 SOLID_SKY_IRRADIANCE = 0.77F;
constexpr float32 SOLID_GROUND_SCALE = 0.3F;
/// Live material versions per material slot: shadow caster, lit, and the replacement
/// generation's while it prepares.
constexpr uint32 MATERIAL_VERSIONS_PER_SLOT = 3;
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
QString DescribeUploadFailure(rhi::DeviceHandle device, const QString& asset, rhi::RasterStatus status)
{
    if (status == rhi::RasterStatus::CapacityExceeded)
    {
        rhi::StorageUsage usage;
        rhi::DeviceInfo info;
        if (rhi::GetStorageUsage(device, usage) == rhi::RasterStatus::Ready &&
            rhi::GetDeviceInfo(device, info) == rhi::DeviceStatus::Ready)
        {
            constexpr uint64 BYTES_PER_MEBIBYTE = uint64{1024} * 1024;
            return QStringLiteral("GPU upload exceeded a capacity limit while preparing %1. The device is using "
                                  "%2 of %3 MiB of host storage and %4 of %5 MiB of GPU resource storage. "
                                  "Close other captures and retry.")
                .arg(asset)
                .arg(usage.HostBytes / BYTES_PER_MEBIBYTE)
                .arg(info.Description.Storage.HostBytes / BYTES_PER_MEBIBYTE)
                .arg(usage.ResourceBytes / BYTES_PER_MEBIBYTE)
                .arg(info.Description.Storage.ResourceBytes / BYTES_PER_MEBIBYTE);
        }
    }
    return QStringLiteral("Could not upload %1 to the GPU (status %2). Reopen the World and retry.")
        .arg(asset)
        .arg(static_cast<uint32>(status));
}

struct BakeMeshVertices final
{
    Array<math::Vector3> PositionsMesh;
    Array<math::Vector3> NormalsMesh;
    Array<math::Vector2> MaterialTexcoords;
    Array<math::Vector2> BakeTexcoords;
    Array<math::Vector4> TangentsMesh;
    Array<uint32> Indices;
};

[[nodiscard]] bool BuildBakeMesh(const resources::SceneMesh& mesh,
                                 BakeMeshVertices& outVertices,
                                 renderer::MeshDescription& inoutDescription) noexcept
{
    namespace format = content::scene_format;
    constexpr usize TRIANGLE_CORNERS = static_cast<usize>(TRIANGLE_INDEX_COUNT);
    const usize count = mesh.Indices.GetSize();
    const bool hasTangents = mesh.Tangents.IsEmpty() == false;
    format::BakeChartLayout layout;
    if (count > format::MAX_VERTICES || count % TRIANGLE_CORNERS != 0 ||
        format::TryMakeBakeChartLayout(static_cast<uint32>(count / TRIANGLE_CORNERS), layout) == false ||
        outVertices.PositionsMesh.TryResize(count) == false || outVertices.NormalsMesh.TryResize(count) == false ||
        outVertices.MaterialTexcoords.TryResize(count) == false ||
        outVertices.BakeTexcoords.TryResize(count) == false || outVertices.Indices.TryResize(count) == false ||
        (hasTangents == true && outVertices.TangentsMesh.TryResize(count) == false))
    {
        return false;
    }
    for (usize vertex = 0; vertex < count; ++vertex)
    {
        const uint32 sourceIndex = mesh.Indices[vertex];
        if (sourceIndex >= mesh.Positions.GetSize())
        {
            return false;
        }
        outVertices.PositionsMesh[vertex] = mesh.Positions[sourceIndex];
        outVertices.NormalsMesh[vertex] = mesh.Normals[sourceIndex];
        outVertices.MaterialTexcoords[vertex] = mesh.Texcoords[sourceIndex];
        if (hasTangents == true)
        {
            outVertices.TangentsMesh[vertex] = mesh.Tangents[sourceIndex];
        }
        outVertices.Indices[vertex] = static_cast<uint32>(vertex);
        if (format::TryGetBakeChartUv(layout,
                                      static_cast<uint32>(vertex / TRIANGLE_CORNERS),
                                      static_cast<uint32>(vertex % TRIANGLE_CORNERS),
                                      outVertices.BakeTexcoords[vertex].X,
                                      outVertices.BakeTexcoords[vertex].Y) == false)
        {
            return false;
        }
    }
    inoutDescription.Positions = outVertices.PositionsMesh.GetData();
    inoutDescription.VertexCount = count;
    inoutDescription.Normals = outVertices.NormalsMesh.GetData();
    inoutDescription.Texcoords = outVertices.MaterialTexcoords.GetData();
    inoutDescription.BakeTexcoords = outVertices.BakeTexcoords.GetData();
    inoutDescription.Tangents = {outVertices.TangentsMesh.GetData(), outVertices.TangentsMesh.GetSize()};
    inoutDescription.Indices = outVertices.Indices.GetData();
    inoutDescription.IndexCount = count;
    return true;
}

/// Diagnostic color uses the display material only. It never enters the scene's emitter table.
void SetUnresolvedMaterialDiagnostic(renderer::LitMaterialDescription& inoutMaterial) noexcept
{
    inoutMaterial.Surface.Tint = {0, 0, 0, 1};
    inoutMaterial.Metallic = 1;
    inoutMaterial.Emission = {UNRESOLVED_MATERIAL_TINT.X, UNRESOLVED_MATERIAL_TINT.Y, UNRESOLVED_MATERIAL_TINT.Z};
    inoutMaterial.IsEmissionTwoSided = true;
    inoutMaterial.DebugView = renderer::MaterialDebugView::None;
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
      LightingKey(preview_lighting::MakeKey(source)), IsReference(referenceOrNull != nullptr),
      mProbeGrid(source.ProbeGrid)
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
    discardBakeCharts();
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
    if (IsPrepared() == true)
    {
        advanceBake();
        return PreviewPreparationStatus::Ready;
    }
    if (prepareResources(surface, outError) == false)
    {
        mHasFailed = true;
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
        rhi::RasterCapabilities capabilities;
        if (outError.isEmpty() == true && IsPreviewLineShading(Shading) == true &&
            rhi::GetRasterCapabilities(Device, capabilities) == rhi::RasterStatus::Ready &&
            capabilities.HasLineFill == false)
        {
            outError = QStringLiteral("Wireframe needs line rasterization, which this GPU backend does not provide. "
                                      "Choose another view.");
        }
#endif
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
    if (status == rhi::RasterStatus::Ready && mIsGridInitialized == false)
    {
        // Optional: without the analytic grid shaders, or when the program cannot take them, the
        // viewport keeps the ribbon grid, so a refusal here is not a failure of the generation.
        static_cast<void>(
            Renderer.InitializeGrid(Shaders.Get(PreviewShaderProgram::Grid, PreviewShaderStage::Vertex),
                                    Shaders.Get(PreviewShaderProgram::Grid, PreviewShaderStage::Fragment)));
        mIsGridInitialized = true;
    }
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
                    mProgram,
                    nullptr,
                    getDebugFillMode()),
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
                                   mProgram,
                                   nullptr,
                                   getDebugFillMode()),
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
                const usize vertexCount = source.Positions.GetSize();
                // Both chart expansion and realtime upload read one normal and UV per vertex.
                if (source.Normals.GetSize() != vertexCount || source.Texcoords.GetSize() != vertexCount ||
                    (source.Tangents.IsEmpty() == false && source.Tangents.GetSize() != vertexCount))
                {
                    outError = QStringLiteral("Mesh %1 has vertex attributes that do not match its %2 positions. "
                                              "Reimport the mesh with one normal and UV per vertex.")
                                   .arg(i + 1)
                                   .arg(vertexCount);
                    return false;
                }
                description.Positions = source.Positions.GetData();
                description.VertexCount = vertexCount;
                description.Indices = source.Indices.GetData();
                description.IndexCount = source.Indices.GetSize();
                description.Texcoords = source.Texcoords.GetData();
                description.Normals = source.Normals.GetData();
                // Cooked v6 tangent frames; vertices without one (W = 0) use the UV-derived frame.
                description.Tangents = {source.Tangents.GetData(), source.Tangents.GetSize()};
                if (shouldBakeLighting() == true)
                {
                    BakeMeshVertices bakeVertices;
                    renderer::MeshDescription chartMesh = description;
                    if (BuildBakeMesh(source, bakeVertices, chartMesh) == true &&
                        IsAccepted(Renderer.CreateMesh(chartMesh, mMeshes[i])) == true)
                    {
                        continue;
                    }
                    mBakeFailure = QStringLiteral("Could not prepare bake chart geometry. Using realtime lighting. "
                                                  "Split large meshes or clear Bake always-on lighting in Properties.");
                }
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
            const rhi::RasterStatus meshStatus = Renderer.CreateMesh(description, mMeshes[i]);
            if (IsAccepted(meshStatus) == false)
            {
                outError = DescribeUploadFailure(
                    Device,
                    QStringLiteral("mesh %1 (%2 vertices)").arg(i + 1).arg(description.VertexCount),
                    meshStatus);
                return false;
            }
        }
        for (int32 i = 0; i < Source.Textures.size(); ++i)
        {
            const QImage& image = Source.Textures[i].Pixels;
            // Surface maps are linear data; colour textures decode sRGB.
            const rhi::RasterFormat format =
                Source.Textures[i].IsLinear == true ? rhi::RasterFormat::Rgba8Unorm : rhi::RasterFormat::Rgba8Srgb;
            const rhi::RasterStatus textureStatus =
                rhi::CreateTexture(Device,
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
                                   Textures[i]);
            if (IsAccepted(textureStatus) == false)
            {
                outError = DescribeUploadFailure(
                    Device,
                    QStringLiteral("texture %1 (%2 × %3)").arg(i + 1).arg(image.width()).arg(image.height()),
                    textureStatus);
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
        lit.Scattering = surface.Scattering;
        lit.Program = surface.Program;
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
            lit.Scattering = {};
            lit.Metallic = 0;
            lit.Roughness = 1;
            lit.Emission = {};
            // Hemispheric ambient: faces that look up are brighter than faces that look down, which keeps the
            // sphere-averaged ambient at the previous constant 0.5 while the form reads.
            lit.EnvironmentIrradiance = {SOLID_SKY_IRRADIANCE, SOLID_SKY_IRRADIANCE, SOLID_SKY_IRRADIANCE};
            lit.EnvironmentGroundScale = SOLID_GROUND_SCALE;
        }
        if (surface.HasUnresolvedMaterial == true)
        {
            description.Tint = UNRESOLVED_MATERIAL_TINT;
            SetUnresolvedMaterialDiagnostic(lit);
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
bool PreviewGpuGeneration::shouldBakeLighting() const noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW) && !defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    return IsReference == false && Shading == PreviewShading::Lit && Source.Lit == true &&
           Source.Definition != nullptr && Source.Definition->Objects.IsEmpty() == false &&
           Source.AlwaysOnLightIds.isEmpty() == false && mBakeFailure.isEmpty() == true;
#else
    return false;
#endif
}

QString PreviewGpuGeneration::DescribeBake() const
{
    if (mBakeFailure.isEmpty() == false)
    {
        return mBakeFailure;
    }
    if (mHasPublishedBake == true)
    {
        return rhi::IsNull(mProbeVolume.Texture) == false ? QStringLiteral("always-on lighting and probe GI baked")
                                                          : QStringLiteral("always-on lighting baked");
    }
    if (shouldBakeLighting() == true)
    {
        return QStringLiteral("baking always-on lighting…");
    }
    if (mProbeGrid.Counts[0] != 0 && Source.AlwaysOnLightIds.isEmpty() == true)
    {
        return QStringLiteral("Probe GI needs an always-on baked light. Opt in to baking in its Properties.");
    }
    return QStringLiteral("direct lighting");
}

bool PreviewGpuGeneration::SetProbeGrid(const renderer::GiProbeGrid& grid) noexcept
{
    if (preview_lighting::AreProbeGridsEqual(mProbeGrid, grid) == true)
    {
        return false;
    }
    discardBakedSnapshot();
    mProbeGrid = grid;
    mSurfaceBake.clear();
    mBakeFailure.clear();
    return true;
}

void PreviewGpuGeneration::SetSurfaceBake(const QSharedPointer<const WorldSurfaceBake>& bake, const QString& failure)
{
    if (bake.isNull() == false)
    {
        mSurfaceBake = bake;
    }
    if (mBakeFailure.isEmpty() == true)
    {
        mBakeFailure = failure;
    }
}

void PreviewGpuGeneration::discardBakeCharts() noexcept
{
    for (renderer::BakedLighting& chart : mBakeCharts)
    {
        static_cast<void>(rhi::Destroy(Device, chart.Texture));
    }
    mBakeCharts.Clear();
    static_cast<void>(rhi::Destroy(Device, mProbeVolume.Texture));
    mProbeVolume = {};
}

void PreviewGpuGeneration::discardBakedSnapshot() noexcept
{
    if (mHasPublishedBake == true)
    {
        static_cast<void>(Renderer.Release(mBakedSnapshot));
        mBakedSnapshot = {};
        mHasPublishedBake = false;
    }
    for (renderer::MaterialVersion& material : mBakedMaterials)
    {
        static_cast<void>(Renderer.Release(material));
    }
    mBakedMaterials.Clear();
    mBakedMaterialCursor = 0;
    discardBakeCharts();
}

rhi::RasterStatus PreviewGpuGeneration::prepareBake() noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW) && !defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    if (mBakeCharts.IsEmpty() == true)
    {
        if (mSurfaceBake.isNull() == true ||
            (mProbeGrid.Counts[0] != 0 && mSurfaceBake->ProbeAtlas.Pixels.isEmpty() == true))
        {
            return rhi::RasterStatus::Pending;
        }
        if (static_cast<usize>(mSurfaceBake->Charts.size()) != Source.Definition->Objects.GetSize())
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        if (mBakeCharts.TryResize(Source.Definition->Objects.GetSize()) == false)
        {
            return rhi::RasterStatus::OutOfMemory;
        }
        if (mProbeGrid.Counts[0] != 0)
        {
            const WorldProbeAtlas& atlas = mSurfaceBake->ProbeAtlas;
            mProbeVolume.Grid = mProbeGrid;
            mProbeVolume.IrradianceScale = atlas.IrradianceScale;
            const rhi::RasterStatus uploaded = rhi::CreateTexture(
                Device,
                { .Width = atlas.Width, .Height = atlas.Height, .Format = rhi::RasterFormat::Rgba8Unorm },
                {{reinterpret_cast<const uint8*>(atlas.Pixels.constData()), static_cast<usize>(atlas.Pixels.size())},
                 usize{atlas.Width} * content::scene_format::TEXEL_BYTES},
                mProbeVolume.Texture);
            if (IsAccepted(uploaded) == false)
            {
                return uploaded;
            }
        }
        for (usize i = 0; i < mBakeCharts.GetSize(); ++i)
        {
            if (Lighting.Surfaces[i].ShouldExcludeFromBake == true)
            {
                continue;
            }
            const WorldSurfaceChart& chart = mSurfaceBake->Charts.at(static_cast<qsizetype>(i));
            const resources::SceneObject& object = Source.Definition->Objects[i];
            renderer::BakedLighting& material = mBakeCharts[i];
            material.Scale = chart.Scale;
            material.UvTransform = {1, 1, 0, 0};
            material.SourceId = object.SourceId;
            material.SceneRevision = Lighting.SceneRevision;
            material.DependencyRevision = Lighting.DependencyRevision;
            material.Transform = Source.Objects.at(static_cast<qsizetype>(i)).Transform;
            material.AreTriangleChartsEnabled = true;
            material.ShouldAddToEnvironment = true;
            const rhi::RasterStatus uploaded = rhi::CreateTexture(
                Device,
                { .Width = chart.Width, .Height = chart.Height, .Format = rhi::RasterFormat::Rgba8Unorm },
                {{reinterpret_cast<const uint8*>(chart.Pixels.constData()), static_cast<usize>(chart.Pixels.size())},
                 usize{chart.Width} * content::scene_format::TEXEL_BYTES},
                material.Texture);
            if (IsAccepted(uploaded) == false)
            {
                return uploaded;
            }
        }
    }
    if (rhi::IsNull(mProbeVolume.Texture) == false)
    {
        const rhi::RasterStatus status = rhi::GetStatus(Device, mProbeVolume.Texture);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
    }
    for (const renderer::BakedLighting& chart : mBakeCharts)
    {
        if (rhi::IsNull(chart.Texture) == true)
        {
            continue;
        }
        const rhi::RasterStatus status = rhi::GetStatus(Device, chart.Texture);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
    }
    return rhi::RasterStatus::Ready;
#else
    return rhi::RasterStatus::Unsupported;
#endif
}

void PreviewGpuGeneration::advanceBake() noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    if (shouldBakeLighting() == false || mHasPublishedBake == true)
    {
        return;
    }
    rhi::RasterStatus status = prepareBake();
    if (status == rhi::RasterStatus::Ready)
    {
        status = prepareBakedSnapshot();
    }
    if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
    {
        discardBakedSnapshot();
        mBakeFailure = QStringLiteral("Always-on bake failed (%1). Using realtime lighting. "
                                      "Simplify the World geometry or clear the bake option in Properties.")
                           .arg(static_cast<uint32>(status));
    }
#endif
}

rhi::RasterStatus PreviewGpuGeneration::prepareBakedSnapshot() noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    const usize count = Source.Definition->Objects.GetSize();
    if (mBakedMaterials.IsEmpty() == true && mBakedMaterials.TryResize(count) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    while (mBakedMaterialCursor < count)
    {
        const usize index = mBakedMaterialCursor;
        const rhi::RasterStatus status = createLightingMaterial(index, true, true, mBakedMaterials[index]);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        ++mBakedMaterialCursor;
    }
    renderer::Snapshot replacement;
    const rhi::RasterStatus status = createLitSnapshot({mBakedMaterials.GetData(), count}, true, replacement);
    if (status == rhi::RasterStatus::Ready)
    {
        mBakedSnapshot = replacement;
        mHasPublishedBake = true;
    }
    return status;
#else
    return rhi::RasterStatus::Unsupported;
#endif
}

rhi::RasterStatus PreviewGpuGeneration::createLitSnapshot(BorrowedView<const renderer::MaterialVersion> materials,
                                                          bool shouldUseBake,
                                                          renderer::Snapshot& outSnapshot) noexcept
{
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    const usize count = Source.Definition->Objects.GetSize();
    Array<renderer::SceneItem> items;
    if (items.TryResize(count) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    for (usize i = 0; i < count; ++i)
    {
        const renderer::MaterialVersion material = materials[shouldUseBake == true ? i : mLightingMaterialOwners[i]];
        const rhi::RasterStatus ready = Renderer.GetStatus(material);
        if (ready != rhi::RasterStatus::Ready)
        {
            return ready;
        }
        const resources::SceneObject& source = Source.Definition->Objects[i];
        renderer::SceneItem& item = items[i];
        item.Geometry = mMeshes[source.Mesh];
        item.Transform = source.Transform;
        item.SourceId = source.SourceId;
        item.Surface = material;
        item.Material.Color = {1, 1, 1, 1};
        if (shouldUseBake == true)
        {
            item.BakeSceneRevision = Lighting.SceneRevision;
            item.BakeDependencyRevision = Lighting.DependencyRevision;
        }
    }
    return Renderer.CreateSnapshot(items.GetData(), count, outSnapshot);
#else
    static_cast<void>(materials);
    static_cast<void>(shouldUseBake);
    static_cast<void>(outSnapshot);
    return rhi::RasterStatus::Unsupported;
#endif
}

rhi::RasterStatus PreviewGpuGeneration::createLightingMaterial(usize objectIndex,
                                                               bool hasShadows,
                                                               bool shouldUseBake,
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
    material.Scattering = surface.Scattering;
    material.Program = surface.Program;
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
    if (shouldUseBake == true)
    {
        if (surface.ShouldExcludeFromBake == true)
        {
            material.ProbeVolume = mProbeVolume;
        }
        else
        {
            material.Bake = mBakeCharts[objectIndex];
            material.BakedDirectLightIds = {Source.AlwaysOnLightIds.constData(),
                                            static_cast<usize>(Source.AlwaysOnLightIds.size())};
        }
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
    if (maps.HasUnresolvedMaterial == true)
    {
        SetUnresolvedMaterialDiagnostic(material);
    }
    renderer::LightSelection report;
    return resources.Renderer.CreateLitMaterial(mProgram, material, outMaterial, report);
#else
    static_cast<void>(objectIndex);
    static_cast<void>(hasShadows);
    static_cast<void>(shouldUseBake);
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
        // Only Lit and Light count, which counts shadowed contributions, read lights. The other debug views need
        // no lighting budget, and neither they nor a reference prepare casters or a shadow atlas.
        preview_lighting::InputsTarget target = preview_lighting::InputsTarget::Raster;
        if (IsReference == true)
        {
            target = preview_lighting::InputsTarget::Reference;
        }
        else if (IsPreviewDebugShading(Shading) == true && Shading != PreviewShading::LightCount)
        {
            target = preview_lighting::InputsTarget::Surfaces;
        }
        preview_lighting::InputsLimit limit = preview_lighting::InputsLimit::Emitter;
        const rhi::RasterStatus status =
            preview_lighting::BuildInputs(resources.Source, resources.LightingKey, resources.Lighting, target, &limit);
        if (status == rhi::RasterStatus::Unsupported && target == preview_lighting::InputsTarget::Reference)
        {
            outError = QStringLiteral("Reference capture needs resolved materials. Select a purple World surface "
                                      "and map its material before capturing.");
            return false;
        }
        if (status == rhi::RasterStatus::CapacityExceeded)
        {
            // Name the budget, so the reader knows what to reduce.
            switch (limit)
            {
                case preview_lighting::InputsLimit::Lights:
                    outError = QStringLiteral("The World's lights, counting one per planar emissive patch, exceed "
                                              "the built-in raster preview's %1 lights; no lights were dropped.")
                                   .arg(renderer::MAX_DIRECT_LIGHTS);
                    break;
                case preview_lighting::InputsLimit::ShadowFaces:
                    outError = QStringLiteral("The World's lights need more than the built-in raster preview's %1 "
                                              "shadow faces (six per point or area light); no lights were dropped.")
                                   .arg(renderer::SHADOW_REGION_CAPACITY);
                    break;
                case preview_lighting::InputsLimit::Emitter:
                    outError = QStringLiteral("An emissive surface has more triangles or more emitted power than the "
                                              "built-in raster preview represents; no lights were dropped.");
                    break;
            }
            return false;
        }
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
        if (preview_lighting::BuildMaterialOwners(resources.Source, mLightingMaterialOwners) == false)
        {
            outError = QStringLiteral("Could not prepare viewport material bindings. Previous view retained.");
            return false;
        }
        // Reference transport traces its own visibility, and surface-only debug views show no lighting.
        resources.LightingStage = target != preview_lighting::InputsTarget::Raster ? PreviewLightingStage::Materials
                                                                                   : PreviewLightingStage::Casters;
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
            const usize index = resources.MaterialCursor;
            if (mLightingMaterialOwners[index] == index &&
                IsAccepted(createLightingMaterial(index, false, false, resources.CasterMaterials[index])) == false)
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
            const renderer::MaterialVersion material = resources.CasterMaterials[mLightingMaterialOwners[i]];
            const rhi::RasterStatus ready = resources.Renderer.GetStatus(material);
            if (ready != rhi::RasterStatus::Ready)
            {
                return ready == rhi::RasterStatus::Pending;
            }
            const resources::SceneObject& source = resources.Source.Definition->Objects[i];
            items[i].Geometry = mMeshes[source.Mesh];
            items[i].Transform = source.Transform;
            items[i].SourceId = source.SourceId;
            items[i].Surface = material;
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
            const usize index = resources.MaterialCursor;
            if (mLightingMaterialOwners[index] == index &&
                IsAccepted(createLightingMaterial(index, true, false, mMaterials[index])) == false)
            {
                return false;
            }
            ++resources.MaterialCursor;
        }
        const rhi::RasterStatus status = createLitSnapshot({mMaterials.GetData(), count}, false, mSnapshot);
        if (status != rhi::RasterStatus::Ready)
        {
            return status == rhi::RasterStatus::Pending;
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
