#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/base/types.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/native_window.h>

#include "internal/readback.h"
#include "internal/resources.h"
#if defined(LUDUS_TEST_DXIL)
#    include "diagnostic.h"
#endif

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
          rhi::FrameStatus::Ready);
    CHECK(rhi::SetFrameTarget({ .Width = 0, .Height = HEIGHT }) == rhi::FrameStatus::Ready);
    CHECK(rhi::BeginFrameStatus() == rhi::FrameStatus::Skipped);
    CHECK(rhi::SetFrameTarget({ .Width = WIDTH, .Height = HEIGHT, .Red = -1 }) == rhi::FrameStatus::Failed);
    // Only a DXIL container reaches this backend; other targets' artifacts are rejected first.
    rhi::ShaderDescription description;
    description.Stage = rhi::ShaderStage::Vertex;
    description.Wgsl = "@vertex fn main() -> @builtin(position) vec4f { return vec4f(); }";
    description.WgslEntry = "main";
    rhi::ShaderHandle shader;
    CHECK(rhi::CreateShader(description, shader) == rhi::ResourceStatus::InvalidDescription);
    const uint8 notContainer[] = {'D', 'X', 'B', 'C'};
    description.Dxil = notContainer;
    description.DxilEntry = "main";
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
    REQUIRE(rhi::SetFrameTarget({ .Width = WIDTH, .Height = HEIGHT, .ShouldPresentToSurface = false }) ==
            rhi::FrameStatus::Ready);
    REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
    REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    ShowWindow(handle, SW_RESTORE);
    REQUIRE(window->HandleEvent({}) == true);
    REQUIRE(rhi::SetFrameTarget({ .Width = resized.Width, .Height = resized.Height }) == rhi::FrameStatus::Ready);
    REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
    REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    // Release the swap chain before its window.
    rhi::Shutdown();
}

#if defined(LUDUS_TEST_DXIL)
namespace
{
/// Diagnostic uniform values: resolution, elapsed time, direction and tint, padded as
/// the shader's constant buffer (direction at byte 16, tint at byte 32).
constexpr usize DIAGNOSTIC_VALUE_COUNT = 12;
constexpr usize DIAGNOSTIC_UNIFORM_BYTES = DIAGNOSTIC_VALUE_COUNT * sizeof(float32);
using ludus::graphics::rhi::internal::UNIFORM_CAPACITY;
/// Readback error budget in 8-bit steps, matching the Vulkan and Metal pixel tests.
constexpr float64 CHANNEL_TOLERANCE = 2.5;

void WriteDiagnosticUniform(uint32 width, uint32 height, float32 elapsed, std::span<uint8> outBytes)
{
    const float32 values[DIAGNOSTIC_VALUE_COUNT] = {
        static_cast<float32>(width),
        static_cast<float32>(height),
        elapsed,
        0,
        0.2F,
        0.4F,
        0.6F,
        0,
        0.6F,
        0.4F,
        0.2F,
        1,
    };
    const uint8* source = reinterpret_cast<const uint8*>(values);
    for (usize i = 0; i < DIAGNOSTIC_UNIFORM_BYTES; ++i)
    {
        outBytes[i] = source[i];
    }
}

/// Pixels farther than the tolerance from the diagnostic image of that extent and time.
usize CountDiagnosticErrors(std::span<const uint8> pixels, uint32 width, uint32 height, float32 elapsed)
{
    usize errors = 0;
    for (usize y = 0; y < height; ++y)
    {
        for (usize x = 0; x < width; ++x)
        {
            const float64 cx = (static_cast<float64>(x) + 0.5 - width * 0.5) / height;
            const float64 cy = (static_cast<float64>(y) + 0.5 - height * 0.5) / height;
            const float64 expected[BYTES_PER_PIXEL] = {
                0.6 * (static_cast<float64>(x) + 0.5) / width + 0.01 * static_cast<float64>(elapsed),
                0.4 * (static_cast<float64>(y) + 0.5) / height + 0.02 * static_cast<float64>(elapsed),
                0.2 * (cx * cx + cy * cy < 0.04 ? 1 : 0) + 0.03 * static_cast<float64>(elapsed),
                1,
            };
            for (usize channel = 0; channel < BYTES_PER_PIXEL; ++channel)
            {
                const float64 error =
                    static_cast<float64>(pixels[(y * width + x) * BYTES_PER_PIXEL + channel]) - expected[channel] * 255;
                if (error > CHANNEL_TOLERANCE || error < -CHANNEL_TOLERANCE)
                {
                    ++errors;
                }
            }
        }
    }
    return errors;
}
} // namespace

TEST_CASE("Direct3D 12 checks the DXIL stage and entry before creating shaders", "[rhi][windows][gpu]")
{
    Guard guard;
    RequireHeadlessDevice();
    const rhi::ShaderDescription vertexDescription = ludus::shaders::diagnostic::Vertex();
    // Reflection sizes the module's constant buffer for both stages.
    CHECK(vertexDescription.UniformSize == DIAGNOSTIC_UNIFORM_BYTES);
    CHECK(ludus::shaders::diagnostic::Fragment().UniformSize == DIAGNOSTIC_UNIFORM_BYTES);
    rhi::ShaderDescription wrongEntry = vertexDescription;
    wrongEntry.DxilEntry = "missing";
    rhi::ShaderHandle failed;
    CHECK(rhi::CreateShader(wrongEntry, failed) == rhi::ResourceStatus::Failed);
    CHECK(rhi::Destroy(failed) == rhi::ResourceStatus::Ready);
    rhi::ShaderDescription wrongStage = vertexDescription;
    wrongStage.Stage = rhi::ShaderStage::Fragment;
    rhi::ShaderHandle mismatched;
    CHECK(rhi::CreateShader(wrongStage, mismatched) == rhi::ResourceStatus::Failed);
    CHECK(rhi::Destroy(mismatched) == rhi::ResourceStatus::Ready);
    // A truncated container no longer matches its recorded size.
    rhi::ShaderDescription truncated = vertexDescription;
    truncated.Dxil = truncated.Dxil.first(truncated.Dxil.size() - 1);
    rhi::ShaderHandle rejected;
    CHECK(rhi::CreateShader(truncated, rejected) == rhi::ResourceStatus::InvalidDescription);
    rhi::ShaderHandle vertex;
    CHECK(rhi::CreateShader(vertexDescription, vertex) == rhi::ResourceStatus::Ready);
}

TEST_CASE("Direct3D 12 draws the fullscreen pipeline across resizes, frame slots and restarts", "[rhi][windows][gpu]")
{
    Guard guard;
    constexpr uint32 WIDE_WIDTH = 96;
    constexpr uint32 WIDE_HEIGHT = 64;
    constexpr usize FRAMES = 40;
    constexpr usize SIZED_FRAMES = 4;
    constexpr usize CHECKED_FRAMES = 8;
    for (usize session = 0; session < 2; ++session)
    {
        rhi::Shutdown();
        REQUIRE(rhi::Start({},
                           { .Width = WIDE_WIDTH, .Height = WIDE_HEIGHT },
                           rhi::BackendSelection::Auto,
                           {
                               .MinFrameDimension2D = WIDE_WIDTH,
                               .MinUniformBufferSize = DIAGNOSTIC_UNIFORM_BYTES,
                           }) == rhi::StartStatus::Ready);
        rhi::ShaderHandle vertex;
        rhi::ShaderHandle fragment;
        rhi::UniformHandle uniform;
        rhi::PipelineHandle pipeline;
        REQUIRE(rhi::CreateShader(ludus::shaders::diagnostic::Vertex(), vertex) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::CreateShader(ludus::shaders::diagnostic::Fragment(), fragment) == rhi::ResourceStatus::Ready);
        // The second session uses the largest uniform the facade admits.
        const usize uniformBytes = session == 0 ? DIAGNOSTIC_UNIFORM_BYTES : UNIFORM_CAPACITY;
        REQUIRE(rhi::CreateUniform(uniformBytes, uniform) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::CreatePipeline({vertex, fragment, uniform}, pipeline) == rhi::ResourceStatus::Ready);
        // The facade keeps shaders alive while a pipeline references them.
        REQUIRE(rhi::Destroy(vertex) == rhi::ResourceStatus::InUse);
        static uint8 payload[UNIFORM_CAPACITY]{};
        static uint8 pixels[static_cast<usize>(WIDE_WIDTH) * WIDE_HEIGHT * BYTES_PER_PIXEL]{};
        for (usize frame = 0; frame < FRAMES; ++frame)
        {
            const uint32 width = frame < SIZED_FRAMES ? WIDE_WIDTH : WIDE_HEIGHT;
            const uint32 height = frame < SIZED_FRAMES ? WIDE_HEIGHT : WIDE_WIDTH;
            const float32 elapsed = frame % 2 == 0 ? 0.0F : 2.0F;
            WriteDiagnosticUniform(width, height, elapsed, payload);
            REQUIRE(rhi::UpdateUniform(uniform, {payload, uniformBytes}) == rhi::ResourceStatus::Ready);
            REQUIRE(rhi::SetFrameTarget({ .Width = width, .Height = height }) == rhi::FrameStatus::Ready);
            REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
            REQUIRE(rhi::DrawFullscreen(pipeline) == rhi::ResourceStatus::Ready);
            CHECK(rhi::DrawFullscreen(pipeline) != rhi::ResourceStatus::Ready);
            REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
            // A burst without readback alternates both frame slots and their uniform
            // buffers; the final frame of the burst is checked afterwards.
            if (frame >= CHECKED_FRAMES && frame != FRAMES - 1)
            {
                continue;
            }
            const std::span<uint8> image{pixels, static_cast<usize>(width) * height * BYTES_PER_PIXEL};
            REQUIRE(rhi::backend::ReadHeadlessPixels(image) == true);
            INFO("session=" << session << " frame=" << frame);
            REQUIRE(CountDiagnosticErrors(image, width, height, elapsed) == 0);
        }
        REQUIRE(rhi::Destroy(pipeline) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::Destroy(uniform) == rhi::ResourceStatus::Ready);
        // Abort a recorded draw before submission; the restart must not retain it.
        rhi::UniformHandle replacement;
        rhi::PipelineHandle restarted;
        REQUIRE(rhi::CreateUniform(DIAGNOSTIC_UNIFORM_BYTES, replacement) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::CreatePipeline({vertex, fragment, replacement}, restarted) == rhi::ResourceStatus::Ready);
        const uint8 zeros[DIAGNOSTIC_UNIFORM_BYTES]{};
        REQUIRE(rhi::UpdateUniform(replacement, zeros) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::SetFrameTarget({ .Width = WIDE_HEIGHT, .Height = WIDE_WIDTH }) == rhi::FrameStatus::Ready);
        REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
        REQUIRE(rhi::DrawFullscreen(restarted) == rhi::ResourceStatus::Ready);
        rhi::Shutdown();
        CHECK(rhi::GetStatus(restarted) == rhi::ResourceStatus::InvalidHandle);
        CHECK(rhi::GetStatus(replacement) == rhi::ResourceStatus::InvalidHandle);
    }
}

TEST_CASE("Direct3D 12 draws the fullscreen pipeline into a Win32 swap chain", "[rhi][windows][gpu]")
{
    Guard guard;
    UniquePtr<ludus::platform::Window> window = OpenWindow(160, 120);
    const ludus::platform::NativeWindowInfo info = window->GetNativeWindowInfo();
    REQUIRE(rhi::Start({}, info) == rhi::StartStatus::Ready);
    rhi::ShaderHandle vertex;
    rhi::ShaderHandle fragment;
    rhi::UniformHandle uniform;
    rhi::PipelineHandle pipeline;
    REQUIRE(rhi::CreateShader(ludus::shaders::diagnostic::Vertex(), vertex) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreateShader(ludus::shaders::diagnostic::Fragment(), fragment) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreateUniform(DIAGNOSTIC_UNIFORM_BYTES, uniform) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreatePipeline({vertex, fragment, uniform}, pipeline) == rhi::ResourceStatus::Ready);
    uint8 payload[DIAGNOSTIC_UNIFORM_BYTES]{};
    for (uint32 frame = 0; frame < 4; ++frame)
    {
        REQUIRE(window->HandleEvent({}) == true);
        const ludus::platform::NativeWindowInfo current = window->GetNativeWindowInfo();
        WriteDiagnosticUniform(current.Width, current.Height, static_cast<float32>(frame), payload);
        REQUIRE(rhi::UpdateUniform(uniform, payload) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::SetFrameTarget({ .Width = current.Width, .Height = current.Height }) == rhi::FrameStatus::Ready);
        REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
        REQUIRE(rhi::DrawFullscreen(pipeline) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
    }
    // In-flight presented frames still reference the pipeline; destruction drains them.
    REQUIRE(rhi::Destroy(pipeline) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::Destroy(uniform) == rhi::ResourceStatus::Ready);
    // Release the swap chain before its window.
    rhi::Shutdown();
}
#endif
