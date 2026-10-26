#include "internal/gpu_preview_data.h"
#include "internal/reference_data.h"

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/raster.h>

#include <cstring>
#include <new>

// GPU-compute RT1 reference: renderer_reference.slang traverses the frozen median BVH
// and integrates the same transport as the CPU oracle (reference_path.cpp). The host
// only packs immutable inputs, submits one sample per pixel with a reduction pass, and
// reads back the reduced status and, periodically, the committed sums. Per-pixel
// transactions follow Jakub Boksansky and Adam Marrs, "The Reference Path Tracer", Ray
// Tracing Gems II, chapter 14, section 14.3.7 (progressive accumulation); thanks to them.
namespace ludus::graphics::renderer::internal
{
using Status = rhi::RasterStatus;
namespace
{
/// Status bytes are three uint4 words.
constexpr usize STATUS_WORD_COUNT = REFERENCE_COMPUTE_STATUS_BYTES / sizeof(uint32);
/// Committed sums are read back on every multiple of this sample count, as well as at
/// powers of two, failure and completion, bounding readback traffic for long captures.
constexpr uint32 READBACK_SAMPLE_INTERVAL = 16;
/// Status word indices; see renderer_reference_layout.h.
constexpr usize MINIMUM_SAMPLES_WORD = 0;
constexpr usize MAXIMUM_SAMPLES_WORD = 1;
constexpr usize COMMITTED_WORD = 2;
constexpr usize INVALID_WORD = 3;
constexpr usize CLOSEST_WORD = 4;
constexpr usize VISIBILITY_WORD = 5;
constexpr usize NULL_BSDF_WORD = 6;
constexpr usize ROULETTE_WORD = 7;
constexpr usize FAILURE_PIXEL_WORD = 8;
constexpr usize FAILURE_DEPTH_WORD = 9;
constexpr usize CAPACITY_WORD = 10;
/// Float components of one read-back pixel: emission, direct and indirect float4 words.
constexpr usize EMISSION_COMPONENT = 0;
constexpr usize SAMPLES_COMPONENT = 3;
constexpr usize DIRECT_COMPONENT = 4;
constexpr usize INDIRECT_COMPONENT = 8;
constexpr usize COMPONENT_BYTES = sizeof(float32);
constexpr uint32 VECTOR_BYTES = sizeof(math::Vector4);
constexpr StaticArray<rhi::RasterBinding, LUDUS_REFERENCE_BINDING_COUNT> COMPUTE_BINDINGS{{
    {LUDUS_REFERENCE_PARAMETERS_BINDING,
     rhi::RasterBindingKind::UniformBuffer,
     rhi::RasterVisibility::Compute,
     sizeof(GpuPreviewUniform)},
    {LUDUS_REFERENCE_SCENE_BINDING, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Compute, VECTOR_BYTES},
    {LUDUS_REFERENCE_ACCUMULATION_BINDING,
     rhi::RasterBindingKind::StorageReadWrite,
     rhi::RasterVisibility::Compute,
     VECTOR_BYTES},
    {LUDUS_REFERENCE_STATUS_BINDING,
     rhi::RasterBindingKind::StorageReadWrite,
     rhi::RasterVisibility::Compute,
     VECTOR_BYTES},
    {LUDUS_REFERENCE_TEXELS_BINDING, rhi::RasterBindingKind::StorageRead, rhi::RasterVisibility::Compute, VECTOR_BYTES},
}};
bool Accepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
bool IsExpectedBinding(const rhi::RasterBinding& actual) noexcept
{
    for (const rhi::RasterBinding& expected : COMPUTE_BINDINGS)
    {
        if (actual.Binding == expected.Binding)
        {
            return actual.Kind == expected.Kind && actual.Visibility == expected.Visibility &&
                   actual.MinSize == expected.MinSize && actual.HasDynamicOffset == false && actual.Count == 1 &&
                   actual.IsRuntimeSized == false && actual.HasPartialBinding == false;
        }
    }
    return false;
}
bool IsExpectedKernel(const rhi::RasterShaderDescription& kernel, bool shouldBindEverything) noexcept
{
    if (kernel.WorkgroupSize[0] != LUDUS_REFERENCE_GROUP_WIDTH || kernel.WorkgroupSize[1] != 1 ||
        kernel.WorkgroupSize[2] != 1 || kernel.Bindings.size() > COMPUTE_BINDINGS.GetSize() ||
        (shouldBindEverything == true && kernel.Bindings.size() != COMPUTE_BINDINGS.GetSize()))
    {
        return false;
    }
    for (const rhi::RasterBinding& binding : kernel.Bindings)
    {
        if (IsExpectedBinding(binding) == false)
        {
            return false;
        }
    }
    return true;
}
bool IsReadbackDue(uint32 samples, uint32 target) noexcept
{
    return samples >= target || (samples & (samples - 1)) == 0 || samples % READBACK_SAMPLE_INTERVAL == 0;
}
float32 ReadComponent(const uint8* bytes, usize component) noexcept
{
    uint32 bits = 0;
    static_cast<void>(TryReadLittleEndian({bytes + component * COMPONENT_BYTES, COMPONENT_BYTES}, bits));
    float32 value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
uint32 ReadWord(const uint8* bytes, usize word) noexcept
{
    uint32 value = 0;
    static_cast<void>(TryReadLittleEndian({bytes + word * sizeof(uint32), sizeof(uint32)}, value));
    return value;
}
math::Vector3 ReadVector(const uint8* bytes, usize first) noexcept
{
    return {ReadComponent(bytes, first), ReadComponent(bytes, first + 1), ReadComponent(bytes, first + 2)};
}
Status Fail(ReferenceJob& inoutJob, Status status) noexcept
{

    inoutJob.Stage = ReferenceStage::Failed;
    return status;
}
// Import every buffer of the shared binding set, then order trace -> reduce writes.
Status BuildDispatchGraph(const ReferenceComputePipelines& pipelines,
                          const ReferenceJob& job,
                          rhi::OrderedGraph& outGraph) noexcept
{
    const ReferenceComputeJob& compute = *job.Compute;
    const rhi::DeviceHandle device = compute.Device;
    const usize pixels = usize(job.Description.Width) * job.Description.Height;
    const usize accumulationBytes = pixels * REFERENCE_COMPUTE_PIXEL_BYTES;
    Status status = rhi::CreateOrderedGraph(device, outGraph);
    rhi::GraphVersion parameters;
    rhi::GraphVersion scene;
    rhi::GraphVersion texels;
    rhi::GraphVersion accumulation;
    rhi::GraphVersion statusVersion;
    if (status == Status::Ready)
    {
        status = rhi::ImportGraphBuffer(device, outGraph, compute.Parameters, parameters);
    }
    if (status == Status::Ready)
    {
        status = rhi::ImportGraphBuffer(device, outGraph, compute.Scene, scene);
    }
    if (status == Status::Ready)
    {
        status = rhi::ImportGraphBuffer(device, outGraph, compute.Texels, texels);
    }
    if (status == Status::Ready)
    {
        status = rhi::ImportGraphBuffer(device, outGraph, compute.Accumulation, accumulation);
    }
    if (status == Status::Ready)
    {
        status = rhi::ImportGraphBuffer(device, outGraph, compute.Status, statusVersion);
    }
    // Each pass writes the next accumulation/status versions; the reduction reads the transport's.
    const auto appendPass = [&](const char* name,
                                rhi::ComputePipelineHandle pipeline,
                                uint32 groupsX,
                                uint32 groupsY) noexcept {
        rhi::GraphVersion nextAccumulation;
        rhi::GraphVersion nextStatus;
        Status result = rhi::NextGraphVersion(device, outGraph, accumulation, nextAccumulation);
        if (result == Status::Ready)
        {
            result = rhi::NextGraphVersion(device, outGraph, statusVersion, nextStatus);
        }
        if (result != Status::Ready)
        {
            return result;
        }
        accumulation = nextAccumulation;
        statusVersion = nextStatus;
        // GraphUse::Visibility defaults to RasterVisibility::Both; every entry sets it explicitly.
        // NOLINTBEGIN(bugprone-invalid-enum-default-initialization)
        const StaticArray<rhi::GraphUse, LUDUS_REFERENCE_BINDING_COUNT> uses{{
            {parameters, rhi::GraphAccessMode::Uniform, 0, sizeof(GpuPreviewUniform), rhi::RasterVisibility::Compute},
            {scene, rhi::GraphAccessMode::StorageRead, 0, compute.SceneBytes, rhi::RasterVisibility::Compute},
            {accumulation,
             rhi::GraphAccessMode::StorageReadWrite,
             0,
             accumulationBytes,
             rhi::RasterVisibility::Compute},
            {statusVersion,
             rhi::GraphAccessMode::StorageReadWrite,
             0,
             REFERENCE_COMPUTE_STATUS_BYTES,
             rhi::RasterVisibility::Compute},
            {texels, rhi::GraphAccessMode::StorageRead, 0, compute.TexelBytes, rhi::RasterVisibility::Compute},
        }};
        // NOLINTEND(bugprone-invalid-enum-default-initialization)
        const rhi::ComputeDispatch dispatch{pipeline, compute.Bindings, {groupsX, groupsY, 1}};
        rhi::GraphPassDescription description;
        description.Name = name;
        description.Uses = uses.GetData();
        description.UseCount = uses.GetSize();
        description.Dispatch = &dispatch;
        return rhi::AddGraphPass(device, outGraph, description);
    };
    if (status == Status::Ready)
    {
        status = appendPass("Reference compute transport",
                            pipelines.TracePipeline,
                            (job.Description.Width + LUDUS_REFERENCE_GROUP_WIDTH - 1) / LUDUS_REFERENCE_GROUP_WIDTH,
                            job.Description.Height);
    }
    if (status == Status::Ready)
    {
        status = appendPass("Reference compute reduction", pipelines.ReducePipeline, 1, 1);
    }
    if (status == Status::Ready)
    {
        status = rhi::AddGraphRoot(device, outGraph, accumulation, rhi::GraphRoot::History);
    }
    if (status == Status::Ready)
    {
        status = rhi::AddGraphRoot(device, outGraph, statusVersion, rhi::GraphRoot::History);
    }

    rhi::GraphReport report;
    if (status == Status::Ready)
    {
        status = rhi::CompileOrderedGraph(device, outGraph, report);
    }

    return status;
}
// Decode the reduced status of the completed dispatch into the capture evidence.
void ApplyStatus(const uint8* bytes, ReferenceJob& inoutJob) noexcept
{
    ReferenceCaptureInfo& info = inoutJob.Info;
    info.MinimumSamples = ReadWord(bytes, MINIMUM_SAMPLES_WORD);
    info.MaximumSamples = ReadWord(bytes, MAXIMUM_SAMPLES_WORD);
    info.CommittedSamples += ReadWord(bytes, COMMITTED_WORD);
    info.ClosestQueries += ReadWord(bytes, CLOSEST_WORD);
    info.VisibilityQueries += ReadWord(bytes, VISIBILITY_WORD);
    info.NullBsdfSamples += ReadWord(bytes, NULL_BSDF_WORD);
    info.RouletteTerminations += ReadWord(bytes, ROULETTE_WORD);
    const uint32 invalid = ReadWord(bytes, INVALID_WORD);
    if (invalid != 0)
    {
        info.InvalidContributions += invalid;
        info.FailurePixel = ReadWord(bytes, FAILURE_PIXEL_WORD);
        info.FailureDepth = ReadWord(bytes, FAILURE_DEPTH_WORD);
        inoutJob.Compute->Failure = ReadWord(bytes, CAPACITY_WORD) != 0 ? Status::CapacityExceeded : Status::Failed;
    }
}
// Copy one completed slice of committed sums into the host pixel array.
void ApplyPixels(BorrowedView<const uint8> bytes, usize firstPixel, ReferenceJob& inoutJob) noexcept
{
    const usize count = bytes.GetSize() / REFERENCE_COMPUTE_SUM_BYTES;
    for (usize index = 0; index < count; ++index)
    {
        const uint8* pixelBytes = bytes.GetDataOrNull() + index * REFERENCE_COMPUTE_SUM_BYTES;
        ReferenceAccumulator& pixel = inoutJob.Pixels[firstPixel + index];
        pixel.Emission = {ReadVector(pixelBytes, EMISSION_COMPONENT), {}};
        pixel.Direct = {ReadVector(pixelBytes, DIRECT_COMPONENT), {}};
        pixel.Indirect = {ReadVector(pixelBytes, INDIRECT_COMPONENT), {}};
        uint32 samples = 0;
        static_cast<void>(
            TryReadLittleEndian({pixelBytes + SAMPLES_COMPONENT * COMPONENT_BYTES, sizeof(uint32)}, samples));
        pixel.Samples = samples;
    }
}
Status AdvanceSetup(const ReferenceComputePipelines& pipelines, ReferenceJob& inoutJob) noexcept
{
    ReferenceComputeJob& compute = *inoutJob.Compute;
    const rhi::BufferHandle buffers[]{compute.Parameters,
                                      compute.Scene,
                                      compute.Texels,
                                      compute.Accumulation,
                                      compute.Status};
    for (const rhi::BufferHandle buffer : buffers)
    {
        const Status status = rhi::GetStatus(compute.Device, buffer);
        if (status != Status::Ready)
        {
            return Accepted(status) == true ? Status::Pending : Fail(inoutJob, status);
        }
    }
    if (compute.IsBindingsCreated == false)
    {
        const usize pixels = usize(inoutJob.Description.Width) * inoutJob.Description.Height;
        const StaticArray<rhi::RasterBindingResource, LUDUS_REFERENCE_BINDING_COUNT> resources{{
            {LUDUS_REFERENCE_PARAMETERS_BINDING, compute.Parameters, 0, sizeof(GpuPreviewUniform)},
            {LUDUS_REFERENCE_SCENE_BINDING, compute.Scene, 0, compute.SceneBytes},
            {LUDUS_REFERENCE_ACCUMULATION_BINDING, compute.Accumulation, 0, pixels * REFERENCE_COMPUTE_PIXEL_BYTES},
            {LUDUS_REFERENCE_STATUS_BINDING, compute.Status, 0, REFERENCE_COMPUTE_STATUS_BYTES},
            {LUDUS_REFERENCE_TEXELS_BINDING, compute.Texels, 0, compute.TexelBytes},
        }};
        const Status created = rhi::CreateBindingSet(compute.Device,
                                                     pipelines.Layout,
                                                     {resources.GetData(), resources.GetSize()},
                                                     compute.Bindings);
        if (Accepted(created) == false)
        {
            return Fail(inoutJob, created);
        }
        compute.IsBindingsCreated = true;
    }
    const Status bindings = rhi::GetStatus(compute.Device, compute.Bindings);
    if (bindings != Status::Ready)
    {
        return Accepted(bindings) == true ? Status::Pending : Fail(inoutJob, bindings);
    }
    inoutJob.Stage = ReferenceStage::Dispatch;
    return Status::Pending;
}
Status
AdvanceDispatch(const ReferenceComputePipelines& pipelines, rhi::SurfaceHandle surface, ReferenceJob& inoutJob) noexcept
{
    ReferenceComputeJob& compute = *inoutJob.Compute;
    rhi::OrderedGraph graph;
    Status status = BuildDispatchGraph(pipelines, inoutJob, graph);
    rhi::SubmissionToken completion;
    if (status == Status::Ready)
    {
        status = rhi::ExecuteOrderedGraph(compute.Device, surface, graph, completion);
    }
    static_cast<void>(rhi::DiscardOrderedGraph(compute.Device, graph));
    if (status == Status::NotReady)
    {
        return Status::Pending; // The frame target was skipped; retry on a later call.
    }
    if (status == Status::InvalidHandle)
    {
        return Status::InvalidDescription; // A foreign/null surface leaves the capture retryable.
    }
    if (status != Status::Ready)
    {
        return Fail(inoutJob, status);
    }
    compute.Dispatch = completion;
    inoutJob.Stage = ReferenceStage::WaitDispatch;
    return Status::Pending;
}
Status AdvanceWaitDispatch(ReferenceJob& inoutJob) noexcept
{
    ReferenceComputeJob& compute = *inoutJob.Compute;
    const Status completion = rhi::GetStatus(compute.Device, compute.Dispatch);
    if (completion != Status::Ready)
    {
        return completion == Status::Pending ? completion : Fail(inoutJob, completion);
    }
    const Status requested = rhi::RequestBufferReadback(compute.Device,
                                                        compute.Status,
                                                        0,
                                                        REFERENCE_COMPUTE_STATUS_BYTES,
                                                        compute.StatusReadback);
    if (requested == Status::CapacityExceeded)
    {
        return Status::Pending; // Every readback slice is busy; retry on a later call.
    }
    if (Accepted(requested) == false)
    {
        return Fail(inoutJob, requested);
    }
    inoutJob.Stage = ReferenceStage::ReadStatus;
    return Status::Pending;
}
Status AdvanceReadStatus(ReferenceJob& inoutJob) noexcept
{
    ReferenceComputeJob& compute = *inoutJob.Compute;
    Status status = rhi::GetStatus(compute.Device, compute.StatusReadback);
    if (status != Status::Ready)
    {
        return status == Status::Pending ? status : Fail(inoutJob, status);
    }
    StaticArray<uint8, REFERENCE_COMPUTE_STATUS_BYTES> bytes{};
    status = rhi::CopyReadback(compute.Device, compute.StatusReadback, bytes.GetData(), bytes.GetSize());
    static_cast<void>(rhi::Release(compute.Device, compute.StatusReadback));
    if (status != Status::Ready)
    {
        return Fail(inoutJob, status);
    }
    static_assert(STATUS_WORD_COUNT > CAPACITY_WORD, "status words cover every decoded field");
    ApplyStatus(bytes.GetData(), inoutJob);
    if (compute.Failure == Status::Ready &&
        IsReadbackDue(inoutJob.Info.MinimumSamples, inoutJob.Description.TargetSamples) == false)
    {
        inoutJob.Stage = ReferenceStage::Dispatch;
        return Status::Pending;
    }
    compute.NextReadPixel = 0;
    compute.ReadPixels = 0;
    inoutJob.Stage = ReferenceStage::ReadPixels;
    return Status::Pending;
}
Status AdvanceReadPixels(ReferenceJob& inoutJob) noexcept
{
    ReferenceComputeJob& compute = *inoutJob.Compute;
    const usize pixels = inoutJob.Pixels.GetSize();
    for (usize slot = 0; slot < REFERENCE_COMPUTE_READBACKS; ++slot)
    {
        rhi::ReadbackTicket& ticket = compute.PixelReadbacks[slot];
        if (compute.ReadbackPixelCounts[slot] != 0)
        {
            Status status = rhi::GetStatus(compute.Device, ticket);
            if (status == Status::Pending)
            {
                continue;
            }
            if (status == Status::Ready)
            {
                status = rhi::CopyReadback(compute.Device, ticket, compute.Chunk.GetData(), compute.Chunk.GetSize());
            }
            static_cast<void>(rhi::Release(compute.Device, ticket));
            if (status != Status::Ready)
            {
                return Fail(inoutJob, status);
            }
            ApplyPixels({compute.Chunk.GetData(), compute.ReadbackPixelCounts[slot] * REFERENCE_COMPUTE_SUM_BYTES},
                        compute.ReadbackFirstPixels[slot],
                        inoutJob);
            compute.ReadPixels += compute.ReadbackPixelCounts[slot];
            compute.ReadbackPixelCounts[slot] = 0;
        }
        if (compute.NextReadPixel < pixels)
        {
            const usize count = math::Min(compute.ChunkPixels, pixels - compute.NextReadPixel);
            const Status requested = rhi::RequestBufferReadback(compute.Device,
                                                                compute.Accumulation,
                                                                compute.NextReadPixel * REFERENCE_COMPUTE_SUM_BYTES,
                                                                count * REFERENCE_COMPUTE_SUM_BYTES,
                                                                ticket);
            if (requested == Status::CapacityExceeded)
            {
                break; // Every readback slice is busy; retry on a later call.
            }
            if (Accepted(requested) == false)
            {
                return Fail(inoutJob, requested);
            }
            compute.ReadbackFirstPixels[slot] = compute.NextReadPixel;
            compute.ReadbackPixelCounts[slot] = count;
            compute.NextReadPixel += count;
        }
    }
    if (compute.ReadPixels < pixels)
    {
        return Status::Pending;
    }
    if (compute.Failure != Status::Ready)
    {
        return Fail(inoutJob, compute.Failure);
    }
    inoutJob.Info.Complete = inoutJob.Info.MinimumSamples >= inoutJob.Description.TargetSamples;
    inoutJob.Stage = inoutJob.Info.Complete == true ? ReferenceStage::Complete : ReferenceStage::Dispatch;
    return inoutJob.Info.Complete == true ? Status::Ready : Status::Pending;
}
} // namespace
ReferenceComputeJob::~ReferenceComputeJob() noexcept
{
    static_cast<void>(rhi::Release(Device, StatusReadback));
    for (rhi::ReadbackTicket& ticket : PixelReadbacks)
    {
        static_cast<void>(rhi::Release(Device, ticket));
    }
    static_cast<void>(rhi::Destroy(Device, Bindings));
    static_cast<void>(rhi::Destroy(Device, Parameters));
    static_cast<void>(rhi::Destroy(Device, Scene));
    static_cast<void>(rhi::Destroy(Device, Texels));
    static_cast<void>(rhi::Destroy(Device, Accumulation));
    static_cast<void>(rhi::Destroy(Device, Status));
}
Status CreateReferenceComputePipelines(rhi::DeviceHandle device,
                                       const rhi::RasterShaderDescription& trace,
                                       const rhi::RasterShaderDescription& reduce,
                                       ReferenceComputePipelines& outPipelines) noexcept
{
    rhi::ComputeCapabilities capabilities;
    const Status supported = rhi::GetComputeCapabilities(device, capabilities);
    if (supported != Status::Ready)
    {
        return supported;
    }
    if (capabilities.MaxStorageBindings < LUDUS_REFERENCE_BINDING_COUNT - 1)
    {
        return Status::Unsupported;
    }
    if (IsExpectedKernel(trace, true) == false || IsExpectedKernel(reduce, false) == false)
    {
        return Status::InvalidDescription;
    }
    ReferenceComputePipelines pipelines;
    Status status = rhi::CreateRasterShader(device, trace, pipelines.TraceShader);
    if (Accepted(status) == true)
    {
        status = rhi::CreateRasterShader(device, reduce, pipelines.ReduceShader);
    }
    if (Accepted(status) == false)
    {
        DestroyReferenceComputePipelines(device, pipelines);
        return status;
    }
    outPipelines = pipelines;
    return Status::Ready;
}
Status PollReferenceComputePipelines(rhi::DeviceHandle device, ReferenceComputePipelines& inoutPipelines) noexcept
{
    constexpr uint32 LAYOUT_PHASE = 0;
    constexpr uint32 TRACE_PHASE = 1;
    constexpr uint32 REDUCE_PHASE = 2;
    ReferenceComputePipelines& pipelines = inoutPipelines;
    while (pipelines.Setup == Status::Pending)
    {
        Status status = Status::Ready;
        bool isCreated = false;
        if (pipelines.Phase == LAYOUT_PHASE)
        {
            status = rhi::GetStatus(device, pipelines.TraceShader);
            if (status == Status::Ready)
            {
                status = rhi::GetStatus(device, pipelines.ReduceShader);
            }
            if (status == Status::Ready)
            {
                status = rhi::CreateBindingLayout(device,
                                                  {COMPUTE_BINDINGS.GetData(), COMPUTE_BINDINGS.GetSize()},
                                                  pipelines.Layout);
                isCreated = Accepted(status);
            }
        }
        else if (pipelines.Phase == TRACE_PHASE)
        {
            status = rhi::GetStatus(device, pipelines.Layout);
            if (status == Status::Ready)
            {
                status = rhi::CreateComputePipeline(device,
                                                    {pipelines.TraceShader, pipelines.Layout},
                                                    pipelines.TracePipeline);
                isCreated = Accepted(status);
            }
        }
        else if (pipelines.Phase == REDUCE_PHASE)
        {
            status = rhi::CreateComputePipeline(device,
                                                {pipelines.ReduceShader, pipelines.Layout},
                                                pipelines.ReducePipeline);
            isCreated = Accepted(status);
        }
        else
        {
            status = rhi::GetStatus(device, pipelines.TracePipeline);
            if (status == Status::Ready)
            {
                status = rhi::GetStatus(device, pipelines.ReducePipeline);
            }
            if (status == Status::Ready)
            {
                pipelines.Setup = Status::Ready;
                break;
            }
        }
        if (Accepted(status) == false)
        {
            pipelines.Setup = status;
            break;
        }
        // A creator may publish Pending: the phase advances once its output exists.
        if (isCreated == true)
        {
            ++pipelines.Phase;
        }
        if (status == Status::Pending)
        {
            return status;
        }
    }
    return pipelines.Setup;
}
void DestroyReferenceComputePipelines(rhi::DeviceHandle device, ReferenceComputePipelines& inoutPipelines) noexcept
{
    static_cast<void>(rhi::Destroy(device, inoutPipelines.TracePipeline));
    static_cast<void>(rhi::Destroy(device, inoutPipelines.ReducePipeline));
    static_cast<void>(rhi::Destroy(device, inoutPipelines.Layout));
    static_cast<void>(rhi::Destroy(device, inoutPipelines.TraceShader));
    static_cast<void>(rhi::Destroy(device, inoutPipelines.ReduceShader));
    inoutPipelines = {};
}
Status CreateReferenceComputeJob(const ReferenceComputeInputs& inputs, ReferenceJob& inoutJob) noexcept
{
    const rhi::DeviceHandle device = inoutJob.Device;
    const uint32 width = inoutJob.Description.Width;
    const uint32 height = inoutJob.Description.Height;
    const usize pixels = usize(width) * height;
    const usize sceneBytes = inputs.Scene.GetSize() * sizeof(math::Vector4);
    const usize texelBytes = inputs.Texels.GetSize() * sizeof(math::Vector4);
    usize accumulationBytes = 0;
    usize gpuBytes = 0;
    if (pixels > LUDUS_REFERENCE_INVALID_PIXEL ||
        TryMultiply(pixels, REFERENCE_COMPUTE_PIXEL_BYTES, accumulationBytes) == false ||
        TryAdd(sceneBytes, texelBytes, gpuBytes) == false || TryAdd(gpuBytes, accumulationBytes, gpuBytes) == false ||
        TryAdd(gpuBytes, inputs.Parameters.GetSize() + REFERENCE_COMPUTE_STATUS_BYTES, gpuBytes) == false ||
        inputs.Zeros.GetSize() < math::Max(accumulationBytes, REFERENCE_COMPUTE_STATUS_BYTES))
    {
        return Status::CapacityExceeded;
    }
    if (gpuBytes > inputs.GpuByteLimit)
    {
        return Status::CapacityExceeded;
    }
    rhi::ComputeCapabilities compute;
    rhi::LifetimeCapabilities lifetime;
    rhi::StorageUsage usage;
    rhi::DeviceInfo deviceInformation;
    Status status = rhi::GetComputeCapabilities(device, compute);
    if (status == Status::Ready)
    {
        status = rhi::GetLifetimeCapabilities(device, lifetime);
    }
    if (status == Status::Ready)
    {
        status = rhi::GetStorageUsage(device, usage);
    }
    if (status == Status::Ready && rhi::GetDeviceInfo(device, deviceInformation) != rhi::DeviceStatus::Ready)
    {
        status = Status::NotReady;
    }
    if (status != Status::Ready)
    {
        return status;
    }
    const uint64 resourceBudget = deviceInformation.Profile.Storage.ResourceBytes;
    const uint64 resourceRemaining = resourceBudget - math::Min(resourceBudget, usage.ResourceBytes);
    if (gpuBytes > resourceRemaining ||
        math::Max(math::Max(sceneBytes, texelBytes), accumulationBytes) > compute.MaxStorageRange ||
        (width + LUDUS_REFERENCE_GROUP_WIDTH - 1) / LUDUS_REFERENCE_GROUP_WIDTH > compute.MaxDispatch[0] ||
        height > compute.MaxDispatch[1])
    {
        return Status::CapacityExceeded;
    }
    const usize chunkPixels = lifetime.TransferBytes / REFERENCE_COMPUTE_SUM_BYTES;
    const usize cpuBytes = sizeof(ReferenceComputeJob) + chunkPixels * REFERENCE_COMPUTE_SUM_BYTES;
    if (chunkPixels == 0 || cpuBytes > inputs.CpuByteLimit)
    {
        return chunkPixels == 0 ? Status::Unsupported : Status::CapacityExceeded;
    }
    ReferenceComputeJob* allocation = new (std::nothrow) ReferenceComputeJob;
    if (allocation == nullptr)
    {
        return Status::OutOfMemory;
    }
    core::UniquePtr<ReferenceComputeJob> job(allocation);
    job->Device = device;
    job->SceneBytes = sceneBytes;
    job->TexelBytes = texelBytes;
    job->GpuBytes = gpuBytes;
    job->ChunkPixels = chunkPixels;
    if (job->Chunk.TryResize(chunkPixels * REFERENCE_COMPUTE_SUM_BYTES) == false)
    {
        return Status::OutOfMemory;
    }
    status = rhi::CreateBuffer(device,
                               {rhi::BufferRole::Storage, sceneBytes},
                               {reinterpret_cast<const uint8*>(inputs.Scene.GetDataOrNull()), sceneBytes},
                               job->Scene);
    if (Accepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Storage, texelBytes},
                                   {reinterpret_cast<const uint8*>(inputs.Texels.GetDataOrNull()), texelBytes},
                                   job->Texels);
    }
    if (Accepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Uniform, inputs.Parameters.GetSize()},
                                   {inputs.Parameters.GetDataOrNull(), inputs.Parameters.GetSize()},
                                   job->Parameters);
    }
    if (Accepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Storage, accumulationBytes},
                                   {inputs.Zeros.GetDataOrNull(), accumulationBytes},
                                   job->Accumulation);
    }
    if (Accepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Storage, REFERENCE_COMPUTE_STATUS_BYTES},
                                   {inputs.Zeros.GetDataOrNull(), REFERENCE_COMPUTE_STATUS_BYTES},
                                   job->Status);
    }
    if (Accepted(status) == false)
    {
        return status;
    }
    inoutJob.Info.CpuBytes += cpuBytes;
    inoutJob.Compute = core::Move(job);
    inoutJob.Stage = ReferenceStage::Setup;
    return Status::Pending;
}
Status AdvanceReferenceCompute(const ReferenceComputePipelines& pipelines,
                               rhi::SurfaceHandle surface,
                               ReferenceJob& inoutJob) noexcept
{
    if (inoutJob.Compute.Get() == nullptr)
    {
        return Fail(inoutJob, Status::InvalidState);
    }
    switch (inoutJob.Stage)
    {
        case ReferenceStage::Setup:
            return AdvanceSetup(pipelines, inoutJob);
        case ReferenceStage::Dispatch:
            return AdvanceDispatch(pipelines, surface, inoutJob);
        case ReferenceStage::WaitDispatch:
            return AdvanceWaitDispatch(inoutJob);
        case ReferenceStage::ReadStatus:
            return AdvanceReadStatus(inoutJob);
        case ReferenceStage::ReadPixels:
            return AdvanceReadPixels(inoutJob);
        case ReferenceStage::Complete:
            return Status::Ready;
        case ReferenceStage::Failed:
            return Status::Failed;
        default:
            return Fail(inoutJob, Status::InvalidState);
    }
}
Status ResumeReferenceCompute(ReferenceJob& inoutJob) noexcept
{
    ReferenceComputeJob& compute = *inoutJob.Compute;
    const bool isIdle = inoutJob.Stage == ReferenceStage::Complete || inoutJob.Stage == ReferenceStage::Failed ||
                        inoutJob.Stage == ReferenceStage::Dispatch;
    inoutJob.Info.Complete = false;
    if (isIdle == false)
    {
        return Status::Pending; // In-flight work compares the new target when its status arrives.
    }
    if (inoutJob.Stage == ReferenceStage::Complete &&
        inoutJob.Info.MinimumSamples >= inoutJob.Description.TargetSamples)
    {
        inoutJob.Info.Complete = true;
        return Status::Ready;
    }
    compute.Failure = Status::Ready;
    if (inoutJob.Info.MinimumSamples >= inoutJob.Description.TargetSamples)
    {
        // A lowered target may sit between readbacks: publish the committed sums first.
        compute.NextReadPixel = 0;
        compute.ReadPixels = 0;
        inoutJob.Stage = ReferenceStage::ReadPixels;
        return Status::Pending;
    }
    inoutJob.Stage = ReferenceStage::Dispatch;
    return Status::Pending;
}
} // namespace ludus::graphics::renderer::internal
