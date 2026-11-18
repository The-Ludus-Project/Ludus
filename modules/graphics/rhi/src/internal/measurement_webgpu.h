#pragma once
// Thanks to W3C/GPUWeb, WebGPU, Timestamp Queries and Buffer Mapping:
// https://www.w3.org/TR/webgpu/#timestamp-query and https://www.w3.org/TR/webgpu/#buffer-mapping
// Two empty compute marker passes bound all frame encoders, including graph pass transitions.
// Precision/quantization is deliberately unknown; generation guards discard late map callbacks.
namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE
{
namespace
{
/// Timestamps of one frame sample: both ends of the first and of the last marker pass.
constexpr uint32 TIMING_QUERY_COUNT = 4;
/// Index of the timestamp that closes the frame interval opened by index 0.
constexpr usize TIMING_LAST_QUERY = 3;
/// Bytes of the resolved timestamps of one sample.
constexpr uint64 TIMING_READBACK_BYTES = uint64{TIMING_QUERY_COUNT} * sizeof(uint64);
/// @brief Frame timestamp queries, readback and sample state of one device owner (R14c).
struct MeasurementWebGpuState final
{
    GpuTiming Timing;
    WGPUQuerySet Queries = nullptr;
    WGPUBuffer Resolve = nullptr;
    WGPUBuffer Readback = nullptr;
    internal::RasterSequence<uint32> Generations;
    uint32 Generation = 0;
    bool IsMapRequested = false;
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<MeasurementWebGpuState, MAX_DEVICE_OWNERS> gMeasurementWebGpuStates{};
/// MeasurementWebGpuState of the bound owner slot.
MeasurementWebGpuState& GetMeasurementWebGpu() noexcept
{
    return gMeasurementWebGpuStates[internal::GetBoundOwnerSlot()];
}
void TimingMapped(WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void* session) noexcept
{
    // Generations count per owner (R14c); the session token binds the owner that mapped.
    if (internal::Current(Token(session)) == false)
    {
        return;
    }
    if (Token(userdata) != GetMeasurementWebGpu().Generation || GetMeasurementWebGpu().Readback == nullptr)
    {
        return;
    }
    if (status != WGPUMapAsyncStatus_Success)
    {
        GetMeasurementWebGpu().Timing.State = GpuTimingState::Failed;
        return;
    }
    const void* mapped = wgpuBufferGetConstMappedRange(GetMeasurementWebGpu().Readback, 0, TIMING_READBACK_BYTES);
    uint64 values[TIMING_QUERY_COUNT]{};
    if (mapped != nullptr)
    {
        std::memcpy(values, mapped, sizeof(values));
    }
    wgpuBufferUnmap(GetMeasurementWebGpu().Readback);
    if (Token(userdata) != GetMeasurementWebGpu().Generation)
    {
        return;
    }
    if (mapped != nullptr && values[TIMING_LAST_QUERY] >= values[0])
    {
        GetMeasurementWebGpu().Timing.DurationNanoseconds = static_cast<float64>(values[TIMING_LAST_QUERY] - values[0]);
        GetMeasurementWebGpu().Timing.State = GpuTimingState::Available;
    }
    else
    {
        GetMeasurementWebGpu().Timing.State = GpuTimingState::Disjoint;
    }
}
void TimingMarker(bool first) noexcept
{
    WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
    writes.querySet = GetMeasurementWebGpu().Queries;
    // The pinned emdawn bridge forwards UINT32_MAX instead of omitting absent indices.
    // Use both endpoints in each marker; indices 0 and 3 enclose the entire frame.
    writes.beginningOfPassWriteIndex = first ? 0 : 2;
    writes.endOfPassWriteIndex = first ? 1 : 3;
    WGPUComputePassDescriptor description = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    description.timestampWrites = &writes;
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(GetWebGpu().Encoder, &description);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
}
} // namespace
bool AllocatorBlockBytes(uint64& bytes) noexcept
{
    bytes = 0;
    return false;
}
MeasurementAdapter TimingAdapter() noexcept
{
    MeasurementAdapter output;
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(GetWebGpu().Adapter, &info) == WGPUStatus_Success)
    {
        internal::CopyMeasurementName(info.description.data, info.description.length, output.Name);
        if (output.Name[0] == '\0')
        {
            internal::CopyMeasurementName(info.device.data, info.device.length, output.Name);
        }
        output.VendorId = info.vendorID;
        output.DeviceId = info.deviceID;
        output.Software = info.adapterType == WGPUAdapterType_CPU;
        output.SoftwareKnown = output.Software || info.adapterType == WGPUAdapterType_IntegratedGPU ||
                               info.adapterType == WGPUAdapterType_DiscreteGPU;
    }
    wgpuAdapterInfoFreeMembers(info);
    return output;
}
void TimingReset() noexcept
{
    GetMeasurementWebGpu().Generation = 0;
    GetMeasurementWebGpu().IsMapRequested = false;
    if (GetMeasurementWebGpu().Queries != nullptr)
    {
        wgpuQuerySetDestroy(GetMeasurementWebGpu().Queries);
        wgpuQuerySetRelease(GetMeasurementWebGpu().Queries);
    }
    if (GetMeasurementWebGpu().Resolve != nullptr)
    {
        wgpuBufferDestroy(GetMeasurementWebGpu().Resolve);
        wgpuBufferRelease(GetMeasurementWebGpu().Resolve);
    }
    if (GetMeasurementWebGpu().Readback != nullptr)
    {
        wgpuBufferDestroy(GetMeasurementWebGpu().Readback);
        wgpuBufferRelease(GetMeasurementWebGpu().Readback);
    }
    GetMeasurementWebGpu().Queries = nullptr;
    GetMeasurementWebGpu().Resolve = nullptr;
    GetMeasurementWebGpu().Readback = nullptr;
    GetMeasurementWebGpu().Timing = {};
}
GpuTiming TimingPoll() noexcept
{
    return GetMeasurementWebGpu().Timing;
}
RasterStatus TimingArm() noexcept
{
    if (GetMeasurementWebGpu().Timing.State == GpuTimingState::Armed ||
        GetMeasurementWebGpu().Timing.State == GpuTimingState::Pending)
    {
        return RasterStatus::InvalidState;
    }
    if (wgpuDeviceHasFeature(GetWebGpu().Device, WGPUFeatureName_TimestampQuery) == false)
    {
        GetMeasurementWebGpu().Timing = { .State = GpuTimingState::Unavailable };
        return RasterStatus::Unsupported;
    }
    internal::TimingArmRollback rollback(GetMeasurementWebGpu().Timing);
    TimingReset();
    GetMeasurementWebGpu().Generation = GetMeasurementWebGpu().Generations.Take();
    if (GetMeasurementWebGpu().Generation == 0)
    {
        return RasterStatus::CapacityExceeded;
    }
    WGPUQuerySetDescriptor query = WGPU_QUERY_SET_DESCRIPTOR_INIT;
    query.type = WGPUQueryType_Timestamp;
    query.count = TIMING_QUERY_COUNT;
    GetMeasurementWebGpu().Queries = wgpuDeviceCreateQuerySet(GetWebGpu().Device, &query);
    WGPUBufferDescriptor buffer = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer.size = TIMING_READBACK_BYTES;
    buffer.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
    GetMeasurementWebGpu().Resolve = wgpuDeviceCreateBuffer(GetWebGpu().Device, &buffer);
    buffer.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    GetMeasurementWebGpu().Readback = wgpuDeviceCreateBuffer(GetWebGpu().Device, &buffer);
    if (GetMeasurementWebGpu().Queries == nullptr || GetMeasurementWebGpu().Resolve == nullptr ||
        GetMeasurementWebGpu().Readback == nullptr)
    {
        TimingReset();
        return RasterStatus::OutOfMemory;
    }
    GetMeasurementWebGpu().Timing.State = GpuTimingState::Armed;
    rollback.Accept();
    return RasterStatus::Ready;
}
void TimingBegin() noexcept
{
    if (GetMeasurementWebGpu().Timing.State == GpuTimingState::Armed)
    {
        TimingMarker(true);
    }
}
void TimingEnd() noexcept
{
    if (GetMeasurementWebGpu().Timing.State != GpuTimingState::Armed)
    {
        return;
    }
    TimingMarker(false);
    wgpuCommandEncoderResolveQuerySet(GetWebGpu().Encoder,
                                      GetMeasurementWebGpu().Queries,
                                      0,
                                      TIMING_QUERY_COUNT,
                                      GetMeasurementWebGpu().Resolve,
                                      0);
    wgpuCommandEncoderCopyBufferToBuffer(GetWebGpu().Encoder,
                                         GetMeasurementWebGpu().Resolve,
                                         0,
                                         GetMeasurementWebGpu().Readback,
                                         0,
                                         TIMING_READBACK_BYTES);
    GetMeasurementWebGpu().Timing.State = GpuTimingState::Pending;
}
void TimingSubmitted() noexcept
{
    if (GetMeasurementWebGpu().Timing.State != GpuTimingState::Pending || GetMeasurementWebGpu().Generation == 0 ||
        GetMeasurementWebGpu().IsMapRequested)
    {
        return;
    }
    GetMeasurementWebGpu().IsMapRequested = true;
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = TimingMapped;
    callback.userdata1 = Userdata(GetMeasurementWebGpu().Generation);
    callback.userdata2 = Userdata(GetWebGpu().Session);
    (void)wgpuBufferMapAsync(GetMeasurementWebGpu().Readback, WGPUMapMode_Read, 0, TIMING_READBACK_BYTES, callback);
}
bool RasterAllocationBytes(internal::RasterKind, usize, uint64& bytes) noexcept
{
    bytes = 0;
    return false;
}
} // namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE
