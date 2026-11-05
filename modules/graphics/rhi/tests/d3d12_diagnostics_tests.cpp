#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/measurement.h>

#include "internal/d3d12_device.h"

#include <chrono>
#include <climits>
#include <string_view>
#include <thread>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace d3d12 = rhi::backend::d3d12;
namespace
{
constexpr uint32 WIDTH = 32;
constexpr uint32 HEIGHT = 24;
constexpr char SCOPE_NAME[] = "D5b/Scoped raster clear";

struct DeviceSession final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;

    void Open(bool shouldEnableTiming)
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.EnableGpuTiming = shouldEnableTiming;
        REQUIRE(rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface) ==
                rhi::DeviceStatus::Ready);
    }

    ~DeviceSession() noexcept
    {
        static_cast<void>(rhi::DestroyDevice(Device));
    }
};

// A CPU-owned gate keeps the queue pending even when the test GPU is fast.
// Its destructor releases the queue on assertion failure before device teardown.
struct SubmissionGate final
{
    d3d12::ComOwner<ID3D12Fence> Fence;

    void Hold()
    {
        ID3D12Fence* fenceOrNull = nullptr;
        REQUIRE(d3d12::GetDeviceOrNull()->CreateFence(0,
                                                      D3D12_FENCE_FLAG_NONE,
                                                      IID_ID3D12Fence,
                                                      d3d12::GetInterfaceOutput(fenceOrNull)) == S_OK);
        Fence = d3d12::ComOwner<ID3D12Fence>(fenceOrNull);
        REQUIRE(d3d12::GetQueueOrNull()->Wait(Fence.Get(), 1) == S_OK);
    }

    ~SubmissionGate() noexcept
    {
        if (Fence.Get() != nullptr)
        {
            static_cast<void>(Fence->Signal(1));
        }
    }
};

struct LogSession final
{
    LogSession()
    {
        logging::LogConfig config;
        config.EnableFile = false;
        config.EnableDebugger = false;
        REQUIRE(logging::LogSystem::Initialize(config).Status == logging::LogStatus::Ok);
    }
    ~LogSession() noexcept
    {
        static_cast<void>(logging::LogSystem::Shutdown());
    }
};

void CheckDuration(const rhi::GpuTiming& timing)
{
    CHECK(timing.State == rhi::GpuTimingState::Available);
    CHECK(timing.DurationNanoseconds >= 0);
    CHECK(timing.TickNanoseconds > 0);
    CHECK(timing.ValidBits == sizeof(uint64) * CHAR_BIT);
}
} // namespace

TEST_CASE("Direct3D 12 frame and pass timestamps follow submission fences", "[rhi][windows][gpu][d5b]")
{
    DeviceSession session;
    session.Open(true);
    const rhi::DeviceHandle device = session.Device;
    REQUIRE(rhi::ArmGpuTiming(device) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ArmGpuPassTimings(device) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(device, session.Surface, {}) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, session.Surface) == rhi::DeviceStatus::Skipped);
    rhi::GpuTiming frame;
    rhi::GpuPassTimings passes;
    REQUIRE(rhi::GetGpuTiming(device, frame) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::GetGpuPassTimings(device, passes) == rhi::RasterStatus::Ready);
    CHECK(frame.State == rhi::GpuTimingState::Armed);
    CHECK(passes.State == rhi::GpuTimingState::Armed);

    REQUIRE(rhi::SetFrameTarget(device, session.Surface, { .Width = WIDTH, .Height = HEIGHT }) ==
            rhi::DeviceStatus::Ready);
    {
        SubmissionGate gate;
        REQUIRE(rhi::BeginFrame(device, session.Surface) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::ArmGpuTiming(device) == rhi::RasterStatus::InvalidState);
        CHECK(rhi::EndGpuScope(device) == rhi::RasterStatus::InvalidState);
        rhi::GpuScopeDescription scope;
        REQUIRE(rhi::MakeGpuScope(17, {SCOPE_NAME, sizeof(SCOPE_NAME) - 1}, scope, 2) == true);
        REQUIRE(rhi::BeginGpuScope(device, scope) == rhi::RasterStatus::Ready);
        CHECK(rhi::BeginGpuScope(device, scope) == rhi::RasterStatus::InvalidState);
        REQUIRE(rhi::BeginRasterPass(device, {}) == rhi::RasterStatus::Ready);
        // Submission closes the last physical pass and its enclosing scope.
        gate.Hold();
        REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
        CHECK(rhi::EndGpuScope(device) == rhi::RasterStatus::InvalidState);
        REQUIRE(rhi::GetGpuTiming(device, frame) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::GetGpuPassTimings(device, passes) == rhi::RasterStatus::Ready);
        CHECK(frame.State == rhi::GpuTimingState::Pending);
        CHECK(passes.State == rhi::GpuTimingState::Pending);
        CHECK(rhi::ArmGpuTiming(device) == rhi::RasterStatus::InvalidState);
        CHECK(rhi::ArmGpuPassTimings(device) == rhi::RasterStatus::InvalidState);
        // A different frame can submit without overwriting either pending probe.
        REQUIRE(rhi::BeginFrame(device, session.Surface) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    }
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        REQUIRE(rhi::GetGpuTiming(device, frame) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::GetGpuPassTimings(device, passes) == rhi::RasterStatus::Ready);
        if (frame.State != rhi::GpuTimingState::Pending && passes.State != rhi::GpuTimingState::Pending)
        {
            break;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    CheckDuration(frame);
    REQUIRE(passes.State == rhi::GpuTimingState::Available);
    REQUIRE(passes.Count == 1);
    CHECK(passes.DroppedPasses == 0);
    CHECK(passes.Passes[0].Scope.Identity == 17);
    CHECK(passes.Passes[0].Scope.LogicalPassCount == 2);
    CHECK(std::string_view(passes.Passes[0].Scope.Name.GetData()) == SCOPE_NAME);
    CHECK(passes.Passes[0].ScopePassIndex == 0);
    CHECK(passes.Passes[0].Kind == rhi::GpuPassKind::Raster);
    CHECK(passes.Passes[0].HasCpuInterval == false);
    CheckDuration(passes.Passes[0].Timing);
    CHECK(passes.Passes[0].Timing.DurationNanoseconds <= frame.DurationNanoseconds);
    REQUIRE(rhi::GetGpuPassTimings(device, passes) == rhi::RasterStatus::Ready);
    CHECK(passes.Count == 1);

    REQUIRE(rhi::ArmGpuPassTimings(device) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, session.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::GetGpuPassTimings(device, passes) == rhi::RasterStatus::Ready);
    CHECK(passes.State == rhi::GpuTimingState::Available);
    CHECK(passes.Count == 0);
    REQUIRE(rhi::DestroyDevice(session.Device) == rhi::DeviceStatus::Ready);
    CHECK(rhi::GetGpuTiming(device, frame) == rhi::RasterStatus::InvalidHandle);
    CheckDuration(frame);

    session.Open(false);
    CHECK(rhi::ArmGpuTiming(session.Device) == rhi::RasterStatus::Unsupported);
    CHECK(rhi::ArmGpuPassTimings(session.Device) == rhi::RasterStatus::Unsupported);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    rhi::GpuScopeDescription annotation;
    REQUIRE(rhi::MakeGpuScope(1, {SCOPE_NAME, sizeof(SCOPE_NAME) - 1}, annotation) == true);
    {
        const rhi::GpuScope scope(session.Device, annotation);
        CHECK(rhi::BeginGpuScope(session.Device, annotation) == rhi::RasterStatus::InvalidState);
    }
    REQUIRE(rhi::EndFrame(session.Device) == rhi::DeviceStatus::Ready);
}

TEST_CASE("Direct3D 12 device removal invalidates pending timestamps and logs diagnostics", "[rhi][windows][gpu][d5b]")
{
    const LogSession logs;
    DeviceSession session;
    session.Open(true);
    ID3D12Device5* removableOrNull = nullptr;
    const HRESULT interfaceResult =
        d3d12::GetDeviceOrNull()->QueryInterface(IID_ID3D12Device5, d3d12::GetInterfaceOutput(removableOrNull));
    if (FAILED(interfaceResult) == true)
    {
        SKIP("The Windows D3D12 runtime has no ID3D12Device5 removal fixture");
    }
    d3d12::ComOwner<ID3D12Device5> removable(removableOrNull);
    REQUIRE(rhi::ArmGpuTiming(session.Device) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(session.Device) == rhi::DeviceStatus::Ready);
    const uint64 recordsBefore = logging::LogSystem::Statistics().Written;
    // Thanks to Microsoft, ID3D12Device5::RemoveDevice, for the supported
    // removal injection path and the corresponding fence completion sentinel.
    // https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice
    removable->RemoveDevice();
    rhi::GpuTiming frame;
    CHECK(rhi::GetGpuTiming(session.Device, frame) == rhi::RasterStatus::DeviceLost);
    CHECK(frame.State == rhi::GpuTimingState::Empty);
    rhi::DeviceInfo info;
    CHECK(rhi::GetDeviceInfo(session.Device, info) == rhi::DeviceStatus::DeviceLost);
    CHECK(logging::LogSystem::Statistics().Written >= recordsBefore + 2);
    removable.Reset();
    REQUIRE(rhi::DestroyDevice(session.Device) == rhi::DeviceStatus::Ready);

    session.Open(true);
    REQUIRE(rhi::GetGpuTiming(session.Device, frame) == rhi::RasterStatus::Ready);
    CHECK(frame.State == rhi::GpuTimingState::Empty);
    REQUIRE(rhi::BeginFrame(session.Device, session.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::EndFrame(session.Device) == rhi::DeviceStatus::Ready);
}
