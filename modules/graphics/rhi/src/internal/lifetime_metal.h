#pragma once
// Thanks to Apple, "MTLBlitCommandEncoder", Metal API documentation,
// https://developer.apple.com/documentation/metal/mtlblitcommandencoder
// for buffer-to-buffer blits; command-buffer completion protects shared slices.
// Private Metal transfer slices. Shared CPU memory is read only after the
// submitted blit command completes; logical cancellation never recycles it early.
namespace ludus::graphics::rhi::backend
{
namespace
{
struct LifetimeMetalTransfer final
{
    id<MTLBuffer> Staging = nil;
    id<MTLCommandBuffer> Command = nil;
    bool Busy = false;
};
/// Staging slices of one transfer direction.
using LifetimeMetalTransfers = StaticArray<LifetimeMetalTransfer, internal::LIFETIME_TRANSFERS>;
/// Upload (0) and readback (1) staging slices of one Metal owner (R14c).
struct LifetimeMetalState final
{
    StaticArray<LifetimeMetalTransfers, 2> Transfers{};
};
StaticArray<LifetimeMetalState, MAX_DEVICE_OWNERS> gLifetimeMetalStates{};
/// The staging slices of the bound owner slot.
LifetimeMetalState& GetLifetimeMetal() noexcept
{
    return gLifetimeMetalStates[internal::GetBoundOwnerSlot()];
}
RasterStatus LifetimeMetalStage(LifetimeMetalTransfer& transfer) noexcept
{
    if (transfer.Busy)
    {
        return RasterStatus::CapacityExceeded;
    }
    if (transfer.Staging == nil)
    {
        transfer.Staging = [GetMetal().Device newBufferWithLength:internal::CurrentStorageLimits().TransferBytes
                                                          options:MTLResourceStorageModeShared];
    }
    return transfer.Staging != nil ? RasterStatus::Ready : RasterStatus::OutOfMemory;
}
RasterStatus LifetimeMetalCopy(LifetimeMetalTransfer& transfer,
                               id<MTLBuffer> source,
                               usize offset,
                               id<MTLBuffer> destination,
                               usize size) noexcept
{
    transfer.Command = [GetMetal().Queue commandBuffer];
    if (transfer.Command == nil)
    {
        return RasterStatus::OutOfMemory;
    }
    id<MTLBlitCommandEncoder> encoder = [transfer.Command blitCommandEncoder];
    if (encoder == nil)
    {
        transfer.Command = nil;
        return RasterStatus::OutOfMemory;
    }
    [encoder copyFromBuffer:source sourceOffset:offset toBuffer:destination destinationOffset:0 size:size];
    [encoder endEncoding];
    transfer.Busy = true;
    [transfer.Command commit];
    return RasterStatus::Ready;
}
} // namespace
bool LifetimeTransferAvailable(bool readback, usize slot) noexcept
{
    return GetLifetimeMetal().Transfers[readback ? 1 : 0][slot].Busy == false;
}
RasterStatus LifetimeUpload(usize transferSlot,
                            const BufferDescription&,
                            const internal::LifetimeUploadTarget& uploadTarget,
                            const uint8* bytes,
                            usize size) noexcept
{
    const auto bufferSlot = uploadTarget.Buffer;

    @autoreleasepool
    {
        LifetimeMetalTransfer& transfer = GetLifetimeMetal().Transfers[0][transferSlot];
        const auto ready = LifetimeMetalStage(transfer);
        if (ready != RasterStatus::Ready)
        {
            return ready;
        }
        GetRasterMetal().Buffers[bufferSlot] = [GetMetal().Device newBufferWithLength:size
                                                                              options:MTLResourceStorageModePrivate];
        if (GetRasterMetal().Buffers[bufferSlot] == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        std::memcpy([transfer.Staging contents], bytes, size);
        return LifetimeMetalCopy(transfer, transfer.Staging, 0, GetRasterMetal().Buffers[bufferSlot], size);
    }
}
RasterStatus LifetimeReadback(usize transferSlot, BufferRole, const internal::LifetimeCopyRange& range) noexcept
{
    const auto bufferSlot = range.Buffer;
    const auto offset = range.Offset;
    const auto size = range.Size;
    @autoreleasepool
    {
        LifetimeMetalTransfer& transfer = GetLifetimeMetal().Transfers[1][transferSlot];
        const auto ready = LifetimeMetalStage(transfer);
        return ready == RasterStatus::Ready
                   ? LifetimeMetalCopy(transfer, GetRasterMetal().Buffers[bufferSlot], offset, transfer.Staging, size)
                   : ready;
    }
}
// Thanks to Apple, "MTLBlitCommandEncoder", Metal API documentation,
// https://developer.apple.com/documentation/metal/mtlblitcommandencoder
// for queued buffer/texture copies; all staging survives command completion.
RasterStatus
LifetimeBufferUpdate(usize slot, BufferRole, const internal::LifetimeCopyRange& range, const uint8* bytes) noexcept
{
    @autoreleasepool
    {
        LifetimeMetalTransfer& transfer = GetLifetimeMetal().Transfers[0][slot];
        const auto ready = LifetimeMetalStage(transfer);
        if (ready != RasterStatus::Ready)
        {
            return ready;
        }
        std::memcpy([transfer.Staging contents], bytes, range.Size);
        transfer.Command = [GetMetal().Queue commandBuffer];
        if (transfer.Command == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto encoder = [transfer.Command blitCommandEncoder];
        if (encoder == nil)
        {
            transfer.Command = nil;
            return RasterStatus::OutOfMemory;
        }
        [encoder copyFromBuffer:transfer.Staging
                   sourceOffset:0
                       toBuffer:GetRasterMetal().Buffers[range.Buffer]
              destinationOffset:range.Offset
                           size:range.Size];
        [encoder endEncoding];
        transfer.Busy = true;
        [transfer.Command commit];
        return RasterStatus::Ready;
    }
}
RasterStatus LifetimeTextureTransfer(usize slot, const internal::TextureTransfer& info, const uint8* bytes) noexcept
{
    @autoreleasepool
    {
        const bool readback = info.Operation == internal::TextureTransferOperation::Readback;
        LifetimeMetalTransfer& transfer = GetLifetimeMetal().Transfers[readback ? 1 : 0][slot];
        const auto ready = LifetimeMetalStage(transfer);
        if (ready != RasterStatus::Ready)
        {
            return ready;
        }
        transfer.Command = [GetMetal().Queue commandBuffer];
        if (transfer.Command == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        auto encoder = [transfer.Command blitCommandEncoder];
        if (encoder == nil)
        {
            transfer.Command = nil;
            return RasterStatus::OutOfMemory;
        }
        const auto& source = info.SourceRegion;
        const auto& destination = info.DestinationRegion;
        if (info.Operation == internal::TextureTransferOperation::Update)
        {
            std::memcpy([transfer.Staging contents], bytes, info.Size);
            [encoder copyFromBuffer:transfer.Staging
                       sourceOffset:0
                  sourceBytesPerRow:info.RowPitch
                sourceBytesPerImage:info.Size
                         sourceSize:MTLSizeMake(destination.Width, destination.Height, 1)
                          toTexture:GetRasterMetal().Textures[info.Destination]
                   destinationSlice:0
                   destinationLevel:destination.Mip
                  destinationOrigin:MTLOriginMake(destination.X, destination.Y, 0)];
        }
        else if (readback)
        {
            [encoder copyFromTexture:GetRasterMetal().Textures[info.Source]
                             sourceSlice:0
                             sourceLevel:source.Mip
                            sourceOrigin:MTLOriginMake(source.X, source.Y, 0)
                              sourceSize:MTLSizeMake(source.Width, source.Height, 1)
                                toBuffer:transfer.Staging
                       destinationOffset:0
                  destinationBytesPerRow:info.RowPitch
                destinationBytesPerImage:info.Size];
        }
        else
        {
            [encoder copyFromTexture:GetRasterMetal().Textures[info.Source]
                         sourceSlice:0
                         sourceLevel:source.Mip
                        sourceOrigin:MTLOriginMake(source.X, source.Y, 0)
                          sourceSize:MTLSizeMake(source.Width, source.Height, 1)
                           toTexture:GetRasterMetal().Textures[info.Destination]
                    destinationSlice:0
                    destinationLevel:destination.Mip
                   destinationOrigin:MTLOriginMake(destination.X, destination.Y, 0)];
        }
        [encoder endEncoding];
        transfer.Busy = true;
        [transfer.Command commit];
        return RasterStatus::Ready;
    }
}
RasterStatus LifetimePollTransfer(bool readback, usize slot) noexcept
{
    LifetimeMetalTransfer& transfer = GetLifetimeMetal().Transfers[readback ? 1 : 0][slot];
    if (transfer.Busy == false || transfer.Command == nil)
    {
        return RasterStatus::Failed;
    }
    if (CheckCommand(transfer.Command) == false)
    {
        return RasterStatus::Failed;
    }
    const auto status = transfer.Command.status;
    return status == MTLCommandBufferStatusCompleted ? RasterStatus::Ready
           : status == MTLCommandBufferStatusError   ? RasterStatus::Failed
                                                     : RasterStatus::Pending;
}
RasterStatus LifetimeCopyReadback(usize slot, uint8* output, usize size) noexcept
{
    const auto ready = LifetimePollTransfer(true, slot);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    std::memcpy(output, [GetLifetimeMetal().Transfers[1][slot].Staging contents], size);
    return RasterStatus::Ready;
}
void LifetimeReleaseTransfer(bool readback, usize slot) noexcept
{
    LifetimeMetalTransfer& transfer = GetLifetimeMetal().Transfers[readback ? 1 : 0][slot];
    transfer.Command = nil;
    transfer.Busy = false;
}
void LifetimeReset() noexcept
{
    for (LifetimeMetalTransfers& direction : GetLifetimeMetal().Transfers)
    {
        for (LifetimeMetalTransfer& transfer : direction)
        {
            if (transfer.Command != nil)
            {
                [transfer.Command waitUntilCompleted];
            }
            transfer = {};
        }
    }
}
void RasterDiscardSubmission() noexcept {}
} // namespace ludus::graphics::rhi::backend
