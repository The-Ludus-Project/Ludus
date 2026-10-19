#pragma once

// Metal producer of the hosted surface (Objective-C++ only). Each generation is
// three IOSurface-backed RGBA8 MTLTextures; their IOSurface Mach send rights go
// to the editor through the hosted window's channel. Completion is fenced on the
// CPU: a frame is announced only after its command buffer has completed. See
// docs/architecture/editor-gui-systems.md, "The hosted surface and frame ownership".

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/platform/hosted_surface.h>

#include "hosted_ring.h"

#import <Metal/MTLCommandBuffer.h>
#import <Metal/MTLDevice.h>
#import <Metal/MTLTexture.h>

namespace ludus::graphics::rhi::backend
{
/// Outcome of HostedProducerMetal::Acquire().
enum class HostedAcquireStatus : foundation::uint8
{
    /// An image is acquired for this frame.
    Ready,
    /// No image is free, the new generation is not yet published, or the channel is gone.
    Skipped,
    /// A command buffer failed or the images could not be created.
    Failed,
};

class HostedProducerMetal final
{
public:
    HostedProducerMetal() noexcept = default;
    ~HostedProducerMetal() noexcept
    {
        Disconnect();
    }
    HostedProducerMetal(const HostedProducerMetal&) = delete;
    HostedProducerMetal& operator=(const HostedProducerMetal&) = delete;
    HostedProducerMetal(HostedProducerMetal&&) = delete;
    HostedProducerMetal& operator=(HostedProducerMetal&&) = delete;

    /// Borrow the device and the hosted window's channel; both outlive Disconnect().
    void Connect(id<MTLDevice> device, platform::HostedSurfaceChannel& inoutChannel) noexcept;
    /// Wait for submitted frames and destroy every image. Safe when not connected.
    void Disconnect() noexcept;
    [[nodiscard]] LUDUS_INLINE bool IsConnected() const noexcept
    {
        return mChannelOrNull != nullptr;
    }

    /// @brief Announce completed frames, apply releases, retire idle generations,
    /// publish a generation for the extent when it changed, then acquire an image.
    /// @param outTexture The acquired image; set only on Ready.
    [[nodiscard]] HostedAcquireStatus
    Acquire(foundation::uint32 width, foundation::uint32 height, id<MTLTexture> __strong& outTexture) noexcept;
    /// The acquired frame was committed with `command`.
    void Submit(id<MTLCommandBuffer> command) noexcept;
    /// The acquired frame was abandoned before its commit.
    void Cancel() noexcept;
    /// @brief Announce every completed frame, in submission order, without waiting.
    /// @return false when a submitted command buffer failed.
    [[nodiscard]] bool Poll() noexcept;

    /// The bookkeeping, for diagnostics and tests.
    [[nodiscard]] LUDUS_INLINE const internal::HostedRing& GetRing() const noexcept
    {
        return mRing;
    }

private:
    [[nodiscard]] bool createGeneration(foundation::usize slot) noexcept;
    void destroyGeneration(foundation::usize slot) noexcept;
    [[nodiscard]] bool publishPending() noexcept;

private:
    id<MTLDevice> mDevice = nil;
    platform::HostedSurfaceChannel* mChannelOrNull = nullptr;
    internal::HostedRing mRing;
    /// IOSurface-backed render targets per generation slot and image.
    foundation::StaticArray<foundation::StaticArray<id<MTLTexture>, internal::HOSTED_IMAGE_COUNT>,
                            internal::MAX_HOSTED_GENERATIONS>
        mTextures{};
    /// The command buffer that last wrote each image.
    foundation::StaticArray<foundation::StaticArray<id<MTLCommandBuffer>, internal::HOSTED_IMAGE_COUNT>,
                            internal::MAX_HOSTED_GENERATIONS>
        mCommands{};
    /// Images of a generation created but not yet accepted by the channel.
    platform::HostedSurfaceImages mPending;
    bool mHasPending = false;
    bool mIsAcquired = false;
    foundation::uint32 mAcquiredImage = 0;
    bool mIsChannelLost = false;
};
} // namespace ludus::graphics::rhi::backend
