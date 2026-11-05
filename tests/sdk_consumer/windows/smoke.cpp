#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/entropy.h>
#include <ludus/platform/native_window.h>

#include "diagnostic.h"

#include <span>

using ludus::foundation::float32;
using ludus::foundation::StaticArray;
using ludus::foundation::uint32;
using ludus::foundation::uint8;
using ludus::foundation::UniquePtr;
using ludus::foundation::usize;
namespace filesystem = ludus::foundation::filesystem;
namespace rhi = ludus::graphics::rhi;
namespace platform = ludus::platform;

static_assert(LUDUS_BUILD_FLAVOR_ID == LUDUS_EXPECTED_SDK_FLAVOR_ID);
static_assert(LUDUS_ENABLE_ASSERTS == LUDUS_EXPECTED_SDK_ASSERTS);

int main()
{
    constexpr uint32 WIDTH = 160;
    constexpr uint32 HEIGHT = 120;
    constexpr usize DIAGNOSTIC_UNIFORM_VALUES = 12;

    uint8 randomByte = 0;
    LUDUS_REQUIRE(platform::TryFillSystemEntropy(&randomByte, 1) == true);
    filesystem::Directory directory;
    LUDUS_REQUIRE(directory.Open(".").Succeeded() == true);

    platform::WindowManager windows;
    UniquePtr<platform::Window> window;
    LUDUS_REQUIRE(windows.Initialize({}) == true);
    LUDUS_REQUIRE(windows.CreateWindow({ .Name = "Installed Ludus SDK WARP smoke", .Width = WIDTH, .Height = HEIGHT },
                                       window) == true);
    LUDUS_REQUIRE(window->HandleEvent({}) == true);
    const platform::NativeWindowInfo extent = window->GetNativeWindowInfo();
    LUDUS_REQUIRE(extent.System == platform::WindowSystem::Win32);

    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    LUDUS_REQUIRE(rhi::CreateDevice({}, extent, {}, device, surface) == rhi::DeviceStatus::Ready);
    rhi::MeasurementAdapter adapter;
    LUDUS_REQUIRE(rhi::GetMeasurementAdapter(device, adapter) == rhi::RasterStatus::Ready);
    LUDUS_REQUIRE(adapter.SoftwareKnown == true && adapter.Software == true);

    const StaticArray<float32, DIAGNOSTIC_UNIFORM_VALUES> values{
        static_cast<float32>(extent.Width),
        static_cast<float32>(extent.Height),
        0,
        0,
        0,
        0,
        0,
        0,
        1,
        1,
        1,
        1,
    };
    const std::span<const uint8> bytes{reinterpret_cast<const uint8*>(values.GetData()), sizeof(values)};
    rhi::ShaderHandle vertex;
    rhi::ShaderHandle fragment;
    rhi::UniformHandle uniform;
    rhi::PipelineHandle pipeline;
    LUDUS_REQUIRE(rhi::CreateShader(device, ludus::shaders::diagnostic::Vertex(), vertex) ==
                  rhi::ResourceStatus::Ready);
    LUDUS_REQUIRE(rhi::CreateShader(device, ludus::shaders::diagnostic::Fragment(), fragment) ==
                  rhi::ResourceStatus::Ready);
    LUDUS_REQUIRE(rhi::CreateUniform(device, bytes.size(), uniform) == rhi::ResourceStatus::Ready);
    LUDUS_REQUIRE(rhi::CreatePipeline(device, {vertex, fragment, uniform}, pipeline) == rhi::ResourceStatus::Ready);
    LUDUS_REQUIRE(rhi::UpdateUniform(device, uniform, bytes) == rhi::ResourceStatus::Ready);
    LUDUS_REQUIRE(rhi::SetFrameTarget(device, surface, { .Width = extent.Width, .Height = extent.Height }) ==
                  rhi::DeviceStatus::Ready);
    LUDUS_REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    LUDUS_REQUIRE(rhi::DrawFullscreen(device, pipeline) == rhi::ResourceStatus::Ready);
    LUDUS_REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    LUDUS_REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
}
