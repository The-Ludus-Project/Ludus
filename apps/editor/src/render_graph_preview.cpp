#include "internal/render_graph_preview.h"

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/runtime/resources/scene.h>

#include "internal/offscreen_frame_reader.h"
#include "internal/render_target_reader.h"

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
/// Ticks (about 16 ms each) to wait for the producer of one frame before reporting a stall.
constexpr uint32 MAX_PRODUCER_TICKS = 600;
/// Straight alpha below one cannot use the opaque static profile.
constexpr float32 OPAQUE_ALPHA = 1.0F;

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
        record.Layout = renderer::RenderGraphLayout::Static;
        extraction.Meshes.append(record);
    }
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
        extraction.Items.append(item);
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
    };
    StaticArray<uint8, format::IDENTITY_BYTES> Interface{};
    rhi::RasterShaderDescription CompositeVertex{};
    rhi::RasterShaderDescription CompositeFragment{};
    renderer::RenderGraphActivation Activation;
    OffscreenFrameReader Reader;
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    bool IsAttached = false;
    bool IsInterfaceValid = false;
    /// Incremented per Attach; activation serials restart with every device session.
    uint64 Session = 0;
    QString RequestedPath;
    UniquePtr<renderer::RenderGraphPackage> WaitingOrNull;
    Target Waiting;
    Target Candidate;
    Target Active;
    RenderGraphExtraction Extraction;
    bool HasExtraction = false;
    uint64 UpdatedRevision = 0;
    bool IsAwaitingProducer = false;
    uint32 ProducerTicks = 0;
    uint64 RenderedGeneration = 0;
    uint64 RenderedRevision = 0;
    /// Texture viewer readback; runs only between frames, after the export copy.
    RenderTargetReader Inspector;
    QString InspectedName;
    bool IsInspectionRequested = false;
    uint64 InspectingGeneration = 0;
    uint64 InspectingRevision = 0;
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
    mState->Reader.Cancel();
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
    state.Device = device;
    state.Surface = surface;
    state.IsAttached = true;
    ++state.Session;
    mStatus.clear();
    // An explicit preview request survives a viewport device restart.
    if (state.WaitingOrNull.Get() == nullptr && state.RequestedPath.isEmpty() == false)
    {
        static_cast<void>(Load(state.RequestedPath));
    }
    else if (state.WaitingOrNull.Get() != nullptr)
    {
        static_cast<void>(stage());
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
    // Drop the export and inspection consumers before the generations that own their textures.
    state.Reader.Cancel();
    state.Inspector.Release();
    state.Activation.Reset();
    state.Device = {};
    state.Surface = {};
    state.IsAttached = false;
    state.Candidate = {};
    state.Active = {};
    state.UpdatedRevision = 0;
    state.IsAwaitingProducer = false;
    state.ProducerTicks = 0;
    Q_EMIT Changed();
}

bool RenderGraphPreview::IsAttached() const noexcept
{
    return mState->IsAttached;
}

RenderGraphPreviewLoad RenderGraphPreview::Load(const QString& runtimePackagePath)
{
    State& state = *mState;
    state.RequestedPath = runtimePackagePath;
    state.WaitingOrNull.Reset();
    QFile file(runtimePackagePath);
    if (QFileInfo(runtimePackagePath).isFile() == false || QFileInfo(runtimePackagePath).size() > MAX_PACKAGE_BYTES ||
        file.open(QIODevice::ReadOnly) == false)
    {
        setStatus(QStringLiteral("No readable runtime generation at %1; cook the runtime first.")
                      .arg(QFileInfo(runtimePackagePath).fileName()));
        return RenderGraphPreviewLoad::Unreadable;
    }
    const QByteArray bytes = file.read(MAX_PACKAGE_BYTES + 1);
    if (bytes.size() > MAX_PACKAGE_BYTES)
    {
        setStatus(QStringLiteral("The runtime generation exceeds the 16 MiB runtime bound."));
        return RenderGraphPreviewLoad::Unreadable;
    }
    renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
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
    // The editor shows an exported color result; presentation surfaces belong to the game host.
    const renderer::RenderGraphPlan plan = package->GetPlan();
    State::Target target;
    bool hasTarget = false;
    for (usize i = 0; i < plan.Resources.GetSize(); ++i)
    {
        const format::Resource& resource = plan.Resources[i];
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
    QFile names(GetTargetNamesPath(runtimePackagePath));
    const QByteArray namesDocument =
        names.open(QIODevice::ReadOnly) == true ? names.read(MAX_PACKAGE_BYTES) : QByteArray();
    target.Catalogue = MakeTargetCatalogue(plan, package->GetGenerationIdentity(), namesDocument);
    state.Waiting = target;
    state.WaitingOrNull.Swap(package);
    if (state.IsAttached == false)
    {
        setStatus(QStringLiteral("Runtime generation decoded; start the World viewport to provide the preview "
                                 "device."));
        return RenderGraphPreviewLoad::Waiting;
    }
    return stage();
}

RenderGraphPreviewLoad RenderGraphPreview::stage()
{
    State& state = *mState;
    const rhi::RasterStatus staged = state.Activation.Stage(Move(state.WaitingOrNull));
    state.WaitingOrNull.Reset();
    const renderer::RenderGraphActivationReport report = state.Activation.GetReport();
    state.Candidate = state.Waiting;
    state.Candidate.Serial = report.CandidateGeneration;
    Q_EMIT Changed();
    if (report.Candidate == renderer::RenderGraphCandidateState::RestartRequired)
    {
        setStatus({});
        return RenderGraphPreviewLoad::RestartRequired;
    }
    if (IsAccepted(staged) == false)
    {
        setStatus({});
        return RenderGraphPreviewLoad::Failed;
    }
    setStatus({});
    return RenderGraphPreviewLoad::Staged;
}

void RenderGraphPreview::SetExtraction(const RenderGraphExtraction& extraction)
{
    mState->Extraction = extraction;
    mState->HasExtraction = extraction.Revision != 0;
}

QSize RenderGraphPreview::GetTargetSize() const noexcept
{
    const State& state = *mState;
    const State::Target& target = state.WaitingOrNull.Get() != nullptr ? state.Waiting
                                  : state.Candidate.Serial != 0        ? state.Candidate
                                                                       : state.Active;
    return {static_cast<int32>(target.Width), static_cast<int32>(target.Height)};
}

void RenderGraphPreview::Tick()
{
    State& state = *mState;
    if (state.IsAttached == false)
    {
        return;
    }
    if (state.Reader.IsBusy() == true)
    {
        finishCopy();
        return;
    }
    if (state.Inspector.IsBusy() == true)
    {
        finishInspection();
        return;
    }
    if (state.IsAwaitingProducer == true)
    {
        renderer::RenderGraph* activeOrNull = state.Activation.GetActiveOrNull();
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect = rhi::TextureAspect::Color;
        const rhi::RasterStatus exported = activeOrNull == nullptr
                                               ? rhi::RasterStatus::InvalidState
                                               : activeOrNull->GetExport(state.Active.Resource, texture, aspect);
        if (exported == rhi::RasterStatus::NotReady && ++state.ProducerTicks < MAX_PRODUCER_TICKS)
        {
            return;
        }
        state.IsAwaitingProducer = false;
        if (exported != rhi::RasterStatus::Ready)
        {
            setStatus(QStringLiteral("The preview frame did not complete (%1); the shown frame is kept.")
                          .arg(DescribeRasterStatus(exported)));
            return;
        }
        if (state.Reader.Begin(state.Device, texture, state.Active.Width, state.Active.Height) == false)
        {
            setStatus(QStringLiteral("The preview frame cannot be copied on this device; the shown frame is kept."));
            return;
        }
        finishCopy();
        return;
    }
    // No frame is open and no export copy is in flight: the active graph's attachments hold
    // the last completed frame until the next Render, so the viewer reads them now.
    if (state.IsInspectionRequested == true && beginInspection() == true)
    {
        return;
    }
    advance();
}

void RenderGraphPreview::finishCopy()
{
    State& state = *mState;
    const OffscreenReadStatus status = state.Reader.Poll();
    if (status == OffscreenReadStatus::Complete)
    {
        mImage = state.Reader.TakeImage();
        mPresentedGeneration = state.RenderedGeneration;
        mPresentedRevision = state.RenderedRevision;
        mPresentedSession = state.Session;
        setStatus({});
        // Live refresh: the viewed texture follows every presented frame.
        state.IsInspectionRequested = state.InspectedName.isEmpty() == false;
        Q_EMIT FramePresented();
    }
    else if (status == OffscreenReadStatus::Failed)
    {
        state.Reader.Cancel();
        setStatus(QStringLiteral("The preview frame copy failed; the shown frame is kept."));
    }
}

void RenderGraphPreview::advance()
{
    State& state = *mState;
    const renderer::RenderGraphActivationReport before = state.Activation.GetReport();
    // No export consumer is outstanding here, so Poll may activate and Reset the old generation.
    static_cast<void>(state.Activation.Poll());
    const renderer::RenderGraphActivationReport report = state.Activation.GetReport();
    if (report.ActiveGeneration != before.ActiveGeneration && report.ActiveGeneration == state.Candidate.Serial)
    {
        state.Active = state.Candidate;
        state.Candidate = {};
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
        const rhi::RasterStatus updated = state.Activation.Update(
            {state.Extraction.Meshes.constData(), static_cast<usize>(state.Extraction.Meshes.size())},
            {state.Extraction.Items.constData(), static_cast<usize>(state.Extraction.Items.size())},
            {cameras.constData(), static_cast<usize>(cameras.size())},
            state.Extraction.Revision,
            renderer::RenderGraphCoveragePolicy::DiagnosticColor,
            graphReport);
        // NotReady means no generation could accept it yet (first candidate still preparing): retry.
        if (updated != rhi::RasterStatus::NotReady)
        {
            state.UpdatedRevision = state.Extraction.Revision;
            if (IsAccepted(updated) == false)
            {
                setStatus(QStringLiteral("The World extraction was rejected (%1); the shown frame is kept.")
                              .arg(DescribeRasterStatus(updated)));
            }
            Q_EMIT Changed();
        }
    }
    renderer::RenderGraph* activeOrNull = state.Activation.GetActiveOrNull();
    if (activeOrNull == nullptr)
    {
        return;
    }
    const uint64 drawable = activeOrNull->GetDrawableSceneRevision();
    if (drawable == 0 || (mPresentedSession == state.Session && report.ActiveGeneration == mPresentedGeneration &&
                          drawable == mPresentedRevision))
    {
        return;
    }
    rhi::SubmissionToken completion;
    const rhi::RasterStatus rendered =
        activeOrNull->Render(state.Surface, state.Active.Width, state.Active.Height, completion);
    if (rendered == rhi::RasterStatus::NotReady)
    {
        return;
    }
    if (IsAccepted(rendered) == false)
    {
        setStatus(QStringLiteral("The active generation could not render (%1); the shown frame is kept.")
                      .arg(DescribeRasterStatus(rendered)));
        return;
    }
    state.RenderedGeneration = report.ActiveGeneration;
    state.RenderedRevision = drawable;
    state.IsAwaitingProducer = true;
    state.ProducerTicks = 0;
}

const RenderGraphTargetCatalogue& RenderGraphPreview::GetTargetCatalogue() const noexcept
{
    return mState->Active.Catalogue;
}

void RenderGraphPreview::SetInspectedTarget(const QString& name)
{
    State& state = *mState;
    // Reselecting reads again; a read already in flight for the old choice is dropped.
    state.Inspector.Cancel();
    state.InspectedName = name;
    state.IsInspectionRequested = name.isEmpty() == false;
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
    renderer::RenderGraph* activeOrNull = state.Activation.GetActiveOrNull();
    if (activeOrNull == nullptr || mPresentedSession != state.Session || mPresentedGeneration == 0)
    {
        return false;
    }
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
    const RenderTargetReadStatus started =
        state.Inspector.Begin(state.Device, state.Surface, texture, aspect, target.Width, target.Height);
    if (started == RenderTargetReadStatus::Pending)
    {
        return true;
    }
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
    State& state = *mState;
    const RenderTargetReadStatus status = state.Inspector.Poll();
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
