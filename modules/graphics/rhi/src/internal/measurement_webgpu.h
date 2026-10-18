#pragma once
// Thanks to W3C/GPUWeb, WebGPU, Timestamp Queries and Buffer Mapping:
// https://www.w3.org/TR/webgpu/#timestamp-query and https://www.w3.org/TR/webgpu/#buffer-mapping
// Two empty compute marker passes bound all frame encoders, including graph pass transitions.
// Precision/quantization is deliberately unknown; generation guards discard late map callbacks.
namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE
{
namespace
{
GpuTiming gTiming;
WGPUQuerySet gTimingQueries = nullptr;
WGPUBuffer gTimingResolve = nullptr, gTimingReadback = nullptr;
internal::RasterSequence<uint32> gTimingGenerations;
uint32 gTimingGeneration = 0;
bool gTimingMapRequested = false;
void TimingMapped(WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void*) noexcept
{
    if (Token(userdata) != gTimingGeneration || gTimingReadback == nullptr)
    {
        return;
    }
    if (status != WGPUMapAsyncStatus_Success)
    {
        gTiming.State = GpuTimingState::Failed;
        return;
    }
    const auto* mapped = wgpuBufferGetConstMappedRange(gTimingReadback, 0, 32);
    uint64 values[4]{};
    if (mapped != nullptr)
    {
        std::memcpy(values, mapped, sizeof(values));
    }
    wgpuBufferUnmap(gTimingReadback);
    if (Token(userdata) != gTimingGeneration)
    {
        return;
    }
    if (mapped != nullptr && values[3] >= values[0])
    {
        gTiming.DurationNanoseconds = static_cast<float64>(values[3] - values[0]);
        gTiming.State = GpuTimingState::Available;
    }
    else
    {
        gTiming.State = GpuTimingState::Disjoint;
    }
}
void TimingMarker(bool first) noexcept
{
    WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
    writes.querySet = gTimingQueries;
    // The pinned emdawn bridge forwards UINT32_MAX instead of omitting absent indices.
    // Use both endpoints in each marker; indices 0 and 3 enclose the entire frame.
    writes.beginningOfPassWriteIndex = first ? 0 : 2;
    writes.endOfPassWriteIndex = first ? 1 : 3;
    WGPUComputePassDescriptor description = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    description.timestampWrites = &writes;
    auto pass = wgpuCommandEncoderBeginComputePass(gEncoder, &description);
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
    if (wgpuAdapterGetInfo(gAdapter, &info) == WGPUStatus_Success)
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
    gTimingGeneration = 0;
    gTimingMapRequested = false;
    if (gTimingQueries != nullptr)
    {
        wgpuQuerySetDestroy(gTimingQueries);
        wgpuQuerySetRelease(gTimingQueries);
    }
    if (gTimingResolve != nullptr)
    {
        wgpuBufferDestroy(gTimingResolve);
        wgpuBufferRelease(gTimingResolve);
    }
    if (gTimingReadback != nullptr)
    {
        wgpuBufferDestroy(gTimingReadback);
        wgpuBufferRelease(gTimingReadback);
    }
    gTimingQueries = nullptr;
    gTimingResolve = nullptr;
    gTimingReadback = nullptr;
    gTiming = {};
}
GpuTiming TimingPoll() noexcept
{
    return gTiming;
}
RasterStatus TimingArm() noexcept
{
    if (gTiming.State == GpuTimingState::Armed || gTiming.State == GpuTimingState::Pending)
    {
        return RasterStatus::InvalidState;
    }
    if (wgpuDeviceHasFeature(gDevice, WGPUFeatureName_TimestampQuery) == false)
    {
        gTiming = { .State = GpuTimingState::Unavailable };
        return RasterStatus::Unsupported;
    }
    internal::TimingArmRollback rollback(gTiming);
    TimingReset();
    gTimingGeneration = gTimingGenerations.Take();
    if (gTimingGeneration == 0)
    {
        return RasterStatus::CapacityExceeded;
    }
    WGPUQuerySetDescriptor query = WGPU_QUERY_SET_DESCRIPTOR_INIT;
    query.type = WGPUQueryType_Timestamp;
    query.count = 4;
    gTimingQueries = wgpuDeviceCreateQuerySet(gDevice, &query);
    WGPUBufferDescriptor buffer = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer.size = 32;
    buffer.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
    gTimingResolve = wgpuDeviceCreateBuffer(gDevice, &buffer);
    buffer.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    gTimingReadback = wgpuDeviceCreateBuffer(gDevice, &buffer);
    if (gTimingQueries == nullptr || gTimingResolve == nullptr || gTimingReadback == nullptr)
    {
        TimingReset();
        return RasterStatus::OutOfMemory;
    }
    gTiming.State = GpuTimingState::Armed;
    rollback.Accept();
    return RasterStatus::Ready;
}
void TimingBegin() noexcept
{
    if (gTiming.State == GpuTimingState::Armed)
    {
        TimingMarker(true);
    }
}
void TimingEnd() noexcept
{
    if (gTiming.State != GpuTimingState::Armed)
    {
        return;
    }
    TimingMarker(false);
    wgpuCommandEncoderResolveQuerySet(gEncoder, gTimingQueries, 0, 4, gTimingResolve, 0);
    wgpuCommandEncoderCopyBufferToBuffer(gEncoder, gTimingResolve, 0, gTimingReadback, 0, 32);
    gTiming.State = GpuTimingState::Pending;
}
void TimingSubmitted() noexcept
{
    if (gTiming.State != GpuTimingState::Pending || gTimingGeneration == 0 || gTimingMapRequested)
    {
        return;
    }
    gTimingMapRequested = true;
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = TimingMapped;
    callback.userdata1 = Userdata(gTimingGeneration);
    (void)wgpuBufferMapAsync(gTimingReadback, WGPUMapMode_Read, 0, 32, callback);
}
bool RasterAllocationBytes(internal::RasterKind, usize, uint64& bytes) noexcept
{
    bytes = 0;
    return false;
}
} // namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE
