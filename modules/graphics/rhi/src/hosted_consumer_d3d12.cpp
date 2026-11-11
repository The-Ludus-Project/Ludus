#include "internal/hosted_adapter.h"
#include "internal/hosted_consumer.h"
#include "internal/hosted_d3d12.h"

#include <ludus/foundation/containers/static_array.hpp>

#include <new>

#undef GetObject

// Thanks to Microsoft, "ID3D12Device::CreateSharedHandle method" and "Shared heaps",
// for the local ownership of a duplicated NT handle and its imported resource.
// https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle
// https://learn.microsoft.com/windows/win32/direct3d12/shared-heaps
namespace ludus::graphics::rhi::internal
{
using namespace backend::d3d12;
using foundation::StaticArray;

struct HostedConsumerBackend final
{
    struct Image final
    {
        ComOwner<ID3D12Resource> Resource;
        uint64 ReleaseFence = 0;
        bool ShouldDrain = false;
    };
    using Images = StaticArray<Image, platform::HOSTED_SURFACE_IMAGE_COUNT>;

    ID3D12Device* DeviceOrNull = nullptr;
    ID3D12CommandQueue* QueueOrNull = nullptr;
    ComOwner<ID3D12Fence> Fence;
    uint64 LastRelease = 0;
    StaticArray<Images, HOSTED_GENERATION_SLOTS> Slots{};
};

namespace
{
constexpr uint64 REMOVED_FENCE_VALUE = ~uint64{0};

[[nodiscard]] bool IsExpectedResource(const D3D12_RESOURCE_DESC& description,
                                      const platform::HostedSurfaceImages& images) noexcept
{
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.Width == images.Width &&
           description.Height == images.Height && description.Format == HOSTED_SURFACE_FORMAT &&
           description.MipLevels == 1 && description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1 &&
           (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
}
} // namespace

bool IsHostedImportAvailable(Backend api) noexcept
{
    return api == Backend::Direct3D12;
}

bool TryReadHostedConsumerAdapter(const NativeDeviceIdentity& device,
                                  platform::HostedAdapterIdentity& outAdapter) noexcept
{
    ID3D12Device* nativeOrNull = static_cast<ID3D12Device*>(device.Device.GetObject());
    if (device.Api != Backend::Direct3D12 || nativeOrNull == nullptr)
    {
        return false;
    }
    outAdapter = EncodeHostedAdapter(nativeOrNull->GetAdapterLuid());
    return true;
}

HostedConsumerStatus CreateHostedConsumerBackend(const NativeDeviceIdentity& device,
                                                 HostedConsumerBackendOwner& outBackend) noexcept
{
    ID3D12Device* nativeOrNull = static_cast<ID3D12Device*>(device.Device.GetObject());
    ID3D12CommandQueue* queueOrNull = static_cast<ID3D12CommandQueue*>(device.CommandQueue.GetObject());
    if (nativeOrNull == nullptr || queueOrNull == nullptr ||
        queueOrNull->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || nativeOrNull->GetNodeCount() != 1)
    {
        return HostedConsumerStatus::InvalidArgument;
    }
    ID3D12Device* queueDeviceOrNull = nullptr;
    const HRESULT queueResult =
        queueOrNull->GetDevice(GetInterfaceId(queueDeviceOrNull), GetInterfaceOutput(queueDeviceOrNull));
    const ComOwner<ID3D12Device> queueDevice(queueDeviceOrNull);
    if (FAILED(queueResult) == true || queueDevice.Get() != nativeOrNull)
    {
        return HostedConsumerStatus::InvalidArgument;
    }
    HostedConsumerBackend* allocationOrNull = new (std::nothrow) HostedConsumerBackend;
    HostedConsumerBackendOwner backend(allocationOrNull);
    if (backend.Get() == nullptr)
    {
        return HostedConsumerStatus::Unavailable;
    }
    ID3D12Fence* fenceOrNull = nullptr;
    if (FAILED(nativeOrNull->CreateFence(0,
                                         D3D12_FENCE_FLAG_NONE,
                                         GetInterfaceId(fenceOrNull),
                                         GetInterfaceOutput(fenceOrNull))) == true)
    {
        return HostedConsumerStatus::Unavailable;
    }
    backend->DeviceOrNull = nativeOrNull;
    backend->QueueOrNull = queueOrNull;
    backend->Fence.Reset(fenceOrNull);
    outBackend = Move(backend);
    return HostedConsumerStatus::Ok;
}

void HostedConsumerBackendDeleter::operator()(HostedConsumerBackend* backendOrNull) const noexcept
{
    delete backendOrNull;
}

HostedConsumerRefusal ImportHostedGeneration(HostedConsumerBackend& inoutBackend,
                                             usize slot,
                                             platform::HostedSurfaceImages& inoutImages) noexcept
{
    const StaticArray<platform::HostedImageHandle, platform::HOSTED_SURFACE_IMAGE_COUNT> handles =
        Move(inoutImages.Handles);
    const platform::HostedAdapterIdentity adapter = EncodeHostedAdapter(inoutBackend.DeviceOrNull->GetAdapterLuid());
    if (AreAdaptersEqual(adapter, {inoutImages.DeviceUuid, inoutImages.DriverUuid}) == false)
    {
        return HostedConsumerRefusal::AdapterMismatch;
    }
    HostedConsumerBackend::Images images;
    for (usize i = 0; i < handles.GetSize(); ++i)
    {
        if (handles[i].GetKind() != platform::HostedHandleKind::Win32SharedResource)
        {
            return HostedConsumerRefusal::UnsupportedHandle;
        }
        ID3D12Resource* resourceOrNull = nullptr;
        const HRESULT opened = inoutBackend.DeviceOrNull->OpenSharedHandle(handles[i].GetWin32Handle(),
                                                                           GetInterfaceId(resourceOrNull),
                                                                           GetInterfaceOutput(resourceOrNull));
        images[i].Resource.Reset(resourceOrNull);
        if (FAILED(opened) == true || resourceOrNull == nullptr ||
            IsExpectedResource(resourceOrNull->GetDesc(), inoutImages) == false)
        {
            return HostedConsumerRefusal::ImportFailed;
        }
    }
    inoutBackend.Slots[slot] = Move(images);
    return HostedConsumerRefusal::None;
}

void DestroyHostedGeneration(HostedConsumerBackend& inoutBackend, usize slot) noexcept
{
    for (uint32 image = 0; image < platform::HOSTED_SURFACE_IMAGE_COUNT; ++image)
    {
        HostedConsumerBackend::Image& entry = inoutBackend.Slots[slot][image];
        if (entry.ShouldDrain == true)
        {
            // If a release signal failed while running, this slot still owns
            // the image. Retry the fence on the teardown path before freeing it.
            while (entry.ReleaseFence == 0 && EnqueueHostedRelease(inoutBackend, slot, image) == false)
            {
                ::Sleep(1);
            }
            CompleteHostedRelease(inoutBackend, slot, image);
        }
    }
    inoutBackend.Slots[slot] = {};
}

bool AcquireHostedImage(HostedConsumerBackend& inoutBackend, usize slot, uint32 image, HostedFrame& inoutFrame) noexcept
{
    ID3D12Resource* resourceOrNull = inoutBackend.Slots[slot][image].Resource.Get();
    if (resourceOrNull == nullptr || inoutBackend.DeviceOrNull->GetDeviceRemovedReason() != S_OK)
    {
        return false;
    }
    // The producer has completed its work and left this simultaneous-access
    // texture in COMMON. The graphics consumer promotes it for sampling.
    inoutFrame.Direct3D12Resource = NativeDirect3D12Resource{resourceOrNull};
    return true;
}

bool EnqueueHostedRelease(HostedConsumerBackend& inoutBackend, usize slot, uint32 image) noexcept
{
    if (inoutBackend.Fence->GetCompletedValue() == REMOVED_FENCE_VALUE)
    {
        return true;
    }
    const uint64 value = inoutBackend.LastRelease + 1;
    if (FAILED(inoutBackend.QueueOrNull->Signal(inoutBackend.Fence.Get(), value)) == true)
    {
        const bool isDeviceLost = inoutBackend.DeviceOrNull->GetDeviceRemovedReason() != S_OK;
        inoutBackend.Slots[slot][image].ShouldDrain = isDeviceLost == false;
        return isDeviceLost;
    }
    inoutBackend.LastRelease = value;
    inoutBackend.Slots[slot][image].ReleaseFence = value;
    return true;
}

bool TryCompleteHostedRelease(HostedConsumerBackend& inoutBackend, usize slot, uint32 image) noexcept
{
    HostedConsumerBackend::Image& entry = inoutBackend.Slots[slot][image];
    if (inoutBackend.Fence->GetCompletedValue() < entry.ReleaseFence)
    {
        return false;
    }
    entry.ReleaseFence = 0;
    entry.ShouldDrain = false;
    return true;
}

void CompleteHostedRelease(HostedConsumerBackend& inoutBackend, usize slot, uint32 image) noexcept
{
    const uint64 value = inoutBackend.Slots[slot][image].ReleaseFence;
    // A null event waits synchronously. Only teardown takes this path.
    while (TryCompleteHostedRelease(inoutBackend, slot, image) == false)
    {
        if (FAILED(inoutBackend.Fence->SetEventOnCompletion(value, nullptr)) == true)
        {
            ::Sleep(1);
        }
    }
}
} // namespace ludus::graphics::rhi::internal
