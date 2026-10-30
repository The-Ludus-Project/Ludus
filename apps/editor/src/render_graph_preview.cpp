#include "internal/render_graph_preview.h"
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/time/time.hpp>

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/runtime/resources/scene.h>

#include "internal/offscreen_frame_reader.h"
#include "internal/render_graph_resources.h"
#include "internal/render_target_reader.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSize>

#include <new>

namespace ludus::editor
{
using namespace foundation;
namespace format = content::render_graph_format;
namespace renderer = graphics::renderer;
namespace rhi = graphics::rhi;
namespace
{
/// Wall-clock producer deadline, independent of the completion polling cadence.
constexpr int32 MAX_PRODUCER_MILLISECONDS = 10'000;
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1'000'000.0;
/// Straight alpha below one cannot use the opaque static profile.
constexpr float32 OPAQUE_ALPHA = 1.0F;
} // namespace

QString DescribeRasterStatus(rhi::RasterStatus status)
{
    switch (status)
    {
        case rhi::RasterStatus::Ready:
            return QStringLiteral("Ready");
        case rhi::RasterStatus::Pending:
            return QStringLiteral("Pending");
        case rhi::RasterStatus::NotReady:
            return QStringLiteral("NotReady");
        case rhi::RasterStatus::Failed:
            return QStringLiteral("Failed");
        case rhi::RasterStatus::InvalidHandle:
            return QStringLiteral("InvalidHandle");
        case rhi::RasterStatus::InvalidState:
            return QStringLiteral("InvalidState");
        case rhi::RasterStatus::InvalidDescription:
            return QStringLiteral("InvalidDescription");
        case rhi::RasterStatus::Unsupported:
            return QStringLiteral("Unsupported");
        case rhi::RasterStatus::CapacityExceeded:
            return QStringLiteral("CapacityExceeded");
        case rhi::RasterStatus::IdentityExhausted:
            return QStringLiteral("IdentityExhausted");
        case rhi::RasterStatus::OutOfMemory:
            return QStringLiteral("OutOfMemory");
        case rhi::RasterStatus::DeviceLost:
            return QStringLiteral("DeviceLost");
    }
    return QStringLiteral("Unknown");
}

namespace
{
QString DescribePackageStatus(renderer::RenderGraphPackageStatus status)
{
    switch (status)
    {
        case renderer::RenderGraphPackageStatus::Ready:
            return QStringLiteral("Ready");
        case renderer::RenderGraphPackageStatus::Malformed:
            return QStringLiteral("Malformed");
        case renderer::RenderGraphPackageStatus::UnsupportedVersion:
            return QStringLiteral("UnsupportedVersion");
        case renderer::RenderGraphPackageStatus::CapacityExceeded:
            return QStringLiteral("CapacityExceeded");
        case renderer::RenderGraphPackageStatus::OutOfMemory:
            return QStringLiteral("OutOfMemory");
        case renderer::RenderGraphPackageStatus::InvalidState:
            return QStringLiteral("InvalidState");
    }
    return QStringLiteral("Unknown");
}

bool IsAccepted(rhi::RasterStatus status)
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
} // namespace

QString DescribeRenderGraphExtractionFailure(rhi::RasterStatus status,
                                             const renderer::SceneShaderDiagnostics& diagnostics)
{
    QString budget;
    switch (diagnostics.ExceededBudget)
    {
        case renderer::SceneMemoryBudget::None:
            break;
        case renderer::SceneMemoryBudget::CpuScene:
            budget = QStringLiteral("ray-tracing CPU scene preparation");
            break;
        case renderer::SceneMemoryBudget::CpuScratch:
            budget = QStringLiteral("ray-tracing CPU scratch/upload");
            break;
        case renderer::SceneMemoryBudget::GpuPreview:
            budget = QStringLiteral("ray-tracing GPU preview (including retained frames)");
            break;
        case renderer::SceneMemoryBudget::RhiResources:
            budget = QStringLiteral("device-wide RHI resource");
            break;
        case renderer::SceneMemoryBudget::DeviceStorageRange:
            budget = QStringLiteral("device storage-buffer binding");
            break;
    }
    const QString detail = budget.isEmpty() == true
                               ? QString{}
                               : QStringLiteral(" %1 memory budget exceeded: requires %2 bytes; %3 bytes available.")
                                     .arg(budget)
                                     .arg(diagnostics.RequiredBytes)
                                     .arg(diagnostics.AvailableBytes);
    return QStringLiteral("The World extraction was rejected (%1).%2 The shown frame is kept.")
        .arg(DescribeRasterStatus(status), detail);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — distinct scene/view/revision inputs.
bool MakeRenderGraphExtraction(const RenderScene& scene,
                               const renderer::ViewDescription& view,
                               uint64 revision,
                               RenderGraphExtraction& outExtraction,
                               QString& outError)
{
    if (revision == 0 || scene.Objects.isEmpty() == true)
    {
        outError = QStringLiteral("Open a World with renderable objects to preview the graph.");
        return false;
    }
    if (static_cast<usize>(scene.Meshes.size()) > format::MAX_MESHES ||
        static_cast<usize>(scene.Objects.size()) > format::MAX_ITEMS)
    {
        outError = QStringLiteral("The World has %1 meshes and %2 objects; the render-graph runtime profile admits at "
                                  "most %3 meshes and %4 draw items.")
                       .arg(scene.Meshes.size())
                       .arg(scene.Objects.size())
                       .arg(format::MAX_MESHES)
                       .arg(format::MAX_ITEMS);
        return false;
    }
    // Cooked (imported or merged) scenes keep geometry only in their immutable decoded
    // definition, whose mesh table parallels Meshes; authored scenes carry it in PreviewMesh.
    const resources::SceneDefinition* definitionOrNull = scene.Definition.Get();
    if (definitionOrNull != nullptr && definitionOrNull->Meshes.GetSize() != static_cast<usize>(scene.Meshes.size()))
    {
        outError = QStringLiteral("The World's cooked scene does not match its mesh table.");
        return false;
    }
    RenderGraphExtraction extraction;
    // Share the scene's geometry storage (including the definition's reference count);
    // borrowed pointers below stay valid while the extraction lives.
    extraction.Scene = scene;
    extraction.View = view;
    extraction.Revision = revision;
    for (qsizetype i = 0; i < extraction.Scene.Meshes.size(); ++i)
    {
        renderer::RenderGraphMesh record;
        if (definitionOrNull != nullptr)
        {
            const resources::SceneMesh& mesh = definitionOrNull->Meshes[static_cast<usize>(i)];
            record.Geometry.Positions = mesh.Positions.GetData();
            record.Geometry.VertexCount = mesh.Positions.GetSize();
            record.Geometry.Indices = mesh.Indices.GetData();
            record.Geometry.IndexCount = mesh.Indices.GetSize();
        }
        else
        {
            const PreviewMesh& mesh = extraction.Scene.Meshes.at(i);
            record.Geometry.Positions = mesh.Positions.constData();
            record.Geometry.VertexCount = static_cast<usize>(mesh.Positions.size());
            record.Geometry.Indices = mesh.Indices.constData();
            record.Geometry.IndexCount = static_cast<usize>(mesh.Indices.size());
        }
        if (definitionOrNull != nullptr)
        {
            const resources::SceneMesh& mesh = definitionOrNull->Meshes[static_cast<usize>(i)];
            record.Geometry.Normals = mesh.Normals.GetData();
            record.Geometry.Texcoords = mesh.Texcoords.GetData();
            record.Geometry.Tangents = {mesh.Tangents.GetData(), mesh.Tangents.GetSize()};
        }
        else
        {
            const PreviewMesh& mesh = extraction.Scene.Meshes.at(i);
            record.Geometry.Normals = mesh.Normals.isEmpty() ? nullptr : mesh.Normals.constData();
            record.Geometry.Texcoords = mesh.Uvs.isEmpty() ? nullptr : mesh.Uvs.constData();
        }
        record.Layout = renderer::RenderGraphLayout::Static;
        extraction.Meshes.append(record);
    }
    extraction.MaterialImages.reserve(extraction.Scene.Textures.size());
    for (const PreviewTexture& texture : extraction.Scene.Textures)
    {
        extraction.MaterialImages.append(texture.Pixels.convertToFormat(QImage::Format_RGBA8888));
    }
    const auto getTexture = [&extraction](int32 index) -> rhi::TextureCpuContent {
        if (index < 0 || index >= extraction.MaterialImages.size())
        {
            return {};
        }
        const QImage& image = extraction.MaterialImages.at(index);
        return {image.constBits(),
                static_cast<usize>(image.sizeInBytes()),
                static_cast<usize>(image.bytesPerLine()),
                static_cast<uint32>(image.width()),
                static_cast<uint32>(image.height()),
                extraction.Scene.Textures.at(index).IsLinear ? rhi::RasterFormat::Rgba8Unorm
                                                             : rhi::RasterFormat::Rgba8Srgb};
    };
    for (qsizetype surfaceIndex = 0; surfaceIndex < extraction.Scene.Surfaces.size(); ++surfaceIndex)
    {
        const PreviewSurface& source = extraction.Scene.Surfaces.at(surfaceIndex);
        renderer::SceneShaderMaterial material;
        material.BaseColor = {source.Tint.X, source.Tint.Y, source.Tint.Z};
        material.Metallic = source.Metallic;
        material.Roughness = source.Roughness;
        material.Emission = source.Emission;
        material.IsEmissionTwoSided = source.IsEmissionTwoSided;
        material.NormalScale = source.NormalScale;
        material.Sampler.U = source.Repeat ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
        material.Sampler.V = material.Sampler.U;
        material.ColorTexture = getTexture(source.Texture);
        material.SurfaceTexture = getTexture(source.SurfaceTexture);
        material.EmissionTexture = getTexture(source.EmissionTexture);
        material.HasError = source.IsAlphaMasked || source.Tint.W != 1 ||
                            source.Texture >= extraction.Scene.Textures.size() ||
                            source.SurfaceTexture >= extraction.Scene.Textures.size() ||
                            source.EmissionTexture >= extraction.Scene.Textures.size();
        extraction.Materials.append(material);
    }
    extraction.MaterialOverrides.reserve(extraction.Scene.Objects.size());
    for (qsizetype i = 0; i < extraction.Scene.Objects.size(); ++i)
    {
        const PreviewObject& object = extraction.Scene.Objects.at(i);
        if (object.Mesh < 0 || object.Mesh >= extraction.Scene.Meshes.size())
        {
            outError = QStringLiteral("World object %1 names a missing mesh.").arg(object.Id);
            return false;
        }
        math::Vector4 color = object.Tint;
        if (object.Surface >= 0 && object.Surface < extraction.Scene.Surfaces.size())
        {
            // The static profile samples no textures; the surface's flat approximation stands in.
            const math::Vector4& tint = extraction.Scene.Surfaces.at(object.Surface).FlatTint;
            color = {color.X * tint.X, color.Y * tint.Y, color.Z * tint.Z, color.W * tint.W};
        }
        renderer::RenderGraphItem item;
        item.SourceId = static_cast<uint64>(i) + 1;
        item.MeshIndex = static_cast<uint32>(object.Mesh);
        item.MeshToWorld = object.Transform;
        item.Material.Color = color;
        item.Coverage =
            color.W < OPAQUE_ALPHA ? renderer::RenderGraphMaterial::Transparent : renderer::RenderGraphMaterial::Opaque;
        item.SurfaceTint = object.Tint;
        uint32 resolvedMaterial =
            object.Surface < 0 ? renderer::DEFAULT_SCENE_MATERIAL : static_cast<uint32>(object.Surface);
        if (extraction.Scene.Lit == false)
        {
            // An unlit World has no lighting model; its tint (times its texture) is what it shows. The
            // lit default receives that colour as emission over a black base, so no light is invented
            // and nothing it emits is reflected.
            renderer::SceneShaderMaterial unlit;
            unlit.BaseColor = {0, 0, 0};
            unlit.Emission = {object.Tint.X, object.Tint.Y, object.Tint.Z};
            if (object.Surface >= 0 && object.Surface < extraction.Scene.Surfaces.size())
            {
                const PreviewSurface& source = extraction.Scene.Surfaces.at(object.Surface);
                const renderer::SceneShaderMaterial& surfaceMaterial = extraction.Materials[object.Surface];
                unlit.Emission = {unlit.Emission.X * source.Tint.X,
                                  unlit.Emission.Y * source.Tint.Y,
                                  unlit.Emission.Z * source.Tint.Z};
                unlit.Sampler = surfaceMaterial.Sampler;
                unlit.EmissionTexture = surfaceMaterial.ColorTexture;
                unlit.HasError = surfaceMaterial.HasError;
            }
            resolvedMaterial = static_cast<uint32>(extraction.Materials.size());
            extraction.Materials.append(unlit);
        }
        else if (object.Surface >= 0 && object.Surface < extraction.Scene.Surfaces.size() &&
                 extraction.Scene.Surfaces[object.Surface].HasEmissionPower == true)
        {
            const PreviewSurface& source = extraction.Scene.Surfaces[object.Surface];
            renderer::SceneShaderMaterial resolved = extraction.Materials[object.Surface];
            const renderer::MeshDescription& geometry = extraction.Meshes[object.Mesh].Geometry;
            float64 areaWorld = 0;
            constexpr usize CORNERS = 3;
            for (usize primitive = 0; primitive < geometry.IndexCount / CORNERS; ++primitive)
            {
                const math::Vector3 a =
                    math::TransformPoint(object.Transform, geometry.Positions[geometry.Indices[primitive * CORNERS]]);
                const math::Vector3 b =
                    math::TransformPoint(object.Transform,
                                         geometry.Positions[geometry.Indices[primitive * CORNERS + 1]]);
                const math::Vector3 c =
                    math::TransformPoint(object.Transform,
                                         geometry.Positions[geometry.Indices[primitive * CORNERS + 2]]);
                areaWorld += static_cast<float64>(math::Length(math::Cross(b - a, c - a))) * 0.5;
            }
            if (areaWorld > 0)
            {
                resolved.HasError = resolved.HasError == true ||
                                    renderer::ResolveMeshEmissionPower(source.Emission,
                                                                       source.EmissionPower,
                                                                       areaWorld,
                                                                       source.IsEmissionTwoSided,
                                                                       resolved.Emission) != rhi::RasterStatus::Ready;
            }
            else
            {
                resolved.Emission = {};
            }
            resolvedMaterial = static_cast<uint32>(extraction.Materials.size());
            extraction.Materials.append(resolved);
        }
        extraction.MaterialOverrides.append(resolvedMaterial);
        extraction.Items.append(item);
    }
    // Complete all Qt mutations before publishing views into their shared storage.
    for (qsizetype i = 0; i < extraction.Items.size(); ++i)
    {
        extraction.Items[i].MaterialOverrides = {extraction.MaterialOverrides.constData() + i, 1};
    }
    extraction.ShaderInputs.Materials = {extraction.Materials.constData(),
                                         static_cast<usize>(extraction.Materials.size())};
    for (uint32 i = 0; i < extraction.Scene.AuthoredLights.Count; ++i)
    {
        extraction.Lights.append(extraction.Scene.AuthoredLights.Lights[i]);
    }
    extraction.ShaderInputs.Lights = {extraction.Lights.constData(), static_cast<usize>(extraction.Lights.size())};
    if (definitionOrNull != nullptr)
    {
        extraction.ShaderInputs.Environment = definitionOrNull->Environment.Rgb;
        extraction.ShaderInputs.EnvironmentWidth = definitionOrNull->Environment.Width;
        extraction.ShaderInputs.EnvironmentHeight = definitionOrNull->Environment.Height;
        extraction.ShaderInputs.EnvironmentPixels = {definitionOrNull->Environment.Pixels.GetData(),
                                                     definitionOrNull->Environment.Pixels.GetSize()};
    }
    // Moving keeps the shared geometry storage the borrowed pointers name.
    outExtraction = Move(extraction);
    return true;
}

struct RenderGraphPreview::State final
{
    /// Exported color result and camera count of one staged generation.
    struct Target final
    {
        uint64 Serial = 0;
        uint32 Resource = 0;
        uint32 Width = 0;
        uint32 Height = 0;
        uint32 Cameras = 0;
        /// Every texture of the generation, for the texture viewer.
        RenderGraphTargetCatalogue Catalogue;
        QString RendererStatus;
        /// Runtime package and generation identity, for ray path capture of this generation.
        QString PackagePath;
        QByteArray GenerationIdentity;
        RenderGraphPreviewSource Source;
        StaticArray<format::Resource, format::MAX_RESOURCES> Descriptions{};
    };
    StaticArray<uint8, format::IDENTITY_BYTES> Interface{};
    rhi::RasterShaderDescription CompositeVertex{};
    rhi::RasterShaderDescription CompositeFragment{};
    renderer::RenderGraphActivation Activation;
    RenderTargetReader Reader;
    GraphDisplayFrames Display;
    bool ShouldPresentDirectly = false;
    bool IsCopyingDisplay = false;
    bool HasGpuFrame = false;
    bool HasPendingPreparation = false;
    GraphViewportStatistics Statistics;
    ViewportFrameTrace Trace;
    QElapsedTimer ReadbackTimer;
    bool HasCompletedStatisticsFrame = false;
    QString OutputName;
    uint64 OutputRevision = 0;
    uint64 ReadingOutputRevision = 0;
    uint64 FailedOutputGeneration = 0;
    uint64 FailedOutputRevision = 0;
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    bool IsAttached = false;
    QSize ReferenceSize{DEFAULT_RENDER_WIDTH, DEFAULT_RENDER_HEIGHT};
    QSize AppliedReference;
    bool IsResizeRejected = false;
    void ResolveTarget(Target& inoutTarget) const
    {
        for (RenderGraphTarget& texture : inoutTarget.Catalogue.Targets)
        {
            format::Resource resolved{};
            if (renderer::ResolveRenderGraphResource(inoutTarget.Descriptions[texture.Resource],
                                                     static_cast<uint32>(ReferenceSize.width()),
                                                     static_cast<uint32>(ReferenceSize.height()),
                                                     resolved) == rhi::RasterStatus::Ready)
            {
                texture.Width = resolved.Width;
                texture.Height = resolved.Height;
                if (texture.Resource == inoutTarget.Resource)
                {
                    inoutTarget.Width = resolved.Width;
                    inoutTarget.Height = resolved.Height;
                }
            }
        }
    }
    bool IsInterfaceValid = false;
    /// Incremented per Attach; activation serials restart with every device session.
    uint64 Session = 0;
    QString RequestedPath;
    RenderGraphPreviewSource RequestedSource;
    QString RasterFallbackPath;
    bool ShouldForceRaster = false;
    bool ShouldReselectRenderer = false;
    UniquePtr<renderer::RenderGraphPackage> WaitingOrNull;
    Target Waiting;
    Target Candidate;
    Target Active;
    RenderGraphExtraction Extraction;
    bool HasExtraction = false;
    uint64 UpdatedRevision = 0;
    uint64 MaterialStatusRevision = 0;
    QString MaterialStatus;
    bool IsAwaitingProducer = false;
    QElapsedTimer ProducerTimer;
    uint64 RenderedGeneration = 0;
    uint64 RenderedRevision = 0;
    /// Texture inspection owns a GPU snapshot; band readback can overlap later frames.
    RenderTargetReader Inspector;
    QString InspectedName;
    bool IsInspectionRequested = false;
    bool IsInspectionEnabled = true;
    QElapsedTimer InspectionTimer;
    float64 InspectionCpuMilliseconds = 0;
    /// Limit automatic diagnostic snapshots to four per second during navigation.
    static constexpr int64 INSPECTION_INTERVAL_MILLISECONDS = 250;
    uint64 InspectingGeneration = 0;
    uint64 InspectingRevision = 0;
    /// Ray path inspection: while frozen the live graph keeps Extraction as it was at the freeze.
    bool IsFrozen = false;
    RayPathFreeze Freeze;
    QString StaleReason;
    /// Latest camera and extraction supplied while frozen, applied by ThawRayPaths.
    renderer::ViewDescription LiveView{};
    uint64 LiveRevision = 0;
    RenderGraphExtraction PendingExtraction;
    bool HasPendingExtraction = false;
    RayPathCapture Capture;
};

RenderGraphPreview::RenderGraphPreview(BorrowedView<const uint8> interfaceIdentity,
                                       const rhi::RasterShaderDescription& compositeVertex,
                                       const rhi::RasterShaderDescription& compositeFragment,
                                       QObject* parentOrNull)
    : QObject(parentOrNull)
{
    mState.Reset(new (std::nothrow) State);
    LUDUS_REQUIRE(mState.Get() != nullptr);
    mState->IsInterfaceValid =
        interfaceIdentity.IsValid() == true && interfaceIdentity.GetSize() == format::IDENTITY_BYTES;
    for (usize i = 0; mState->IsInterfaceValid == true && i < format::IDENTITY_BYTES; ++i)
    {
        mState->Interface[i] = interfaceIdentity[i];
    }
    mState->CompositeVertex = compositeVertex;
    mState->CompositeFragment = compositeFragment;
}

RenderGraphPreview::~RenderGraphPreview()
{
    // Same release order as Detach, without notifying observers during destruction.
    mState->Capture.Reset();
    mState->Display.Reset();
    mState->Reader.Release();
    mState->Inspector.Release();
    mState->Activation.Reset();
}

bool RenderGraphPreview::Attach(rhi::DeviceHandle device, rhi::SurfaceHandle surface)
{
    State& state = *mState;
    if (state.IsAttached == true || state.IsInterfaceValid == false)
    {
        return false;
    }
    const rhi::RasterStatus initialized =
        state.Activation.Initialize(device,
                                    {state.Interface.GetData(), state.Interface.GetSize()},
                                    state.CompositeVertex,
                                    state.CompositeFragment);
    if (initialized != rhi::RasterStatus::Ready)
    {
        setStatus(QStringLiteral("The preview could not bind the viewport device (%1).")
                      .arg(DescribeRasterStatus(initialized)));
        return false;
    }
    state.Display.Initialize(device, state.CompositeVertex, state.CompositeFragment);
    state.Device = device;
    state.Surface = surface;
    state.IsResizeRejected = false;
    state.IsAttached = true;
    if (state.ReferenceSize.isEmpty() == false)
    {
        static_cast<void>(state.Activation.Resize(static_cast<uint32>(state.ReferenceSize.width()),
                                                  static_cast<uint32>(state.ReferenceSize.height())));
        state.AppliedReference = state.ReferenceSize;
    }
    ++state.Session;
    mStatus.clear();
    // Re-evaluate the enabled logical device, including a change of API or adapter.
    if (state.RequestedPath.isEmpty() == false)
    {
        static_cast<void>(Load(state.RequestedPath, state.RasterFallbackPath, state.RequestedSource));
    }
    Q_EMIT Changed();
    return true;
}

void RenderGraphPreview::Detach() noexcept
{
    State& state = *mState;
    if (state.IsAttached == false)
    {
        return;
    }
    state.Trace.Cancel("preview detached");
    // The capture graph lives on this device too; decoded results stay for the inspector.
    state.Capture.Release(QStringLiteral("The viewport device stopped before the capture finished."));
    if (state.IsFrozen == true)
    {
        markRayPathStale(QStringLiteral("The viewport device stopped; capture again after it restarts."));
    }
    // Drop the export and inspection consumers before the generations that own their textures.
    state.Display.Reset();
    state.IsCopyingDisplay = false;
    state.HasPendingPreparation = false;
    state.Reader.Release();
    state.Statistics.Reset();
    state.ReadbackTimer.invalidate();
    state.Inspector.Release();
    state.InspectionTimer.invalidate();
    state.Activation.Reset();
    state.Device = {};
    state.Surface = {};
    state.IsAttached = false;
    state.Candidate = {};
    state.Active = {};
    state.UpdatedRevision = 0;
    state.IsAwaitingProducer = false;
    state.ProducerTimer.invalidate();
    Q_EMIT Changed();
}

bool RenderGraphPreview::IsAttached() const noexcept
{
    return mState->IsAttached;
}

void RenderGraphPreview::SetForceRaster(bool shouldForceRaster)
{
    State& state = *mState;
    if (state.ShouldForceRaster != shouldForceRaster)
    {
        state.ShouldForceRaster = shouldForceRaster;
        state.ShouldReselectRenderer = state.RequestedPath.isEmpty() == false;
        Q_EMIT Changed();
    }
}
bool RenderGraphPreview::ShouldForceRaster() const noexcept
{
    return mState->ShouldForceRaster;
}
RenderGraphPreviewLoad RenderGraphPreview::Load(const QString& runtimePackagePath,
                                                const QString& rasterFallbackPath,
                                                const RenderGraphPreviewSource& source)
{
    State& state = *mState;
    state.RequestedPath = runtimePackagePath;
    state.RequestedSource = source;
    state.RasterFallbackPath = rasterFallbackPath;
    state.ShouldReselectRenderer = false;
    state.WaitingOrNull.Reset();
    UniquePtr<renderer::RenderGraphPackage> package;
    QByteArray decodedRuntimeIdentity;
    const auto readPackage = [this, &package, &decodedRuntimeIdentity](const QString& path) {
        QFile file(path);
        if (QFileInfo(path).isFile() == false || QFileInfo(path).size() > MAX_PACKAGE_BYTES ||
            file.open(QIODevice::ReadOnly) == false)
        {
            setStatus(QStringLiteral("No readable runtime generation at %1; cook the runtime first.")
                          .arg(QFileInfo(path).fileName()));
            return RenderGraphPreviewLoad::Unreadable;
        }
        const QByteArray bytes = file.read(MAX_PACKAGE_BYTES + 1);
        if (bytes.size() > MAX_PACKAGE_BYTES)
        {
            setStatus(QStringLiteral("The runtime generation exceeds the 16 MiB runtime bound."));
            return RenderGraphPreviewLoad::Unreadable;
        }
        decodedRuntimeIdentity = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        package.Reset(new (std::nothrow) renderer::RenderGraphPackage);
        if (package.Get() == nullptr)
        {
            setStatus(QStringLiteral("The runtime generation could not be allocated."));
            return RenderGraphPreviewLoad::Failed;
        }
        const renderer::RenderGraphPackageStatus decoded =
            package->Decode({reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())});
        if (decoded != renderer::RenderGraphPackageStatus::Ready)
        {
            setStatus(QStringLiteral("The runtime generation was rejected (%1); the shown frame is kept.")
                          .arg(DescribePackageStatus(decoded)));
            return RenderGraphPreviewLoad::Malformed;
        }
        return RenderGraphPreviewLoad::Staged;
    };
    RenderGraphPreviewLoad loaded = readPackage(runtimePackagePath);
    if (loaded != RenderGraphPreviewLoad::Staged)
    {
        return loaded;
    }
    QString selectedPath = runtimePackagePath;
    QString rendererStatus = QStringLiteral("Raster renderer.");
    if (renderer::UsesRayTracing(package->GetPlan()) == true)
    {
        const bool hasNativeRayQueries = renderer::RequiresHardwareRayTracing(package->GetPlan());
        renderer::RenderGraphRenderer selected = hasNativeRayQueries == true
                                                     ? renderer::RenderGraphRenderer::HardwareRayTracing
                                                     : renderer::RenderGraphRenderer::ComputeRayTracing;
        const renderer::RenderGraphPreference preference = state.ShouldForceRaster == true
                                                               ? renderer::RenderGraphPreference::ForceRaster
                                                               : renderer::RenderGraphPreference::Automatic;
        if (state.IsAttached == true && hasNativeRayQueries == true)
        {
            const rhi::RasterStatus status = renderer::SelectRenderGraphRenderer(state.Device, preference, selected);
            if (status != rhi::RasterStatus::Ready)
            {
                setStatus(QStringLiteral("Renderer selection failed (%1); the shown frame is kept.")
                              .arg(DescribeRasterStatus(status)));
                return RenderGraphPreviewLoad::Failed;
            }
        }
        if (state.ShouldForceRaster == true || selected == renderer::RenderGraphRenderer::Raster)
        {
            if (rasterFallbackPath.isEmpty() == true)
            {
                setStatus(QStringLiteral("A raster runtime package is required for this renderer selection."));
                return RenderGraphPreviewLoad::Unsupported;
            }
            loaded = readPackage(rasterFallbackPath);
            if (loaded != RenderGraphPreviewLoad::Staged)
            {
                return loaded;
            }
            if (renderer::UsesRayTracing(package->GetPlan()) == true)
            {
                setStatus(QStringLiteral("The raster fallback contains a raytracing pass; it was rejected."));
                return RenderGraphPreviewLoad::Unsupported;
            }
            selectedPath = rasterFallbackPath;
            rendererStatus = state.ShouldForceRaster == true
                                 ? QStringLiteral("Raster renderer (Force raster).")
                                 : QStringLiteral("Raster renderer (hardware raytracing unavailable on this GPU/API).");
        }
        else
        {
            rendererStatus = hasNativeRayQueries == true
                                 ? QStringLiteral("Compute raytracing renderer (native hardware BVH queries).")
                                 : QStringLiteral("Compute raytracing renderer (shader traversal).");
        }
    }
    // Denoising only filters the image. Without the optional denoiser, preview the graph as if its
    // Denoise nodes were disabled rather than show nothing; staging would otherwise reject it.
    if (renderer::RequiresDenoising(package->GetPlan()) == true && renderer::IsDenoisingAvailable() == false)
    {
        package->BypassDenoising();
        rendererStatus += QStringLiteral(" Denoise skipped: this editor was set up without the optional OIDN "
                                         "denoiser, so the preview is unfiltered.");
    }
    // The editor shows an exported color result; presentation surfaces belong to the game host.
    const renderer::RenderGraphPlan plan = package->GetPlan();
    State::Target target;
    target.RendererStatus = rendererStatus;
    bool hasTarget = false;
    for (usize i = 0; i < plan.Resources.GetSize(); ++i)
    {
        const format::Resource& resource = plan.Resources[i];
        target.Descriptions[i] = resource;
        if (resource.IsSurface == 1)
        {
            setStatus(QStringLiteral("This configuration renders to a presentation surface. Preview an Export "
                                     "(ColorTexture) root; Present roots run in the game host."));
            return RenderGraphPreviewLoad::Unsupported;
        }
        if (hasTarget == false && resource.IsExported == 1 && resource.Kind == format::Aspect::Color)
        {
            target.Resource = static_cast<uint32>(i);
            target.Width = resource.Width;
            target.Height = resource.Height;
            hasTarget = true;
        }
    }
    if (hasTarget == false)
    {
        setStatus(QStringLiteral("This configuration exports no color texture for the preview to show."));
        return RenderGraphPreviewLoad::Unsupported;
    }
    target.Cameras = static_cast<uint32>(plan.Description.CameraCount);
    // Names are optional: a missing or stale sidecar only leaves numeric texture names.
    QFile names(GetTargetNamesPath(selectedPath));
    const QByteArray namesDocument =
        names.open(QIODevice::ReadOnly) == true ? names.read(MAX_PACKAGE_BYTES) : QByteArray();
    target.Catalogue =
        MakeTargetCatalogue(plan, package->GetGenerationIdentity(), namesDocument, decodedRuntimeIdentity);
    target.PackagePath = Move(selectedPath);
    const BorrowedView<const uint8> generationIdentity = package->GetGenerationIdentity();
    target.GenerationIdentity = QByteArray(reinterpret_cast<const char*>(generationIdentity.GetDataOrNull()),
                                           static_cast<qsizetype>(generationIdentity.GetSize()));
    if (target.PackagePath == runtimePackagePath && target.GenerationIdentity.toHex() == source.Generation &&
        source.Generation.isEmpty() == false && decodedRuntimeIdentity == source.RuntimeIdentity)
    {
        target.Source = source;
    }
    state.ResolveTarget(target);
    state.Waiting = target;
    state.WaitingOrNull.Swap(package);
    if (state.IsAttached == false)
    {
        setStatus(QStringLiteral("Runtime generation decoded; start the World viewport to provide the preview "
                                 "device."));
        return RenderGraphPreviewLoad::Waiting;
    }
    if (state.Reader.IsBusy() == true || state.Inspector.IsSourcePending() == true || state.IsAwaitingProducer == true)
    {
        return RenderGraphPreviewLoad::Staged;
    }
    return stage();
}

RenderGraphPreviewLoad RenderGraphPreview::stage()
{
    State& state = *mState;
    state.IsResizeRejected = false;
    const rhi::RasterStatus staged = state.Activation.Stage(Move(state.WaitingOrNull));
    state.WaitingOrNull.Reset();
    const renderer::RenderGraphActivationReport report = state.Activation.GetReport();
    state.Candidate = state.Waiting;
    state.Candidate.Serial = report.CandidateGeneration;
    mStatus.clear();
    Q_EMIT Changed();
    if (report.Candidate == renderer::RenderGraphCandidateState::RestartRequired)
    {
        return RenderGraphPreviewLoad::RestartRequired;
    }
    if (IsAccepted(staged) == false)
    {
        return RenderGraphPreviewLoad::Failed;
    }
    return RenderGraphPreviewLoad::Staged;
}

void RenderGraphPreview::SetOutputTarget(const QString& name)
{
    if (mState->OutputName != name)
    {
        mState->OutputName = name;
        ++mState->OutputRevision;
        mState->FailedOutputGeneration = 0;
        mState->FailedOutputRevision = 0;
        // Redraw the current extraction; keep the last complete image during the new read.
        mPresentedSession = 0;
        Q_EMIT Changed();
    }
}
const QString& RenderGraphPreview::GetRequestedPackagePath() const noexcept
{
    return mState->RequestedPath;
}
const QString& RenderGraphPreview::GetRasterFallbackPath() const noexcept
{
    return mState->RasterFallbackPath;
}
const QString& RenderGraphPreview::GetOutputTarget() const noexcept
{
    return mState->OutputName;
}
bool RenderGraphPreview::IsCopyingOutput() const noexcept
{
    return mState->Reader.IsBusy() == true || mState->IsCopyingDisplay == true;
}
void RenderGraphPreview::SetExtraction(const RenderGraphExtraction& extraction)
{
    State& state = *mState;
    if (state.IsFrozen == true)
    {
        // The live graph keeps the frozen inputs; the edit applies when inspection ends.
        state.PendingExtraction = extraction;
        state.HasPendingExtraction = true;
        state.LiveView = extraction.View;
        state.LiveRevision = extraction.Revision;
        markRayPathStale(QStringLiteral("The World changed after this capture; capture again to trace the edit."));
        return;
    }
    state.Extraction = extraction;
    state.HasExtraction = extraction.Revision != 0;
}

void RenderGraphPreview::SetCamera(const renderer::ViewDescription& view, uint64 revision)
{
    State& state = *mState;
    if (state.IsFrozen == true)
    {
        // Navigation explores the frozen paths; it never moves the frozen graph camera.
        state.LiveView = view;
        state.LiveRevision = revision;
        return;
    }
    state.Extraction.View = view;
    state.Extraction.Revision = revision;
}
void RenderGraphPreview::SetReferenceSize(const QSize& size)
{
    State& state = *mState;
    if (state.IsFrozen == true && size != state.Freeze.ReferenceSize)
    {
        markRayPathStale(QStringLiteral("The viewport was resized after this capture; capture again at the new size."));
    }
    state.ReferenceSize = size;
}

QSize RenderGraphPreview::GetTargetSize() const noexcept
{
    const State& state = *mState;
    const State::Target& target = state.WaitingOrNull.Get() != nullptr ? state.Waiting
                                  : state.Candidate.Serial != 0        ? state.Candidate
                                                                       : state.Active;
    format::Resource resolved{};
    if (state.ReferenceSize.isEmpty() == false &&
        renderer::ResolveRenderGraphResource(target.Descriptions[target.Resource],
                                             static_cast<uint32>(state.ReferenceSize.width()),
                                             static_cast<uint32>(state.ReferenceSize.height()),
                                             resolved) == rhi::RasterStatus::Ready)
    {
        return {static_cast<int32>(resolved.Width), static_cast<int32>(resolved.Height)};
    }
    return {static_cast<int32>(target.Width), static_cast<int32>(target.Height)};
}

const GraphViewportStatistics& RenderGraphPreview::GetStatistics() const noexcept
{
    return mState->Statistics;
}

void RenderGraphPreview::RecordInspectionConversion(float64 workMilliseconds,
                                                    float64 elapsedMilliseconds,
                                                    float64 publicationMilliseconds) noexcept
{
    if (mState->IsAttached == true)
    {
        mState->Statistics.AddInspectionConversion(workMilliseconds, elapsedMilliseconds);
        mState->Statistics.AddCpuWork(publicationMilliseconds);
    }
}
ViewportFrameTrace& RenderGraphPreview::GetFrameTrace() noexcept
{
    return mState->Trace;
}
void RenderGraphPreview::SetDirectPresentation(bool shouldPresentDirectly) noexcept
{
    mState->ShouldPresentDirectly = shouldPresentDirectly;
}
bool RenderGraphPreview::HasPendingWork() const noexcept
{
    const State& state = *mState;
    const renderer::RenderGraphCandidateState candidate = state.Activation.GetReport().Candidate;
    return state.IsAwaitingProducer == true || state.Reader.IsBusy() == true || state.IsCopyingDisplay == true ||
           state.Inspector.IsBusy() == true || state.HasPendingPreparation == true ||
           state.Capture.HasPendingWork() == true || candidate == renderer::RenderGraphCandidateState::Preparing ||
           (candidate == renderer::RenderGraphCandidateState::NeedsUpdate && state.HasExtraction == true);
}
bool RenderGraphPreview::HasGpuFrame() const noexcept
{
    return mState->HasGpuFrame;
}
GraphDisplayFrames& RenderGraphPreview::GetDisplayFrames() noexcept
{
    return mState->Display;
}
void RenderGraphPreview::Tick(float64 preparationMilliseconds)
{
    State& state = *mState;
    renderer::RenderGraph* activeOrNull = state.Activation.GetActiveOrNull();
    const bool hasPendingSamples = activeOrNull != nullptr && activeOrNull->HasPendingSamples() == true;
    const bool hasActiveWork =
        state.IsAttached == true &&
        (hasPendingSamples == true || state.IsCopyingDisplay == true || state.Reader.IsBusy() == true ||
         state.IsAwaitingProducer == true || state.Inspector.IsBusy() == true || state.IsInspectionRequested == true ||
         state.Candidate.Serial != 0 ||
         (state.HasExtraction == true &&
          (state.Extraction.Revision != mPresentedRevision || state.Session != mPresentedSession)));
    QElapsedTimer timer;
    timer.start();
    state.HasCompletedStatisticsFrame = false;
    tick();
    if (state.IsAttached == false)
    {
        return;
    }
    if (hasActiveWork == true || state.HasCompletedStatisticsFrame == true)
    {
        state.Statistics.AddCpuWork(preparationMilliseconds +
                                    static_cast<float64>(timer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND);
    }
    if (state.HasCompletedStatisticsFrame == true)
    {
        state.Statistics.CompleteFrame(time::Now().Nanoseconds,
                                       state.HasGpuFrame == true
                                           ? 0
                                           : static_cast<float64>(state.ReadbackTimer.nsecsElapsed()) /
                                                 NANOSECONDS_PER_MILLISECOND);
        Q_EMIT FramePresented();
        if (state.ShouldPresentDirectly == true)
        {
            Q_EMIT state.Display.FrameReady();
        }
    }
}
void RenderGraphPreview::tick()
{
    LUDUS_PROFILE_SCOPE(EditorRenderGraphTick);
    State& state = *mState;
    if (state.IsAttached == false)
    {
        return;
    }
    if (state.Inspector.IsBusy() == true)
    {
        const QElapsedTimer inspectionTimer = state.InspectionTimer;
        QElapsedTimer inspectionCpuTimer;
        inspectionCpuTimer.start();
        finishInspection();
        if (state.IsAttached == false)
        {
            return;
        }
        state.InspectionCpuMilliseconds +=
            static_cast<float64>(inspectionCpuTimer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND;
        if (state.Inspector.IsBusy() == false)
        {
            state.Statistics.AddInspection(state.InspectionCpuMilliseconds,
                                           static_cast<float64>(inspectionTimer.nsecsElapsed()) /
                                               NANOSECONDS_PER_MILLISECOND);
        }
        if (state.Inspector.IsSourcePending() == true)
        {
            return;
        }
    }
    if (state.IsCopyingDisplay == true)
    {
        finishDisplayCopy();
        return;
    }
    if (state.Reader.IsBusy() == true)
    {
        finishCopy();
        return;
    }
    if (state.IsAwaitingProducer == true)
    {
        renderer::RenderGraph* activeOrNull = state.Activation.GetActiveOrNull();
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect = rhi::TextureAspect::Color;
        uint32 resource = state.Active.Resource;
        uint32 width = state.Active.Width;
        uint32 height = state.Active.Height;
        if (state.OutputName.isEmpty() == false)
        {
            const qsizetype index = state.Active.Catalogue.Find(state.OutputName);
            if (index < 0)
            {
                state.IsAwaitingProducer = false;
                state.FailedOutputGeneration = state.RenderedGeneration;
                state.FailedOutputRevision = state.RenderedRevision;
                state.Trace.Cancel("selected output unavailable");
                setStatus(QStringLiteral("The selected graph target is unavailable; the shown frame is kept."));
                return;
            }
            const RenderGraphTarget& target = state.Active.Catalogue.Targets[index];
            resource = target.Resource;
            width = target.Width;
            height = target.Height;
        }
        const rhi::RasterStatus exported = activeOrNull == nullptr
                                               ? rhi::RasterStatus::InvalidState
                                               : activeOrNull->GetResourceAllocation(resource, texture, aspect);
        if (exported == rhi::RasterStatus::NotReady && state.ProducerTimer.elapsed() < MAX_PRODUCER_MILLISECONDS)
        {
            return;
        }
        if (exported != rhi::RasterStatus::Ready)
        {
            state.IsAwaitingProducer = false;
            state.Trace.Cancel("producer failed");
            setStatus(QStringLiteral("The preview frame did not complete (%1); the shown frame is kept.")
                          .arg(DescribeRasterStatus(exported)));
            return;
        }
        format::Resource description;
        const bool canUseGpuDisplay = state.ShouldPresentDirectly == true && aspect == rhi::TextureAspect::Color &&
                                      activeOrNull->GetResourceDescription(resource, description) == true &&
                                      description.IsExported != 0;
        // Transient attachments remain owned by their graph pool and cannot be imported into
        // the display-copy graph. Explicit inspection keeps the existing readback path.
        if (canUseGpuDisplay == true)
        {
            const rhi::RasterStatus copied = state.Display.Copy(state.Surface, texture, width, height);
            if (copied == rhi::RasterStatus::NotReady)
            {
                state.Trace.SetStage("Await GPU display capacity");
                return;
            }
            state.IsAwaitingProducer = false;
            state.ReadingOutputRevision = state.OutputRevision;
            if (copied == rhi::RasterStatus::Ready)
            {
                state.Display.PublishCopy();
                publishFrame(true);
            }
            else if (copied == rhi::RasterStatus::Pending)
            {
                state.IsCopyingDisplay = true;
                state.Trace.SetStage("Await GPU display snapshot");
            }
            else
            {
                state.FailedOutputGeneration = state.RenderedGeneration;
                state.FailedOutputRevision = state.RenderedRevision;
                state.Trace.Cancel("GPU display copy failed");
                setStatus(QStringLiteral("GPU presentation failed (%1); the shown frame is kept.")
                              .arg(DescribeRasterStatus(copied)));
            }
            return;
        }
        state.IsAwaitingProducer = false;
        state.ReadingOutputRevision = state.OutputRevision;
        state.Trace.SetStage("Readback requests and wait (observed)");
        state.ReadbackTimer.start();
        const RenderTargetReadStatus started =
            state.Reader.Begin(state.Device, state.Surface, texture, aspect, width, height);
        if (started != RenderTargetReadStatus::Pending && started != RenderTargetReadStatus::Complete)
        {
            state.FailedOutputGeneration = state.RenderedGeneration;
            state.FailedOutputRevision = state.RenderedRevision;
            state.Trace.Cancel("readback request rejected");
            setStatus(
                started == RenderTargetReadStatus::Undefined
                    ? QStringLiteral("The selected graph target was discarded or has undefined contents; "
                                     "the shown frame is kept.")
                    : QStringLiteral("The preview frame cannot be copied on this device; the shown frame is kept."));
            return;
        }
        finishCopy();
        return;
    }
    // No frame is open and no export copy is in flight: the active graph's attachments hold
    // the last completed frame until the next Render, so the viewer reads them now.
    if (state.IsInspectionEnabled == true && state.IsInspectionRequested == true && state.Inspector.IsBusy() == false &&
        beginInspection() == true)
    {
        return;
    }
    if (state.ShouldReselectRenderer == true)
    {
        static_cast<void>(Load(state.RequestedPath, state.RasterFallbackPath, state.RequestedSource));
    }
    else if (state.WaitingOrNull.Get() != nullptr)
    {
        static_cast<void>(stage());
    }
    advance();
}

void RenderGraphPreview::finishCopy()
{
    State& state = *mState;
    const RenderTargetReadStatus status = state.Reader.Poll();
    if (status != RenderTargetReadStatus::Pending && state.ReadingOutputRevision != state.OutputRevision)
    {
        // A superseded read cannot consume the new selection's pending publication marker.
        state.Reader.Cancel();
        state.Trace.Cancel("output selection superseded");
        return;
    }
    if (status == RenderTargetReadStatus::Complete)
    {
        state.Trace.SetStage("CPU display image conversion");
        InspectedTexture texture = state.Reader.TakeTexture();
        TextureDisplaySettings settings;
        if (texture.Encoding == TexelEncoding::Depth32Float)
        {
            settings.Range = TextureRangeMode::Automatic;
        }
        TextureValueRange range;
        {
            LUDUS_PROFILE_SCOPE(EditorGraphDisplayImageConversion);
            mImage = MakeDisplayImage(texture, settings, range);
        }
        mOutputTexture = Move(texture);
        publishFrame();
    }
    else if (status == RenderTargetReadStatus::Failed || status == RenderTargetReadStatus::Undefined)
    {
        state.Reader.Cancel();
        state.Trace.Cancel("output readback failed");
        setStatus(QStringLiteral("The preview frame copy failed; the shown frame is kept."));
    }
}

void RenderGraphPreview::publishFrame(bool isGpuFrame)
{
    State& state = *mState;
    state.HasGpuFrame = isGpuFrame;
    state.Trace.Publish();
    mPresentedGeneration = state.RenderedGeneration;
    mPresentedRevision = state.RenderedRevision;
    mPresentedSession = state.Session;
    setStatus(mPresentedRevision == state.MaterialStatusRevision ? state.MaterialStatus : QString{});
    state.IsInspectionRequested = state.IsInspectionEnabled == true && state.InspectedName.isEmpty() == false;
    state.HasCompletedStatisticsFrame = true;
}
void RenderGraphPreview::finishDisplayCopy()
{
    State& state = *mState;
    const rhi::RasterStatus status = state.Display.PollCopy();
    if (status == rhi::RasterStatus::Pending)
    {
        return;
    }
    state.IsCopyingDisplay = false;
    if (status == rhi::RasterStatus::Ready && state.ReadingOutputRevision == state.OutputRevision)
    {
        state.Display.PublishCopy();
        publishFrame(true);
    }
    else
    {
        state.Display.DiscardCopy();
        if (state.ReadingOutputRevision == state.OutputRevision)
        {
            state.FailedOutputGeneration = state.RenderedGeneration;
            state.FailedOutputRevision = state.RenderedRevision;
        }
        state.Trace.Cancel("GPU display copy failed or superseded");
        setStatus(QStringLiteral("GPU presentation was superseded or failed; the shown frame is kept."));
    }
}

void RenderGraphPreview::advance()
{
    State& state = *mState;
    // No live frame or export copy is in flight here, so the capture graph may submit its sample.
    if (state.Capture.Advance() == true)
    {
        Q_EMIT RayPathCaptureChanged();
    }
    if (state.ReferenceSize.isEmpty() == true)
    {
        return;
    }
    if (state.AppliedReference != state.ReferenceSize)
    {
        const rhi::RasterStatus resized = state.Activation.Resize(static_cast<uint32>(state.ReferenceSize.width()),
                                                                  static_cast<uint32>(state.ReferenceSize.height()));
        state.AppliedReference = state.ReferenceSize;
        state.IsResizeRejected = IsAccepted(resized) == false;
        if (IsAccepted(resized) == false)
        {
            setStatus(QStringLiteral("The requested render size was rejected (%1); the shown frame is kept. Reduce the "
                                     "connected RenderSize scale or fixed dimensions, then retry.")
                          .arg(DescribeRasterStatus(resized)));
            return;
        }
        state.UpdatedRevision = 0;
        state.ResolveTarget(state.Candidate);
    }
    if (state.IsResizeRejected == true)
    {
        return;
    }
    const renderer::RenderGraphActivationReport before = state.Activation.GetReport();
    // No export consumer is outstanding here, so Poll may activate and Reset the old generation.
    const rhi::RasterStatus preparation = state.Activation.Poll();
    state.HasPendingPreparation = preparation == rhi::RasterStatus::Pending;
    const renderer::RenderGraphActivationReport report = state.Activation.GetReport();
    if (report.ActiveGeneration != before.ActiveGeneration && report.ActiveGeneration == state.Candidate.Serial)
    {
        state.Active = state.Candidate;
        state.Candidate = {};
        if (state.IsFrozen == true)
        {
            markRayPathStale(QStringLiteral("The graph was rebuilt after this capture; capture again to trace the "
                                            "new generation."));
        }
        // A rebuilt generation renumbers textures; the selection follows its name.
        Q_EMIT TargetsChanged();
    }
    if (report.Candidate != before.Candidate || report.ActiveGeneration != before.ActiveGeneration)
    {
        Q_EMIT Changed();
    }
    if (state.HasExtraction == true && (state.Extraction.Revision != state.UpdatedRevision ||
                                        report.Candidate == renderer::RenderGraphCandidateState::NeedsUpdate))
    {
        // One editor view feeds every generation camera; the drawing generation sets the count.
        const uint32 cameraCount = report.ActiveGeneration != 0 ? state.Active.Cameras : state.Candidate.Cameras;
        QVector<renderer::ViewDescription> cameras(static_cast<qsizetype>(cameraCount), state.Extraction.View);
        renderer::RenderGraphReport graphReport;
        renderer::SceneShaderInputs shaderInputs = state.Extraction.ShaderInputs;
        shaderInputs.Lights = {state.Extraction.Scene.AuthoredLights.Lights,
                               state.Extraction.Scene.AuthoredLights.Count};
        const rhi::RasterStatus updated = state.Activation.Update(
            {state.Extraction.Meshes.constData(), static_cast<usize>(state.Extraction.Meshes.size())},
            {state.Extraction.Items.constData(), static_cast<usize>(state.Extraction.Items.size())},
            {cameras.constData(), static_cast<usize>(cameras.size())},
            state.Extraction.Revision,
            renderer::RenderGraphCoveragePolicy::DiagnosticColor,
            graphReport,
            &shaderInputs,
            state.Extraction.ContentRevision);
        state.HasPendingPreparation = state.HasPendingPreparation == true || updated == rhi::RasterStatus::Pending;
        if (IsAccepted(updated) == true)
        {
            state.MaterialStatusRevision = state.Extraction.Revision;
            state.MaterialStatus =
                graphReport.SceneDiagnostics.ErrorBindings == 0
                    ? QString{}
                    : QStringLiteral(
                          "%1 material bindings use the non-emissive error material; first source %2, material %3.")
                          .arg(graphReport.SceneDiagnostics.ErrorBindings)
                          .arg(graphReport.SceneDiagnostics.ErrorSourceId)
                          .arg(graphReport.SceneDiagnostics.ErrorMaterial);
        }
        // NotReady means no generation could accept it yet (first candidate still preparing): retry.
        if (updated != rhi::RasterStatus::NotReady)
        {
            state.UpdatedRevision = state.Extraction.Revision;
            if (IsAccepted(updated) == false)
            {
                setStatus(DescribeRenderGraphExtractionFailure(updated, graphReport.SceneDiagnostics));
            }
            Q_EMIT Changed();
        }
    }
    renderer::RenderGraph* activeOrNull = state.Activation.GetActiveOrNull();
    if (activeOrNull == nullptr)
    {
        return;
    }
    bool hasResized = false;
    for (RenderGraphTarget& target : state.Active.Catalogue.Targets)
    {
        format::Resource resolved{};
        if (activeOrNull->GetResourceDescription(target.Resource, resolved) == true)
        {
            hasResized = hasResized == true || target.Width != resolved.Width || target.Height != resolved.Height;
            target.Width = resolved.Width;
            target.Height = resolved.Height;
            if (target.Resource == state.Active.Resource)
            {
                state.Active.Width = resolved.Width;
                state.Active.Height = resolved.Height;
            }
        }
    }
    if (hasResized == true)
    {
        mPresentedSession = 0;
        state.FailedOutputGeneration = 0;
        state.FailedOutputRevision = 0;
        Q_EMIT TargetsChanged();
    }
    const uint64 drawable = activeOrNull->GetDrawableSceneRevision();
    // While frozen the inspection view is rasterized from the free camera; the live graph waits.
    if (state.IsFrozen == true ||
        (state.FailedOutputGeneration == report.ActiveGeneration && state.FailedOutputRevision == drawable) ||
        drawable == 0 ||
        (mPresentedSession == state.Session && report.ActiveGeneration == mPresentedGeneration &&
         drawable == mPresentedRevision && activeOrNull->HasPendingSamples() == false))
    {
        return;
    }
    state.Trace.BeginFrame();
    rhi::SubmissionToken completion;
    Q_EMIT PreparingSubmission();
    const rhi::RasterStatus rendered =
        activeOrNull->Render(state.Surface, state.Active.Width, state.Active.Height, completion);
    if (rendered == rhi::RasterStatus::NotReady)
    {
        state.Trace.SetStage("Await graph submission admission (NotReady)");
        return;
    }
    if (IsAccepted(rendered) == false)
    {
        state.Trace.Cancel("render submission rejected");
        setStatus(QStringLiteral("The active generation could not render (%1); the shown frame is kept.")
                      .arg(DescribeRasterStatus(rendered)));
        return;
    }
    Q_EMIT SubmissionRecorded();
    state.RenderedGeneration = report.ActiveGeneration;
    state.RenderedRevision = drawable;
    state.Trace.SetStage("Await producer completion (observed)");
    state.IsAwaitingProducer = true;
    state.ProducerTimer.start();
}

const RenderGraphPreviewSource& RenderGraphPreview::GetActiveSource() const noexcept
{
    return mState->Active.Source;
}
bool RenderGraphPreview::HasPresentedCatalogue() const noexcept
{
    return mState->IsAttached == true && mPresentedSession == mState->Session && mPresentedGeneration != 0 &&
           mState->Activation.GetReport().ActiveGeneration == mPresentedGeneration;
}
const RenderGraphTargetCatalogue& RenderGraphPreview::GetTargetCatalogue() const noexcept
{
    return mState->Active.Catalogue;
}

void RenderGraphPreview::SetInspectionEnabled(bool isEnabled) noexcept
{
    State& state = *mState;
    if (state.IsInspectionEnabled == isEnabled)
    {
        return;
    }
    state.IsInspectionEnabled = isEnabled;
    state.Statistics.SetInspectionEnabled(isEnabled);
    state.Inspector.Cancel();
    state.InspectionTimer.invalidate();
    state.IsInspectionRequested = isEnabled == true && state.InspectedName.isEmpty() == false;
}

void RenderGraphPreview::SetInspectedTarget(const QString& name)
{
    State& state = *mState;
    // Reselecting reads again; a read already in flight for the old choice is dropped.
    state.Inspector.Cancel();
    state.InspectedName = name;
    state.InspectionTimer.invalidate();
    state.IsInspectionRequested = state.IsInspectionEnabled == true && name.isEmpty() == false;
    if (name.isEmpty() == true)
    {
        mInspected = {};
        mInspectionStatus.clear();
        mInspectedGeneration = 0;
        mInspectedRevision = 0;
        Q_EMIT TargetInspected();
    }
}

const QString& RenderGraphPreview::GetInspectedTarget() const noexcept
{
    return mState->InspectedName;
}

bool RenderGraphPreview::beginInspection()
{
    State& state = *mState;
    if (state.InspectionTimer.isValid() == true &&
        state.InspectionTimer.elapsed() < State::INSPECTION_INTERVAL_MILLISECONDS)
    {
        return false;
    }
    renderer::RenderGraph* activeOrNull = state.Activation.GetActiveOrNull();
    if (activeOrNull == nullptr || mPresentedSession != state.Session || mPresentedGeneration == 0)
    {
        return false;
    }
    LUDUS_PROFILE_SCOPE(EditorTextureInspectionSnapshot);
    const qsizetype index = state.Active.Catalogue.Find(state.InspectedName);
    if (index < 0)
    {
        state.IsInspectionRequested = false;
        mInspected = {};
        mInspectionStatus =
            QStringLiteral("%1 is not a texture of generation %2.").arg(state.InspectedName).arg(mPresentedGeneration);
        Q_EMIT TargetInspected();
        return false;
    }
    const RenderGraphTarget& target = state.Active.Catalogue.Targets[index];
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect = rhi::TextureAspect::Color;
    const rhi::RasterStatus allocated = activeOrNull->GetResourceAllocation(target.Resource, texture, aspect);
    if (allocated == rhi::RasterStatus::NotReady)
    {
        // The producer is still running; keep the request and let the frame continue.
        return false;
    }
    state.IsInspectionRequested = false;
    if (allocated != rhi::RasterStatus::Ready)
    {
        mInspected = {};
        mInspectionStatus =
            allocated == rhi::RasterStatus::Unsupported
                ? QStringLiteral("%1 is the presentation surface, which the game host owns.").arg(target.Name)
                : QStringLiteral("%1 cannot be inspected (%2).").arg(target.Name, DescribeRasterStatus(allocated));
        Q_EMIT TargetInspected();
        return false;
    }
    state.InspectingGeneration = mPresentedGeneration;
    state.InspectingRevision = mPresentedRevision;
    state.InspectionTimer.start();
    QElapsedTimer inspectionCpuTimer;
    inspectionCpuTimer.start();
    const RenderTargetReadStatus started =
        state.Inspector.Begin(state.Device, state.Surface, texture, aspect, target.Width, target.Height, true);
    state.InspectionCpuMilliseconds =
        static_cast<float64>(inspectionCpuTimer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND;
    if (started == RenderTargetReadStatus::Pending)
    {
        return true;
    }
    state.Statistics.AddInspection(state.InspectionCpuMilliseconds,
                                   static_cast<float64>(state.InspectionTimer.nsecsElapsed()) /
                                       NANOSECONDS_PER_MILLISECOND);
    mInspected = {};
    mInspectedGeneration = state.InspectingGeneration;
    mInspectedRevision = state.InspectingRevision;
    mInspectionStatus =
        started == RenderTargetReadStatus::Undefined
            ? QStringLiteral("%1 holds no stored contents after the frame: its last pass discards it (Store = "
                             "Discard) or nothing writes it.")
                  .arg(target.Name)
            : QStringLiteral("%1 could not be read back on this device.").arg(target.Name);
    Q_EMIT TargetInspected();
    return false;
}

void RenderGraphPreview::finishInspection()
{
    LUDUS_PROFILE_SCOPE(EditorTextureInspectionPoll);
    State& state = *mState;
    // Retire finished diagnostic bands first, leaving staging capacity for the delivered image.
    const RenderTargetReadStatus status = state.Inspector.Poll(state.Reader.IsBusy() == false);
    if (status == RenderTargetReadStatus::Pending)
    {
        return;
    }
    if (status == RenderTargetReadStatus::Complete)
    {
        mInspected = state.Inspector.TakeTexture();
        mInspectionStatus.clear();
    }
    else
    {
        state.Inspector.Cancel();
        mInspected = {};
        mInspectionStatus =
            QStringLiteral("The %1 readback failed; select it again to retry.").arg(state.InspectedName);
    }
    mInspectedGeneration = state.InspectingGeneration;
    mInspectedRevision = state.InspectingRevision;
    Q_EMIT TargetInspected();
}

QString RenderGraphPreview::DescribeRayPathCaptureProblem() const
{
    const State& state = *mState;
    if (state.IsAttached == false)
    {
        return QStringLiteral("Start the World viewport to capture ray paths.");
    }
    const renderer::RenderGraphActivationReport report = state.Activation.GetReport();
    if (report.ActiveGeneration == 0 || state.Active.Serial != report.ActiveGeneration || state.HasExtraction == false)
    {
        return QStringLiteral("Wait until the graph draws the World, then capture ray paths.");
    }
    switch (report.ActiveRenderer)
    {
        case renderer::RenderGraphRenderer::HardwareRayTracing:
            return {};
        case renderer::RenderGraphRenderer::ComputeRayTracing:
            return QStringLiteral("Ray path capture is unavailable: this graph traces with software traversal, and "
                                  "capture records native ray queries.");
        case renderer::RenderGraphRenderer::Raster:
            break;
    }
    return state.ShouldForceRaster == true
               ? QStringLiteral("Ray path capture is unavailable while Force raster is selected.")
               : QStringLiteral("Ray path capture is unavailable: the drawing renderer is raster (this GPU/API has "
                                "no native ray queries, or the graph has no ray pass).");
}

bool RenderGraphPreview::FreezeRayPaths(QString& outError)
{
    State& state = *mState;
    const QString problem = DescribeRayPathCaptureProblem();
    if (problem.isEmpty() == false)
    {
        outError = problem;
        return false;
    }
    RayPathFreeze freeze;
    freeze.View = state.Extraction.View;
    freeze.ReferenceSize = state.ReferenceSize;
    freeze.OutputSize = {static_cast<int32>(state.Active.Width), static_cast<int32>(state.Active.Height)};
    freeze.SceneRevision = state.Extraction.Revision;
    freeze.ContentRevision = state.Extraction.ContentRevision;
    freeze.Generation = state.Active.Serial;
    freeze.GenerationIdentity = state.Active.GenerationIdentity;
    freeze.InterfaceIdentity = QByteArray(reinterpret_cast<const char*>(state.Interface.GetData()),
                                          static_cast<qsizetype>(format::IDENTITY_BYTES));
    freeze.PackagePath = state.Active.PackagePath;
    state.Freeze = Move(freeze);
    state.IsFrozen = true;
    state.StaleReason.clear();
    state.LiveView = state.Extraction.View;
    state.LiveRevision = state.Extraction.Revision;
    state.HasPendingExtraction = false;
    state.PendingExtraction = {};
    state.Capture.Reset();
    Q_EMIT RayPathCaptureChanged();
    return true;
}

void RenderGraphPreview::ThawRayPaths()
{
    State& state = *mState;
    if (state.IsFrozen == false)
    {
        return;
    }
    state.Capture.Reset();
    state.IsFrozen = false;
    state.Freeze = {};
    state.StaleReason.clear();
    if (state.HasPendingExtraction == true)
    {
        state.Extraction = state.PendingExtraction;
        state.HasExtraction = state.Extraction.Revision != 0;
        state.PendingExtraction = {};
        state.HasPendingExtraction = false;
    }
    // Revisions only grow, so a camera supplied after the last extraction carries a newer one.
    if (state.LiveRevision != 0 && state.LiveRevision != state.Extraction.Revision)
    {
        state.Extraction.View = state.LiveView;
        state.Extraction.Revision = state.LiveRevision;
    }
    Q_EMIT RayPathCaptureChanged();
    Q_EMIT Changed();
}

bool RenderGraphPreview::IsRayPathFrozen() const noexcept
{
    return mState->IsFrozen;
}

const RayPathFreeze& RenderGraphPreview::GetRayPathFreeze() const noexcept
{
    return mState->Freeze;
}

void RenderGraphPreview::BeginRayPathBuild()
{
    mState->Capture.BeginBuild();
    Q_EMIT RayPathCaptureChanged();
}

void RenderGraphPreview::FailRayPathCapture(const QString& reason, bool isUnavailable)
{
    if (isUnavailable == true)
    {
        mState->Capture.SetUnavailable(reason);
    }
    else
    {
        mState->Capture.Fail(reason);
    }
    Q_EMIT RayPathCaptureChanged();
}

bool RenderGraphPreview::StartRayPathCapture(const QString& packagePath,
                                             math::Vector2 pixel,
                                             const RayPathTarget& target,
                                             uint32 sampleCount)
{
    State& state = *mState;
    if (state.IsFrozen == false || state.IsAttached == false)
    {
        state.Capture.Fail(QStringLiteral("Freeze the World viewport before tracing ray paths."));
        Q_EMIT RayPathCaptureChanged();
        return false;
    }
    QFile file(packagePath);
    if (QFileInfo(packagePath).isFile() == false || QFileInfo(packagePath).size() > MAX_PACKAGE_BYTES ||
        file.open(QIODevice::ReadOnly) == false)
    {
        state.Capture.Fail(QStringLiteral("The ray path capture package %1 is missing or unreadable.")
                               .arg(QFileInfo(packagePath).fileName()));
        Q_EMIT RayPathCaptureChanged();
        return false;
    }
    const QByteArray bytes = file.read(MAX_PACKAGE_BYTES);
    const bool isStarted = state.Capture.Start(state.Device,
                                               state.Surface,
                                               bytes,
                                               state.Extraction,
                                               state.Freeze,
                                               pixel,
                                               target,
                                               sampleCount,
                                               state.CompositeVertex,
                                               state.CompositeFragment);
    Q_EMIT RayPathCaptureChanged();
    Q_EMIT Changed();
    return isStarted;
}

const RayPathCapture& RenderGraphPreview::GetRayPathCapture() const noexcept
{
    return mState->Capture;
}

const QString& RenderGraphPreview::GetRayPathStaleReason() const noexcept
{
    return mState->StaleReason;
}

void RenderGraphPreview::markRayPathStale(const QString& reason)
{
    State& state = *mState;
    if (state.StaleReason.isEmpty() == true)
    {
        state.StaleReason = reason;
        Q_EMIT RayPathCaptureChanged();
    }
}

renderer::RenderGraphActivationReport RenderGraphPreview::GetReport() const noexcept
{
    return mState->Activation.GetReport();
}

void RenderGraphPreview::setStatus(const QString& text)
{
    if (mStatus != text)
    {
        mStatus = text;
        Q_EMIT Changed();
    }
}

QString RenderGraphPreview::GetDisplayStatus() const
{
    const renderer::RenderGraphActivationReport report = mState->Activation.GetReport();
    if (report.Candidate == renderer::RenderGraphCandidateState::Failed ||
        report.Candidate == renderer::RenderGraphCandidateState::RestartRequired)
    {
        const QString failure = report.Candidate == renderer::RenderGraphCandidateState::RestartRequired
                                    ? QStringLiteral("Renderer activation requires an editor restart")
                                    : QStringLiteral("Renderer activation failed");
        return QStringLiteral("%1 (%2) for generation %3; the last good scene is kept.")
            .arg(failure, DescribeRasterStatus(report.LastFailure))
            .arg(report.CandidateGeneration);
    }
    if (mStatus.isEmpty() == false)
    {
        return mStatus;
    }
    if (mState->IsAttached == false)
    {
        return mImage.isNull() == true ? QStringLiteral("Open a World to render it here.")
                                       : QStringLiteral("Viewport paused.");
    }
    switch (mState->Activation.GetReport().Candidate)
    {
        case renderer::RenderGraphCandidateState::Preparing:
        case renderer::RenderGraphCandidateState::NeedsUpdate:
            return QStringLiteral("Updating renderer…");
        case renderer::RenderGraphCandidateState::Failed:
        case renderer::RenderGraphCandidateState::RestartRequired:
            break;
        case renderer::RenderGraphCandidateState::None:
            return mImage.isNull() == true && mState->HasGpuFrame == false ? QStringLiteral("Preparing renderer…")
                                                                           : QString();
    }
    return {};
}

QString RenderGraphPreview::GetStatus() const
{
    const State& state = *mState;
    QStringList parts;
    if (mPresentedGeneration != 0)
    {
        parts.append(QStringLiteral("Showing generation %1 at World revision %2.")
                         .arg(mPresentedGeneration)
                         .arg(mPresentedRevision));
    }
    if (state.Active.Serial != 0 && mPresentedGeneration == state.Active.Serial && mPresentedSession == state.Session)
    {
        parts.append(state.Active.RendererStatus);
    }
    if (state.Candidate.Serial != 0)
    {
        parts.append(QStringLiteral("Selected: %1").arg(state.Candidate.RendererStatus));
    }
    if (state.IsAttached == false)
    {
        parts.append(mImage.isNull() == true
                         ? QStringLiteral("Start the World viewport to provide the preview device.")
                         : QStringLiteral("The viewport device stopped; this is the last presented frame."));
    }
    else
    {
        const renderer::RenderGraphActivationReport report = state.Activation.GetReport();
        switch (report.Candidate)
        {
            case renderer::RenderGraphCandidateState::None:
                if (report.ActiveGeneration == 0 && state.RequestedPath.isEmpty() == true)
                {
                    parts.append(QStringLiteral("Cook the runtime, then choose Preview Runtime."));
                }
                else if (mPresentedSession != state.Session || mPresentedGeneration != report.ActiveGeneration)
                {
                    parts.append(QStringLiteral("Generation %1 is active; its first frame is being prepared.")
                                     .arg(report.ActiveGeneration));
                }
                break;
            case renderer::RenderGraphCandidateState::Preparing:
            case renderer::RenderGraphCandidateState::NeedsUpdate:
                parts.append(QStringLiteral("Candidate generation %1 is preparing; the shown frame is kept.")
                                 .arg(report.CandidateGeneration));
                break;
            case renderer::RenderGraphCandidateState::Failed:
                parts.append(QStringLiteral("Candidate generation %1 failed (%2); the shown frame is kept.")
                                 .arg(report.CandidateGeneration)
                                 .arg(DescribeRasterStatus(report.LastFailure)));
                break;
            case renderer::RenderGraphCandidateState::RestartRequired:
                parts.append(QStringLiteral("Candidate generation %1 needs a restart: its CPU interface differs from "
                                            "this editor's compiled interface. Nothing was staged.")
                                 .arg(report.CandidateGeneration));
                break;
        }
    }
    if (mStatus.isEmpty() == false)
    {
        parts.append(mStatus);
    }
    return parts.join(QLatin1Char(' '));
}
} // namespace ludus::editor
