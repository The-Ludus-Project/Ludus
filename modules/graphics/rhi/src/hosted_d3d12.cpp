#include "internal/hosted_d3d12.h"

#include <ludus/foundation/base/byte_order.hpp>

// Thanks to Microsoft, "Shared heaps" and "Multi-engine synchronization", for
// shared committed resources and the fence-ordered transfer of queue ownership.
// https://learn.microsoft.com/windows/win32/direct3d12/shared-heaps
// https://learn.microsoft.com/windows/win32/direct3d12/user-mode-heap-synchronization
namespace ludus::graphics::rhi::backend::d3d12
{
namespace
{
constexpr uint32 DESCRIPTOR_COUNT = internal::MAX_HOSTED_GENERATIONS * internal::HOSTED_IMAGE_COUNT;
constexpr uint64 REMOVED_FENCE_VALUE = ~uint64{0};
constexpr D3D12_RESOURCE_FLAGS HOSTED_TEXTURE_FLAGS =
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
} // namespace

platform::HostedAdapterIdentity EncodeHostedAdapter(LUID adapter) noexcept
{
    platform::HostedAdapterIdentity identity;
    uint8* bytes = identity.DeviceUuid.GetData();
    const bool hasLowPart = TryWriteLittleEndian<uint32>(static_cast<uint32>(adapter.LowPart), {bytes, sizeof(uint32)});
    const bool hasHighPart =
        TryWriteLittleEndian<uint32>(static_cast<uint32>(adapter.HighPart), {bytes + sizeof(uint32), sizeof(uint32)});
    LUDUS_REQUIRE(hasLowPart == true && hasHighPart == true);
    return identity;
}

LUID DecodeHostedAdapter(const platform::HostedAdapterIdentity& adapter) noexcept
{
    const uint8* bytes = adapter.DeviceUuid.GetData();
    uint32 low = 0;
    uint32 high = 0;
    const bool hasLowPart = TryReadLittleEndian<uint32>({bytes, sizeof(uint32)}, low);
    const bool hasHighPart = TryReadLittleEndian<uint32>({bytes + sizeof(uint32), sizeof(uint32)}, high);
    LUDUS_REQUIRE(hasLowPart == true && hasHighPart == true);
    return {static_cast<DWORD>(low), static_cast<LONG>(high)};
}

void HostedProducer::Connect(platform::HostedSurfaceChannel& inoutChannel) noexcept
{
    mChannelOrNull = &inoutChannel;
}

void HostedProducer::Disconnect() noexcept
{
    // The caller drains the producer queue first. The editor's imported COM
    // resources keep already-exported allocations alive on its own device.
    mChannelOrNull = nullptr;
    mRing.Reset();
    mImages = {};
    mPending = {};
    mDescriptors.Reset();
    mDescriptorSize = 0;
    mHasAcquired = false;
}

bool HostedProducer::IsConnected() const noexcept
{
    return mChannelOrNull != nullptr;
}

bool HostedProducer::poll() noexcept
{
    const uint64 completed = GetFenceOrNull()->GetCompletedValue();
    if (completed == REMOVED_FENCE_VALUE)
    {
        return false;
    }
    platform::HostedImageRelease release;
    while (mChannelOrNull->TakeRelease(release) == platform::HostedSurfaceStatus::Ok)
    {
        static_cast<void>(mRing.Release(release.Generation, release.Image));
    }
    internal::HostedSubmission submission;
    while (mRing.TryPeekSubmission(submission) == true && mImages[submission.Slot][submission.Image].Fence <= completed)
    {
        bool isPresented = false;
        if (mRing.IsCurrentSubmission(submission) == true)
        {
            const platform::HostedSurfaceStatus status =
                mChannelOrNull->Present(mRing.GetGeneration(submission.Slot), submission.Image, submission.Sequence);
            if (status == platform::HostedSurfaceStatus::Busy)
            {
                break;
            }
            isPresented = status == platform::HostedSurfaceStatus::Ok;
        }
        mRing.CompleteSubmission(isPresented);
    }
    usize retired = 0;
    while (mRing.TryTakeRetired(retired) == true)
    {
        mImages[retired] = {};
    }
    return true;
}

bool HostedProducer::createGeneration(usize slot) noexcept
{
    ID3D12Device* device = GetDeviceOrNull();
    LUDUS_ASSERT(device != nullptr);
    if (mDescriptors.Get() == nullptr)
    {
        const D3D12_DESCRIPTOR_HEAP_DESC description =
        {
            .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            .NumDescriptors = DESCRIPTOR_COUNT,
        };
        ID3D12DescriptorHeap* heapOrNull = nullptr;
        if (FAILED(device->CreateDescriptorHeap(&description,
                                                GetInterfaceId(heapOrNull),
                                                GetInterfaceOutput(heapOrNull))) == true)
        {
            return false;
        }
        mDescriptors.Reset(heapOrNull);
        mDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    const uint32 width = mRing.GetWidth(slot);
    const uint32 height = mRing.GetHeight(slot);
    const D3D12_HEAP_PROPERTIES heap =
    {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    const D3D12_RESOURCE_DESC description =
    {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = HOSTED_SURFACE_FORMAT,
        .SampleDesc = { .Count = 1 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = HOSTED_TEXTURE_FLAGS,
    };
    platform::HostedSurfaceImages pending;
    pending.Generation = mRing.GetGeneration(slot);
    pending.Width = width;
    pending.Height = height;
    pending.DeviceUuid = EncodeHostedAdapter(device->GetAdapterLuid()).DeviceUuid;
    Images images;
    for (usize image = 0; image < images.GetSize(); ++image)
    {
        ID3D12Resource* resourceOrNull = nullptr;
        const HRESULT created = device->CreateCommittedResource(&heap,
                                                                D3D12_HEAP_FLAG_SHARED,
                                                                &description,
                                                                D3D12_RESOURCE_STATE_COMMON,
                                                                nullptr,
                                                                GetInterfaceId(resourceOrNull),
                                                                GetInterfaceOutput(resourceOrNull));
        images[image].Resource.Reset(resourceOrNull);
        HANDLE handleOrNull = nullptr;
        if (FAILED(created) == true ||
            FAILED(device->CreateSharedHandle(resourceOrNull, nullptr, GENERIC_ALL, nullptr, &handleOrNull)) == true)
        {
            return false;
        }
        pending.Handles[image] = platform::HostedImageHandle::FromWin32Handle(handleOrNull);
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor = mDescriptors->GetCPUDescriptorHandleForHeapStart();
        descriptor.ptr += (slot * internal::HOSTED_IMAGE_COUNT + image) * mDescriptorSize;
        device->CreateRenderTargetView(resourceOrNull, nullptr, descriptor);
    }
    mPending = Move(pending);
    mImages[slot] = Move(images);
    return true;
}

HostedAcquire HostedProducer::Acquire(uint32 width,
                                      uint32 height,
                                      ID3D12Resource*& outResourceOrNull,
                                      D3D12_CPU_DESCRIPTOR_HANDLE& outDescriptor) noexcept
{
    LUDUS_ASSERT(mChannelOrNull != nullptr && mHasAcquired == false);
    if (poll() == false)
    {
        return HostedAcquire::Failed;
    }
    if (mRing.IsCurrentExtent(width, height) == false)
    {
        if (mPending.Generation != 0)
        {
            mRing.AbandonGeneration(mRing.GetCurrentSlot());
            mPending = {};
        }
        usize slot = 0;
        if (mRing.TryBeginGeneration(width, height, slot) == false)
        {
            return HostedAcquire::Pending;
        }
        if (createGeneration(slot) == false)
        {
            mRing.AbandonGeneration(slot);
            return HostedAcquire::Failed;
        }
    }
    const usize slot = mRing.GetCurrentSlot();
    if (mPending.Generation != 0)
    {
        const platform::HostedSurfaceStatus status = mChannelOrNull->PublishImages(mPending);
        if (status != platform::HostedSurfaceStatus::Ok)
        {
            return status == platform::HostedSurfaceStatus::Busy ||
                           status == platform::HostedSurfaceStatus::Disconnected
                       ? HostedAcquire::Pending
                       : HostedAcquire::Failed;
        }
        mRing.MarkPublished(slot);
        mPending = {};
    }
    if (mRing.TryAcquire(mAcquiredImage) == false)
    {
        return HostedAcquire::Pending;
    }
    mHasAcquired = true;
    outResourceOrNull = mImages[slot][mAcquiredImage].Resource.Get();
    outDescriptor = mDescriptors->GetCPUDescriptorHandleForHeapStart();
    outDescriptor.ptr += (slot * internal::HOSTED_IMAGE_COUNT + mAcquiredImage) * mDescriptorSize;
    return HostedAcquire::Ready;
}

void HostedProducer::Cancel() noexcept
{
    if (mHasAcquired == true)
    {
        mRing.CancelAcquire(mAcquiredImage);
        mHasAcquired = false;
    }
}

void HostedProducer::Submit(uint64 fence) noexcept
{
    if (mHasAcquired == true)
    {
        const internal::HostedSubmission submitted = mRing.Submit(mAcquiredImage);
        mImages[submitted.Slot][submitted.Image].Fence = fence;
        mHasAcquired = false;
    }
}
} // namespace ludus::graphics::rhi::backend::d3d12
