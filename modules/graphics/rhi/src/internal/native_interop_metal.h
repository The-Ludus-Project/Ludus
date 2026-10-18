#pragma once

#include "native_interop.h"

namespace ludus::graphics::rhi::backend
{
namespace
{
struct NativeMetalRead final
{
    id<MTLTexture> Texture = nil;
    id<MTLCommandBuffer> Publication = nil;
    id<MTLCommandBuffer> Completion = nil;
    bool Released = false;
};
internal::StorageTable<NativeMetalRead> gNativeMetalReads;
} // namespace
bool NativeInteropSupported() noexcept
{
    return true;
}
RasterStatus NativeDeviceSnapshot(NativeDeviceInfo& output) noexcept
{
    if (gDevice == nil || gQueue == nil)
    {
        return RasterStatus::NotReady;
    }
    output =
    {
        .Identity =
        {
            .Api = Backend::Metal,
            .Device = NativeLogicalDevice{(__bridge void*)gDevice},
            .CommandQueue = NativeCommandQueue{(__bridge void*)gQueue},
        },
        .RegistryId = gDevice.registryID,
    };
    return RasterStatus::Ready;
}
RasterStatus NativeTexturePublish(const internal::NativeTexturePublication& publication,
                                  NativeTextureInfo& output) noexcept
{
    const auto slot = publication.Lease;
    const auto textureSlot = publication.Texture;
    if (gNativeMetalReads.EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    auto& record = gNativeMetalReads[slot];
    record.Texture = gRasterTextures[textureSlot];
    record.Publication = [gQueue commandBuffer];
    record.Completion = [gQueue commandBuffer];
    if (record.Texture == nil || record.Publication == nil || record.Completion == nil)
    {
        record = {};
        return RasterStatus::OutOfMemory;
    }
    // RHI allocations use Metal's tracked texture hazards. Both frameworks must
    // enqueue and commit on this exact queue after publication, in owner order.
    [record.Publication commit];
    output.MetalTexture = NativeMetalTexture{(__bridge void*)record.Texture};
    return RasterStatus::Ready;
}
RasterStatus NativeTextureRelease(usize slot) noexcept
{
    auto& record = gNativeMetalReads[slot];
    if (record.Released == false)
    {
        [record.Completion commit];
        record.Released = true;
    }
    return RasterStatus::Pending;
}
RasterStatus NativeTexturePoll(usize slot, bool wait) noexcept
{
    auto& record = gNativeMetalReads[slot];
    const auto command = record.Released ? record.Completion : record.Publication;
    if (command == nil)
    {
        return RasterStatus::InvalidState;
    }
    if (wait)
    {
        [command waitUntilCompleted];
    }
    return command.status == MTLCommandBufferStatusError       ? RasterStatus::DeviceLost
           : command.status == MTLCommandBufferStatusCompleted ? RasterStatus::Ready
                                                               : RasterStatus::Pending;
}
void NativeTextureDestroy(usize slot) noexcept
{
    if (slot < gNativeMetalReads.GetSize())
    {
        gNativeMetalReads[slot] = {};
    }
}
void NativeInteropDetachSurface() noexcept
{
    ShutdownRendering();
    if (gView != nil && gView.layer == gLayer)
    {
        gView.layer = gPreviousLayer;
        gView.wantsLayer = gPreviousWantsLayer;
    }
    gView = nil;
    gWindow = nil;
    gLayer = nil;
    gPreviousLayer = nil;
    gPreviousWantsLayer = NO;
    gTarget = {};
    gConnected = false;
}
void NativeInteropReset() noexcept {}
} // namespace ludus::graphics::rhi::backend
