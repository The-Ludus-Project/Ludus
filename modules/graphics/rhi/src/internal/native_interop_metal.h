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
/// Native texture reads of one Metal owner (R14c).
StaticArray<internal::StorageTable<NativeMetalRead>, MAX_DEVICE_OWNERS> gNativeMetalReads{};
/// The native texture reads of the bound owner slot.
internal::StorageTable<NativeMetalRead>& GetNativeMetalReads() noexcept
{
    return gNativeMetalReads[internal::GetBoundOwnerSlot()];
}
} // namespace
bool NativeInteropSupported() noexcept
{
    return true;
}
RasterStatus NativeDeviceSnapshot(NativeDeviceInfo& output) noexcept
{
    if (GetMetal().Device == nil || GetMetal().Queue == nil)
    {
        return RasterStatus::NotReady;
    }
    output =
    {
        .Identity =
        {
            .Api = Backend::Metal,
            .Device = NativeLogicalDevice{(__bridge void*)GetMetal().Device},
            .CommandQueue = NativeCommandQueue{(__bridge void*)GetMetal().Queue},
        },
        .RegistryId = GetMetal().Device.registryID,
    };
    return RasterStatus::Ready;
}
RasterStatus NativeTexturePublish(const internal::NativeTexturePublication& publication,
                                  NativeTextureInfo& output) noexcept
{
    const auto slot = publication.Lease;
    const auto textureSlot = publication.Texture;
    if (GetNativeMetalReads().EnsureSlots(slot + 1) == false)
    {
        return internal::StorageFailure();
    }
    NativeMetalRead& record = GetNativeMetalReads()[slot];
    record.Texture = GetRasterMetal().Textures[textureSlot];
    record.Publication = [GetMetal().Queue commandBuffer];
    record.Completion = [GetMetal().Queue commandBuffer];
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
    NativeMetalRead& record = GetNativeMetalReads()[slot];
    if (record.Released == false)
    {
        [record.Completion commit];
        record.Released = true;
    }
    return RasterStatus::Pending;
}
RasterStatus NativeTexturePoll(usize slot, bool wait) noexcept
{
    NativeMetalRead& record = GetNativeMetalReads()[slot];
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
    if (slot < GetNativeMetalReads().GetSize())
    {
        GetNativeMetalReads()[slot] = {};
    }
}
void NativeInteropDetachSurface() noexcept
{
    ReleaseFrameResources();
    DisconnectSurfaces();
    GetMetal().IsConnected = false;
}
void NativeInteropReset() noexcept {}
} // namespace ludus::graphics::rhi::backend
