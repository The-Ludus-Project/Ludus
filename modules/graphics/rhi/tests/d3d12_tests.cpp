#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/base/types.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/native_window.h>

#include "internal/readback.h"

#include <span>

#include <catch2/catch_test_macros.hpp>

#include <windows.h>
// The SDK maps CreateWindow to CreateWindowW; Ludus names its factory CreateWindow.
#undef CreateWindow

// Direct3D 12 bring-up (docs/architecture/windows-direct3d12.md). Hosts without a
// GPU, such as CI runners, start the same backend on the WARP software adapter.
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace
{
constexpr uint32 WIDTH = 17;
constexpr uint32 HEIGHT = 9;
constexpr usize BYTES_PER_PIXEL = 4;

struct Guard final
{
    Guard() noexcept
    {
        rhi::Shutdown();
    }
    ~Guard() noexcept
    {
        rhi::Shutdown();
    }
};

void RequireHeadlessDevice()
{
    REQUIRE(rhi::Start({}, { .Width = WIDTH, .Height = HEIGHT }) == rhi::StartStatus::Ready);
}

UniquePtr<ludus::platform::Window> OpenWindow(uint32 width, uint32 height)
{
    ludus::platform::WindowManager manager;
    REQUIRE(manager.Initialize({}) == true);
    UniquePtr<ludus::platform::Window> window;
    REQUIRE(manager.CreateWindow(
                {
                    .Name = "Ludus Direct3D 12 test",
                    .Width = width,
                    .Height = height,
                },
                window) == true);
    REQUIRE(window->HandleEvent({}) == true);
    return window;
}
} // namespace

TEST_CASE("Direct3D 12 is the compiled native backend and enforces its startup policy", "[rhi][windows]")
{
    Guard guard;
    const rhi::BackendAvailability compiled = rhi::GetCompiledBackends();
    CHECK(compiled.HasDirect3D12 == true);
    CHECK(compiled.Vulkan == false);
    CHECK(compiled.Metal == false);
    CHECK(rhi::Start({}, {}, rhi::BackendSelection::WebGPU) == rhi::StartStatus::Failed);
    CHECK(rhi::GetStartup().Error == rhi::StartupError::BackendUnavailable);
    rhi::Shutdown();
    rhi::WindowInfo missing;
    missing.System = ludus::platform::WindowSystem::Win32;
    CHECK(rhi::Start({}, missing) == rhi::StartStatus::Failed);
    CHECK(rhi::GetStartup().Error == rhi::StartupError::InvalidWindow);
    rhi::Shutdown();
    rhi::WindowInfo hosted;
    hosted.System = ludus::platform::WindowSystem::Hosted;
    CHECK(rhi::Start({}, hosted) == rhi::StartStatus::Failed);
    CHECK(rhi::GetStartup().Error == rhi::StartupError::InvalidWindow);
}

TEST_CASE("Direct3D 12 clears and reads back a headless target", "[rhi][windows][gpu]")
{
    Guard guard;
    RequireHeadlessDevice();
    const rhi::StartupInfo startup = rhi::GetStartup();
    CHECK(startup.SelectedBackend == rhi::Backend::Direct3D12);
    CHECK(startup.Capabilities.MaxFrameDimension2D == 16384);
    CHECK(startup.Capabilities.MaxUniformBufferSize >= 16);
    REQUIRE(rhi::SetFrameTarget(
    {
        .Width = WIDTH,
        .Height = HEIGHT,
        .Red = 0.25,
        .Green = 0.5,
        .Blue = 0.75,
        .Alpha = 1,
    }) == rhi::FrameStatus::Ready);
    REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
    CHECK(rhi::GetFrameInfo().Width == WIDTH);
    CHECK(rhi::GetFrameInfo().Height == HEIGHT);
    CHECK(rhi::GetFrameInfo().Encoding == rhi::SurfaceEncoding::Unorm);
    REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    uint8 pixels[WIDTH * HEIGHT * BYTES_PER_PIXEL]{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels) == true);
    for (usize i = 0; i < sizeof(pixels); i += BYTES_PER_PIXEL)
    {
        // UNORM conversion rounds to nearest: 0.25 -> 64, 0.5 -> 128, 0.75 -> 191.
        CHECK(pixels[i] == 64);
        CHECK(pixels[i + 1] == 128);
        CHECK(pixels[i + 2] == 191);
        CHECK(pixels[i + 3] == 255);
    }
    // A wrong-size destination is rejected without reading.
    uint8 small[BYTES_PER_PIXEL]{};
    CHECK(rhi::backend::ReadHeadlessPixels(small) == false);
}

TEST_CASE("Direct3D 12 resizes the headless target and keeps frames fenced", "[rhi][windows][gpu]")
{
    Guard guard;
    RequireHeadlessDevice();
    // More frames than slots exercise the per-slot fence waits.
    for (uint32 frame = 0; frame < 6; ++frame)
    {
        const uint32 width = frame % 2 == 0 ? 8 : 5;
        REQUIRE(rhi::SetFrameTarget({ .Width = width, .Height = 3, .Red = 1, .Green = 0, .Blue = 0, .Alpha = 1 }) ==
                rhi::FrameStatus::Ready);
        REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
        CHECK(rhi::GetFrameInfo().Width == width);
        REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    }
    uint8 pixels[5 * 3 * BYTES_PER_PIXEL]{};
    REQUIRE(rhi::backend::ReadHeadlessPixels(pixels) == true);
    CHECK(pixels[0] == 255);
    CHECK(pixels[1] == 0);
    CHECK(pixels[2] == 0);
    CHECK(pixels[3] == 255);
}

TEST_CASE("Direct3D 12 reports the bring-up limits explicitly", "[rhi][windows][gpu]")
{
    Guard guard;
    RequireHeadlessDevice();
    CHECK(rhi::SetFrameTarget({ .Width = WIDTH, .Height = HEIGHT, .ShouldPresentToSurface = false }) ==
          rhi::FrameStatus::Unsupported);
    CHECK(rhi::SetFrameTarget({ .Width = 0, .Height = HEIGHT }) == rhi::FrameStatus::Ready);
    CHECK(rhi::BeginFrameStatus() == rhi::FrameStatus::Skipped);
    CHECK(rhi::SetFrameTarget({ .Width = WIDTH, .Height = HEIGHT, .Red = -1 }) == rhi::FrameStatus::Failed);
    // No DXIL artifact exists yet, so every shader description is rejected before the backend.
    rhi::ShaderDescription description;
    description.Stage = rhi::ShaderStage::Vertex;
    description.Wgsl = "@vertex fn main() -> @builtin(position) vec4f { return vec4f(); }";
    description.WgslEntry = "main";
    rhi::ShaderHandle shader;
    CHECK(rhi::CreateShader(description, shader) == rhi::ResourceStatus::InvalidDescription);
}

TEST_CASE("Direct3D 12 legacy sessions restart and abort an open frame", "[rhi][windows][gpu]")
{
    Guard guard;
    REQUIRE(rhi::Initialize({}) == true);
    REQUIRE(rhi::ConnectWindow({ .Width = WIDTH, .Height = HEIGHT }) == true);
    REQUIRE(rhi::InitializeRendering() == true);
    REQUIRE(rhi::BeginFrame() == true);
    REQUIRE(rhi::EndFrame() == true);
    rhi::ShutdownRendering();
    REQUIRE(rhi::InitializeRendering() == true);
    REQUIRE(rhi::BeginFrame() == true);
    // Shut down with the command list still recording.
    rhi::Shutdown();
    REQUIRE(rhi::Start({}, {}) == rhi::StartStatus::Ready);
}

TEST_CASE("Direct3D 12 presents to a Win32 window and follows its extent", "[rhi][windows][gpu]")
{
    Guard guard;
    UniquePtr<ludus::platform::Window> window = OpenWindow(160, 120);
    const ludus::platform::NativeWindowInfo info = window->GetNativeWindowInfo();
    REQUIRE(info.System == ludus::platform::WindowSystem::Win32);
    REQUIRE(rhi::Start({}, info) == rhi::StartStatus::Ready);
    CHECK(rhi::GetStartup().SelectedBackend == rhi::Backend::Direct3D12);
    for (uint32 frame = 0; frame < 4; ++frame)
    {
        REQUIRE(window->HandleEvent({}) == true);
        const ludus::platform::NativeWindowInfo current = window->GetNativeWindowInfo();
        REQUIRE(rhi::SetFrameTarget({ .Width = current.Width, .Height = current.Height, .Blue = 1 }) ==
                rhi::FrameStatus::Ready);
        REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
        CHECK(rhi::GetFrameInfo().Width == current.Width);
        REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    }
    // Resizing the window resizes the swap chain on the next frame.
    const HWND handle = static_cast<HWND>(info.Win32WindowOrNull);
    REQUIRE(SetWindowPos(handle, nullptr, 0, 0, 400, 300, SWP_NOMOVE | SWP_NOZORDER) != FALSE);
    REQUIRE(window->HandleEvent({}) == true);
    const ludus::platform::NativeWindowInfo resized = window->GetNativeWindowInfo();
    CHECK(resized.Width != info.Width);
    REQUIRE(rhi::SetFrameTarget({ .Width = resized.Width, .Height = resized.Height, .Green = 1 }) ==
            rhi::FrameStatus::Ready);
    REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
    CHECK(rhi::GetFrameInfo().Width == resized.Width);
    CHECK(rhi::GetFrameInfo().Height == resized.Height);
    REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    // A minimized window skips acquisition instead of presenting.
    ShowWindow(handle, SW_MINIMIZE);
    REQUIRE(window->HandleEvent({}) == true);
    CHECK(rhi::BeginFrameStatus() == rhi::FrameStatus::Skipped);
    ShowWindow(handle, SW_RESTORE);
    REQUIRE(window->HandleEvent({}) == true);
    REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
    REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    // Release the swap chain before its window.
    rhi::Shutdown();
}
