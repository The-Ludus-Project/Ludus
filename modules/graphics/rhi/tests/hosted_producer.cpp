// Game side of the two-process hosted surface tests (hosted_surface_tests.mm on
// macOS, hosted_surface_vulkan_tests.cpp on Linux). It is an ordinary Ludus
// application: with LUDUS_HOSTED_WINDOW set, WindowManager opens a hosted
// window and CreateDevice renders into the hosted surface. Every frame clears to
// HOSTED_TEST_COLOR at the window's current extent until the editor closes the
// window. It then writes its frame statistics (ProducerStatistics) to standard
// output.
//
// Usage: ludus_rhi_hosted_producer [render|forged]
//   render  Render frames as described above.
//   forged  Publish one generation whose adapter UUIDs and handles belong to no
//           real image, so the consumer must refuse it, then wait for the close.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/hosted_surface.h>
#include <ludus/platform/native_window.h>

#include "hosted_test_protocol.h"

#include <cstdlib>
#include <string_view>
#include <unistd.h>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace platform = ludus::platform;
namespace hosted_test = ludus::graphics::rhi::hosted_test;

namespace
{
constexpr int32 EXIT_WINDOW = 2;
constexpr int32 EXIT_DEVICE = 3;
constexpr int32 EXIT_FRAME = 4;
constexpr int32 EXIT_STATISTICS = 5;
constexpr uint64 NANOSECONDS_PER_SECOND = 1'000'000'000;
constexpr uint64 RUN_DEADLINE_NANOSECONDS = 60 * NANOSECONDS_PER_SECOND;
constexpr uint32 IDLE_MICROSECONDS = 500;

bool WriteStatistics(const hosted_test::ProducerStatistics& statistics)
{
    return ::write(STDOUT_FILENO, &statistics, sizeof(statistics)) == static_cast<ssize_t>(sizeof(statistics));
}

int32 RunForged(platform::Window& inoutWindow)
{
    platform::HostedSurfaceChannel* channelOrNull = inoutWindow.GetNativeWindowInfo().HostedSurfaceOrNull;
    if (channelOrNull == nullptr)
    {
        return EXIT_WINDOW;
    }
    platform::HostedSurfaceImages images;
    images.Generation = 1;
    images.Width = hosted_test::INITIAL_WIDTH;
    images.Height = hosted_test::INITIAL_HEIGHT;
    for (usize i = 0; i < images.Handles.GetSize(); ++i)
    {
        // Ordinary open files stand in for exported memory of another adapter.
        images.Handles[i] = platform::HostedImageHandle::FromFileDescriptor(::dup(STDERR_FILENO));
        images.AllocationSizes[i] = hosted_test::FORGED_ALLOCATION_BYTES;
    }
    images.DeviceUuid.Fill(hosted_test::FORGED_UUID_BYTE);
    images.DriverUuid.Fill(hosted_test::FORGED_UUID_BYTE);
    const time::Timestamp start = time::Now();
    platform::HostedSurfaceStatus status = platform::HostedSurfaceStatus::Busy;
    while (status == platform::HostedSurfaceStatus::Busy &&
           time::Now().Nanoseconds - start.Nanoseconds < RUN_DEADLINE_NANOSECONDS)
    {
        status = channelOrNull->PublishImages(images);
        static_cast<void>(::usleep(IDLE_MICROSECONDS));
    }
    if (status != platform::HostedSurfaceStatus::Ok)
    {
        return EXIT_FRAME;
    }
    while (inoutWindow.HandleEvent({}) == true &&
           time::Now().Nanoseconds - start.Nanoseconds < RUN_DEADLINE_NANOSECONDS)
    {
        static_cast<void>(::usleep(IDLE_MICROSECONDS));
    }
    return WriteStatistics({}) == true ? 0 : EXIT_STATISTICS;
}

int32 RunRender(platform::Window& inoutWindow)
{
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::ApplicationInfo application;
    application.Name = "ludus_rhi_hosted_producer";
    if (rhi::CreateDevice(application, inoutWindow.GetNativeWindowInfo(), {}, device, surface) !=
        rhi::DeviceStatus::Ready)
    {
        return EXIT_DEVICE;
    }
    hosted_test::ProducerStatistics statistics;
    int32 exitCode = 0;
    const time::Timestamp start = time::Now();
    while (inoutWindow.HandleEvent({}) == true &&
           time::Now().Nanoseconds - start.Nanoseconds < RUN_DEADLINE_NANOSECONDS)
    {
        const platform::NativeWindowInfo info = inoutWindow.GetNativeWindowInfo();
        rhi::FrameTarget target;
        target.Width = info.Width;
        target.Height = info.Height;
        target.Red = hosted_test::HOSTED_TEST_COLOR[0];
        target.Green = hosted_test::HOSTED_TEST_COLOR[1];
        target.Blue = hosted_test::HOSTED_TEST_COLOR[2];
        target.Alpha = hosted_test::HOSTED_TEST_COLOR[3];
        if (rhi::SetFrameTarget(device, surface, target) != rhi::DeviceStatus::Ready)
        {
            exitCode = EXIT_FRAME;
            break;
        }
        const time::Timestamp before = time::Now();
        const rhi::DeviceStatus status = rhi::BeginFrame(device, surface);
        const uint64 elapsed = time::Now().Nanoseconds - before.Nanoseconds;
        if (status == rhi::DeviceStatus::Ready)
        {
            if (rhi::EndFrame(device) != rhi::DeviceStatus::Ready)
            {
                exitCode = EXIT_FRAME;
                break;
            }
            ++statistics.ReadyFrames;
        }
        else if (status == rhi::DeviceStatus::Skipped)
        {
            ++statistics.SkippedFrames;
            statistics.MaxSkippedNanoseconds =
                elapsed > statistics.MaxSkippedNanoseconds ? elapsed : statistics.MaxSkippedNanoseconds;
        }
        else
        {
            exitCode = EXIT_FRAME;
            break;
        }
        static_cast<void>(::usleep(IDLE_MICROSECONDS));
    }
    static_cast<void>(rhi::DestroyDevice(device));
    if (WriteStatistics(statistics) == false)
    {
        return EXIT_STATISTICS;
    }
    return exitCode;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    const std::string_view mode = argumentCount > 1 ? std::string_view(arguments[1]) : std::string_view("render");
    platform::WindowManager manager;
    UniquePtr<platform::Window> window;
    platform::Window::CreateInfo info;
    info.Name = "hosted producer";
    if (manager.Initialize({}) == false || manager.CreateWindow(info, window) == false ||
        window->GetNativeWindowInfo().System != platform::WindowSystem::Hosted)
    {
        return EXIT_WINDOW;
    }
    return mode == "forged" ? RunForged(*window) : RunRender(*window);
}
