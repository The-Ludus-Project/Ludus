#pragma once

// Native side of HostedSurfaceConsumer. The portable bookkeeping
// (hosted_consumer.cpp) owns generations, frames and releases, and names native
// objects only by generation slot and image index. Each backend keeps its native
// objects behind HostedConsumerBackend: Metal wraps IOSurfaces received as Mach
// send rights; Vulkan imports opaque memory file descriptors and transfers queue
// family ownership on the consumer's queue. Builds without a hosted surface refuse
// every import.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/graphics/rhi/hosted_surface.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/platform/hosted_surface.h>

namespace ludus::graphics::rhi::internal
{
/// Generation slots of one consumer: the newest plus older ones whose frames are still held.
/// The producer keeps at most three alive, so four leave room for a late release.
inline constexpr foundation::usize HOSTED_GENERATION_SLOTS = 4;

/// Native state of one consumer, defined by its backend.
struct HostedConsumerBackend;
/// Destroys a backend made by CreateHostedConsumerBackend(); defined by each backend.
struct HostedConsumerBackendDeleter final
{
    void operator()(HostedConsumerBackend* backendOrNull) const noexcept;
};
/// Owner of a consumer's native state.
using HostedConsumerBackendOwner = foundation::UniquePtr<HostedConsumerBackend, HostedConsumerBackendDeleter>;

/// True when this build can import hosted images onto a device of `api`.
[[nodiscard]] bool IsHostedImportAvailable(Backend api) noexcept;

/// @brief Create the native state for `device`, whose API IsHostedImportAvailable() accepts.
/// @return Ok; InvalidArgument when the identity lacks what the backend needs; Unavailable when
/// its entry points cannot be loaded or allocation failed. `outBackend` is unchanged unless Ok.
[[nodiscard]] HostedConsumerStatus CreateHostedConsumerBackend(const NativeDeviceIdentity& device,
                                                               HostedConsumerBackendOwner& outBackend) noexcept;

/// @brief Import one generation into an empty slot.
///
/// Takes the handles out of `inoutImages` whatever the outcome; a failed import releases
/// everything it created.
/// @return None on success, or why the images cannot be used.
[[nodiscard]] HostedConsumerRefusal ImportHostedGeneration(HostedConsumerBackend& inoutBackend,
                                                           foundation::usize slot,
                                                           platform::HostedSurfaceImages& inoutImages) noexcept;

/// @brief Destroy the native objects of a slot, leaving it empty.
/// @pre No image of the slot has a queued release, and work that sampled its images has completed.
void DestroyHostedGeneration(HostedConsumerBackend& inoutBackend, foundation::usize slot) noexcept;

/// @brief Make an image ready for the consumer's sampling and describe it in `inoutFrame`.
///
/// Vulkan submits the acquire half of the game's queue family transfer to the consumer's
/// queue; call on the thread that submits to it. Metal has nothing to submit.
/// @return False when the image cannot be made ready; `inoutFrame` is unchanged.
[[nodiscard]] bool AcquireHostedImage(HostedConsumerBackend& inoutBackend,
                                      foundation::usize slot,
                                      foundation::uint32 image,
                                      HostedFrame& inoutFrame) noexcept;

/// @brief Commit an image's return behind everything already submitted to the consumer's queue.
///
/// Vulkan submits the image's release to the external queue family with a fence; Metal commits
/// an empty command buffer. Call on the thread that submits to the queue.
/// @return False when nothing could be committed (the caller cannot release after the queue).
[[nodiscard]] bool
EnqueueHostedRelease(HostedConsumerBackend& inoutBackend, foundation::usize slot, foundation::uint32 image) noexcept;

/// @brief Finish a queued release once the queue has completed it (or the device was lost).
/// @return True when the release is finished, or none is queued; false while it is pending.
[[nodiscard]] bool TryCompleteHostedRelease(HostedConsumerBackend& inoutBackend,
                                            foundation::usize slot,
                                            foundation::uint32 image) noexcept;

/// Block until a queued release completes, then finish it; teardown only, never while presenting.
void CompleteHostedRelease(HostedConsumerBackend& inoutBackend,
                           foundation::usize slot,
                           foundation::uint32 image) noexcept;
} // namespace ludus::graphics::rhi::internal
