#pragma once

// Hosted surface transport: the GPU images a game renders for a hosted window and
// the Present/Release exchange that returns them. Platform moves the native
// sharing handles and the messages; RHI allocates, renders and imports the images.
// The contract is owned by docs/architecture/editor-gui-systems.md, "The hosted
// surface and frame ownership" and "Hosted surface channel".

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::platform
{
/// Images in each hosted surface generation.
inline constexpr foundation::usize HOSTED_SURFACE_IMAGE_COUNT = 3;
/// Bytes of a Vulkan `deviceUUID` or `driverUUID` (`VK_UUID_SIZE`).
inline constexpr foundation::usize HOSTED_ADAPTER_UUID_BYTES = 16;

/// @brief Pixel encoding of hosted surface images.
enum class HostedImageFormat : foundation::uint8
{
    /// Four 8-bit unsigned normalized channels in R, G, B, A byte order, not sRGB-encoded.
    Rgba8Unorm = 1,
};

/// @brief Kind of operating-system handle that shares one hosted image.
enum class HostedHandleKind : foundation::uint8
{
    /// No handle.
    None = 0,
    /// macOS: a Mach send right from `IOSurfaceCreateMachPort`.
    MachPort = 1,
    /// Linux: an opaque file descriptor from `vkGetMemoryFdKHR`
    /// (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`).
    OpaqueFileDescriptor = 2,
};

/// @brief Owning operating-system handle of one hosted image.
///
/// Move-only. Destruction or Reset() releases what it owns: a Mach send right is
/// deallocated with `mach_port_deallocate`, a file descriptor is closed. The
/// Release calls give ownership back to the caller without releasing anything.
/// Not synchronized; one owner at a time.
class HostedImageHandle final
{
public:
    /// Construct an empty handle.
    HostedImageHandle() noexcept = default;
    /// Release the owned handle, if any.
    ~HostedImageHandle() noexcept
    {
        Reset();
    }
    /// Not copyable: a handle has exactly one owner.
    HostedImageHandle(const HostedImageHandle&) = delete;
    /// Not copyable: a handle has exactly one owner.
    HostedImageHandle& operator=(const HostedImageHandle&) = delete;
    /// Take the handle of `other`, leaving it empty.
    HostedImageHandle(HostedImageHandle&& other) noexcept;
    /// Release this handle and take the handle of `other`, leaving it empty.
    HostedImageHandle& operator=(HostedImageHandle&& other) noexcept;

    /// @brief Take ownership of one Mach send right.
    /// @param sendRight A `mach_port_t` send right owned by the caller; zero yields an empty handle.
    [[nodiscard]] static HostedImageHandle FromMachPort(foundation::uint32 sendRight) noexcept;
    /// @brief Take ownership of one open file descriptor.
    /// @param descriptor A descriptor owned by the caller; a negative value yields an empty handle.
    [[nodiscard]] static HostedImageHandle FromFileDescriptor(foundation::int32 descriptor) noexcept;

    /// Kind of the owned handle; None when empty.
    [[nodiscard]] LUDUS_INLINE HostedHandleKind GetKind() const noexcept
    {
        return mKind;
    }
    /// True when a handle is owned.
    [[nodiscard]] LUDUS_INLINE bool IsValid() const noexcept
    {
        return mKind != HostedHandleKind::None;
    }
    /// The owned Mach send right (borrowed), or zero.
    [[nodiscard]] LUDUS_INLINE foundation::uint32 GetMachPort() const noexcept
    {
        return mMachPort;
    }
    /// The owned file descriptor (borrowed), or -1.
    [[nodiscard]] LUDUS_INLINE foundation::int32 GetFileDescriptor() const noexcept
    {
        return mFileDescriptor;
    }
    /// @brief Give the Mach send right to the caller and leave this handle empty.
    /// @return The send right, or zero when this handle holds no Mach port.
    [[nodiscard]] foundation::uint32 ReleaseMachPort() noexcept;
    /// @brief Give the file descriptor to the caller and leave this handle empty.
    /// @return The descriptor, or -1 when this handle holds no file descriptor.
    [[nodiscard]] foundation::int32 ReleaseFileDescriptor() noexcept;
    /// Release the owned handle, if any, and become empty.
    void Reset() noexcept;

private:
    HostedHandleKind mKind = HostedHandleKind::None;
    foundation::uint32 mMachPort = 0;
    foundation::int32 mFileDescriptor = -1;
};

/// @brief The images of one hosted surface generation.
///
/// A generation is a fixed extent and format. The producer allocates a new
/// generation whenever its extent changes; image indices are 0 to
/// HOSTED_SURFACE_IMAGE_COUNT - 1 within a generation.
struct HostedSurfaceImages final
{
    /// Nonzero identity, strictly increasing within one session.
    foundation::uint32 Generation = 0;
    /// Image width in pixels, 1 to 32768.
    foundation::uint32 Width = 0;
    /// Image height in pixels, 1 to 32768.
    foundation::uint32 Height = 0;
    /// Pixel encoding of every image.
    HostedImageFormat Format = HostedImageFormat::Rgba8Unorm;
    /// @brief Vulkan only: byte size of each exported memory allocation, which the
    /// importer must allocate exactly. Zero on macOS.
    foundation::StaticArray<foundation::uint64, HOSTED_SURFACE_IMAGE_COUNT> AllocationSizes{};
    /// Vulkan only: the producer's memory type index for every image. Zero on macOS.
    foundation::uint32 MemoryTypeIndex = 0;
    /// Vulkan only: the producer's `VkPhysicalDeviceIDProperties::deviceUUID`. Zero on macOS.
    foundation::StaticArray<foundation::uint8, HOSTED_ADAPTER_UUID_BYTES> DeviceUuid{};
    /// Vulkan only: the producer's `VkPhysicalDeviceIDProperties::driverUUID`. Zero on macOS.
    foundation::StaticArray<foundation::uint8, HOSTED_ADAPTER_UUID_BYTES> DriverUuid{};
    /// One owned sharing handle per image: Mach send rights on macOS, opaque fds on Linux.
    foundation::StaticArray<HostedImageHandle, HOSTED_SURFACE_IMAGE_COUNT> Handles{};
};

/// @brief The consumer has finished with one presented image.
struct HostedImageRelease final
{
    /// Generation of the released image.
    foundation::uint32 Generation = 0;
    /// Index of the released image in its generation.
    foundation::uint32 Image = 0;
};

/// @brief Kind of a HostedSurfaceEvent.
enum class HostedSurfaceEventKind : foundation::uint8
{
    /// A new generation: HostedSurfaceEvent::Images is set.
    Images,
    /// A completed frame: Generation, Image and Sequence are set.
    Present,
};

/// @brief One surface message the editor endpoint received from its window, in arrival order.
struct HostedSurfaceEvent final
{
    /// Which fields are meaningful.
    HostedSurfaceEventKind Kind = HostedSurfaceEventKind::Present;
    /// Images: the generation and its owned sharing handles.
    HostedSurfaceImages Images;
    /// Present: generation of the completed image.
    foundation::uint32 Generation = 0;
    /// Present: index of the completed image in its generation.
    foundation::uint32 Image = 0;
    /// Present: the producer's frame sequence number.
    foundation::uint64 Sequence = 0;
};

/// @brief Result of a HostedSurfaceChannel operation.
enum class HostedSurfaceStatus : foundation::uint8
{
    /// Done. For TakeRelease(), a release was returned.
    Ok,
    /// Nothing is available now (TakeRelease()), or the channel cannot accept the
    /// message without blocking. Nothing was sent or consumed; retry on a later frame.
    Busy,
    /// The channel is closed or was lost. The window reports a close from its next HandleEvent().
    Disconnected,
    /// An argument is out of range: zero generation or extent, an image index or
    /// handle kind the platform does not use, or a non-increasing sequence.
    InvalidArgument,
    /// The endpoint offers no transport for the handle kind (no Mach rendezvous, or
    /// a target without file-descriptor passing).
    Unavailable,
};

/// @brief Producer side of the hosted surface channel, owned by a hosted window.
///
/// RHI reaches it through NativeWindowInfo::HostedSurfaceOrNull when it connects a
/// device to a WindowSystem::Hosted window. The order of calls is: PublishImages()
/// for a generation, then Present() for each completed frame of it, and
/// TakeRelease() to learn which presented images the consumer has returned.
/// Every call is nonblocking. Use it on the thread that pumps the window; it is
/// not synchronized. The channel belongs to the window and is valid until the
/// window is destroyed.
class HostedSurfaceChannel
{
public:
    /// @brief Send a new generation to the consumer.
    ///
    /// On macOS the Mach send rights travel through the session's Mach rendezvous
    /// and the description over the socket; on Linux the file descriptors travel
    /// with the description using `SCM_RIGHTS`.
    /// @param inoutImages Generation to publish. On Ok its handles are consumed and
    /// left empty; on any other result they stay owned by the caller.
    /// @return Ok, Busy, Disconnected, InvalidArgument or Unavailable.
    [[nodiscard]] virtual HostedSurfaceStatus PublishImages(HostedSurfaceImages& inoutImages) noexcept = 0;

    /// @brief Announce one completed frame.
    /// @pre The GPU work that wrote the image has completed; the consumer samples it without further waits.
    /// @param generation A published generation.
    /// @param image Image index in that generation.
    /// @param sequence Frame number, strictly increasing across the session.
    /// @return Ok, Busy, Disconnected or InvalidArgument.
    [[nodiscard]] virtual HostedSurfaceStatus
    Present(foundation::uint32 generation, foundation::uint32 image, foundation::uint64 sequence) noexcept = 0;

    /// @brief Take the oldest release the consumer has sent, receiving from the channel first.
    ///
    /// Window events received on the way stay queued for the next HandleEvent(),
    /// in order.
    /// @param outRelease The release; unchanged unless Ok.
    /// @return Ok, Busy when none is pending, or Disconnected.
    [[nodiscard]] virtual HostedSurfaceStatus TakeRelease(HostedImageRelease& outRelease) noexcept = 0;

protected:
    /// Constructed only as part of a hosted window.
    HostedSurfaceChannel() noexcept = default;
    /// Not destroyed through this interface.
    ~HostedSurfaceChannel() = default;
    /// Not copyable through this interface.
    HostedSurfaceChannel(const HostedSurfaceChannel&) = default;
    /// Not copyable through this interface.
    HostedSurfaceChannel& operator=(const HostedSurfaceChannel&) = default;
    /// Not movable through this interface.
    HostedSurfaceChannel(HostedSurfaceChannel&&) = default;
    /// Not movable through this interface.
    HostedSurfaceChannel& operator=(HostedSurfaceChannel&&) = default;
};
} // namespace ludus::platform
