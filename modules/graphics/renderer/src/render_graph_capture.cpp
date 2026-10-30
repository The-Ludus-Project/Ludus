#include <ludus/graphics/renderer/render_graph_capture.hpp>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/core.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <cstring>
#include <initializer_list>
#include <new>
#include <span>

// Design and acceptance: docs/architecture/render-graph-frame-capture.md. The read-back follows the
// accumulation layout of renderer_gpu_preview.slang: a compensated sum with the committed count's bits in W,
// then the compensation with an even history tag in W that becomes odd when the pixel latches a failure.
namespace ludus::graphics::renderer
{
namespace
{
namespace format = content::render_graph_format;
namespace math = foundation::math;
using foundation::float32;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;
using Status = rhi::RasterStatus;

/// Bytes of one pixel's accumulation record.
constexpr usize ACCUMULATION_PIXEL_BYTES = usize{RENDER_GRAPH_ACCUMULATION_WORDS} * sizeof(math::Vector4);
/// Read-back slice used when the device reports no transfer budget.
constexpr usize FALLBACK_TRANSFER_BYTES = usize{64} * 1024;

/// The captured route: the output resource and the ray pass whose Accumulate feeds it.
struct CaptureRoute final
{
    uint32 Output = format::ABSENT;
    uint32 Pass = format::ABSENT;
    RenderGraphCaptureProblem Problem = RenderGraphCaptureProblem::None;
};

/// Resolve settings.Output (or the only exported colour resource) back through processing passes, such as
/// a ToneMap that reads SourceColor, to the ray pass whose Accumulate writes the linear mean.
[[nodiscard]] CaptureRoute ResolveRoute(const RenderGraphPlan& plan, uint32 requestedOutput) noexcept
{
    CaptureRoute route;
    uint32 output = requestedOutput;
    if (output == format::ABSENT)
    {
        for (uint32 resource = 0; resource < plan.Resources.GetSize(); ++resource)
        {
            const format::Resource& candidate = plan.Resources[resource];
            if (candidate.IsExported == 1 && candidate.Kind == format::Aspect::Color)
            {
                if (output != format::ABSENT)
                {
                    route.Problem = RenderGraphCaptureProblem::NoOutput;
                    return route;
                }
                output = resource;
            }
        }
    }
    // A surface-backed output would present the capture's frames; captures read offscreen exports only.
    if (output >= plan.Resources.GetSize() || plan.Resources[output].Kind != format::Aspect::Color ||
        plan.Resources[output].IsSurface == 1)
    {
        route.Problem = RenderGraphCaptureProblem::NoOutput;
        return route;
    }
    route.Output = output;
    uint32 resource = output;
    usize end = plan.Passes.GetSize();
    while (end > 0)
    {
        usize writer = end;
        for (usize pass = end; pass > 0; --pass)
        {
            if (plan.Passes[pass - 1].Color == resource)
            {
                writer = pass - 1;
                break;
            }
        }
        if (writer == end)
        {
            break;
        }
        const format::Pass& operation = plan.Passes[writer];
        if (operation.Program == format::Technique::RayTracing)
        {
            if (operation.AccumulationSamples == 0)
            {
                route.Problem = RenderGraphCaptureProblem::NoAccumulation;
                return route;
            }
            route.Pass = static_cast<uint32>(writer);
            return route;
        }
        if (operation.SourceColor == format::ABSENT)
        {
            route.Problem = RenderGraphCaptureProblem::RasterRoute;
            return route;
        }
        resource = operation.SourceColor;
        end = writer;
    }
    route.Problem = RenderGraphCaptureProblem::NoOutput;
    return route;
}

void WriteUint32(std::span<uint8> bytes, usize offset, uint32 value) noexcept
{
    static_cast<void>(foundation::TryWriteLittleEndian(value, bytes.subspan(offset, sizeof(uint32))));
}
void WriteUint64(std::span<uint8> bytes, usize offset, uint64 value) noexcept
{
    static_cast<void>(foundation::TryWriteLittleEndian(value, bytes.subspan(offset, sizeof(uint64))));
}
/// Write consecutive float32 values from offset and return the offset after them.
[[nodiscard]] usize WriteFloats(std::span<uint8> bytes, usize offset, std::initializer_list<float32> values) noexcept
{
    for (const float32 value : values)
    {
        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        WriteUint32(bytes, offset, bits);
        offset += sizeof(float32);
    }
    return offset;
}
} // namespace

struct RenderGraphCapture::State final
{
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    RenderGraph Graph;
    RenderGraphCaptureSettings Settings{};
    RenderGraphCaptureInfo Info{};
    RenderGraphCaptureProblem Problem = RenderGraphCaptureProblem::None;
    Status FailureStatus = Status::Ready;
    uint32 ReferenceWidth = 0;
    uint32 ReferenceHeight = 0;
    /// Camera of the accumulation pass, recorded in the file.
    ViewDescription Camera{};
    uint32 CameraIndex = 0;
    rhi::SubmissionToken Producer{};
    rhi::ReadbackTicket Readback{};
    rhi::BufferHandle Accumulation{};
    bool HasReadback = false;
    bool ShouldStop = false;
    /// Whether the read-back in progress ends the capture rather than refreshing its progress.
    bool IsFinalReadback = false;
    usize ReadOffset = 0;
    usize SliceBytes = 0;
    foundation::Array<math::Vector4> Records;
    foundation::Array<RenderGraphCapturePixel> Pixels;

    void Fail(RenderGraphCaptureProblem problem, Status status) noexcept
    {
        Problem = problem;
        FailureStatus = status;
        Info.Phase = RenderGraphCapturePhase::Failed;
    }
    void ReleaseReadback() noexcept
    {
        if (HasReadback == true)
        {
            static_cast<void>(rhi::Release(Device, Readback));
            HasReadback = false;
        }
    }
    /// Whether the last submitted frame finished; a failed frame fails the capture.
    [[nodiscard]] bool IsProducerIdle() noexcept
    {
        if (Producer.IsNull() == true)
        {
            return true;
        }
        const Status status = rhi::GetStatus(Device, Producer);
        if (status == Status::Pending)
        {
            return false;
        }
        Producer = {};
        if (status != Status::Ready)
        {
            Fail(RenderGraphCaptureProblem::GraphFailure, status);
            return false;
        }
        return true;
    }
    void BeginReadback(bool isFinal) noexcept
    {
        IsFinalReadback = isFinal;
        const usize pixelCount = usize{Info.Width} * Info.Height;
        const Status borrowed = Graph.GetAccumulation(Info.AccumulationPass, Accumulation);
        if (borrowed != Status::Ready)
        {
            Fail(RenderGraphCaptureProblem::ReadbackFailure, borrowed);
            return;
        }
        if (Records.TryResize(pixelCount * RENDER_GRAPH_ACCUMULATION_WORDS) == false)
        {
            Fail(RenderGraphCaptureProblem::ReadbackFailure, Status::OutOfMemory);
            return;
        }
        rhi::LifetimeCapabilities capabilities;
        const usize transferBytes =
            rhi::GetLifetimeCapabilities(Device, capabilities) == Status::Ready && capabilities.TransferBytes != 0
                ? capabilities.TransferBytes
                : FALLBACK_TRANSFER_BYTES;
        // Whole pixel records per slice, so every slice offset stays a multiple of four.
        SliceBytes = math::Max(transferBytes / ACCUMULATION_PIXEL_BYTES, usize{1}) * ACCUMULATION_PIXEL_BYTES;
        ReadOffset = 0;
        Info.Phase = RenderGraphCapturePhase::ReadingBack;
    }
    void ServiceReadback() noexcept
    {
        const usize totalBytes = Records.GetSize() * sizeof(math::Vector4);
        if (HasReadback == false)
        {
            const usize bytes = math::Min(SliceBytes, totalBytes - ReadOffset);
            const Status requested = rhi::RequestBufferReadback(Device, Accumulation, ReadOffset, bytes, Readback);
            if (requested != Status::Pending && requested != Status::Ready)
            {
                Fail(RenderGraphCaptureProblem::ReadbackFailure, requested);
                return;
            }
            HasReadback = true;
            return;
        }
        const Status status = rhi::GetStatus(Device, Readback);
        if (status == Status::Pending)
        {
            return;
        }
        const usize bytes = math::Min(SliceBytes, totalBytes - ReadOffset);
        const Status copied =
            status == Status::Ready
                ? rhi::CopyReadback(Device, Readback, reinterpret_cast<uint8*>(Records.GetData()) + ReadOffset, bytes)
                : status;
        ReleaseReadback();
        if (copied != Status::Ready)
        {
            Fail(RenderGraphCaptureProblem::ReadbackFailure, copied);
            return;
        }
        ReadOffset += bytes;
        if (ReadOffset == totalBytes)
        {
            Decode();
        }
    }
    void Decode() noexcept
    {
        const usize pixelCount = usize{Info.Width} * Info.Height;
        if (Pixels.TryResize(pixelCount) == false)
        {
            Fail(RenderGraphCaptureProblem::ReadbackFailure, Status::OutOfMemory);
            return;
        }
        uint32 failed = 0;
        uint32 minimum = Info.SubmittedSamples;
        for (usize pixel = 0; pixel < pixelCount; ++pixel)
        {
            const math::Vector4& sum = Records[pixel * RENDER_GRAPH_ACCUMULATION_WORDS];
            const math::Vector4& compensation = Records[pixel * RENDER_GRAPH_ACCUMULATION_WORDS + 1];
            uint32 count = 0;
            std::memcpy(&count, &sum.W, sizeof(count));
            // The history tag is an even integer; adding one marks a latched failure.
            const bool hasFailed = static_cast<uint32>(compensation.W) % 2 == 1;
            failed += hasFailed == true ? 1 : 0;
            minimum = math::Min(minimum, count);
            const float32 scale = count != 0 ? 1.0F / static_cast<float32>(count) : 0.0F;
            Pixels[pixel] = {math::Vector3{sum.X * scale, sum.Y * scale, sum.Z * scale}, count};
        }
        Records.Clear();
        Info.FailedPixels = failed;
        Info.MinimumSamples = minimum;
        Info.ReadbackSamples = Info.SubmittedSamples;
        // A latched pixel never recovers, so a progress read-back that finds one ends the capture too.
        if (failed != 0)
        {
            Fail(RenderGraphCaptureProblem::LatchedPixels, Status::Ready);
            return;
        }
        if (IsFinalReadback == false)
        {
            Info.Phase = RenderGraphCapturePhase::Rendering;
            return;
        }
        Info.Phase = ShouldStop == true && Info.SubmittedSamples < Info.TargetSamples
                         ? RenderGraphCapturePhase::Stopped
                         : RenderGraphCapturePhase::Complete;
    }
};

RenderGraphCapture::RenderGraphCapture() noexcept = default;
RenderGraphCapture::~RenderGraphCapture() noexcept
{
    Release();
}

Status RenderGraphCapture::Start(rhi::DeviceHandle device,
                                 rhi::SurfaceHandle surface,
                                 const RenderGraphPlan& plan,
                                 const RenderGraphPrograms& programs,
                                 uint32 referenceWidth,
                                 uint32 referenceHeight,
                                 const RenderGraphCaptureSettings& settings) noexcept
{
    Release();
    State* allocationOrNull = new (std::nothrow) State;
    foundation::core::UniquePtr<State> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    mState = foundation::Move(candidate);
    State& state = *mState;
    state.Device = device;
    state.Surface = surface;
    state.Settings = settings;
    state.ReferenceWidth = referenceWidth;
    state.ReferenceHeight = referenceHeight;
    state.Info.TargetSamples = settings.TargetSamples;
    if (settings.TargetSamples < MIN_RENDER_GRAPH_CAPTURE_SAMPLES ||
        settings.TargetSamples > MAX_RENDER_GRAPH_CAPTURE_SAMPLES || referenceWidth == 0 || referenceHeight == 0)
    {
        state.Fail(RenderGraphCaptureProblem::InvalidSettings, Status::InvalidDescription);
        return Status::InvalidDescription;
    }
    const CaptureRoute route = ResolveRoute(plan, settings.Output);
    if (route.Problem != RenderGraphCaptureProblem::None)
    {
        state.Fail(route.Problem, Status::InvalidDescription);
        return Status::InvalidDescription;
    }
    const format::Pass& operation = plan.Passes[route.Pass];
    format::Resource resolved{};
    const Status extent =
        ResolveRenderGraphResource(plan.Resources[operation.Color], referenceWidth, referenceHeight, resolved);
    if (extent != Status::Ready)
    {
        state.Fail(RenderGraphCaptureProblem::InvalidSettings, extent);
        return extent;
    }
    state.Info.Output = route.Output;
    state.Info.AccumulationPass = route.Pass;
    state.Info.AuthoredLimit = operation.AccumulationSamples;
    state.CameraIndex = operation.Camera;
    state.Info.Width = resolved.Width;
    state.Info.Height = resolved.Height;
    const Status initialized = state.Graph.Initialize(device, plan, programs, nullptr, referenceWidth, referenceHeight);
    if (initialized != Status::Ready && initialized != Status::Pending)
    {
        state.Fail(RenderGraphCaptureProblem::GraphFailure, initialized);
        return initialized;
    }
    state.Info.Phase = RenderGraphCapturePhase::Initializing;
    return initialized;
}

Status RenderGraphCapture::SubmitScene(foundation::BorrowedView<const RenderGraphMesh> meshes,
                                       foundation::BorrowedView<const RenderGraphItem> items,
                                       foundation::BorrowedView<const ViewDescription> cameras,
                                       uint64 sceneRevision,
                                       const SceneShaderInputs* sceneInputsOrNull) noexcept
{
    if (mState.Get() == nullptr || mState->Info.Phase != RenderGraphCapturePhase::AwaitingScene)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    if (sceneRevision == 0 || state.CameraIndex >= cameras.GetSize())
    {
        state.Fail(RenderGraphCaptureProblem::InvalidSettings, Status::InvalidDescription);
        return Status::InvalidDescription;
    }
    RenderGraphReport report;
    const Status updated = state.Graph.Update(meshes,
                                              items,
                                              cameras,
                                              sceneRevision,
                                              RenderGraphCoveragePolicy::RejectIncomplete,
                                              report,
                                              sceneInputsOrNull,
                                              sceneRevision);
    if (updated != Status::Ready && updated != Status::Pending)
    {
        state.Fail(RenderGraphCaptureProblem::GraphFailure, updated);
        return updated;
    }
    state.Info.SceneRevision = sceneRevision;
    state.Camera = cameras[state.CameraIndex];
    state.Info.Phase =
        updated == Status::Ready ? RenderGraphCapturePhase::Rendering : RenderGraphCapturePhase::PreparingScene;
    return updated;
}

RenderGraphCapturePhase RenderGraphCapture::Service() noexcept
{
    if (mState.Get() == nullptr)
    {
        return RenderGraphCapturePhase::Idle;
    }
    State& state = *mState;
    switch (state.Info.Phase)
    {
        case RenderGraphCapturePhase::Initializing: {
            const Status polled = state.Graph.Poll();
            if (polled == Status::Pending)
            {
                break;
            }
            if (polled != Status::Ready)
            {
                state.Fail(RenderGraphCaptureProblem::GraphFailure, polled);
                break;
            }
            // The override makes every accumulating pass stop exactly at the target, above or below the
            // authored limit, so the submitted frame count is the committed sample count.
            const Status overridden = state.Graph.SetAccumulationLimitOverride(state.Info.TargetSamples);
            if (overridden != Status::Ready)
            {
                state.Fail(RenderGraphCaptureProblem::GraphFailure, overridden);
                break;
            }
            state.Info.Phase = RenderGraphCapturePhase::AwaitingScene;
            break;
        }
        case RenderGraphCapturePhase::PreparingScene: {
            const Status polled = state.Graph.Poll();
            if (polled == Status::Ready)
            {
                state.Info.Phase = RenderGraphCapturePhase::Rendering;
            }
            else if (polled != Status::Pending)
            {
                state.Fail(RenderGraphCaptureProblem::GraphFailure, polled);
            }
            break;
        }
        case RenderGraphCapturePhase::Rendering: {
            if (state.IsProducerIdle() == false)
            {
                break;
            }
            const uint32 submitted = state.Info.SubmittedSamples;
            const bool isFinished = state.ShouldStop == true || submitted >= state.Info.TargetSamples;
            const uint32 interval = state.Settings.PreviewInterval;
            const bool isPreviewDue = interval != 0 && submitted != 0 && submitted != state.Info.ReadbackSamples &&
                                      (submitted == 1 || submitted % interval == 0);
            if (isFinished == true || isPreviewDue == true)
            {
                state.BeginReadback(isFinished);
                break;
            }
            rhi::SubmissionToken completion;
            const Status rendered =
                state.Graph.Render(state.Surface, state.ReferenceWidth, state.ReferenceHeight, completion);
            if (rendered == Status::NotReady)
            {
                break;
            }
            if (rendered != Status::Ready)
            {
                state.Fail(RenderGraphCaptureProblem::GraphFailure, rendered);
                break;
            }
            state.Producer = completion;
            ++state.Info.SubmittedSamples;
            break;
        }
        case RenderGraphCapturePhase::ReadingBack:
            state.ServiceReadback();
            break;
        default:
            break;
    }
    return state.Info.Phase;
}

void RenderGraphCapture::Stop() noexcept
{
    if (mState.Get() != nullptr)
    {
        mState->ShouldStop = true;
    }
}

void RenderGraphCapture::Release() noexcept
{
    if (mState.Get() == nullptr)
    {
        return;
    }
    mState->ReleaseReadback();
    mState->Graph.Reset();
    mState.Reset();
}

RenderGraphCapturePhase RenderGraphCapture::GetPhase() const noexcept
{
    return mState.Get() != nullptr ? mState->Info.Phase : RenderGraphCapturePhase::Idle;
}
RenderGraphCaptureProblem RenderGraphCapture::GetProblem() const noexcept
{
    return mState.Get() != nullptr ? mState->Problem : RenderGraphCaptureProblem::None;
}
Status RenderGraphCapture::GetStatus() const noexcept
{
    return mState.Get() != nullptr ? mState->FailureStatus : Status::Ready;
}
RenderGraphCaptureInfo RenderGraphCapture::GetInfo() const noexcept
{
    return mState.Get() != nullptr ? mState->Info : RenderGraphCaptureInfo{};
}
foundation::BorrowedView<const RenderGraphCapturePixel> RenderGraphCapture::GetPixels() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return {};
    }
    return {mState->Pixels.GetData(), mState->Pixels.GetSize()};
}

Status RenderGraphCapture::Encode(foundation::Array<uint8>& outBytes) const noexcept
{
    if (mState.Get() == nullptr || mState->Pixels.IsEmpty() == true)
    {
        return Status::NotReady;
    }
    const State& state = *mState;
    const usize pixelCount = state.Pixels.GetSize();
    const usize size = format::CAPTURE_HEADER_BYTES + pixelCount * format::CAPTURE_PIXEL_BYTES;
    foundation::Array<uint8> bytes;
    if (bytes.TryResize(size, 0) == false)
    {
        return Status::OutOfMemory;
    }
    const std::span<uint8> file{bytes.GetData(), bytes.GetSize()};
    std::memcpy(file.data(), format::CAPTURE_MAGIC, format::CAPTURE_MAGIC_BYTES);
    uint32 flags = 0;
    flags |=
        state.Info.Phase == RenderGraphCapturePhase::Stopped ? static_cast<uint32>(format::CaptureFlag::Partial) : 0U;
    flags |= state.Info.FailedPixels != 0 ? static_cast<uint32>(format::CaptureFlag::Failed) : 0U;
    flags |= state.Info.TargetSamples != state.Info.AuthoredLimit
                 ? static_cast<uint32>(format::CaptureFlag::LimitOverride)
                 : 0U;
    flags |= state.Camera.OrthographicCamera == true ? static_cast<uint32>(format::CaptureFlag::Orthographic) : 0U;
    flags |=
        state.Camera.Depth == DepthConvention::ReverseZ ? static_cast<uint32>(format::CaptureFlag::ReverseDepth) : 0U;
    flags |= state.Camera.InfiniteFar == true ? static_cast<uint32>(format::CaptureFlag::InfiniteFar) : 0U;
    WriteUint32(file, format::CAPTURE_VERSION_OFFSET, format::CAPTURE_VERSION);
    WriteUint32(file, format::CAPTURE_HEADER_SIZE_OFFSET, format::CAPTURE_HEADER_BYTES);
    WriteUint32(file, format::CAPTURE_PIXEL_SIZE_OFFSET, format::CAPTURE_PIXEL_BYTES);
    WriteUint32(file, format::CAPTURE_FLAGS_OFFSET, flags);
    WriteUint32(file, format::CAPTURE_WIDTH_OFFSET, state.Info.Width);
    WriteUint32(file, format::CAPTURE_HEIGHT_OFFSET, state.Info.Height);
    WriteUint32(file, format::CAPTURE_TARGET_SAMPLES_OFFSET, state.Info.TargetSamples);
    WriteUint32(file, format::CAPTURE_SUBMITTED_SAMPLES_OFFSET, state.Info.SubmittedSamples);
    WriteUint32(file, format::CAPTURE_FAILED_PIXELS_OFFSET, state.Info.FailedPixels);
    WriteUint32(file, format::CAPTURE_OUTPUT_RESOURCE_OFFSET, state.Info.Output);
    WriteUint32(file, format::CAPTURE_ACCUMULATION_PASS_OFFSET, state.Info.AccumulationPass);
    WriteUint32(file, format::CAPTURE_AUTHORED_LIMIT_OFFSET, state.Info.AuthoredLimit);
    WriteUint64(file, format::CAPTURE_SCENE_REVISION_OFFSET, state.Info.SceneRevision);
    std::memcpy(file.data() + format::CAPTURE_PACKAGE_IDENTITY_OFFSET,
                state.Settings.PackageIdentity.GetData(),
                format::CAPTURE_IDENTITY_BYTES);
    std::memcpy(file.data() + format::CAPTURE_SCENE_IDENTITY_OFFSET,
                state.Settings.SceneIdentity.GetData(),
                format::CAPTURE_IDENTITY_BYTES);
    usize offset = format::CAPTURE_WORLD_TO_CLIP_OFFSET;
    for (const math::Vector4& values : state.Camera.WorldToClip.Columns)
    {
        offset = WriteFloats(file, offset, {values.X, values.Y, values.Z, values.W});
    }
    const math::Vector3& eye = state.Camera.CameraPosition;
    static_cast<void>(WriteFloats(file, format::CAPTURE_CAMERA_POSITION_OFFSET, {eye.X, eye.Y, eye.Z}));
    for (usize pixel = 0; pixel < pixelCount; ++pixel)
    {
        const RenderGraphCapturePixel& value = state.Pixels[pixel];
        const usize samplesOffset = WriteFloats(file,
                                                format::CAPTURE_HEADER_BYTES + pixel * format::CAPTURE_PIXEL_BYTES,
                                                {value.Mean.X, value.Mean.Y, value.Mean.Z});
        WriteUint32(file, samplesOffset, value.Samples);
    }
    outBytes = foundation::Move(bytes);
    return Status::Ready;
}
} // namespace ludus::graphics::renderer
