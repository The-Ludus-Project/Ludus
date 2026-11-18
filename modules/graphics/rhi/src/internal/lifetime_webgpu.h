#pragma once
// Thanks to W3C, "WebGPU", GPUBuffer mapping and GPUQueue:
// https://www.w3.org/TR/webgpu/#buffer-mapping
// https://www.w3.org/TR/webgpu/#gpuqueue
// Mapping success and queue progress remain separate. Stable bounded callback
// cells outlive logical cancellation and shutdown; callbacks publish only into
// their own cell, and the device owner polls the shared registry later.
namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE
{
namespace
{
struct LifetimeWebTransfer final
{
    WGPUBuffer Staging = nullptr;
    uint32 Session = 0;
    uint32 Callbacks = 0;
    RasterStatus Result = RasterStatus::Ready;
    bool Busy = false;
    bool Mapped = false;
    bool Abandoned = false;
};
/// @brief Upload (0) and readback (1) transfer cells of one device owner (R14c).
///
/// Cells are never reset while callbacks reference them; see LifetimeWebCompleted.
struct LifetimeWebGpuState final
{
    foundation::StaticArray<foundation::StaticArray<LifetimeWebTransfer, internal::LIFETIME_TRANSFERS>,
                            internal::LIFETIME_DIRECTIONS>
        Transfers{};
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<LifetimeWebGpuState, MAX_DEVICE_OWNERS> gLifetimeWebGpuStates{};
/// LifetimeWebGpuState of the bound owner slot.
LifetimeWebGpuState& GetLifetimeWebGpu() noexcept
{
    return gLifetimeWebGpuStates[internal::GetBoundOwnerSlot()];
}
void LifetimeWebDrop(LifetimeWebTransfer& transfer) noexcept
{
    auto buffer = transfer.Staging;
    const bool mapped = transfer.Mapped;
    transfer = {};
    if (buffer != nullptr)
    {
        if (mapped)
        {
            wgpuBufferUnmap(buffer);
        }
        wgpuBufferRelease(buffer);
    }
}
void LifetimeWebCompleted(LifetimeWebTransfer& transfer, RasterStatus result) noexcept
{
    if (result != RasterStatus::Ready)
    {
        transfer.Result = result;
    }
    if (transfer.Callbacks != 0)
    {
        --transfer.Callbacks;
    }
    if ((transfer.Abandoned || internal::Current(transfer.Session) == false) && transfer.Callbacks == 0)
    {
        LifetimeWebDrop(transfer);
    }
}
void LifetimeWebDone(WGPUQueueWorkDoneStatus status, WGPUStringView, void* userdata, void*) noexcept
{
    auto& transfer = *static_cast<LifetimeWebTransfer*>(userdata);
    if (status != WGPUQueueWorkDoneStatus_Success && internal::Current(transfer.Session))
    {
        // Failed queue progress cannot prove that submitted writes stopped.
        // Close the current session; stale callbacks only drain their own cell.
        internal::Fail(transfer.Session, StartupError::DeviceLost);
    }
    LifetimeWebCompleted(transfer,
                         status == WGPUQueueWorkDoneStatus_Success ? RasterStatus::Ready : RasterStatus::Failed);
}
void LifetimeWebMapped(WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void*) noexcept
{
    auto& transfer = *static_cast<LifetimeWebTransfer*>(userdata);
    transfer.Mapped = status == WGPUMapAsyncStatus_Success;
    LifetimeWebCompleted(transfer, transfer.Mapped ? RasterStatus::Ready : RasterStatus::Failed);
}
void LifetimeWebValidated(WGPUPopErrorScopeStatus status,
                          WGPUErrorType error,
                          WGPUStringView message,
                          void* userdata,
                          void*) noexcept
{
    const auto result = status != WGPUPopErrorScopeStatus_Success ? RasterStatus::Failed
                        : error == WGPUErrorType_NoError          ? RasterStatus::Ready
                        : error == WGPUErrorType_OutOfMemory      ? RasterStatus::OutOfMemory
                                                                  : RasterStatus::Failed;
    if (result != RasterStatus::Ready)
    {
        Diagnose(message);
    }
    LifetimeWebCompleted(*static_cast<LifetimeWebTransfer*>(userdata), result);
}
void LifetimeWebPop(LifetimeWebTransfer& transfer) noexcept
{
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = LifetimeWebValidated;
    callback.userdata1 = &transfer;
    (void)wgpuDevicePopErrorScope(GetWebGpu().Device, callback);
    (void)wgpuDevicePopErrorScope(GetWebGpu().Device, callback);
}
RasterStatus LifetimeWebStage(LifetimeWebTransfer& transfer, bool readback) noexcept
{
    if (transfer.Busy || transfer.Callbacks != 0)
    {
        return RasterStatus::CapacityExceeded;
    }
    transfer.Session = GetWebGpu().Session;
    transfer.Result = RasterStatus::Ready;
    transfer.Abandoned = false;
    if (transfer.Staging == nullptr)
    {
        WGPUBufferDescriptor buffer = WGPU_BUFFER_DESCRIPTOR_INIT;
        buffer.size = internal::CurrentStorageLimits().TransferBytes;
        buffer.usage = readback ? WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst
                                : WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
        transfer.Staging = wgpuDeviceCreateBuffer(GetWebGpu().Device, &buffer);
    }
    return transfer.Staging != nullptr ? RasterStatus::Ready : RasterStatus::OutOfMemory;
}
} // namespace
bool LifetimeTransferAvailable(bool readback, usize slot) noexcept
{
    const LifetimeWebTransfer& transfer = GetLifetimeWebGpu().Transfers[readback ? 1 : 0][slot];
    return transfer.Busy == false && transfer.Callbacks == 0;
}
RasterStatus LifetimeUpload(usize transferSlot,
                            const BufferDescription& info,
                            const internal::LifetimeUploadTarget& uploadTarget,
                            const uint8* bytes,
                            usize size) noexcept
{
    const auto bufferSlot = uploadTarget.Buffer;
    const auto request = uploadTarget.Request;

    LifetimeWebTransfer& transfer = GetLifetimeWebGpu().Transfers[0][transferSlot];
    if (transfer.Busy || transfer.Callbacks != 0)
    {
        return RasterStatus::CapacityExceeded;
    }
    RasterScopes(request);
    const auto ready = LifetimeWebStage(transfer, false);
    transfer.Busy = true;
    WGPUBufferDescriptor buffer = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer.size = size;
    buffer.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst |
                   (info.Role == BufferRole::Uniform  ? WGPUBufferUsage_Uniform
                    : info.Role == BufferRole::Vertex ? WGPUBufferUsage_Vertex
                                                      : WGPUBufferUsage_Index);
    GetRasterWebGpu().Buffers[bufferSlot] = wgpuDeviceCreateBuffer(GetWebGpu().Device, &buffer);
    if (ready != RasterStatus::Ready || GetRasterWebGpu().Buffers[bufferSlot] == nullptr)
    {
        transfer.Result = RasterStatus::OutOfMemory;
        internal::RasterFail(request, RasterStatus::OutOfMemory);
        return RasterPop(request);
    }
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(GetWebGpu().Device, nullptr);
    if (encoder == nullptr)
    {
        transfer.Result = RasterStatus::OutOfMemory;
        internal::RasterFail(request, RasterStatus::OutOfMemory);
        return RasterPop(request);
    }
    wgpuQueueWriteBuffer(GetWebGpu().Queue, transfer.Staging, 0, bytes, size);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, transfer.Staging, 0, GetRasterWebGpu().Buffers[bufferSlot], 0, size);
    auto command = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    if (command == nullptr)
    {
        transfer.Result = RasterStatus::OutOfMemory;
        internal::RasterFail(request, RasterStatus::OutOfMemory);
        return RasterPop(request);
    }
    wgpuQueueSubmit(GetWebGpu().Queue, 1, &command);
    wgpuCommandBufferRelease(command);
    transfer.Callbacks = 1;
    WGPUQueueWorkDoneCallbackInfo callback = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = LifetimeWebDone;
    callback.userdata1 = &transfer;
    (void)wgpuQueueOnSubmittedWorkDone(GetWebGpu().Queue, callback);
    return RasterPop(request);
}
RasterStatus LifetimeReadback(usize transferSlot, BufferRole, const internal::LifetimeCopyRange& range) noexcept
{
    const auto bufferSlot = range.Buffer;
    const auto offset = range.Offset;
    const auto size = range.Size;
    LifetimeWebTransfer& transfer = GetLifetimeWebGpu().Transfers[1][transferSlot];
    if (transfer.Busy || transfer.Callbacks != 0)
    {
        return RasterStatus::CapacityExceeded;
    }
    wgpuDevicePushErrorScope(GetWebGpu().Device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(GetWebGpu().Device, WGPUErrorFilter_Validation);
    const auto ready = LifetimeWebStage(transfer, true);
    transfer.Busy = true;
    transfer.Callbacks = 2;
    if (ready != RasterStatus::Ready)
    {
        transfer.Result = ready;
        LifetimeWebPop(transfer);
        return RasterStatus::Pending;
    }
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(GetWebGpu().Device, nullptr);
    if (encoder == nullptr)
    {
        transfer.Result = RasterStatus::OutOfMemory;
        LifetimeWebPop(transfer);
        return RasterStatus::Pending;
    }
    wgpuCommandEncoderCopyBufferToBuffer(encoder,
                                         GetRasterWebGpu().Buffers[bufferSlot],
                                         offset,
                                         transfer.Staging,
                                         0,
                                         size);
    auto command = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    if (command == nullptr)
    {
        transfer.Result = RasterStatus::OutOfMemory;
        LifetimeWebPop(transfer);
        return RasterStatus::Pending;
    }
    wgpuQueueSubmit(GetWebGpu().Queue, 1, &command);
    wgpuCommandBufferRelease(command);
    // A failed map is not proof that the submitted copy completed. Track queue
    // retirement independently so cancellation cannot recycle its slice/source.
    transfer.Callbacks += 2;
    WGPUQueueWorkDoneCallbackInfo done = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    done.mode = WGPUCallbackMode_AllowSpontaneous;
    done.callback = LifetimeWebDone;
    done.userdata1 = &transfer;
    (void)wgpuQueueOnSubmittedWorkDone(GetWebGpu().Queue, done);
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = LifetimeWebMapped;
    callback.userdata1 = &transfer;
    (void)wgpuBufferMapAsync(transfer.Staging, WGPUMapMode_Read, 0, size, callback);
    LifetimeWebPop(transfer);
    return RasterStatus::Pending;
}
// Thanks to W3C, "WebGPU", GPUCommandEncoder texture copies and texel layouts,
// https://www.w3.org/TR/webgpu/#copying-texture-data ; rows are packed to 256 bytes.
namespace
{
void LifetimeWebCommit(LifetimeWebTransfer& transfer, WGPUCommandEncoder encoder, bool readback, usize size) noexcept
{
    auto command = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuCommandEncoderRelease(encoder);
    if (command == nullptr)
    {
        transfer.Result = RasterStatus::OutOfMemory;
        LifetimeWebPop(transfer);
        return;
    }
    wgpuQueueSubmit(GetWebGpu().Queue, 1, &command);
    wgpuCommandBufferRelease(command);
    transfer.Callbacks += readback ? 2U : 1U;
    WGPUQueueWorkDoneCallbackInfo done = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    done.mode = WGPUCallbackMode_AllowSpontaneous;
    done.callback = LifetimeWebDone;
    done.userdata1 = &transfer;
    (void)wgpuQueueOnSubmittedWorkDone(GetWebGpu().Queue, done);
    if (readback)
    {
        WGPUBufferMapCallbackInfo mapped = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        mapped.mode = WGPUCallbackMode_AllowSpontaneous;
        mapped.callback = LifetimeWebMapped;
        mapped.userdata1 = &transfer;
        (void)wgpuBufferMapAsync(transfer.Staging, WGPUMapMode_Read, 0, size, mapped);
    }
    LifetimeWebPop(transfer);
}
RasterStatus LifetimeWebBeginMutable(LifetimeWebTransfer& transfer, bool readback, WGPUCommandEncoder& encoder) noexcept
{
    if (transfer.Busy || transfer.Callbacks != 0)
    {
        return RasterStatus::CapacityExceeded;
    }
    wgpuDevicePushErrorScope(GetWebGpu().Device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(GetWebGpu().Device, WGPUErrorFilter_Validation);
    const auto ready = LifetimeWebStage(transfer, readback);
    transfer.Busy = true;
    transfer.Callbacks = 2;
    if (ready != RasterStatus::Ready)
    {
        transfer.Result = ready;
        LifetimeWebPop(transfer);
        return RasterStatus::Pending;
    }
    encoder = wgpuDeviceCreateCommandEncoder(GetWebGpu().Device, nullptr);
    if (encoder == nullptr)
    {
        transfer.Result = RasterStatus::OutOfMemory;
        LifetimeWebPop(transfer);
    }
    return RasterStatus::Pending;
}
WGPUTexelCopyTextureInfo LifetimeWebTexture(usize slot, const TextureRegion& region) noexcept
{
    WGPUTexelCopyTextureInfo texture = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    texture.texture = GetRasterWebGpu().Textures[slot];
    texture.mipLevel = region.Mip;
    texture.origin = {region.X, region.Y, 0};
    return texture;
}
} // namespace
RasterStatus
LifetimeBufferUpdate(usize slot, BufferRole, const internal::LifetimeCopyRange& range, const uint8* bytes) noexcept
{
    LifetimeWebTransfer& transfer = GetLifetimeWebGpu().Transfers[0][slot];
    WGPUCommandEncoder encoder = nullptr;
    const auto result = LifetimeWebBeginMutable(transfer, false, encoder);
    if (encoder != nullptr)
    {
        wgpuQueueWriteBuffer(GetWebGpu().Queue, transfer.Staging, 0, bytes, range.Size);
        wgpuCommandEncoderCopyBufferToBuffer(encoder,
                                             transfer.Staging,
                                             0,
                                             GetRasterWebGpu().Buffers[range.Buffer],
                                             range.Offset,
                                             range.Size);
        LifetimeWebCommit(transfer, encoder, false, 0);
    }
    return result;
}
RasterStatus LifetimeTextureTransfer(usize slot, const internal::TextureTransfer& info, const uint8* bytes) noexcept
{
    const bool readback = info.Operation == internal::TextureTransferOperation::Readback;
    LifetimeWebTransfer& transfer = GetLifetimeWebGpu().Transfers[readback ? 1 : 0][slot];
    WGPUCommandEncoder encoder = nullptr;
    const auto result = LifetimeWebBeginMutable(transfer, readback, encoder);
    if (encoder == nullptr)
    {
        return result;
    }
    const auto source = LifetimeWebTexture(info.Source, info.SourceRegion);
    const auto destination = LifetimeWebTexture(info.Destination, info.DestinationRegion);
    WGPUTexelCopyBufferInfo buffer = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    buffer.buffer = transfer.Staging;
    buffer.layout.bytesPerRow = static_cast<uint32>(info.RowPitch);
    buffer.layout.rowsPerImage = readback ? info.SourceRegion.Height : info.DestinationRegion.Height;
    if (info.Operation == internal::TextureTransferOperation::Update)
    {
        const WGPUExtent3D extent{info.DestinationRegion.Width, info.DestinationRegion.Height, 1};
        wgpuQueueWriteBuffer(GetWebGpu().Queue, transfer.Staging, 0, bytes, info.Size);
        wgpuCommandEncoderCopyBufferToTexture(encoder, &buffer, &destination, &extent);
    }
    else
    {
        const WGPUExtent3D extent{info.SourceRegion.Width, info.SourceRegion.Height, 1};
        if (readback)
        {
            wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &buffer, &extent);
        }
        else
        {
            wgpuCommandEncoderCopyTextureToTexture(encoder, &source, &destination, &extent);
        }
    }
    LifetimeWebCommit(transfer, encoder, readback, info.Size);
    return result;
}
RasterStatus LifetimePollTransfer(bool readback, usize slot) noexcept
{
    LifetimeWebTransfer& transfer = GetLifetimeWebGpu().Transfers[readback ? 1 : 0][slot];
    if (transfer.Busy == false || transfer.Session != GetWebGpu().Session)
    {
        return RasterStatus::Failed;
    }
    if (transfer.Callbacks != 0)
    {
        return RasterStatus::Pending;
    }
    if (readback && transfer.Mapped == false && transfer.Result == RasterStatus::Ready)
    {
        return RasterStatus::Failed;
    }
    return transfer.Result;
}
RasterStatus LifetimeCopyReadback(usize slot, uint8* output, usize size) noexcept
{
    const auto ready = LifetimePollTransfer(true, slot);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    LifetimeWebTransfer& transfer = GetLifetimeWebGpu().Transfers[1][slot];
    const void* mapped = wgpuBufferGetConstMappedRange(transfer.Staging, 0, size);
    if (mapped == nullptr)
    {
        return RasterStatus::Failed;
    }
    std::memcpy(output, mapped, size);
    return RasterStatus::Ready;
}
void LifetimeReleaseTransfer(bool readback, usize slot) noexcept
{
    LifetimeWebTransfer& transfer = GetLifetimeWebGpu().Transfers[readback ? 1 : 0][slot];
    if (transfer.Mapped)
    {
        wgpuBufferUnmap(transfer.Staging);
        transfer.Mapped = false;
    }
    transfer.Busy = false;
}
void LifetimeReset() noexcept
{
    for (foundation::StaticArray<LifetimeWebTransfer, internal::LIFETIME_TRANSFERS>& direction :
         GetLifetimeWebGpu().Transfers)
    {
        for (auto& transfer : direction)
        {
            if (transfer.Callbacks == 0)
            {
                LifetimeWebDrop(transfer);
            }
            else
            {
                transfer.Abandoned = true;
                // Map cancellation is asynchronous; callback storage remains occupied.
                if (transfer.Staging != nullptr)
                {
                    wgpuBufferDestroy(transfer.Staging);
                }
            }
        }
    }
}
void RasterDiscardSubmission() noexcept
{
    if (GetRasterWebGpu().ReservedOrNull != nullptr)
    {
        *GetRasterWebGpu().ReservedOrNull = {};
        GetRasterWebGpu().ReservedOrNull = nullptr;
    }
}
} // namespace ludus::graphics::rhi::LUDUS_RHI_WEBGPU_NAMESPACE
