#include "internal/frame_renderer.h"
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/platform/base/window.h>

#include <cstdio>
#include <string_view>

namespace
{
using namespace ludus;
using namespace world_demo;
namespace rhi = graphics::rhi;
namespace rr = graphics::renderer;
namespace ft = foundation::time;
using RasterStatus = rhi::RasterStatus;
constexpr uint32 LOGICAL_WIDTH = 480;
constexpr uint32 LOGICAL_HEIGHT = 320;
constexpr uint32 BASE_WIDTH = LOGICAL_WIDTH * 2;
constexpr uint32 BASE_HEIGHT = LOGICAL_HEIGHT * 2;
constexpr uint32 WARM_FRAMES = 300;
constexpr uint32 COUNTED_FRAMES = 1200;
constexpr uint32 TARGET_HZ = 60;
constexpr uint32 PIXEL_BYTES = 4;
constexpr uint64 WAIT_LIMIT = 10 * ft::NANOSECONDS_PER_SECOND;
constexpr float32 CAMERA_SCALES[]{1, 2, 1.5F};

template <typename Operation>
bool WaitFor(rhi::DeviceHandle device, Operation operation)
{
    const uint64 start = ft::NowTicks();
    do
    {
        static_cast<void>(rhi::PollLifetime(device));
        const RasterStatus status = operation();
        if (status != RasterStatus::Pending && status != RasterStatus::NotReady)
        {
            return status == RasterStatus::Ready;
        }
        threading::SleepFor(ft::Microseconds(100));
    } while (ft::NowTicks() - start < WAIT_LIMIT);
    return false;
}

bool SaveImage(FrameRenderer& inoutRenderer, rhi::DeviceHandle device)
{
    rr::ViewportFrame lease;
    rr::ViewportImage image;
    if (WaitFor(device, [&] { return inoutRenderer.Acquire(lease, image); }) == false)
    {
        return false;
    }
    static StaticArray<uint8, BASE_WIDTH * PIXEL_BYTES> row;
    const uint32 pitch = image.Width * PIXEL_BYTES;
    // A row fits the device transfer slice even for the largest pilot configuration.
    if (pitch > row.GetSize())
    {
        return false;
    }
    for (uint32 y = 0; y < image.Height; ++y)
    {
        rhi::ReadbackTicket ticket;
        const RasterStatus requested = rhi::RequestTextureReadback(device,
                                                                   image.Texture,
                                                                   { .Y = y, .Width = image.Width, .Height = 1 },
                                                                   pitch,
                                                                   ticket);
        if ((requested != RasterStatus::Ready && requested != RasterStatus::Pending) ||
            WaitFor(device, [&] { return rhi::GetStatus(device, ticket); }) == false ||
            rhi::CopyReadback(device, ticket, row.GetData(), pitch) != RasterStatus::Ready ||
            rhi::Release(device, ticket) != RasterStatus::Ready ||
            std::fwrite(row.GetData(), 1, pitch, stdout) != pitch)
        {
            return false;
        }
    }
    return inoutRenderer.Release(lease) == RasterStatus::Ready;
}

bool Run(uint32 width, uint32 height, bool isTimed)
{
    platform::WindowManager manager;
    core::UniquePtr<platform::Window> window;
    if (manager.Initialize({}) == false ||
        manager.CreateWindow({ .Name = "Ludus Metal budget", .Width = LOGICAL_WIDTH, .Height = LOGICAL_HEIGHT },
                             window) == false)
    {
        return false;
    }
    const platform::NativeWindowInfo native = window->GetNativeWindowInfo();
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.EnableGpuTiming = isTimed;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    if (rhi::CreateDevice({}, native, description, device, surface) != rhi::DeviceStatus::Ready)
    {
        return false;
    }
    rhi::MeasurementAdapter adapter;
    rhi::DeviceInfo info;
    if (rhi::GetMeasurementAdapter(device, adapter) != RasterStatus::Ready ||
        rhi::GetDeviceInfo(device, info) != rhi::DeviceStatus::Ready ||
        info.Startup.SelectedBackend != rhi::Backend::Metal)
    {
        return false;
    }
    std::fputs(adapter.Name, stderr);
    std::fputs("\n", stderr);
    if (native.Width != BASE_WIDTH || native.Height != BASE_HEIGHT)
    {
        std::fputs("Use a 2x Retina display for the 480 x 320 point pilot window.\n", stderr);
        return false;
    }
    FrameRenderer renderer;
    Session session;
    if (session.RequestLoad(ExampleLevel()).Error != LevelError::None)
    {
        return false;
    }
    while (session.GetLoadStage() != LoadStage::Ready)
    {
        if (session.PollLoad() != Status::Success)
        {
            return false;
        }
    }
    RenderFrame frame;
    if (session.Activate() == false || session.World().Extract(0, frame, false, 1) == false ||
        WaitFor(device, [&] { return renderer.Prepare(device); }) == false ||
        rhi::SetFrameTarget(device, surface, { .Width = width, .Height = height }) != rhi::DeviceStatus::Ready)
    {
        return false;
    }
    const gameplay::camera::CameraLens original = frame.Camera.Lens;
    for (const float32 scale : CAMERA_SCALES)
    {
        frame.Camera.Lens.VerticalSpan = original.VerticalSpan * scale;
        frame.Camera.Lens.MinimumVisibleWidth = original.MinimumVisibleWidth * scale;
        rr::ViewReport report;
        if (WaitFor(device, [&] { return renderer.Update(frame, width, height, &report); }) == false)
        {
            return false;
        }
        uint64 previous = ft::NowTicks();
        uint64 due = previous;
        for (uint32 index = 0; index < WARM_FRAMES + COUNTED_FRAMES; ++index)
        {
            if (window->HandleEvent({}) == false || window->GetNativeWindowInfo().Width != native.Width ||
                window->GetNativeWindowInfo().Height != native.Height)
            {
                return false;
            }
            const RasterStatus armed = rhi::ArmGpuTiming(device);
            rhi::SubmissionToken token;
            if (WaitFor(device, [&] { return renderer.Present(surface, token); }) == false)
            {
                return false;
            }
            const uint64 returned = ft::NowTicks();
            if (WaitFor(device, [&] { return rhi::GetStatus(device, token); }) == false)
            {
                return false;
            }
            rhi::GpuTiming sample;
            const RasterStatus queried = rhi::GetGpuTiming(device, sample);
            const bool hasSample = armed == RasterStatus::Ready && queried == RasterStatus::Ready &&
                                   sample.State == rhi::GpuTimingState::Available;
            if (index >= WARM_FRAMES)
            {
                const uint64 record[]{index - WARM_FRAMES,
                                      report.Visible,
                                      report.Draws,
                                      returned - previous,
                                      static_cast<uint64>(sample.State),
                                      hasSample == true ? static_cast<uint64>(sample.DurationNanoseconds) : 0,
                                      static_cast<uint64>(armed),
                                      static_cast<uint64>(queried)};
                if (std::fwrite(record, sizeof(record), 1, stdout) != 1)
                {
                    return false;
                }
            }
            previous = returned;
            due += ft::NANOSECONDS_PER_SECOND / TARGET_HZ;
            const uint64 now = ft::NowTicks();
            if (now < due)
            {
                threading::SleepFor({due - now});
            }
            else
            {
                due = now;
            }
        }
        if (SaveImage(renderer, device) == false)
        {
            return false;
        }
    }
    renderer.Reset();
    return rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    if (argumentCount == 2 && std::string_view(arguments[1]) == "--version")
    {
        std::puts("MetalBudget 0.1.0");
        return 0;
    }
    if (argumentCount != 3)
    {
        std::fputs("Expected full, medium or half followed by timed or disabled.\n", stderr);
        return 1;
    }
    const std::string_view extent(arguments[1]);
    const std::string_view timer(arguments[2]);
    constexpr uint32 MEDIUM_WIDTH = 720;
    constexpr uint32 MEDIUM_HEIGHT = 480;
    if ((extent != "full" && extent != "half" && extent != "medium") || (timer != "timed" && timer != "disabled"))
    {
        return 1;
    }
    const uint32 width = extent == "full" ? BASE_WIDTH : (extent == "half" ? LOGICAL_WIDTH : MEDIUM_WIDTH);
    const uint32 height = extent == "full" ? BASE_HEIGHT : (extent == "half" ? LOGICAL_HEIGHT : MEDIUM_HEIGHT);
    const bool isComplete = Run(width, height, timer == "timed");
    rhi::Shutdown();
    if (isComplete == false)
    {
        std::fputs(
            "Capture failed. Verify a visible Retina window, Metal device, available storage and matching SDK.\n",
            stderr);
    }
    return isComplete == true ? 0 : 1;
}
