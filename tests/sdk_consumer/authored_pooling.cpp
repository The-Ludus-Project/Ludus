// Qualify the saved bilinear document using the installed SDK's unmodified runtime package.
#include "authored_bilinear_bytes.h"
#include "renderer_composite.h"

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/graphics/rhi/native_interop.h>

#include <cstring>
#include <new>

namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::render_graph_format;
namespace composite = ludus::shaders::renderer_composite;
using Status = rhi::RasterStatus;
constexpr logging::LogCategory LOG_POOL{"AuthoredPool"};
constexpr uint32 WIDTH = 127;
constexpr uint32 HEIGHT = 95;
constexpr usize RGBA_BYTES = 4;
constexpr usize IMAGE_BYTES = WIDTH * HEIGHT * RGBA_BYTES;
constexpr usize TRIANGLE_VERTICES = 3;
constexpr usize PHASE_STEPS = 6;
constexpr usize PHASE_COUNT = 3;
constexpr usize STEP_COUNT = PHASE_COUNT * PHASE_STEPS;
constexpr uint64 TIMEOUT = 30 * time::NANOSECONDS_PER_SECOND;
constexpr float64 NANOSECONDS_PER_MICROSECOND = 1000;
using Images = StaticArray<Array<uint8>, STEP_COUNT>;

bool IsAccepted(Status status)
{
    return status == Status::Ready || status == Status::Pending;
}

template <typename Poll>
bool Wait(rhi::DeviceHandle device, const char* operation, Poll poll)
{
    const uint64 start = time::NowTicks();
    Status status = Status::Pending;
    do
    {
        static_cast<void>(rhi::PollLifetime(device));
        status = poll();
        if (status == Status::Ready)
        {
            return true;
        }
    } while ((status == Status::Pending || status == Status::NotReady) && time::NowTicks() - start < TIMEOUT);
    LUDUS_LOG_ERROR(LOG_POOL, "{} status={}", operation, static_cast<uint32>(status));
    return false;
}

struct Frame final
{
    rhi::TextureHandle Intermediate;
    rhi::TextureHandle Export;
    rhi::SubmissionToken LastUse;
};

// Same consumer lease as the shared-device World viewport. A failed assertion must also
// release the pin before shutdown. Drain is appropriate in this host qualification.
struct ExportReader final
{
    rhi::NativeDeviceLease Device;
    rhi::NativeTextureRead Read;

    bool Acquire(rhi::DeviceHandle device, const Frame& frame)
    {
        rhi::NativeDeviceInfo native;
        rhi::NativeTextureInfo image;
        return rhi::AcquireNativeDevice(device, Device) == Status::Ready &&
               rhi::GetNativeDeviceInfo(Device, native) == Status::Ready &&
               rhi::AcquireNativeTextureRead(Device, native.Identity, frame.Export, frame.LastUse, Read, image) ==
                   Status::Ready;
    }

    ~ExportReader()
    {
        static_cast<void>(rhi::DrainNativeTextureRead(Read));
        static_cast<void>(rhi::ReleaseNativeDevice(Device));
    }
};

struct Qualification final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    renderer::RenderGraphActivation Activation;
    renderer::RenderGraphMesh Mesh;
    renderer::RenderGraphItem Item;
    renderer::ViewDescription Camera;
    uint32 Source = 0;
    uint32 Depth = 0;
    uint32 Destination = 0;
    uint32 Width = WIDTH;
    uint32 Height = HEIGHT;
    uint64 Revision = 0;
    uint64 PeakLogical = 0;
    uint64 PeakApi = 0;
    StaticArray<uint32, PHASE_COUNT> Created{};
    StaticArray<uint32, PHASE_COUNT> Reused{};
    StaticArray<Frame, STEP_COUNT> History{};
    uint32 PendingAtStage = 0;
    float64 GpuNanoseconds = 0;
    uint64 CpuNanoseconds = 0;
    uint64 StageNanoseconds = 0;
    uint32 StageCount = 0;

    bool Sample()
    {
        rhi::AllocationTelemetry memory;
        if (rhi::GetAllocationTelemetry(Device, memory) != Status::Ready || memory.ApiBytesAvailable == false)
        {
            return false;
        }
        PeakLogical = memory.LogicalBytes > PeakLogical ? memory.LogicalBytes : PeakLogical;
        PeakApi = memory.ApiBytes > PeakApi ? memory.ApiBytes : PeakApi;
        return true;
    }

    bool Stage(Frame* previousFrameOrNull)
    {
        renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
        core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
        if (package.Get() == nullptr || package->Decode({AUTHORED_BILINEAR.GetData(), AUTHORED_BILINEAR.GetSize()}) !=
                                            renderer::RenderGraphPackageStatus::Ready)
        {
            return false;
        }
        const renderer::RenderGraphPlan plan = package->GetPlan();
        if (plan.Passes.GetSize() != 2 || plan.Passes[0].Program != format::Technique::ColorAndDepth ||
            plan.Passes[1].Program != format::Technique::BilinearUpscale ||
            plan.Passes[1].SourceColor != plan.Passes[0].Color || plan.Cameras.GetSize() != 1 ||
            plan.Cameras[0].Target != plan.Passes[1].Color)
        {
            return false;
        }
        Source = plan.Passes[0].Color;
        Depth = plan.Passes[0].Depth;
        Destination = plan.Passes[1].Color;
        if (previousFrameOrNull != nullptr)
        {
            rhi::SubmissionToken replacementFrame;
            if (Submit(replacementFrame) == false)
            {
                return false;
            }
            previousFrameOrNull->LastUse = replacementFrame;
            rhi::RasterTextureState sampled;
            if (rhi::GetTextureState(Device, previousFrameOrNull->Intermediate, sampled) != Status::Ready ||
                sampled.Dependency.IsNull() == true)
            {
                return false;
            }
            PendingAtStage += rhi::GetStatus(Device, sampled.Dependency) == Status::Pending ? 1U : 0U;
        }
        const uint64 start = time::NowTicks();
        const Status status = Activation.Stage(Move(package));
        StageNanoseconds += time::NowTicks() - start;
        ++StageCount;
        return IsAccepted(status) == true && Sample() == true;
    }

    bool Publish(bool isResize)
    {
        ++Revision;
        // Alternate colors so a held consumer returning the replacement's pixels fails.
        Item.Material.Color = Revision % 2 == 0 ? math::Vector4{1, 0, 0, 1} : math::Vector4{0, 1, 0, 1};
        const uint64 targetGeneration = Activation.GetReport().CandidateGeneration;
        bool hasUpdated = false;
        return Wait(Device, "publication", [&]() {
            const Status status = Activation.Poll();
            const renderer::RenderGraphActivationReport report = Activation.GetReport();
            if (report.Candidate == renderer::RenderGraphCandidateState::Failed ||
                report.Candidate == renderer::RenderGraphCandidateState::RestartRequired)
            {
                return report.LastFailure;
            }
            renderer::RenderGraph* graphOrNull = Activation.GetActiveOrNull();
            if (hasUpdated == true && graphOrNull != nullptr && graphOrNull->GetDrawableSceneRevision() == Revision &&
                report.ActiveGeneration == targetGeneration && graphOrNull->HasPendingResize() == false)
            {
                return Status::Ready;
            }
            if (hasUpdated == false &&
                (isResize == true || report.Candidate == renderer::RenderGraphCandidateState::NeedsUpdate ||
                 (graphOrNull != nullptr && report.ActiveGeneration == targetGeneration)))
            {
                renderer::RenderGraphReport extraction;
                const Status updated = Activation.Update({&Mesh, 1},
                                                         {&Item, 1},
                                                         {&Camera, 1},
                                                         Revision,
                                                         renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                         extraction);
                hasUpdated = IsAccepted(updated);
                return hasUpdated == true ? Status::Pending : updated;
            }
            return IsAccepted(status) == true ? Status::Pending : status;
        });
    }

    bool Submit(rhi::SubmissionToken& outToken)
    {
        return rhi::SetFrameTarget(Device, Surface, { .Width = Width, .Height = Height }) == rhi::DeviceStatus::Ready &&
               IsAccepted(Activation.GetActiveOrNull()->Render(Surface, Width, Height, outToken)) == true &&
               outToken.IsNull() == false;
    }

    bool Capture(Frame& outFrame, Array<uint8>& outPixels)
    {
        if (rhi::ArmGpuTiming(Device) != Status::Ready)
        {
            return false;
        }
        const uint64 start = time::NowTicks();
        if (Submit(outFrame.LastUse) == false)
        {
            return false;
        }
        CpuNanoseconds += time::NowTicks() - start;
        if (Wait(Device, "frame", [&] { return rhi::GetStatus(Device, outFrame.LastUse); }) == false)
        {
            return false;
        }
        rhi::GpuTiming timing;
        if (Wait(Device, "GPU timestamps", [&] {
                const Status status = rhi::GetGpuTiming(Device, timing);
                if (status != Status::Ready)
                {
                    return status;
                }
                return timing.State == rhi::GpuTimingState::Pending     ? Status::Pending
                       : timing.State == rhi::GpuTimingState::Available ? Status::Ready
                                                                        : Status::Failed;
            }) == false)
        {
            return false;
        }
        GpuNanoseconds += timing.DurationNanoseconds;
        renderer::RenderGraph& graph = *Activation.GetActiveOrNull();
        rhi::TextureHandle depth;
        rhi::TextureAspect aspect{};
        rhi::RasterTextureState sampled;
        rhi::TextureDescription intermediate;
        rhi::TextureDescription output;
        if (graph.GetResourceAllocation(Source, outFrame.Intermediate, aspect) != Status::Ready ||
            aspect != rhi::TextureAspect::Color || graph.GetResourceAllocation(Depth, depth, aspect) != Status::Ready ||
            aspect != rhi::TextureAspect::Depth || depth != outFrame.Intermediate ||
            graph.GetExport(Destination, outFrame.Export, aspect) != Status::Ready ||
            aspect != rhi::TextureAspect::Color || outFrame.Export == depth ||
            rhi::GetTextureState(Device, depth, sampled) != Status::Ready ||
            (sampled.Use != rhi::RasterTextureUse::SampledFragment &&
             sampled.Use != rhi::RasterTextureUse::SampledBoth) ||
            sampled.ColorDefined == false || sampled.DepthDefined == false || sampled.Dependency.IsNull() == true ||
            rhi::GetStatus(Device, sampled.Dependency) != Status::Ready ||
            rhi::GetTextureDescription(Device, depth, intermediate) != Status::Ready ||
            intermediate.Width != (Width + 1) / 2 || intermediate.Height != (Height + 1) / 2 ||
            rhi::GetTextureDescription(Device, outFrame.Export, output) != Status::Ready ||
            output.Attachment == false || output.Width != Width || output.Height != Height)
        {
            LUDUS_LOG_ERROR(LOG_POOL,
                            "binding check failed: last use={} defined={}/{}",
                            static_cast<uint32>(sampled.Use),
                            sampled.ColorDefined,
                            sampled.DepthDefined);
            return false;
        }
        // Color and private depth are two views of the same export allocation. Its identity must
        // retire as a whole; never find either view's backing among later transient acquisitions.
        rhi::TextureViewHandle depthView;
        if (rhi::CreateTextureView(Device, outFrame.Export, { .Aspect = rhi::TextureAspect::Depth }, depthView) !=
                Status::Ready ||
            rhi::Destroy(Device, depthView) != Status::Ready)
        {
            return false;
        }
        rhi::ReadbackTicket read;
        return Request(outFrame.Export, read) == true && Read(read, outPixels) == true &&
               std::memcmp(outPixels.GetData(),
                           outPixels.GetData() + (Width * (Height / 2) + Width / 2) * RGBA_BYTES,
                           RGBA_BYTES) != 0;
    }

    bool Request(rhi::TextureHandle texture, rhi::ReadbackTicket& outTicket)
    {
        return IsAccepted(rhi::RequestTextureReadback(Device,
                                                      texture,
                                                      { .Width = Width, .Height = Height },
                                                      Width * RGBA_BYTES,
                                                      outTicket));
    }

    bool Read(rhi::ReadbackTicket& inoutTicket, Array<uint8>& outPixels)
    {
        return outPixels.TryResize(IMAGE_BYTES) == true &&
               Wait(Device,
                    "readback",
                    [&] { return rhi::CopyReadback(Device, inoutTicket, outPixels.GetData(), outPixels.GetSize()); }) ==
                   true &&
               rhi::Release(Device, inoutTicket) == Status::Ready;
    }
};

bool Measure(Qualification& inoutRun, bool isPooled, Images& inoutControl)
{
    Qualification& run = inoutRun;
    renderer::RenderGraphPackage identity;
    if (identity.Decode({AUTHORED_BILINEAR.GetData(), AUTHORED_BILINEAR.GetSize()}) !=
            renderer::RenderGraphPackageStatus::Ready ||
        run.Activation.Initialize(run.Device,
                                  identity.GetInterfaceIdentity(),
                                  composite::Vertex(),
                                  composite::Fragment(),
                                  isPooled == true ? renderer::RenderGraphAllocation::ExactPool
                                                   : renderer::RenderGraphAllocation::SerialControl) != Status::Ready ||
        run.Activation.Resize(WIDTH, HEIGHT) != Status::Ready)
    {
        return false;
    }
    for (usize step = 0; step < STEP_COUNT; ++step)
    {
        const usize phase = step / PHASE_STEPS;
        const bool isResize = phase == 1;
        const bool isTall = isResize == true && step % 2 == 0;
        const bool shouldHold = step == 1;
        ExportReader reader;
        rhi::ReadbackTicket held;
        if (shouldHold == true && reader.Acquire(run.Device, run.History[step - 1]) == false)
        {
            LUDUS_LOG_ERROR(LOG_POOL, "Could not retain the previous Metal export");
            return false;
        }
        renderer::RenderGraphAllocationReport before;
        if (isResize == true)
        {
            before = run.Activation.GetActiveOrNull()->GetAllocationReport();
        }
        run.Width = isTall == true ? HEIGHT : WIDTH;
        run.Height = isTall == true ? WIDTH : HEIGHT;
        if (isResize == true ? IsAccepted(run.Activation.Resize(run.Width, run.Height)) == false
                             : run.Stage(phase == 2 ? &run.History[step - 1] : nullptr) == false)
        {
            LUDUS_LOG_ERROR(LOG_POOL, "step {} could not stage/resize", step);
            return false;
        }
        // Record incomplete uses after the new backing was acquired. No such handle may be
        // found on this candidate even if the old submission finishes during publication.
        StaticArray<bool, STEP_COUNT> pending{};
        for (usize i = 0; i < step; ++i)
        {
            pending[i] = rhi::GetStatus(run.Device, run.History[i].LastUse) == Status::Pending;
        }
        // Activation's contract allows Stage with an export reader. Its GPU work must have
        // finished before Poll can publish; retain the downstream ticket across publication.
        if (shouldHold == true &&
            (rhi::GetStatus(reader.Read) != Status::Ready ||
             rhi::DrainNativeTextureRead(reader.Read) != Status::Ready ||
             run.Request(run.History[step - 1].Export, held) == false ||
             Wait(run.Device, "held consumer", [&] { return rhi::GetStatus(run.Device, held); }) == false))
        {
            return false;
        }
        if (run.Sample() == false || run.Publish(isResize) == false)
        {
            LUDUS_LOG_ERROR(LOG_POOL, "step {} could not publish", step);
            return false;
        }
        const renderer::RenderGraphAllocationReport allocation =
            run.Activation.GetActiveOrNull()->GetAllocationReport();
        const uint32 created = allocation.CreatedAllocations - before.CreatedAllocations;
        const uint32 reused = allocation.ReusedAllocations - before.ReusedAllocations;
        if (allocation.ExportAllocations != 1 || allocation.TransientAllocations != 1 ||
            allocation.IsPooled != isPooled || created + reused != 2 || created == 0)
        {
            LUDUS_LOG_ERROR(LOG_POOL,
                            "step {} allocation created={} reused={} exports={} transient={}",
                            step,
                            created,
                            reused,
                            allocation.ExportAllocations,
                            allocation.TransientAllocations);
            return false;
        }
        run.Created[phase] += created;
        run.Reused[phase] += reused;
        Frame& frame = run.History[step];
        Array<uint8> pixels;
        if (run.Capture(frame, pixels) == false || run.Sample() == false)
        {
            LUDUS_LOG_ERROR(LOG_POOL, "step {} failed frame checks", step);
            return false;
        }
        bool hasSeenIntermediate = false;
        for (usize i = 0; i < step; ++i)
        {
            const Frame& old = run.History[i];
            if (old.Export == frame.Export || old.Export == frame.Intermediate || old.Intermediate == frame.Export ||
                (old.Intermediate == frame.Intermediate && pending[i] == true))
            {
                LUDUS_LOG_ERROR(LOG_POOL, "step {} reused an exported or pending backing", step);
                return false;
            }
            hasSeenIntermediate = hasSeenIntermediate == true || old.Intermediate == frame.Intermediate;
        }
        if (hasSeenIntermediate != (reused == 1) || (isPooled == false && reused != 0))
        {
            LUDUS_LOG_ERROR(LOG_POOL, "step {} allocation report disagrees with texture identity", step);
            return false;
        }
        if (shouldHold == true)
        {
            Array<uint8> previous;
            if (run.Read(held, previous) == false ||
                std::memcmp(previous.GetData(), inoutControl[step - 1].GetData(), IMAGE_BYTES) != 0 ||
                std::memcmp(previous.GetData(), pixels.GetData(), IMAGE_BYTES) == 0)
            {
                LUDUS_LOG_ERROR(LOG_POOL,
                                "step {} held export contents differ or replacement is indistinguishable",
                                step);
                return false;
            }
        }
        if (isPooled == false)
        {
            inoutControl[step] = Move(pixels);
        }
        else if (std::memcmp(pixels.GetData(), inoutControl[step].GetData(), IMAGE_BYTES) != 0)
        {
            LUDUS_LOG_ERROR(LOG_POOL, "step {} differs from SerialControl", step);
            return false;
        }
    }
    const renderer::RenderGraphPoolReport pool = run.Activation.GetPoolReport();
    rhi::AllocationTelemetry steady;
    rhi::AllocationTelemetry trimmed;
    rhi::StorageUsage storage;
    if (rhi::GetAllocationTelemetry(run.Device, steady) != Status::Ready ||
        rhi::GetStorageUsage(run.Device, storage) != Status::Ready)
    {
        return false;
    }
    run.Activation.TrimPool();
    if (rhi::PollLifetime(run.Device) != Status::Ready ||
        rhi::GetAllocationTelemetry(run.Device, trimmed) != Status::Ready ||
        steady.LogicalBytes < trimmed.LogicalBytes || steady.ApiBytes < trimmed.ApiBytes)
    {
        return false;
    }
    LUDUS_LOG_INFO(
        LOG_POOL,
        "{} created={}/{}/{} reused={}/{}/{} deferred={} pending_at_replacement={} idle_backings={} logical_peak={} "
        "api_peak={} logical_high_water={} logical_steady={} api_steady={} logical_idle={} api_idle={} "
        "physical_available={} "
        "allocator_blocks_available={} stage_mean_us={} render_cpu_mean_us={} frame_gpu_mean_us={}",
        isPooled == true ? "ExactPool" : "SerialControl",
        run.Created[0],
        run.Created[1],
        run.Created[2],
        run.Reused[0],
        run.Reused[1],
        run.Reused[2],
        pool.DeferredAcquisitions,
        run.PendingAtStage,
        pool.IdleAllocations,
        run.PeakLogical,
        run.PeakApi,
        storage.PeakResourceBytes,
        steady.LogicalBytes,
        steady.ApiBytes,
        steady.LogicalBytes - trimmed.LogicalBytes,
        steady.ApiBytes - trimmed.ApiBytes,
        steady.PhysicalBytesAvailable,
        steady.AllocatorBlockBytesAvailable,
        static_cast<float64>(run.StageNanoseconds) / run.StageCount / NANOSECONDS_PER_MICROSECOND,
        static_cast<float64>(run.CpuNanoseconds) / STEP_COUNT / NANOSECONDS_PER_MICROSECOND,
        run.GpuNanoseconds / STEP_COUNT / NANOSECONDS_PER_MICROSECOND);
    return run.PendingAtStage != 0 &&
           (isPooled == false || (run.Reused[0] != 0 && run.Reused[1] != 0 && run.Reused[2] != 0)) &&
           (steady.LogicalBytes > trimmed.LogicalBytes) == isPooled;
}

bool RunMode(bool isPooled, Images& inoutControl)
{
    Qualification run;
    const StaticArray<math::Vector3, TRIANGLE_VERTICES> positions{{{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}}};
    const StaticArray<uint32, TRIANGLE_VERTICES> indices{{0, 1, 2}};
    run.Mesh.Geometry = {positions.GetData(), positions.GetSize(), indices.GetData(), indices.GetSize()};
    run.Item.SourceId = 1;
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.EnableGpuTiming = true;
    if (rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, run.Device, run.Surface) !=
        rhi::DeviceStatus::Ready)
    {
        return false;
    }
    rhi::MeasurementAdapter adapter;
    if (rhi::GetMeasurementAdapter(run.Device, adapter) != Status::Ready)
    {
        return false;
    }
    LUDUS_LOG_INFO(LOG_POOL,
                   "Metal adapter={} pixels={}",
                   static_cast<const char*>(adapter.Name),
                   WIDTH * HEIGHT * STEP_COUNT);
    return Measure(run, isPooled, inoutControl);
}
} // namespace

int main()
{
    static_cast<void>(logging::LogSystem::Initialize({ .EnableFile = false, .Directory = {} }));
    Images control;
    const bool isControlValid = RunMode(false, control);
    rhi::Shutdown();
    const bool hasPassed = isControlValid == true && RunMode(true, control) == true;
    rhi::Shutdown();
    LUDUS_LOG_INFO(LOG_POOL, "authored bilinear pooling {}", hasPassed == true ? "passed" : "failed");
    static_cast<void>(logging::LogSystem::Shutdown());
    return hasPassed == true ? 0 : 1;
}
