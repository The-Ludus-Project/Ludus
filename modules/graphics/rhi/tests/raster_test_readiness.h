#pragma once

#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace ludus::graphics::rhi::testing
{
// Both synchronous and queued creation must reach Ready before a fixture binds a resource.
template <typename Handle>
void RequireReady(DeviceHandle device, Handle handle, RasterStatus status = RasterStatus::Pending)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (status == RasterStatus::Pending && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
        status = GetStatus(device, handle);
    }
    REQUIRE(status == RasterStatus::Ready);
}
} // namespace ludus::graphics::rhi::testing
