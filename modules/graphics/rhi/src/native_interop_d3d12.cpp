#include "internal/native_interop.h"

#include "internal/d3d12_device.h"
#include "internal/d3d12_transfer.h"
#include "internal/storage.h"

namespace ludus::graphics::rhi::backend
{
namespace
{
using namespace d3d12;

struct NativeTextureCommands final
{
    ComOwner<ID3D12CommandAllocator> Allocator;
    ComOwner<ID3D12GraphicsCommandList> List;
    uint64 ReleaseFence = 0;
};

internal::StorageTable<NativeTextureCommands> gTextureCommands;

[[nodiscard]] bool RecordPublication(ID3D12Device& inoutDevice,
                                     ID3D12Resource& inoutResource,
                                     D3D12_RESOURCE_STATES before,
                                     NativeTextureCommands& outCommands) noexcept
{
    ID3D12CommandAllocator* allocatorOrNull = nullptr;
    if (FAILED(inoutDevice.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  GetInterfaceId(allocatorOrNull),
                                                  GetInterfaceOutput(allocatorOrNull))) == true)
    {
        return false;
    }
    outCommands.Allocator.Reset(allocatorOrNull);
    ID3D12GraphicsCommandList* listOrNull = nullptr;
    if (FAILED(inoutDevice.CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             allocatorOrNull,
                                             nullptr,
                                             GetInterfaceId(listOrNull),
                                             GetInterfaceOutput(listOrNull))) == true)
    {
        return false;
    }
    outCommands.List.Reset(listOrNull);
    const D3D12_RESOURCE_BARRIER barrier =
    {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition =
        {
            .pResource = &inoutResource,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = before,
            .StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        },
    };
    listOrNull->ResourceBarrier(1, &barrier);
    return SUCCEEDED(listOrNull->Close()) == true;
}
} // namespace

bool NativeInteropSupported() noexcept
{
    return true;
}

RasterStatus NativeDeviceSnapshot(NativeDeviceInfo& outInfo) noexcept
{
    ID3D12Device* deviceOrNull = GetDeviceOrNull();
    ID3D12CommandQueue* queueOrNull = GetQueueOrNull();
    if (deviceOrNull == nullptr || queueOrNull == nullptr)
    {
        return RasterStatus::NotReady;
    }
    outInfo =
    {
        .Identity =
        {
            .Api = Backend::Direct3D12,
            .Device = NativeLogicalDevice{deviceOrNull},
            .CommandQueue = NativeCommandQueue{queueOrNull},
        },
    };
    return RasterStatus::Ready;
}

RasterStatus NativeTexturePublish(const internal::NativeTexturePublication& publication,
                                  NativeTextureInfo& inoutInfo) noexcept
{
    ID3D12Device* deviceOrNull = GetDeviceOrNull();
    ID3D12CommandQueue* queueOrNull = GetQueueOrNull();
    if (internal::FrameOpen() == true)
    {
        return RasterStatus::InvalidState;
    }
    ID3D12Resource* textureOrNull = GetTexture(publication.Texture);
    if (deviceOrNull == nullptr || queueOrNull == nullptr || textureOrNull == nullptr)
    {
        return RasterStatus::NotReady;
    }
    if (deviceOrNull->GetDeviceRemovedReason() != S_OK)
    {
        return RasterStatus::DeviceLost;
    }
    if (gTextureCommands.EnsureSlots(publication.Lease + 1) == false)
    {
        return internal::StorageFailure();
    }
    const D3D12_RESOURCE_STATES before = GetTextureState(publication.Texture);
    if (before != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        NativeTextureCommands& commands = gTextureCommands[publication.Lease];
        if (RecordPublication(*deviceOrNull, *textureOrNull, before, commands) == false)
        {
            return deviceOrNull->GetDeviceRemovedReason() == S_OK ? RasterStatus::Failed : RasterStatus::DeviceLost;
        }
        ID3D12CommandList* list = commands.List.Get();
        queueOrNull->ExecuteCommandLists(1, &list);
        SetTextureState(publication.Texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    // The portable lease pins the texture, and this slot pins the transition
    // commands until the consumer's fence completes on the same direct queue.
    inoutInfo.Direct3D12Resource = NativeDirect3D12Resource{textureOrNull};
    return RasterStatus::Ready;
}

RasterStatus NativeTextureRelease(usize slot) noexcept
{
    ID3D12Device* deviceOrNull = GetDeviceOrNull();
    if (deviceOrNull == nullptr || deviceOrNull->GetDeviceRemovedReason() != S_OK)
    {
        return RasterStatus::DeviceLost;
    }
    return SignalQueueFence(gTextureCommands[slot].ReleaseFence) == true ? RasterStatus::Pending
           : deviceOrNull->GetDeviceRemovedReason() != S_OK              ? RasterStatus::DeviceLost
                                                                         : RasterStatus::Failed;
}

RasterStatus NativeTexturePoll(usize slot, bool shouldWait) noexcept
{
    ID3D12Fence* fenceOrNull = GetFenceOrNull();
    if (fenceOrNull == nullptr)
    {
        return RasterStatus::DeviceLost;
    }
    const uint64 completed = fenceOrNull->GetCompletedValue();
    if (completed == ~uint64{0})
    {
        return RasterStatus::DeviceLost;
    }
    const uint64 value = gTextureCommands[slot].ReleaseFence;
    if (completed >= value)
    {
        return RasterStatus::Ready;
    }
    if (shouldWait == false)
    {
        return RasterStatus::Pending;
    }
    if (WaitForQueueFence(value) == true)
    {
        return RasterStatus::Ready;
    }
    return fenceOrNull->GetCompletedValue() == ~uint64{0} ? RasterStatus::DeviceLost : RasterStatus::Failed;
}

void NativeTextureDestroy(usize slot) noexcept
{
    if (slot < gTextureCommands.GetSize())
    {
        gTextureCommands[slot] = {};
    }
}

void NativeInteropReset() noexcept {}
} // namespace ludus::graphics::rhi::backend
