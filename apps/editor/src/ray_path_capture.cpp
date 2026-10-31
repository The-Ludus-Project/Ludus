#include "internal/ray_path_capture.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/lifetime.h>

#include "internal/render_graph_preview.h"

#include <new>

namespace ludus::editor
{
using namespace foundation;
namespace format = content::render_graph_format;
namespace renderer = graphics::renderer;
namespace rhi = graphics::rhi;
namespace
{
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
bool AreBytesEqual(BorrowedView<const uint8> actual, const QByteArray& expected) noexcept
{
    if (actual.GetSize() != static_cast<usize>(expected.size()))
    {
        return false;
    }
    for (usize i = 0; i < actual.GetSize(); ++i)
    {
        if (actual[i] != static_cast<uint8>(expected[static_cast<qsizetype>(i)]))
        {
            return false;
        }
    }
    return true;
}
} // namespace

int32 FindRayPathPass(const QStringList& passNames, const RayPathTarget& target)
{
    if (target.NodeId.isEmpty() == true)
    {
        return -1;
    }
    const QString expanded = target.GraphId + QLatin1Char('/') + target.NodeId;
    for (qsizetype i = 0; i < passNames.size(); ++i)
    {
        if (passNames.at(i) == expanded)
        {
            return static_cast<int32>(i);
        }
    }
    // A subgraph document opened on its own matches its first instance by the node's last ID segment.
    for (qsizetype i = 0; i < passNames.size(); ++i)
    {
        if (passNames.at(i).endsWith(QLatin1Char('/') + target.NodeId) == true)
        {
            return static_cast<int32>(i);
        }
    }
    return -1;
}

struct RayPathCapture::State final
{
    enum class Step : uint8
    {
        Initializing,
        Updating,
        Rendering,
        AwaitingProducer,
        Reading,
    };
    RayPathCaptureState CaptureState = RayPathCaptureState::Idle;
    Step CurrentStep = Step::Initializing;
    QString Message;
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    /// Owns the artifacts the graph was initialized from; declared first so the graph is destroyed first.
    UniquePtr<renderer::RenderGraphPackage> PackageOrNull;
    renderer::RenderGraph Graph;
    RenderGraphExtraction Extraction;
    RayPathFreeze Freeze;
    math::Vector2 Pixel{};
    int32 TargetPass = -1;
    uint32 SampleCount = renderer::DEFAULT_RAY_PATH_SAMPLES;
    uint32 RenderedSamples = 0;
    uint32 ExportResource = 0;
    uint32 PixelX = 0;
    uint32 PixelY = 0;
    QSize LaunchExtent;
    rhi::SubmissionToken Producer{};
    QVector<renderer::RayPathEvent> Events;
    QVector<renderer::RayPathSummary> Summaries;
    renderer::RayPathCaptureReport Report{};
    QStringList PassNames;

    void ReleaseGraph() noexcept
    {
        Graph.Reset();
        PackageOrNull.Reset();
        Producer = {};
    }
    void ClearResults()
    {
        Events.clear();
        Summaries.clear();
        Report = {};
        PassNames.clear();
        PixelX = 0;
        PixelY = 0;
        LaunchExtent = {};
        RenderedSamples = 0;
    }
};

RayPathCapture::RayPathCapture()
{
    mState.Reset(new (std::nothrow) State);
    LUDUS_REQUIRE(mState.Get() != nullptr);
}

RayPathCapture::~RayPathCapture()
{
    mState->ReleaseGraph();
}

void RayPathCapture::BeginBuild()
{
    Reset();
    mState->CaptureState = RayPathCaptureState::Building;
    mState->Message = QStringLiteral("Compiling the ray path capture variant of the frozen generation…");
}

void RayPathCapture::SetUnavailable(const QString& reason)
{
    Reset();
    mState->CaptureState = RayPathCaptureState::Unavailable;
    mState->Message = reason;
}

void RayPathCapture::Fail(const QString& reason)
{
    Reset();
    fail(reason);
}

void RayPathCapture::fail(const QString& reason) noexcept
{
    mState->ReleaseGraph();
    mState->CaptureState = RayPathCaptureState::Failed;
    mState->Message = reason;
}

bool RayPathCapture::Start(rhi::DeviceHandle device,
                           rhi::SurfaceHandle surface,
                           const QByteArray& package,
                           const RenderGraphExtraction& extraction,
                           const RayPathFreeze& freeze,
                           math::Vector2 pixel,
                           const RayPathTarget& target,
                           uint32 sampleCount,
                           const rhi::RasterShaderDescription& compositeVertex,
                           const rhi::RasterShaderDescription& compositeFragment)
{
    Reset();
    State& state = *mState;
    if (sampleCount == 0 || sampleCount > renderer::MAX_RAY_PATH_SAMPLES || freeze.ReferenceSize.isEmpty() == true ||
        math::IsFinite(pixel.X) == false || math::IsFinite(pixel.Y) == false)
    {
        fail(QStringLiteral("The ray path capture request is invalid."));
        return false;
    }
    state.PackageOrNull.Reset(new (std::nothrow) renderer::RenderGraphPackage);
    if (state.PackageOrNull.Get() == nullptr)
    {
        fail(QStringLiteral("The ray path capture package could not be allocated."));
        return false;
    }
    const renderer::RenderGraphPackageStatus decoded = state.PackageOrNull->DecodeRayPathCapture(
        {reinterpret_cast<const uint8*>(package.constData()), static_cast<usize>(package.size())});
    if (decoded != renderer::RenderGraphPackageStatus::Ready)
    {
        fail(QStringLiteral("The ray path capture package was rejected; rebuild the graph and capture again."));
        return false;
    }
    if (AreBytesEqual(state.PackageOrNull->GetInterfaceIdentity(), freeze.InterfaceIdentity) == false)
    {
        fail(QStringLiteral("The capture variant does not match the frozen generation's CPU interface."));
        return false;
    }
    const renderer::RenderGraphPlan plan = state.PackageOrNull->GetPlan();
    if (renderer::UsesRayTracing(plan) == false)
    {
        SetUnavailable(QStringLiteral("Ray path capture needs a ray pass; this generation traces no rays."));
        return false;
    }
    if (renderer::RequiresDenoising(plan) == true && renderer::IsDenoisingAvailable() == false)
    {
        state.PackageOrNull->BypassDenoising();
    }
    bool hasExport = false;
    for (uint32 resource = 0; hasExport == false && resource < plan.Resources.GetSize(); ++resource)
    {
        if (plan.Resources[resource].IsExported == 1 && plan.Resources[resource].Kind == format::Aspect::Color)
        {
            state.ExportResource = resource;
            hasExport = true;
        }
    }
    if (hasExport == false)
    {
        fail(QStringLiteral("The capture variant exports no color result."));
        return false;
    }
    for (usize pass = 0; pass < plan.Passes.GetSize(); ++pass)
    {
        state.PassNames.append(pass < plan.PassNames.GetSize()
                                   ? QString::fromUtf8(plan.PassNames[pass].GetDataOrNull(),
                                                       static_cast<qsizetype>(plan.PassNames[pass].GetSize()))
                                   : QString{});
    }
    const int32 targetPass = FindRayPathPass(state.PassNames, target);
    if (target.NodeId.isEmpty() == false &&
        (targetPass < 0 || plan.Passes[static_cast<usize>(targetPass)].Program != format::Technique::RayTracing))
    {
        fail(QStringLiteral("%1 traces no ray pass in this configuration; capture its configuration or the whole "
                            "graph.")
                 .arg(target.NodeId));
        return false;
    }
    const renderer::RenderGraphPrograms programs =
        state.PackageOrNull->GetPrograms(compositeVertex, compositeFragment, true);
    const rhi::RasterStatus initialized = state.Graph.Initialize(device,
                                                                 plan,
                                                                 programs,
                                                                 nullptr,
                                                                 static_cast<uint32>(freeze.ReferenceSize.width()),
                                                                 static_cast<uint32>(freeze.ReferenceSize.height()));
    if (initialized == rhi::RasterStatus::Unsupported)
    {
        SetUnavailable(renderer::RequiresHardwareRayTracing(plan) == true
                           ? QStringLiteral("This device cannot run the capture variant's native ray queries.")
                           : QStringLiteral("This device cannot run the capture variant's compute traversal."));
        return false;
    }
    if (IsAccepted(initialized) == false)
    {
        fail(QStringLiteral("The capture graph could not be prepared (%1).").arg(DescribeRasterStatus(initialized)));
        return false;
    }
    state.Device = device;
    state.Surface = surface;
    state.Extraction = extraction;
    state.Freeze = freeze;
    state.Pixel = pixel;
    state.TargetPass = targetPass;
    state.SampleCount = sampleCount;
    state.CaptureState = RayPathCaptureState::Preparing;
    state.CurrentStep = State::Step::Initializing;
    state.Message = QStringLiteral("Preparing the capture variant's pipelines…");
    return true;
}

bool RayPathCapture::Advance() noexcept
{
    if (HasPendingWork() == false)
    {
        return false;
    }
    rhi::DeviceInfo device;
    if (rhi::GetDeviceInfo(mState->Device, device) != rhi::DeviceStatus::Ready)
    {
        fail(QStringLiteral("The viewport device is unavailable; the capture stopped."));
        return true;
    }
    if (device.FrameOpen == true)
    {
        return false;
    }
    switch (mState->CaptureState)
    {
        case RayPathCaptureState::Preparing:
            return prepare();
        case RayPathCaptureState::Tracing:
            return trace();
        case RayPathCaptureState::Reading:
            return read();
        case RayPathCaptureState::Idle:
        case RayPathCaptureState::Building:
        case RayPathCaptureState::Ready:
        case RayPathCaptureState::Failed:
        case RayPathCaptureState::Unavailable:
            break;
    }
    return false;
}

bool RayPathCapture::prepare() noexcept
{
    State& state = *mState;
    const rhi::RasterStatus polled = state.Graph.Poll();
    if (polled == rhi::RasterStatus::Pending)
    {
        return false;
    }
    if (polled != rhi::RasterStatus::Ready)
    {
        fail(QStringLiteral("The capture graph failed to prepare (%1).").arg(DescribeRasterStatus(polled)));
        return true;
    }
    if (state.CurrentStep == State::Step::Updating)
    {
        if (state.Graph.GetDrawableSceneRevision() == 0)
        {
            return false;
        }
        state.CaptureState = RayPathCaptureState::Tracing;
        state.CurrentStep = State::Step::Rendering;
        state.Message = QStringLiteral("Tracing %1 samples from the frozen camera…").arg(state.SampleCount);
        return true;
    }
    // Pipelines are Ready: choose the launch pixel in the traced pass's color extent.
    const renderer::RenderGraphPlan plan = state.PackageOrNull->GetPlan();
    int32 pass = -1;
    if (state.TargetPass >= 0 && static_cast<usize>(state.TargetPass) < plan.Passes.GetSize() &&
        plan.Passes[static_cast<usize>(state.TargetPass)].Program == format::Technique::RayTracing)
    {
        pass = state.TargetPass;
    }
    for (usize i = 0; pass < 0 && i < plan.Passes.GetSize(); ++i)
    {
        pass = plan.Passes[i].Program == format::Technique::RayTracing ? static_cast<int32>(i) : -1;
    }
    format::Resource color{};
    if (pass < 0 || state.Graph.GetResourceDescription(plan.Passes[static_cast<usize>(pass)].Color, color) == false ||
        color.Width == 0 || color.Height == 0)
    {
        fail(QStringLiteral("The capture graph has no ray pass color extent to pick a pixel in."));
        return true;
    }
    state.LaunchExtent = {static_cast<int32>(color.Width), static_cast<int32>(color.Height)};
    state.PixelX =
        math::Min(static_cast<uint32>(math::Clamp(state.Pixel.X, 0.0F, 1.0F) * static_cast<float32>(color.Width)),
                  color.Width - 1);
    state.PixelY =
        math::Min(static_cast<uint32>(math::Clamp(state.Pixel.Y, 0.0F, 1.0F) * static_cast<float32>(color.Height)),
                  color.Height - 1);
    renderer::RayPathCaptureDescription description;
    description.PixelX = state.PixelX;
    description.PixelY = state.PixelY;
    description.SampleCount = state.SampleCount;
    const rhi::RasterStatus requested = state.Graph.RequestRayPathCapture(description);
    if (requested == rhi::RasterStatus::Unsupported)
    {
        state.ReleaseGraph();
        state.CaptureState = RayPathCaptureState::Unavailable;
        state.Message = QStringLiteral("Two ray passes of this configuration share one callback set, so their rays "
                                       "cannot be told apart.");
        return true;
    }
    if (IsAccepted(requested) == false)
    {
        fail(QStringLiteral("The capture buffer was rejected (%1).").arg(DescribeRasterStatus(requested)));
        return true;
    }
    const RenderGraphExtraction& extraction = state.Extraction;
    QVector<renderer::ViewDescription> cameras(static_cast<qsizetype>(plan.Description.CameraCount), state.Freeze.View);
    renderer::SceneShaderInputs inputs = extraction.ShaderInputs;
    inputs.Lights = {extraction.Scene.AuthoredLights.Lights, extraction.Scene.AuthoredLights.Count};
    renderer::RenderGraphReport report;
    const rhi::RasterStatus updated =
        state.Graph.Update({extraction.Meshes.constData(), static_cast<usize>(extraction.Meshes.size())},
                           {extraction.Items.constData(), static_cast<usize>(extraction.Items.size())},
                           {cameras.constData(), static_cast<usize>(cameras.size())},
                           state.Freeze.SceneRevision,
                           renderer::RenderGraphCoveragePolicy::DiagnosticColor,
                           report,
                           &inputs);
    if (IsAccepted(updated) == false)
    {
        fail(DescribeRenderGraphExtractionFailure(updated, report.SceneDiagnostics));
        return true;
    }
    state.CurrentStep = State::Step::Updating;
    state.Message = QStringLiteral("Preparing the frozen World for the capture…");
    return true;
}

bool RayPathCapture::trace() noexcept
{
    State& state = *mState;
    if (state.CurrentStep == State::Step::AwaitingProducer)
    {
        const rhi::RasterStatus completed = rhi::GetStatus(state.Device, state.Producer);
        if (completed == rhi::RasterStatus::Pending)
        {
            return false;
        }
        state.Producer = {};
        if (completed != rhi::RasterStatus::Ready)
        {
            fail(QStringLiteral("A capture sample did not complete (%1).").arg(DescribeRasterStatus(completed)));
            return true;
        }
        if (state.RenderedSamples < state.SampleCount && state.Graph.HasPendingSamples() == true)
        {
            state.CurrentStep = State::Step::Rendering;
            return false;
        }
        state.CaptureState = RayPathCaptureState::Reading;
        state.CurrentStep = State::Step::Reading;
        state.Message = QStringLiteral("Reading the captured rays back…");
        return true;
    }
    format::Resource output{};
    if (state.Graph.GetResourceDescription(state.ExportResource, output) == false)
    {
        fail(QStringLiteral("The capture graph lost its exported color result."));
        return true;
    }
    rhi::SubmissionToken completion;
    const rhi::RasterStatus rendered = state.Graph.Render(state.Surface, output.Width, output.Height, completion);
    if (rendered == rhi::RasterStatus::NotReady)
    {
        return false;
    }
    if (IsAccepted(rendered) == false)
    {
        fail(QStringLiteral("A capture sample was rejected (%1).").arg(DescribeRasterStatus(rendered)));
        return true;
    }
    state.Producer = completion;
    ++state.RenderedSamples;
    state.CurrentStep = State::Step::AwaitingProducer;
    state.Message = QStringLiteral("Tracing sample %1 of %2…").arg(state.RenderedSamples).arg(state.SampleCount);
    return true;
}

bool RayPathCapture::read() noexcept
{
    State& state = *mState;
    renderer::RayPathCaptureReport report;
    const rhi::RasterStatus polled = state.Graph.PollRayPathCapture(report);
    if (polled == rhi::RasterStatus::Pending)
    {
        return false;
    }
    if (polled != rhi::RasterStatus::Ready)
    {
        fail(QStringLiteral("The capture readback failed (%1).").arg(DescribeRasterStatus(polled)));
        return true;
    }
    const BorrowedView<const renderer::RayPathEvent> events = state.Graph.GetRayPathEvents();
    const BorrowedView<const renderer::RayPathSummary> summaries = state.Graph.GetRayPathSummaries();
    state.Events.reserve(static_cast<qsizetype>(events.GetSize()));
    for (usize i = 0; i < events.GetSize(); ++i)
    {
        state.Events.append(events[i]);
    }
    state.Summaries.reserve(static_cast<qsizetype>(summaries.GetSize()));
    for (usize i = 0; i < summaries.GetSize(); ++i)
    {
        state.Summaries.append(summaries[i]);
    }
    state.Report = report;
    // The decoded copy is all inspection needs; return the diagnostic graph's GPU memory now.
    state.ReleaseGraph();
    state.CaptureState = RayPathCaptureState::Ready;
    state.Message.clear();
    return true;
}

bool RayPathCapture::HasPendingWork() const noexcept
{
    const RayPathCaptureState captureState = mState->CaptureState;
    return captureState == RayPathCaptureState::Preparing || captureState == RayPathCaptureState::Tracing ||
           captureState == RayPathCaptureState::Reading;
}

void RayPathCapture::Release(const QString& reason) noexcept
{
    if (HasPendingWork() == true)
    {
        fail(reason);
        return;
    }
    mState->ReleaseGraph();
}

void RayPathCapture::Reset() noexcept
{
    State& state = *mState;
    state.ReleaseGraph();
    state.ClearResults();
    state.Extraction = {};
    state.Freeze = {};
    state.Device = {};
    state.Surface = {};
    state.TargetPass = -1;
    state.CaptureState = RayPathCaptureState::Idle;
    state.Message.clear();
}

RayPathCaptureState RayPathCapture::GetState() const noexcept
{
    return mState->CaptureState;
}
const QString& RayPathCapture::GetMessage() const noexcept
{
    return mState->Message;
}
const QVector<renderer::RayPathEvent>& RayPathCapture::GetEvents() const noexcept
{
    return mState->Events;
}
const QVector<renderer::RayPathSummary>& RayPathCapture::GetSummaries() const noexcept
{
    return mState->Summaries;
}
const renderer::RayPathCaptureReport& RayPathCapture::GetReport() const noexcept
{
    return mState->Report;
}
const QStringList& RayPathCapture::GetPassNames() const noexcept
{
    return mState->PassNames;
}
int32 RayPathCapture::GetTargetPass() const noexcept
{
    return mState->TargetPass;
}
uint32 RayPathCapture::GetPixelX() const noexcept
{
    return mState->PixelX;
}
uint32 RayPathCapture::GetPixelY() const noexcept
{
    return mState->PixelY;
}
QSize RayPathCapture::GetLaunchExtent() const noexcept
{
    return mState->LaunchExtent;
}
uint32 RayPathCapture::GetSampleCount() const noexcept
{
    return mState->SampleCount;
}
uint32 RayPathCapture::GetRenderedSamples() const noexcept
{
    return mState->RenderedSamples;
}
QString RayPathCapture::GetObjectId(uint64 sourceId) const
{
    // MakeRenderGraphExtraction numbers items by World object index, starting at one.
    const QVector<PreviewObject>& objects = mState->Extraction.Scene.Objects;
    if (sourceId == 0 || sourceId > static_cast<uint64>(objects.size()))
    {
        return {};
    }
    return objects.at(static_cast<qsizetype>(sourceId - 1)).Id;
}
} // namespace ludus::editor
