#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

#include "diagnostic.h"
#include "diagnostic_slang.h"
#include "internal/readback.h"

#include <span>

#include <catch2/catch_test_macros.hpp>

// LUSL L5d wave 1 (issue 1735): tests/sdk_consumer/shaders/diagnostic.lusl replaces the Slang
// fullscreen diagnostic for the RHI and SDK consumers. The Slang original stays as the installed
// helper's Slang fixture and as this differential oracle (config/lusl_migration.json). Both are
// drawn on the same device in the same run and must give identical bytes on each native backend (Vulkan, Metal,
// Direct3D 12).
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace
{
/// Odd, non-square extent, so a mirrored axis or a swapped extent changes the image.
constexpr uint32 PARITY_WIDTH = 97;
constexpr uint32 PARITY_HEIGHT = 61;
constexpr usize PIXEL_BYTES = 4;
constexpr usize PARITY_PIXEL_BYTES = static_cast<usize>(PARITY_WIDTH) * PARITY_HEIGHT * PIXEL_BYTES;
/// The diagnostic uniform: resolution, elapsed time, direction and tint (48 bytes).
constexpr usize DIAGNOSTIC_UNIFORM_FLOATS = 12;
constexpr usize DIAGNOSTIC_UNIFORM_BYTES = DIAGNOSTIC_UNIFORM_FLOATS * sizeof(float32);
/// Bytes per row; the top and bottom rows show the vertical gradient's direction.
constexpr usize ROW_BYTES = static_cast<usize>(PARITY_WIDTH) * PIXEL_BYTES;
constexpr usize GREEN_CHANNEL = 1;

using ParityPixels = StaticArray<uint8, PARITY_PIXEL_BYTES>;

/// Destroy the owner a test created, so a failed REQUIRE does not leak it.
struct ParityCleanup final
{
    rhi::DeviceHandle Device{};
    ParityCleanup() noexcept
    {
        rhi::Shutdown();
    }
    ParityCleanup(const ParityCleanup&) = delete;
    ParityCleanup& operator=(const ParityCleanup&) = delete;
    ~ParityCleanup()
    {
        static_cast<void>(rhi::DestroyDevice(Device));
        rhi::Shutdown();
    }
};

/// Draw one fullscreen program with `values` on the owner's headless surface and read it back.
void DrawProgram(rhi::DeviceHandle device,
                 rhi::SurfaceHandle surface,
                 const rhi::ShaderDescription& vertexDescription,
                 const rhi::ShaderDescription& fragmentDescription,
                 const StaticArray<float32, DIAGNOSTIC_UNIFORM_FLOATS>& values,
                 ParityPixels& outPixels)
{
    REQUIRE(fragmentDescription.UniformSize == DIAGNOSTIC_UNIFORM_BYTES);
    rhi::ShaderHandle vertex;
    rhi::ShaderHandle fragment;
    rhi::UniformHandle uniform;
    rhi::PipelineHandle pipeline;
    REQUIRE(rhi::CreateShader(device, vertexDescription, vertex) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreateShader(device, fragmentDescription, fragment) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreateUniform(device, DIAGNOSTIC_UNIFORM_BYTES, uniform) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::CreatePipeline(device, {vertex, fragment, uniform}, pipeline) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::UpdateUniform(device,
                               uniform,
                               {reinterpret_cast<const uint8*>(values.GetData()), DIAGNOSTIC_UNIFORM_BYTES}) ==
            rhi::ResourceStatus::Ready);
    REQUIRE(rhi::SetFrameTarget(device, surface, { .Width = PARITY_WIDTH, .Height = PARITY_HEIGHT }) ==
            rhi::DeviceStatus::Ready);
    REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::DrawFullscreen(device, pipeline) == rhi::ResourceStatus::Ready);
    REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(std::span<uint8>{outPixels.GetData(), outPixels.GetSize()}));
}

/// Sum of one channel over a row of the image.
usize SumRowChannel(const ParityPixels& pixels, usize row, usize channel) noexcept
{
    usize sum = 0;
    for (usize x = 0; x < PARITY_WIDTH; ++x)
    {
        sum += pixels[row * ROW_BYTES + x * PIXEL_BYTES + channel];
    }
    return sum;
}

/// Index of the first differing byte, or the size when the images are identical.
usize FindFirstDifference(const ParityPixels& left, const ParityPixels& right) noexcept
{
    for (usize i = 0; i < left.GetSize(); ++i)
    {
        if (left[i] != right[i])
        {
            return i;
        }
    }
    return left.GetSize();
}
} // namespace

TEST_CASE("LUSL diagnostic port draws its Slang original's pixels", "[rhi][gpu][lusl]")
{
    ParityCleanup cleanup;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    const rhi::DeviceStatus status = rhi::CreateDevice({},
                                                       {
                                                           .Width = PARITY_WIDTH,
                                                           .Height = PARITY_HEIGHT,
                                                       },
                                                       {},
                                                       cleanup.Device,
                                                       surface,
                                                       &failure);
#if defined(LUDUS_TEST_METAL)
    if (status != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter on this host");
    }
#endif
    REQUIRE(status == rhi::DeviceStatus::Ready);
    const rhi::DeviceHandle device = cleanup.Device;
    // ludus-style: magic-number an arbitrary elapsed time, direction and tint the image depends on
    const StaticArray<float32, DIAGNOSTIC_UNIFORM_FLOATS> values{static_cast<float32>(PARITY_WIDTH),
                                                                 static_cast<float32>(PARITY_HEIGHT),
                                                                 1.75F,
                                                                 0,
                                                                 0.3F,
                                                                 0.1F,
                                                                 0.2F,
                                                                 0,
                                                                 0.9F,
                                                                 0.7F,
                                                                 0.5F,
                                                                 1};
    ParityPixels lusl{};
    ParityPixels slang{};
    DrawProgram(device,
                surface,
                ludus::shaders::diagnostic::Vertex(),
                ludus::shaders::diagnostic::Fragment(),
                values,
                lusl);
    DrawProgram(device,
                surface,
                ludus::shaders::diagnostic_slang::Vertex(),
                ludus::shaders::diagnostic_slang::Fragment(),
                values,
                slang);
    CHECK(FindFirstDifference(lusl, slang) == lusl.GetSize());
    // Top-left pixel origin: green follows uv.y, so the bottom row is greener than the top row.
    CHECK(SumRowChannel(lusl, 0, GREEN_CHANNEL) < SumRowChannel(lusl, PARITY_HEIGHT - 1, GREEN_CHANNEL));
}
