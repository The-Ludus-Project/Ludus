#pragma once

// Editor side of the hosted surface: import the images a hosted game renders and
// hand its completed frames to a viewport. The producer side needs no API: a
// device created on a WindowSystem::Hosted window renders into the hosted surface
// through the ordinary BeginFrame/EndFrame calls. The contract is owned by
// docs/architecture/editor-gui-systems.md, "The hosted surface and frame ownership".

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/platform/hosted_window.h>

namespace ludus::graphics::rhi
{
/// `VK_QUEUE_FAMILY_EXTERNAL`: the queue family a Vulkan hosted image is released to.
inline constexpr foundation::uint32 HOSTED_VULKAN_QUEUE_FAMILY_EXTERNAL = ~foundation::uint32{0} - 1;
/// `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`: the layout of a Vulkan hosted image when presented.
inline constexpr foundation::uint32 HOSTED_VULKAN_SHADER_READ_LAYOUT = 5;

/// @brief Why a HostedSurfaceConsumer stopped showing the game's images.
enum class HostedConsumerRefusal : foundation::uint8
{
    /// Not refused.
    None,
    /// Vulkan: the game renders on another adapter or driver (`deviceUUID` or
    /// `driverUUID` differ), so opaque memory handles cannot be imported.
    AdapterMismatch,
    /// The images use a handle kind this consumer's backend cannot import (for
    /// example Mach ports on Linux), or the consumer was started on another API.
    UnsupportedHandle,
    /// An image could not be imported: the shared surface or memory does not match
    /// the announced extent and format, or the native import failed.
    ImportFailed,
    /// The game announced a frame of an image it did not share.
    ProtocolViolation,
};

/// @brief Result of a HostedSurfaceConsumer operation.
enum class HostedConsumerStatus : foundation::uint8
{
    /// Done.
    Ok,
    /// No newer frame has arrived since the last AcquireFrame().
    Empty,
    /// The consumer refused the game's images; GetRefusal() says why. Terminal for the session.
    Refused,
    /// The endpoint has no connected window.
    NotConnected,
    /// The call does not apply now: not started, already started, or a frame this consumer does not hold.
    InvalidState,
    /// The device identity is incomplete or of an API this build has no hosted import for.
    InvalidArgument,
    /// The target has no hosted surface (browser, Windows) or the consumer could not allocate its state.
    Unavailable,
};

/// @brief A presented game frame that the consumer holds until ReleaseFrame().
///
/// The native image stays valid until the frame is released and its generation is
/// retired. Use only the field of the consumer's API.
struct HostedFrame final
{
    /// Generation of the image.
    foundation::uint32 Generation = 0;
    /// Index of the image in its generation.
    foundation::uint32 Image = 0;
    /// The game's frame number; increases from frame to frame.
    foundation::uint64 Sequence = 0;
    /// Image width in pixels. Show it 1:1; there is no scaling.
    foundation::uint32 Width = 0;
    /// Image height in pixels.
    foundation::uint32 Height = 0;
    /// Pixel encoding; RGBA8 unorm, top-left origin.
    platform::HostedImageFormat Format = platform::HostedImageFormat::Rgba8Unorm;
    /// Metal: the IOSurface-backed `id<MTLTexture>` on the consumer's device, sampled read-only.
    NativeMetalTexture MetalTexture{nullptr};
    /// Vulkan: the imported `VkImage` (2D, one mip and layer, `VK_FORMAT_R8G8B8A8_UNORM`, sampled usage).
    NativeVulkanImage VulkanImage{0};
    /// @brief Vulkan: the image layout when presented, HOSTED_VULKAN_SHADER_READ_LAYOUT.
    ///
    /// Before sampling, record an acquire barrier from HOSTED_VULKAN_QUEUE_FAMILY_EXTERNAL
    /// to the consumer's queue family with this old and new layout. No barrier is needed
    /// to give the image back: the game discards its contents. Zero on Metal.
    foundation::uint32 VulkanLayout = 0;
    /// Vulkan: the queue family that owns the image when presented, HOSTED_VULKAN_QUEUE_FAMILY_EXTERNAL; zero on Metal.
    foundation::uint32 VulkanSourceQueueFamily = 0;
};

/// @brief Imports a hosted game's surface onto the editor's GPU device.
///
/// Start() borrows a started HostedWindowEndpoint and the editor's own device.
/// Pump() takes the endpoint's surface events: it imports each new generation and
/// records completed frames. AcquireFrame() returns the newest completed frame;
/// frames superseded before they were acquired are returned to the game at once.
/// ReleaseFrame() returns a frame after the editor's sampling has completed, and
/// destroys a replaced generation once none of its frames is held.
///
/// Completion is fenced on the CPU: the game announces a frame only after its GPU
/// work completed, so a frame needs no wait before sampling; and the editor must
/// release it only after its own GPU work that sampled it has completed. Neither
/// side shares fence objects.
///
/// - macOS: the images are IOSurfaces received as Mach send rights through the
///   session's Mach rendezvous; the consumer creates its textures on the given
///   `id<MTLDevice>`. IOSurfaces work across GPUs, so there is no adapter check.
/// - Linux: the images are Vulkan memory exported as opaque file descriptors
///   (`VK_KHR_external_memory_fd`). They import only when the game's
///   `deviceUUID` and `driverUUID` equal the consumer's; otherwise the consumer
///   refuses with AdapterMismatch. The consumer device must have been created with
///   `VK_KHR_external_memory_fd` enabled.
///
/// Qt-free and not synchronized: use it on the thread that owns the device and
/// pumps the endpoint. The endpoint and the device must outlive Stop().
class HostedSurfaceConsumer final
{
public:
    /// Construct a stopped consumer. Does not allocate.
    HostedSurfaceConsumer() noexcept;
    /// Stop(); the caller must already have completed GPU work that samples held frames.
    ~HostedSurfaceConsumer() noexcept;
    /// Not copyable: one consumer owns the imported images.
    HostedSurfaceConsumer(const HostedSurfaceConsumer&) = delete;
    /// Not copyable: one consumer owns the imported images.
    HostedSurfaceConsumer& operator=(const HostedSurfaceConsumer&) = delete;
    /// Not movable: frames refer to the consumer that holds them.
    HostedSurfaceConsumer(HostedSurfaceConsumer&&) = delete;
    /// Not movable: frames refer to the consumer that holds them.
    HostedSurfaceConsumer& operator=(HostedSurfaceConsumer&&) = delete;

    /// @brief Start importing the surface of the window admitted by `endpoint`.
    /// @param inoutEndpoint Started endpoint, borrowed until Stop(); the consumer takes its
    /// surface events and sends releases through it. The caller keeps pumping it.
    /// @param device The consumer's device. Metal: `Api` Metal and `Device` an `id<MTLDevice>`.
    /// Vulkan: `Api` Vulkan with `Instance`, `PhysicalDevice` and `Device`; the instance
    /// must use Vulkan 1.1 or later.
    /// @return Ok; InvalidState when already started; InvalidArgument for an incomplete
    /// identity or an API without a hosted import in this build; Unavailable when the
    /// target has no hosted surface or allocation failed.
    [[nodiscard]] HostedConsumerStatus Start(platform::HostedWindowEndpoint& inoutEndpoint,
                                             const NativeDeviceIdentity& device) noexcept;

    /// @brief Destroy every imported image and forget held frames.
    /// @pre GPU work that samples any held frame has completed.
    void Stop() noexcept;

    /// @brief Import new generations and record completed frames from the endpoint.
    ///
    /// Call after HostedWindowEndpoint::Pump(), once per editor frame.
    /// @return Ok; Refused once the game's images were refused; NotConnected when the
    /// window is gone; InvalidState when stopped.
    [[nodiscard]] HostedConsumerStatus Pump() noexcept;

    /// True when a completed frame newer than every acquired frame is waiting.
    [[nodiscard]] bool HasFrame() const noexcept;

    /// @brief Take the newest completed frame.
    ///
    /// The consumer holds it until ReleaseFrame(). Several frames may be held at once,
    /// for example one on screen and one pending; the game renders into the rest.
    /// @param outFrame The frame; unchanged unless Ok.
    /// @return Ok, Empty, Refused or InvalidState.
    [[nodiscard]] HostedConsumerStatus AcquireFrame(HostedFrame& outFrame) noexcept;

    /// @brief Return a held frame to the game.
    /// @pre The consumer's GPU work that sampled the frame has completed.
    /// @param frame A frame from AcquireFrame() that is still held.
    /// @return Ok (also when the window has gone; the frame is forgotten); InvalidState
    /// for a frame this consumer does not hold.
    [[nodiscard]] HostedConsumerStatus ReleaseFrame(const HostedFrame& frame) noexcept;

    /// Why the consumer refused the game's images, or None.
    [[nodiscard]] HostedConsumerRefusal GetRefusal() const noexcept;
    /// Generations with imported images, the newest and those still held.
    [[nodiscard]] foundation::usize GetImportedGenerationCount() const noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::graphics::rhi
