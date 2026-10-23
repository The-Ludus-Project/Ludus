#include "internal/gpu_denoiser.h"

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/worker_queue.hpp>
#include <ludus/graphics/rhi/native_compute.h>

#if defined(LUDUS_ENABLE_OIDN)
#    include <new>

#    include <OpenImageDenoise/oidn.h>
#endif

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
using Status = rhi::RasterStatus;
namespace format = content::render_graph_format;
namespace
{
#if defined(LUDUS_ENABLE_OIDN)
constexpr usize DENOISER_BINDING_COUNT = 1;
// Thanks to Intel/RenderKit, "Open Image Denoise API", RT filter, Metal buffer
// sharing and asynchronous execution: https://www.openimagedenoise.org/documentation.html
// Use the C status API, one cached RT filter per job and an RHI-managed private Metal queue.
// This initial color-only profile preserves raw accumulation and does not reproject history.
struct OidnState final
{
    usize References = 1;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 Quality = 0;
    threading::WorkerQueue Worker;
    rhi::NativeDeviceIdentity Identity{};
    rhi::NativeComputeBuffer Input{};
    Status Result = Status::Pending;
    bool HasStarted = false;
    bool IsComplete = false;
    OIDNDevice DeviceOrNull = nullptr;
    OIDNBuffer ColorsOrNull = nullptr;
    OIDNFilter FilterOrNull = nullptr;
    ~OidnState() noexcept
    {
        // Only shutdown can retire a still-preparing pipeline. Join before releasing
        // the device or any native buffer borrowed by its initialization task.
        static_cast<void>(Worker.Shutdown());
        if (FilterOrNull != nullptr)
        {
            oidnReleaseFilter(FilterOrNull);
        }
        if (ColorsOrNull != nullptr)
        {
            oidnReleaseBuffer(ColorsOrNull);
        }
        if (DeviceOrNull != nullptr)
        {
            oidnReleaseDevice(DeviceOrNull);
        }
    }
};
Status GetOidnStatus(OIDNDevice deviceOrNull) noexcept
{
    const OIDNError error = oidnGetDeviceError(deviceOrNull, nullptr);
    switch (error)
    {
        case OIDN_ERROR_NONE:
            return Status::Ready;
        case OIDN_ERROR_OUT_OF_MEMORY:
            return Status::OutOfMemory;
        case OIDN_ERROR_UNSUPPORTED_HARDWARE:
            return Status::Unsupported;
        default:
            return Status::Failed;
    }
}
void RetainOidn(void* contextOrNull) noexcept
{
    ++static_cast<OidnState*>(contextOrNull)->References;
}
void DrainOidn(void* contextOrNull) noexcept
{
    static_cast<void>(static_cast<OidnState*>(contextOrNull)->Worker.Shutdown());
}
void ReleaseOidn(void* contextOrNull) noexcept
{
    OidnState* state = static_cast<OidnState*>(contextOrNull);
    if (--state->References == 0)
    {
        delete state;
    }
}
void InitializeOidn(void* contextOrNull) noexcept
{
    OidnState& state = *static_cast<OidnState*>(contextOrNull);
    const rhi::NativeDeviceIdentity& device = state.Identity;
    state.Result = [&]() noexcept -> Status {
        if (device.Api != rhi::Backend::Metal || oidnIsMetalDeviceSupported(device.Device.GetObject()) == false)
        {
            return Status::Unsupported;
        }
        MTLCommandQueue_id queue = device.CommandQueue.GetObject();
        state.DeviceOrNull = oidnNewMetalDevice(&queue, 1);
        if (state.DeviceOrNull == nullptr)
        {
            const Status status = GetOidnStatus(nullptr);
            return status == Status::Ready ? Status::Failed : status;
        }
        oidnCommitDevice(state.DeviceOrNull);
        Status status = GetOidnStatus(state.DeviceOrNull);
        if (status != Status::Ready)
        {
            return status;
        }
        state.ColorsOrNull = oidnNewSharedBufferFromMetal(state.DeviceOrNull, state.Input.ObjectOrNull);
        state.FilterOrNull = oidnNewFilter(state.DeviceOrNull, "RT");
        if (state.ColorsOrNull == nullptr || state.FilterOrNull == nullptr)
        {
            status = GetOidnStatus(state.DeviceOrNull);
            return status == Status::Ready ? Status::Failed : status;
        }
        const usize pixelStride = sizeof(math::Vector4);
        const usize rowStride = usize{state.Width} * pixelStride;
        oidnSetFilterImage(state.FilterOrNull,
                           "color",
                           state.ColorsOrNull,
                           OIDN_FORMAT_FLOAT3,
                           state.Width,
                           state.Height,
                           state.Input.Offset,
                           pixelStride,
                           rowStride);
        oidnSetFilterImage(state.FilterOrNull,
                           "output",
                           state.ColorsOrNull,
                           OIDN_FORMAT_FLOAT3,
                           state.Width,
                           state.Height,
                           state.Input.Offset,
                           pixelStride,
                           rowStride);
        oidnSetFilterBool(state.FilterOrNull, "hdr", true);
        const OIDNQuality quality =
            state.Quality == static_cast<uint32>(format::ProcessingQuality::Fast)       ? OIDN_QUALITY_FAST
            : state.Quality == static_cast<uint32>(format::ProcessingQuality::Balanced) ? OIDN_QUALITY_BALANCED
                                                                                        : OIDN_QUALITY_HIGH;
        oidnSetFilterInt(state.FilterOrNull, "quality", quality);
        // A library scratch target, not a hard allocation ceiling. Model/driver allocations
        // are outside Renderer resource accounting, as documented in the graph guide.
        constexpr int32 SCRATCH_TARGET_MEBIBYTES = 256;
        oidnSetFilterInt(state.FilterOrNull, "maxMemoryMB", SCRATCH_TARGET_MEBIBYTES);
        oidnCommitFilter(state.FilterOrNull);
        return GetOidnStatus(state.DeviceOrNull);
    }();
}
void CompleteOidn(void* contextOrNull) noexcept
{
    static_cast<OidnState*>(contextOrNull)->IsComplete = true;
}
Status PrepareOidn(void* contextOrNull,
                   const rhi::NativeDeviceIdentity& device,
                   BorrowedView<const rhi::NativeComputeBuffer> buffers) noexcept
{
    OidnState& state = *static_cast<OidnState*>(contextOrNull);
    if (device.Api != rhi::Backend::Metal || buffers.GetSize() != DENOISER_BINDING_COUNT)
    {
        return Status::Unsupported;
    }
    if (state.HasStarted == false)
    {
        state.Identity = device;
        state.Input = buffers[0];
        if (state.Worker.Initialize() != threading::WorkerQueueStatus::Ready)
        {
            return Status::OutOfMemory;
        }
        if (state.Worker.Submit(InitializeOidn, CompleteOidn, &state) != threading::WorkerQueueStatus::Ready)
        {
            return Status::Failed;
        }
        state.HasStarted = true;
    }
    static_cast<void>(state.Worker.Poll());
    return state.IsComplete == true ? state.Result : Status::Pending;
}
// Thanks to Apple and Intel, Open Image Denoise 2.5.1, MetalEngine::flush, for
// committing native work before
// returning, which lets the following native event cover its completion:
// https://github.com/RenderKit/oidn/blob/v2.5.1/devices/metal/metal_engine.mm
Status SubmitOidn(void* contextOrNull) noexcept
{
    OidnState& state = *static_cast<OidnState*>(contextOrNull);
    oidnExecuteFilterAsync(state.FilterOrNull);
    return GetOidnStatus(state.DeviceOrNull);
}
#endif
} // namespace
bool IsGpuDenoisingAvailable() noexcept
{
#if defined(LUDUS_ENABLE_OIDN)
    return true;
#else
    return false;
#endif
}
GpuDenoiser::~GpuDenoiser() noexcept
{
    Reset();
}
Status GpuDenoiser::Prepare(rhi::DeviceHandle device,
                            rhi::BufferHandle colors,
                            uint32 width,
                            uint32 height,
                            uint32 quality) noexcept
{
    if (quality == 0 || quality > static_cast<uint32>(format::ProcessingQuality::High) || width == 0 || height == 0)
    {
        return Status::InvalidDescription;
    }
    if (mHasPipeline == true)
    {
        return rhi::GetStatus(mDevice, mPipeline);
    }
#if defined(LUDUS_ENABLE_OIDN)
    usize bytes = 0;
    if (TryMultiply(usize{width}, usize{height}, bytes) == false ||
        TryMultiply(bytes, sizeof(math::Vector4), bytes) == false)
    {
        return Status::CapacityExceeded;
    }
    mDevice = device;
    const StaticArray<rhi::RasterBinding, DENOISER_BINDING_COUNT> layout{{
        {0, rhi::RasterBindingKind::StorageReadWrite, rhi::RasterVisibility::Compute, sizeof(math::Vector4)},
    }};
    const StaticArray<rhi::RasterBindingResource, DENOISER_BINDING_COUNT> bindings{{{0, colors, 0, bytes}}};
    Status status = rhi::CreateBindingLayout(device, layout.AsSpan(), mLayout);
    if (status == Status::Ready)
    {
        status = rhi::CreateBindingSet(device, mLayout, bindings.AsSpan(), mBindings);
    }
    if (status != Status::Ready)
    {
        Reset();
        return status;
    }
    OidnState* stateOrNull = new (std::nothrow) OidnState;
    if (stateOrNull == nullptr)
    {
        Reset();
        return Status::OutOfMemory;
    }
    stateOrNull->Width = width;
    stateOrNull->Height = height;
    stateOrNull->Quality = quality;
    const rhi::NativeComputePipelineDescription description
    {
        .Bindings = mBindings,
        .ContextOrNull = stateOrNull,
        .PrepareOrNull = PrepareOidn,
        .SubmitOrNull = SubmitOidn,
        .RetainOrNull = RetainOidn,
        .ReleaseOrNull = ReleaseOidn,
        .DrainOrNull = DrainOidn,
    };
    status = rhi::CreateNativeComputePipeline(device, description, mPipeline);
    ReleaseOidn(stateOrNull);
    if (status != Status::Ready && status != Status::Pending)
    {
        Reset();
        return status;
    }
    mHasPipeline = true;
    return status;
#else
    (void)device;
    (void)colors;
    return Status::Unsupported;
#endif
}
void GpuDenoiser::Reset() noexcept
{
    static_cast<void>(rhi::Destroy(mDevice, mPipeline));
    static_cast<void>(rhi::Destroy(mDevice, mBindings));
    static_cast<void>(rhi::Destroy(mDevice, mLayout));
    mPipeline = {};
    mBindings = {};
    mLayout = {};
    mHasPipeline = false;
}
rhi::ComputeDispatch GpuDenoiser::GetDispatch() const noexcept
{
    return {mPipeline, mBindings, {1, 1, 1}};
}
} // namespace ludus::graphics::renderer::internal
