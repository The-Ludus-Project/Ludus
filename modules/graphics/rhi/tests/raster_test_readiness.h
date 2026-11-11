#pragma once

#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

#include <catch2/catch_test_macros.hpp>

namespace ludus::graphics::rhi::testing
{
// Both synchronous and queued creation must reach Ready before a fixture binds a resource.
template <typename Handle>
void RequireReady(DeviceHandle device, Handle handle, RasterStatus status = RasterStatus::Pending)
{
    foundation::time::Deadline deadline;
    REQUIRE(deadline.Arm(foundation::time::Now(), foundation::time::Seconds(5)) == foundation::time::TimeStatus::Ok);
    while (status == RasterStatus::Pending && deadline.IsExpired(foundation::time::Now()) == false)
    {
        foundation::threading::YieldThread();
        status = GetStatus(device, handle);
    }
    REQUIRE(status == RasterStatus::Ready);
}
} // namespace ludus::graphics::rhi::testing
