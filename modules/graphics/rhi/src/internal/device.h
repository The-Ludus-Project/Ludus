#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::rhi::internal
{
/// Bind the handle's owner and report its raster device readiness.
[[nodiscard]] RasterStatus ValidateDeviceReadiness(DeviceHandle device) noexcept;

// Process identities are never reused, including after failed/lost sessions.
// Kept separate from callback-attempt tokens so Auto retries keep one owner.
struct DeviceOwnerSequence final
{
    ludus::foundation::uint64 Next = 1;

    ludus::foundation::uint64 Take() noexcept
    {
        const auto owner = Next;
        Next = owner == ~ludus::foundation::uint64{0} ? 0 : owner == 0 ? 0 : owner + 1;
        return owner;
    }
};
} // namespace ludus::graphics::rhi::internal
