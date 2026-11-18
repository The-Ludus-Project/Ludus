#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/device.h>

// R14c multiple device owners. Every facade and backend state table keeps one record per
// owner slot and addresses the record of the bound slot. The facade binds a slot on the
// owner thread when it admits a device handle, a startup callback token or a device-less
// facade call (slot 0), so each operation reads and writes only its own owner's state.
// Worker threads never read the binding: they touch only captured inputs. See
// docs/development/fullscreen-rendering.md, "Multiple device owners (R14c)".
namespace ludus::graphics::rhi::internal
{
/// Owner slot the current owner-thread operation addresses; below MAX_DEVICE_OWNERS.
inline foundation::usize gBoundOwnerSlot = 0;

/// Slot whose state tables the current owner-thread operation reads and writes.
[[nodiscard]] LUDUS_INLINE foundation::usize GetBoundOwnerSlot() noexcept
{
    return gBoundOwnerSlot;
}
/// Address `slot`'s state for the following owner-thread work. Between operations only;
/// a slot at or above MAX_DEVICE_OWNERS is ignored.
LUDUS_INLINE void BindOwnerSlot(foundation::usize slot) noexcept
{
    if (slot < MAX_DEVICE_OWNERS)
    {
        gBoundOwnerSlot = slot;
    }
}
/// Slot of the live device owner `owner`, or MAX_DEVICE_OWNERS when no slot holds it.
/// Does not change the binding.
[[nodiscard]] foundation::usize FindOwnerSlot(foundation::uint64 owner) noexcept;
/// Slot whose session issued startup callback `token`, or MAX_DEVICE_OWNERS for a stale
/// or zero token. Does not change the binding.
[[nodiscard]] foundation::usize FindTokenSlot(foundation::uint32 token) noexcept;
} // namespace ludus::graphics::rhi::internal

namespace ludus::graphics::rhi::backend
{
/// @brief Device owners this backend hosts at once when admitting a new owner, between 1 and
/// MAX_DEVICE_OWNERS, counting the live owners and the new one.
///
/// `isNativeDevice` is true for an owner that adopts an imported native device
/// (CreateNativeDevice). A result of 1 admits the new owner only while no other owner is
/// live; the facade then reports InvalidState rather than CapacityExceeded.
[[nodiscard]] foundation::usize GetDeviceOwnerLimit(bool isNativeDevice) noexcept;
} // namespace ludus::graphics::rhi::backend
