#pragma once

// Adapter preference for a device connected to a hosted window. A Vulkan game
// shares opaque memory with the editor only from the adapter whose deviceUUID
// and driverUUID equal the editor's; the endpoint announces them in its Welcome
// (platform::HostedSurfaceChannel::TryGetConsumerAdapter). Selection tries the
// matching adapters first, in enumeration order, and then the rest, so a host
// without a match still gets a device and the consumer refuses its images as
// AdapterMismatch. The contract is owned by docs/architecture/editor-gui-systems.md,
// "The hosted surface and frame ownership".

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/platform/hosted_surface.h>

namespace ludus::graphics::rhi::internal
{
/// True when both UUIDs of `left` equal those of `right`.
[[nodiscard]] LUDUS_INLINE bool AreAdaptersEqual(const platform::HostedAdapterIdentity& left,
                                                 const platform::HostedAdapterIdentity& right) noexcept
{
    for (foundation::usize i = 0; i < platform::HOSTED_ADAPTER_UUID_BYTES; ++i)
    {
        if (left.DeviceUuid[i] != right.DeviceUuid[i] || left.DriverUuid[i] != right.DriverUuid[i])
        {
            return false;
        }
    }
    return true;
}

/// @brief Order adapter candidates so those equal to the preferred adapter come first.
///
/// The order is stable: matching adapters keep their enumeration order, followed by
/// the others in theirs. Without a preference the order is the enumeration order.
/// @param adapters Identity of each enumerated adapter, in enumeration order.
/// @param preferredOrNull The consumer's adapter, or null when there is none.
/// @param outOrder Receives each index into `adapters` exactly once.
/// @pre `outOrder` has as many elements as `adapters`.
/// @return How many adapters equal the preferred one; zero without a preference.
[[nodiscard]] LUDUS_INLINE foundation::usize
OrderAdaptersByPreference(foundation::BorrowedView<const platform::HostedAdapterIdentity> adapters,
                          const platform::HostedAdapterIdentity* preferredOrNull,
                          foundation::BorrowedView<foundation::usize> outOrder) noexcept
{
    LUDUS_ASSERT(outOrder.GetSize() == adapters.GetSize());
    foundation::usize matchCount = 0;
    if (preferredOrNull != nullptr)
    {
        for (foundation::usize i = 0; i < adapters.GetSize(); ++i)
        {
            if (AreAdaptersEqual(adapters[i], *preferredOrNull) == true)
            {
                outOrder[matchCount] = i;
                ++matchCount;
            }
        }
    }
    foundation::usize next = matchCount;
    for (foundation::usize i = 0; i < adapters.GetSize(); ++i)
    {
        if (preferredOrNull == nullptr || AreAdaptersEqual(adapters[i], *preferredOrNull) == false)
        {
            outOrder[next] = i;
            ++next;
        }
    }
    return matchCount;
}
} // namespace ludus::graphics::rhi::internal
