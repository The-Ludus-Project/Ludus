// RG4 exact-descriptor transient pooling evidence through the installed SDK, natively
// (Metal, Vulkan) and in a browser (WebGPU). Two workloads, each run first with the
// diagnostic serial control and then with the pooled path through RenderGraphActivation:
//   export-only: the cooked offscreen generation, as every admitted cooked graph is;
//   scratch-2:   the same generation rewritten with two unexported scratch backings
//                (render_graph_scratch.h), the only form a pooled transient can take today.
// A run reloads generations at the named extent, then alternates extents (each resize is a
// reload of a generation with the other extent), then reloads without waiting so every
// generation retires with its frame in flight, and finally times CPU Encode over many frames.
// Every captured export of the pooled run must equal the control's byte for byte. Each run
// logs one `RG4 run` line of measured allocation counts, RHI telemetry bytes and CPU
// p50/p95 times; see docs/development/render-graph-authoring.md#pool-transient-attachments-rg4.
#include "render_graph_scratch.h"
#include "renderer_composite.h"
#include "rg3_runtime_package.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>

#include <new>
#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/platform/base/window.h>
#endif
namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
namespace scratch = ludus::render_graph_scratch;
constexpr logging::LogCategory LOG_RG4{"RG4"};
/// Named measurement profile: one 640x360 offscreen export, plus its 360x640 resize partner.
constexpr uint32 WIDTH = 640;
constexpr uint32 HEIGHT = 360;
constexpr usize BYTES_PER_PIXEL = 4;
constexpr usize EXPORT_BYTES = usize{WIDTH} * HEIGHT * BYTES_PER_PIXEL;
constexpr uint32 EXPORTED_COLOR = 0;
/// Captured reloads at the named extent.
constexpr usize RELOADS = 6;
/// Captured reloads alternating the two extents.
constexpr usize RESIZES = 6;
/// Reloads rendered without waiting for their frame.
constexpr usize IN_FLIGHT = 6;
/// Every step, ending with one captured reload after the in-flight ones.
constexpr usize STEP_COUNT = RELOADS + RESIZES + IN_FLIGHT + 1;
constexpr usize CAPTURE_COUNT = RELOADS + RESIZES + 1;
/// Frames whose CPU Encode time is sampled on the final generation.
constexpr usize ENCODES = 120;
constexpr usize WORKLOAD_COUNT = 2;
constexpr usize MODE_COUNT = 2;
constexpr usize RUN_COUNT = WORKLOAD_COUNT * MODE_COUNT;
constexpr uint32 PERCENTILE_50 = 50;
constexpr uint32 PERCENTILE_95 = 95;
constexpr uint32 PERCENT = 100;
constexpr float64 NANOSECONDS_PER_MICROSECOND = 1e3;
constexpr usize VERTEX_COUNT = 3;
constexpr usize INDEX_COUNT = 3;
/// Reload, resize and in-flight step phases, each with its own allocation counters.
constexpr usize MEASURED_PHASE_COUNT = 3;
/// RHI readback staging packs rows to this many bytes (RequestTextureReadback); bands must fit a slice.
constexpr usize STAGING_ROW_ALIGNMENT = 256;
struct Workload final
{
    const char* Name = "";
    uint32 Scratch = 0;
};
const StaticArray<Workload, WORKLOAD_COUNT> WORKLOADS{{{"export-only", 0}, {"scratch-2", 2}}};
const StaticArray<renderer::RenderGraphAllocation, MODE_COUNT> MODES{
    {renderer::RenderGraphAllocation::SerialControl, renderer::RenderGraphAllocation::ExactPool}};
const StaticArray<const char*, MODE_COUNT> MODE_NAMES{{"serial-control", "exact-pool"}};
enum class Phase : uint8
{
    StartRun,
    Stage,
    Activate,
    Render,
    AwaitExport,
    AwaitReadback,
    Encode,
    AwaitEncode,
    FinishRun,
};
/// Measured counters of one run; byte figures are RHI logical telemetry and, where the
/// backend exposes them, API allocation sizes.
struct RunMeasurement final
{
    StaticArray<uint32, MEASURED_PHASE_COUNT> Created{};
    StaticArray<uint32, MEASURED_PHASE_COUNT> Reused{};
    uint64 Deferred = 0;
    uint64 LogicalSteady = 0;
    uint64 LogicalPeak = 0;
    uint64 ApiSteady = 0;
    uint64 ApiPeak = 0;
    uint64 PoolIdleLogical = 0;
    uint64 PoolIdleApi = 0;
    usize Mismatches = 0;
};
rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
core::UniquePtr<renderer::RenderGraphActivation> gActivation;
StaticArray<math::Vector3, VERTEX_COUNT> gPositions{{{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}}};
StaticArray<uint32, INDEX_COUNT> gIndices{{0, 1, 2}};
StaticArray<renderer::RenderGraphMesh, 1> gMeshes{};
StaticArray<renderer::RenderGraphItem, 1> gItems{};
StaticArray<renderer::ViewDescription, 1> gCameras{};
/// The control run's captured exports of the current workload, compared with the pooled run's.
StaticArray<Array<uint8>, CAPTURE_COUNT> gControl{};
Array<uint8> gPixels;
Array<uint64> gStageNanoseconds;
Array<uint64> gEncodeNanoseconds;
rhi::ReadbackTicket gReadback;
RunMeasurement gRun{};
Phase gPhase = Phase::StartRun;
usize gRunIndex = 0;
usize gStep = 0;
usize gCapture = 0;
usize gEncode = 0;
uint32 gRow = 0;
uint64 gRevision = 0;
uint64 gSerial = 0;
uint64 gUpdatedSerial = 0;
usize gTotalMismatches = 0;
rhi::RasterStatus gLastStatus = rhi::RasterStatus::Ready;
/// Lifecycle and reporting flags of the whole qualification.
struct ProgressFlags final
{
    bool IsDone = false;
    bool HasFailed = false;
    bool HasApiBytes = false;
    bool HasReportedDevice = false;
};
ProgressFlags gFlags{};
#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// clang-format off
EM_JS(void, Report, (int32 state, uint32 run, uint32 step, uint32 phase, uint32 mismatches, uint32 status), {
    const node = document.querySelector('#status');
    node.dataset.state = state === 2 ? 'failed' : state === 1 ? 'passed' : 'loading';
    Object.assign(node.dataset, {run, step, phase, mismatches, status});
    node.textContent = node.dataset.state + ' run=' + run + ' step=' + step + ' mismatches=' + mismatches;
});
// clang-format on
#else
void Report(int32, uint32, uint32, uint32, uint32, uint32) noexcept {}
#endif
const Workload& GetWorkload() noexcept
{
    return WORKLOADS[gRunIndex / MODE_COUNT];
}
usize GetMode() noexcept
{
    return gRunIndex % MODE_COUNT;
}
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
bool IsWaiting(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Pending || status == rhi::RasterStatus::NotReady;
}
/// Steps of the reload, resize and in-flight phases; index 0, 1 or 2 of the counters.
usize GetStepPhase(usize step) noexcept
{
    return step < RELOADS ? 0 : step < RELOADS + RESIZES ? 1 : 2;
}
bool IsCaptured(usize step) noexcept
{
    return step < RELOADS + RESIZES || step + 1 == STEP_COUNT;
}
bool IsTall(usize step) noexcept
{
    return GetStepPhase(step) == 1 && (step - RELOADS) % 2 == 0;
}
uint32 GetWidth(usize step) noexcept
{
    return IsTall(step) == true ? HEIGHT : WIDTH;
}
uint32 GetHeight(usize step) noexcept
{
    return IsTall(step) == true ? WIDTH : HEIGHT;
}
void Publish(int32 state) noexcept
{
    Report(state,
           static_cast<uint32>(gRunIndex),
           static_cast<uint32>(gStep),
           static_cast<uint32>(gPhase),
           static_cast<uint32>(gTotalMismatches),
           static_cast<uint32>(gLastStatus));
}
void Stop(bool hasPassed) noexcept
{
    gFlags.IsDone = true;
    gFlags.HasFailed = hasPassed == false;
    LUDUS_LOG_INFO(LOG_RG4,
                   "pooling {} runs={} mismatched_bytes={} last_status={}",
                   hasPassed == true ? "passed" : "failed",
                   gRunIndex,
                   gTotalMismatches,
                   static_cast<uint32>(gLastStatus));
    Publish(hasPassed == true ? 1 : 2);
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_cancel_main_loop();
#endif
    static_cast<void>(rhi::Release(gDevice, gReadback));
    gActivation.Reset();
    rhi::Shutdown();
}
void Fail(rhi::RasterStatus status) noexcept
{
    gLastStatus = status;
    Stop(false);
}
/// Sample RHI telemetry into the run's steady and peak byte figures.
void SampleBytes(bool isSteady) noexcept
{
    rhi::AllocationTelemetry telemetry;
    if (rhi::GetAllocationTelemetry(gDevice, telemetry) != rhi::RasterStatus::Ready)
    {
        return;
    }
    gFlags.HasApiBytes = telemetry.ApiBytesAvailable;
    gRun.LogicalPeak = telemetry.LogicalBytes > gRun.LogicalPeak ? telemetry.LogicalBytes : gRun.LogicalPeak;
    gRun.ApiPeak = telemetry.ApiBytes > gRun.ApiPeak ? telemetry.ApiBytes : gRun.ApiPeak;
    if (isSteady == true)
    {
        gRun.LogicalSteady = telemetry.LogicalBytes;
        gRun.ApiSteady = telemetry.ApiBytes;
    }
}
float64 GetPercentileMicroseconds(Array<uint64>& inoutSamples, uint32 percentile) noexcept
{
    if (inoutSamples.IsEmpty() == true)
    {
        return 0;
    }
    Sort(inoutSamples.GetData(), inoutSamples.GetSize(), [](uint64 left, uint64 right) noexcept {
        return left < right;
    });
    const usize index = (inoutSamples.GetSize() - 1) * percentile / PERCENT;
    return static_cast<float64>(inoutSamples[index]) / NANOSECONDS_PER_MICROSECOND;
}
rhi::RasterStatus UpdateScene() noexcept
{
    renderer::RenderGraphReport report;
    return gActivation->Update({gMeshes.GetData(), gMeshes.GetSize()},
                               {gItems.GetData(), gItems.GetSize()},
                               {gCameras.GetData(), gCameras.GetSize()},
                               ++gRevision,
                               renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                               report);
}
bool StageGeneration() noexcept
{
    Array<uint8> bytes;
    if (scratch::WriteScratchGeneration({RG3_RUNTIME_PACKAGE.GetData(), RG3_RUNTIME_PACKAGE.GetSize()},
                                        {GetWorkload().Scratch, GetWidth(gStep), GetHeight(gStep)},
                                        bytes) == false)
    {
        Fail(rhi::RasterStatus::InvalidDescription);
        return false;
    }
    renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
    if (package.Get() == nullptr ||
        package->Decode({bytes.GetData(), bytes.GetSize()}) != renderer::RenderGraphPackageStatus::Ready)
    {
        Fail(rhi::RasterStatus::InvalidDescription);
        return false;
    }
    // Staging initializes the graph: pipeline prewarm and attachment allocation or reuse.
    const uint64 start = time::NowTicks();
    const rhi::RasterStatus staged = gActivation->Stage(Move(package));
    gStageNanoseconds.Add(time::NowTicks() - start);
    if (IsAccepted(staged) == false)
    {
        Fail(staged);
        return false;
    }
    gSerial = gActivation->GetReport().CandidateGeneration;
    SampleBytes(false);
    return true;
}
/// Poll until the staged generation is active with a drawable extraction.
bool AdvanceActivation() noexcept
{
    const rhi::RasterStatus polled = gActivation->Poll();
    const renderer::RenderGraphActivationReport report = gActivation->GetReport();
    if (report.Candidate == renderer::RenderGraphCandidateState::Failed ||
        report.Candidate == renderer::RenderGraphCandidateState::RestartRequired)
    {
        Fail(report.LastFailure);
        return false;
    }
    if (IsWaiting(polled) == false && IsAccepted(polled) == false)
    {
        Fail(polled);
        return false;
    }
    renderer::RenderGraph* activeOrNull = gActivation->GetActiveOrNull();
    const bool isActive = report.ActiveGeneration == gSerial && activeOrNull != nullptr;
    // Update a ready candidate or a newly active first generation once, then let preparation finish.
    const bool isSceneNeeded = report.Candidate == renderer::RenderGraphCandidateState::NeedsUpdate ||
                               (isActive == true && activeOrNull->GetDrawableSceneRevision() == 0);
    if (isSceneNeeded == true && gUpdatedSerial != gSerial)
    {
        const rhi::RasterStatus updated = UpdateScene();
        if (IsWaiting(updated) == false && IsAccepted(updated) == false)
        {
            Fail(updated);
            return false;
        }
        gUpdatedSerial = IsAccepted(updated) == true ? gSerial : gUpdatedSerial;
    }
    return isActive == true && activeOrNull->GetDrawableSceneRevision() != 0;
}
/// Rows of the current step's next readback band: caller and staging rows fit one transfer slice.
uint32 GetBandRows() noexcept
{
    rhi::LifetimeCapabilities capabilities;
    static_cast<void>(rhi::GetLifetimeCapabilities(gDevice, capabilities));
    const usize rowPitch = usize{GetWidth(gStep)} * BYTES_PER_PIXEL;
    const usize stagingPitch = (rowPitch + STAGING_ROW_ALIGNMENT - 1) / STAGING_ROW_ALIGNMENT * STAGING_ROW_ALIGNMENT;
    const uint32 bandRows = static_cast<uint32>(capabilities.TransferBytes / stagingPitch);
    const uint32 remaining = GetHeight(gStep) - gRow;
    return remaining < bandRows ? remaining : bandRows;
}
/// Request the next row band of the export.
bool RequestBand(rhi::TextureHandle texture) noexcept
{
    const uint32 width = GetWidth(gStep);
    const usize rowPitch = usize{width} * BYTES_PER_PIXEL;
    const uint32 rows = GetBandRows();
    if (rows == 0)
    {
        Fail(rhi::RasterStatus::Unsupported);
        return false;
    }
    const rhi::RasterStatus requested =
        rhi::RequestTextureReadback(gDevice,
                                    texture,
                                    { .Mip = 0, .X = 0, .Y = gRow, .Width = width, .Height = rows },
                                    rowPitch,
                                    gReadback);
    if (IsAccepted(requested) == false)
    {
        Fail(requested);
        return false;
    }
    return true;
}
void StartRun() noexcept
{
    renderer::RenderGraphActivation* allocationOrNull = new (std::nothrow) renderer::RenderGraphActivation;
    core::UniquePtr<renderer::RenderGraphActivation> activation{allocationOrNull};
    gActivation.Swap(activation);
    renderer::RenderGraphPackage identity;
    if (gActivation.Get() == nullptr ||
        identity.Decode({RG3_RUNTIME_PACKAGE.GetData(), RG3_RUNTIME_PACKAGE.GetSize()}) !=
            renderer::RenderGraphPackageStatus::Ready)
    {
        Fail(rhi::RasterStatus::OutOfMemory);
        return;
    }
    const rhi::RasterStatus initialized = gActivation->Initialize(gDevice,
                                                                  identity.GetInterfaceIdentity(),
                                                                  shaders::renderer_composite::Vertex(),
                                                                  shaders::renderer_composite::Fragment(),
                                                                  MODES[GetMode()]);
    if (initialized != rhi::RasterStatus::Ready)
    {
        Fail(initialized);
        return;
    }
    gRun = {};
    gStageNanoseconds.Clear();
    gEncodeNanoseconds.Clear();
    gStep = 0;
    gCapture = 0;
    gEncode = 0;
    gPhase = Phase::Stage;
}
void FinishRun() noexcept
{
    gRun.Deferred = gActivation->GetPoolReport().DeferredAcquisitions;
    // Idle pooled bytes: telemetry before and after trimming, with every frame complete.
    rhi::AllocationTelemetry before;
    rhi::AllocationTelemetry after;
    static_cast<void>(rhi::GetAllocationTelemetry(gDevice, before));
    gActivation->TrimPool();
    static_cast<void>(rhi::PollLifetime(gDevice));
    static_cast<void>(rhi::GetAllocationTelemetry(gDevice, after));
    gRun.PoolIdleLogical = before.LogicalBytes - after.LogicalBytes;
    gRun.PoolIdleApi = before.ApiBytes >= after.ApiBytes ? before.ApiBytes - after.ApiBytes : 0;
    const usize mode = GetMode();
    LUDUS_LOG_INFO(LOG_RG4,
                   "run workload={} mode={} extent={}x{} reload_created={} reload_reused={} resize_created={} "
                   "resize_reused={} inflight_created={} inflight_reused={} deferred={} logical_steady={} "
                   "logical_peak={} api_bytes={} api_steady={} api_peak={} pool_idle_logical={} pool_idle_api={} "
                   "stage_p50_us={} stage_p95_us={} encode_p50_us={} encode_p95_us={} encode_samples={} "
                   "mismatched_bytes={}",
                   GetWorkload().Name,
                   MODE_NAMES[mode],
                   WIDTH,
                   HEIGHT,
                   gRun.Created[0],
                   gRun.Reused[0],
                   gRun.Created[1],
                   gRun.Reused[1],
                   gRun.Created[2],
                   gRun.Reused[2],
                   gRun.Deferred,
                   gRun.LogicalSteady,
                   gRun.LogicalPeak,
                   gFlags.HasApiBytes,
                   gRun.ApiSteady,
                   gRun.ApiPeak,
                   gRun.PoolIdleLogical,
                   gRun.PoolIdleApi,
                   GetPercentileMicroseconds(gStageNanoseconds, PERCENTILE_50),
                   GetPercentileMicroseconds(gStageNanoseconds, PERCENTILE_95),
                   GetPercentileMicroseconds(gEncodeNanoseconds, PERCENTILE_50),
                   GetPercentileMicroseconds(gEncodeNanoseconds, PERCENTILE_95),
                   gEncodeNanoseconds.GetSize(),
                   gRun.Mismatches);
    gTotalMismatches += gRun.Mismatches;
    // The serial control never reuses; the pooled scratch workload must, and export-only never can.
    const uint32 reused = gRun.Reused[0] + gRun.Reused[1] + gRun.Reused[2];
    const bool isExpected = mode == 0 || GetWorkload().Scratch == 0 ? reused == 0 : reused != 0;
    gActivation.Reset();
    if (gRun.Mismatches != 0 || isExpected == false)
    {
        Stop(false);
        return;
    }
    if (++gRunIndex == RUN_COUNT)
    {
        Stop(true);
        return;
    }
    gPhase = Phase::StartRun;
}
/// Advance as far as the GPU allows; every wait returns and resumes on a later tick.
void Advance() noexcept
{
    if (gPhase == Phase::StartRun)
    {
        StartRun();
    }
    if (gPhase == Phase::Stage)
    {
        if (StageGeneration() == false)
        {
            return;
        }
        gPhase = Phase::Activate;
    }
    if (gPhase == Phase::Activate)
    {
        if (AdvanceActivation() == false)
        {
            return;
        }
        const renderer::RenderGraphAllocationReport allocation = gActivation->GetActiveOrNull()->GetAllocationReport();
        gRun.Created[GetStepPhase(gStep)] += allocation.CreatedAllocations;
        gRun.Reused[GetStepPhase(gStep)] += allocation.ReusedAllocations;
        gPhase = Phase::Render;
    }
    if (gPhase == Phase::Render)
    {
        // A browser surface starts with a zero frame target, which skips acquisition; size it every frame.
        if (rhi::SetFrameTarget(gDevice, gSurface, { .Width = WIDTH, .Height = HEIGHT }) != rhi::DeviceStatus::Ready)
        {
            Fail(rhi::RasterStatus::InvalidState);
            return;
        }
        rhi::SubmissionToken completion;
        const rhi::RasterStatus rendered = gActivation->GetActiveOrNull()->Render(gSurface, WIDTH, HEIGHT, completion);
        if (rendered == rhi::RasterStatus::NotReady)
        {
            return;
        }
        if (IsAccepted(rendered) == false)
        {
            Fail(rendered);
            return;
        }
        if (IsCaptured(gStep) == false)
        {
            // In flight: stage the next generation without waiting for this frame.
            ++gStep;
            gPhase = Phase::Stage;
            return;
        }
        gRow = 0;
        gPhase = Phase::AwaitExport;
    }
    if (gPhase == Phase::AwaitExport)
    {
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect = rhi::TextureAspect::Color;
        const rhi::RasterStatus exported = gActivation->GetActiveOrNull()->GetExport(EXPORTED_COLOR, texture, aspect);
        if (IsWaiting(exported) == true)
        {
            return;
        }
        if (exported != rhi::RasterStatus::Ready || gPixels.TryResize(EXPORT_BYTES) == false ||
            RequestBand(texture) == false)
        {
            Fail(exported);
            return;
        }
        SampleBytes(true);
        gPhase = Phase::AwaitReadback;
    }
    while (gPhase == Phase::AwaitReadback)
    {
        const usize rowPitch = usize{GetWidth(gStep)} * BYTES_PER_PIXEL;
        const usize offset = usize{gRow} * rowPitch;
        const uint32 rows = GetBandRows();
        const rhi::RasterStatus copied =
            rhi::CopyReadback(gDevice, gReadback, gPixels.GetData() + offset, usize{rows} * rowPitch);
        if (IsWaiting(copied) == true)
        {
            return;
        }
        if (copied != rhi::RasterStatus::Ready || rhi::Release(gDevice, gReadback) != rhi::RasterStatus::Ready)
        {
            Fail(copied);
            return;
        }
        gRow += rows;
        if (gRow < GetHeight(gStep))
        {
            rhi::TextureHandle texture;
            rhi::TextureAspect aspect = rhi::TextureAspect::Color;
            if (gActivation->GetActiveOrNull()->GetExport(EXPORTED_COLOR, texture, aspect) !=
                    rhi::RasterStatus::Ready ||
                RequestBand(texture) == false)
            {
                Fail(rhi::RasterStatus::InvalidState);
                return;
            }
            continue;
        }
        if (GetMode() == 0)
        {
            gControl[gCapture] = Move(gPixels);
        }
        else
        {
            const Array<uint8>& control = gControl[gCapture];
            gRun.Mismatches += control.GetSize() == gPixels.GetSize() ? 0 : EXPORT_BYTES;
            for (usize i = 0; i < control.GetSize() && i < gPixels.GetSize(); ++i)
            {
                gRun.Mismatches += control[i] != gPixels[i] ? 1U : 0U;
            }
        }
        ++gCapture;
        ++gStep;
        gPhase = gStep == STEP_COUNT ? Phase::Encode : Phase::Stage;
        return;
    }
    if (gPhase == Phase::AwaitEncode)
    {
        renderer::RenderGraph& graph = *gActivation->GetActiveOrNull();
        if (graph.GetOutputSceneRevision() == 0)
        {
            return;
        }
        gPhase = gEncode == ENCODES ? Phase::FinishRun : Phase::Encode;
    }
    if (gPhase == Phase::Encode)
    {
        renderer::RenderGraph& graph = *gActivation->GetActiveOrNull();
        if (rhi::SetFrameTarget(gDevice, gSurface, { .Width = WIDTH, .Height = HEIGHT }) != rhi::DeviceStatus::Ready)
        {
            Fail(rhi::RasterStatus::InvalidState);
            return;
        }
        const rhi::DeviceStatus started = rhi::BeginFrame(gDevice, gSurface);
        if (started != rhi::DeviceStatus::Ready)
        {
            // Skipped or not-yet-acquired frames retry on a later tick.
            return;
        }
        const uint64 start = time::NowTicks();
        const rhi::RasterStatus encoded = graph.Encode(gSurface);
        const uint64 elapsed = time::NowTicks() - start;
        rhi::SubmissionToken completion;
        const rhi::RasterStatus submitted = rhi::EndFrame(gDevice, completion);
        const rhi::RasterStatus confirmed = graph.ConfirmSubmission(completion);
        if (encoded != rhi::RasterStatus::Ready || IsAccepted(submitted) == false || IsAccepted(confirmed) == false)
        {
            Fail(encoded != rhi::RasterStatus::Ready ? encoded : submitted);
            return;
        }
        gEncodeNanoseconds.Add(elapsed);
        ++gEncode;
        gPhase = Phase::AwaitEncode;
        return;
    }
    if (gPhase == Phase::FinishRun)
    {
        FinishRun();
    }
}
void Tick() noexcept
{
    if (gFlags.IsDone == true)
    {
        return;
    }
    rhi::DeviceInfo information;
    const rhi::DeviceStatus device = rhi::GetDeviceInfo(gDevice, information);
    if (device == rhi::DeviceStatus::Pending)
    {
        return;
    }
    if (device != rhi::DeviceStatus::Ready)
    {
        Fail(rhi::RasterStatus::DeviceLost);
        return;
    }
    const rhi::RasterStatus lifetime = rhi::PollLifetime(gDevice);
    if (IsAccepted(lifetime) == false && lifetime != rhi::RasterStatus::NotReady)
    {
        Fail(lifetime);
        return;
    }
    if (gFlags.HasReportedDevice == false)
    {
        gFlags.HasReportedDevice = true;
        rhi::MeasurementAdapter adapter;
        static_cast<void>(rhi::GetMeasurementAdapter(gDevice, adapter));
        LUDUS_LOG_INFO(LOG_RG4,
                       "device backend={} adapter=\"{}\" vendor={} device={} software_known={} software={} "
                       "workloads={} reloads={} resizes={} in_flight={} encodes={}",
                       static_cast<uint32>(information.Startup.SelectedBackend),
                       static_cast<const char*>(adapter.Name),
                       adapter.VendorId,
                       adapter.DeviceId,
                       adapter.SoftwareKnown,
                       adapter.Software,
                       WORKLOAD_COUNT,
                       RELOADS,
                       RESIZES,
                       IN_FLIGHT,
                       ENCODES);
    }
    Publish(0);
    Advance();
}
} // namespace
int main()
{
    static_cast<void>(logging::LogSystem::Initialize({ .EnableFile = false, .Directory = {} }));
    gMeshes[0].Geometry = {gPositions.GetData(), gPositions.GetSize(), gIndices.GetData(), gIndices.GetSize()};
    gItems[0].SourceId = 1;
    gItems[0].Material.Color = {.25F, .5F, .75F, 1};
    rhi::WindowInfo window;
    window.Width = WIDTH;
    window.Height = HEIGHT;
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo settings;
    settings.Width = WIDTH;
    settings.Height = HEIGHT;
    settings.CanvasSelector = "#canvas";
    settings.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(settings, gWindow) == false)
    {
        return 1;
    }
    window = gWindow->GetNativeWindowInfo();
#endif
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
#if defined(LUDUS_PLATFORM_WEB)
    description.Selection = rhi::BackendSelection::WebGPU;
#endif
    rhi::StartupInfo failure;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, window, description, gDevice, gSurface, &failure);
    if (started != rhi::DeviceStatus::Ready && started != rhi::DeviceStatus::Pending)
    {
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    // Native hosts have no browser event loop; bound the polling loop instead.
    constexpr uint64 NATIVE_TIMEOUT_SECONDS = 120;
    const uint64 start = time::NowTicks();
    while (gFlags.IsDone == false && time::NowTicks() - start < NATIVE_TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND)
    {
        Tick();
    }
    if (gFlags.IsDone == false)
    {
        Fail(rhi::RasterStatus::Pending);
    }
#endif
    static_cast<void>(logging::LogSystem::Shutdown());
    return gFlags.HasFailed == true ? 1 : 0;
}
