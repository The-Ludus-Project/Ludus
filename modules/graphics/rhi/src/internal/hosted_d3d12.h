#pragma once

#include "d3d12_device.h"
#include "hosted_ring.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/platform/hosted_surface.h>

namespace ludus::graphics::rhi::backend::d3d12
{
inline constexpr DXGI_FORMAT HOSTED_SURFACE_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

[[nodiscard]] platform::HostedAdapterIdentity EncodeHostedAdapter(LUID adapter) noexcept;
[[nodiscard]] LUID DecodeHostedAdapter(const platform::HostedAdapterIdentity& adapter) noexcept;

enum class HostedAcquire : uint8
{
    Ready,
    Pending,
    Failed,
};

class HostedProducer final
{
public:
    void Connect(platform::HostedSurfaceChannel& inoutChannel) noexcept;
    void Disconnect() noexcept;
    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] HostedAcquire Acquire(uint32 width,
                                        uint32 height,
                                        ID3D12Resource*& outResourceOrNull,
                                        D3D12_CPU_DESCRIPTOR_HANDLE& outDescriptor) noexcept;
    void Cancel() noexcept;
    void Submit(uint64 fence) noexcept;

private:
    [[nodiscard]] bool poll() noexcept;
    [[nodiscard]] bool createGeneration(usize slot) noexcept;

    struct Image final
    {
        ComOwner<ID3D12Resource> Resource;
        uint64 Fence = 0;
    };
    using Images = StaticArray<Image, internal::HOSTED_IMAGE_COUNT>;

    platform::HostedSurfaceChannel* mChannelOrNull = nullptr;
    internal::HostedRing mRing;
    StaticArray<Images, internal::MAX_HOSTED_GENERATIONS> mImages{};
    platform::HostedSurfaceImages mPending;
    ComOwner<ID3D12DescriptorHeap> mDescriptors;
    uint32 mDescriptorSize = 0;
    uint32 mAcquiredImage = 0;
    bool mHasAcquired = false;
};
} // namespace ludus::graphics::rhi::backend::d3d12
