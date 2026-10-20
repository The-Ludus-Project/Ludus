#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>

#include "diagnostic.h"
#include "internal/readback.h"

#include <algorithm>
#include <span>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
TEST_CASE("Explicit device path preserves facade fullscreen pixels and dependency ownership", "[rhi][gpu][device]")
{
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::Shutdown();
    uint8 reference[96 * 64 * 4]{};
    uint8 owned[96 * 64 * 4]{};
    for (usize mode = 0; mode < 2; ++mode)
    {
        const bool explicitOwner = mode != 0;
        rhi::DeviceHandle device;
        rhi::SurfaceHandle surface;
        if (explicitOwner)
        {
            REQUIRE(rhi::CreateDevice({}, { .Width = 96, .Height = 64 }, {}, device, surface) ==
                    rhi::DeviceStatus::Ready);
        }
        else
        {
            const auto started = rhi::Start({}, { .Width = 96, .Height = 64 });
#if defined(LUDUS_TEST_METAL)
            if (started != rhi::StartStatus::Ready)
            {
                REQUIRE(rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable);
                SKIP("No Metal adapter on this host");
            }
#endif
            REQUIRE(started == rhi::StartStatus::Ready);
        }
        rhi::ShaderHandle vertex, fragment;
        rhi::UniformHandle uniform;
        rhi::PipelineHandle pipeline;
        const auto vertexDescription = ludus::shaders::diagnostic::Vertex();
        const auto fragmentDescription = ludus::shaders::diagnostic::Fragment();
        REQUIRE((explicitOwner ? rhi::CreateShader(device, vertexDescription, vertex)
                               : rhi::CreateShader(vertexDescription, vertex)) == rhi::ResourceStatus::Ready);
        REQUIRE((explicitOwner ? rhi::CreateShader(device, fragmentDescription, fragment)
                               : rhi::CreateShader(fragmentDescription, fragment)) == rhi::ResourceStatus::Ready);
        REQUIRE((explicitOwner ? rhi::CreateUniform(device, 48, uniform) : rhi::CreateUniform(48, uniform)) ==
                rhi::ResourceStatus::Ready);
        REQUIRE((explicitOwner
                     ? rhi::CreatePipeline(device, {vertex, fragment, uniform}, pipeline)
                     : rhi::CreatePipeline({vertex, fragment, uniform}, pipeline)) == rhi::ResourceStatus::Ready);
        const float32 values[12] = {96, 64, 2, 0, 0.2F, 0.4F, 0.6F, 0, 0.6F, 0.4F, 0.2F, 1};
        const std::span<const uint8> bytes{reinterpret_cast<const uint8*>(values), sizeof(values)};
        REQUIRE((explicitOwner ? rhi::UpdateUniform(device, uniform, bytes) : rhi::UpdateUniform(uniform, bytes)) ==
                rhi::ResourceStatus::Ready);
        if (explicitOwner)
        {
            REQUIRE(rhi::SetFrameTarget(device, surface, { .Width = 96, .Height = 64 }) == rhi::DeviceStatus::Ready);
            REQUIRE(rhi::SetFrameTarget(device,
                                        surface,
                                        {
                                            .Width = 1,
                                            .Height = 1,
                                            .ShouldPresentToSurface = false,
                                        }) == rhi::DeviceStatus::Unsupported);
            REQUIRE(rhi::BeginFrame(device, surface) == rhi::DeviceStatus::Ready);
            REQUIRE(rhi::DrawFullscreen(device, pipeline) == rhi::ResourceStatus::Ready);
            REQUIRE(rhi::EndFrame(device) == rhi::DeviceStatus::Ready);
            REQUIRE(rhi::Destroy(device, uniform) == rhi::ResourceStatus::InUse);
        }
        else
        {
            REQUIRE(rhi::SetFrameTarget({ .Width = 96, .Height = 64 }) == rhi::FrameStatus::Ready);
            REQUIRE(rhi::SetFrameTarget({ .Width = 1, .Height = 1, .ShouldPresentToSurface = false }) ==
                    rhi::FrameStatus::Unsupported);
            REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
            REQUIRE(rhi::DrawFullscreen(pipeline) == rhi::ResourceStatus::Ready);
            REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
        }
        REQUIRE(
            rhi::backend::ReadHeadlessPixels(explicitOwner ? std::span<uint8>{owned} : std::span<uint8>{reference}));
        if (explicitOwner)
        {
            REQUIRE(rhi::DestroyDevice(device) == rhi::DeviceStatus::Ready);
            CHECK(rhi::GetStatus(device, pipeline) == rhi::ResourceStatus::InvalidHandle);
        }
        else
        {
            rhi::Shutdown();
        }
    }
    CHECK(std::equal(reference, reference + sizeof(reference), owned));
}
TEST_CASE("RHI rejects requirements beyond the engine limit before exposing resources", "[rhi][gpu]")
{
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::Shutdown();
#if defined(LUDUS_TEST_METAL)
    if (rhi::Start({}, {}) != rhi::StartStatus::Ready)
    {
        REQUIRE(rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable);
        SKIP("No Metal adapter on this host");
    }
    rhi::Shutdown();
#endif
    REQUIRE(rhi::Start({},
                       {
                           .Width = 96,
                           .Height = 64,
                       },
                       rhi::BackendSelection::Auto,
                       {
                           .MinUniformBufferSize = 16385,
                       }) == rhi::StartStatus::Failed);
    CHECK(rhi::GetStartup().Error == rhi::StartupError::RequirementsUnsatisfied);
    CHECK(rhi::GetStartup().UnmetRequirement == rhi::RequirementFailure::UniformBufferSize);
    CHECK(rhi::GetStartup().Capabilities.MaxUniformBufferSize == 0);
    rhi::UniformHandle uniform;
    CHECK(rhi::CreateUniform(16, uniform) == rhi::ResourceStatus::NotReady);
    rhi::Shutdown();
#if defined(LUDUS_TEST_METAL)
    if (rhi::Start({}, {}) != rhi::StartStatus::Ready)
    {
        REQUIRE(rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable);
        SKIP("No Metal adapter on this host");
    }
    rhi::Shutdown();
#endif
    REQUIRE(rhi::Start({}, { .Width = 96, .Height = 64 }) == rhi::StartStatus::Ready);
    CHECK(rhi::GetStartup().Capabilities.MaxUniformBufferSize == 16384);
}
TEST_CASE("Public RHI resources render changing uniforms across resize and restart", "[rhi][gpu]")
{
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
            ludus::foundation::logging::LogSystem::Shutdown();
        }
    } guard;
    ludus::foundation::logging::LogConfig log;
    log.EnableConsole = true;
    log.EnableFile = false;
    log.EnableDebugger = false;
    (void)ludus::foundation::logging::LogSystem::Initialize(log);

    for (usize session = 0; session < 2; ++session)
    {
        rhi::Shutdown();
#if defined(LUDUS_TEST_METAL)
        if (rhi::Start({}, {}) != rhi::StartStatus::Ready)
        {
            REQUIRE(rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable);
            SKIP("No Metal adapter on this host");
        }
        rhi::Shutdown();
#endif
        REQUIRE(rhi::Start({},
                           { .Width = 96, .Height = 64 },
                           rhi::BackendSelection::Auto,
                           { .MinFrameDimension2D = 96, .MinUniformBufferSize = 48 }) == rhi::StartStatus::Ready);
        rhi::ShaderHandle vertex, fragment;
        rhi::UniformHandle uniform;
        rhi::PipelineHandle pipeline;
        auto invalid = ludus::shaders::diagnostic::Vertex();
#if defined(LUDUS_TEST_METAL)
        invalid.MslEntry = "missing";
#else
        invalid.SpirvEntry = "missing";
#endif
        rhi::ShaderHandle failed;
        REQUIRE(rhi::CreateShader(invalid, failed) == rhi::ResourceStatus::Failed);
        REQUIRE(rhi::Destroy(failed) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::CreateShader(ludus::shaders::diagnostic::Vertex(), vertex) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::CreateShader(ludus::shaders::diagnostic::Fragment(), fragment) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::CreateUniform(session == 0 ? 48 : 16384, uniform) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::CreatePipeline({vertex, fragment, uniform}, pipeline) == rhi::ResourceStatus::Ready);
        uint8 pixels[96 * 64 * 4]{};
        for (usize frame = 0; frame < 40; ++frame)
        {
            const uint32 width = frame < 4 ? 96 : 64, height = frame < 4 ? 64 : 96;
            const float32 elapsed = frame % 2 == 0 ? 0.0F : 2.0F;
            // Independently padded scalar payload; neither backend handle enters this test.
            const float32 values[12] = {static_cast<float32>(width),
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
                                        1};
            uint8 payload[16384]{};
            std::copy_n(reinterpret_cast<const uint8*>(values), sizeof(values), payload);
            const usize uploadSize = session == 0 ? sizeof(values) : sizeof(payload);
            REQUIRE(rhi::UpdateUniform(uniform, {payload, uploadSize}) == rhi::ResourceStatus::Ready);
            REQUIRE(rhi::SetFrameTarget({ .Width = width, .Height = height }) == rhi::FrameStatus::Ready);
            REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
            REQUIRE(rhi::DrawFullscreen(pipeline) == rhi::ResourceStatus::Ready);
            REQUIRE(rhi::EndFrameStatus() == rhi::FrameStatus::Ready);
            // Burst frames without CPU/GPU idle readback exercise both in-flight
            // slots; inspect the final submitted image after the burst.
            if (frame >= 8 && frame != 39)
            {
                continue;
            }
            REQUIRE(rhi::backend::ReadHeadlessPixels(pixels));
            usize errors = 0;
            for (usize y = 0; y < height; ++y)
            {
                for (usize x = 0; x < width; ++x)
                {
                    const float64 cx = (static_cast<float64>(x) + 0.5 - width * 0.5) / height;
                    const float64 cy = (static_cast<float64>(y) + 0.5 - height * 0.5) / height;
                    const float64 expected[4] = {
                        0.6 * (static_cast<float64>(x) + 0.5) / width + 0.01 * static_cast<float64>(elapsed),
                        0.4 * (static_cast<float64>(y) + 0.5) / height + 0.02 * static_cast<float64>(elapsed),
                        0.2 * (cx * cx + cy * cy < 0.04 ? 1 : 0) + 0.03 * static_cast<float64>(elapsed),
                        1};
                    for (usize channel = 0; channel < 4; ++channel)
                    {
                        const int32 actual = pixels[(y * width + x) * 4 + channel];
                        const float64 error = static_cast<float64>(actual) - expected[channel] * 255;
                        if (error > 2.5 || error < -2.5)
                        {
                            ++errors;
                        }
                    }
                }
            }
            INFO("session=" << session << " frame=" << frame);
            REQUIRE(errors == 0);
        }
        REQUIRE(rhi::Destroy(pipeline) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::Destroy(uniform) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::GetStatus(uniform) == rhi::ResourceStatus::InvalidHandle);
        rhi::UniformHandle replacement;
        pipeline = {};
        REQUIRE(rhi::CreateUniform(48, replacement) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::CreatePipeline({vertex, fragment, replacement}, pipeline) == rhi::ResourceStatus::Ready);
        const uint8 zeros[48]{};
        REQUIRE(rhi::UpdateUniform(replacement, zeros) == rhi::ResourceStatus::Ready);
        REQUIRE(rhi::SetFrameTarget({}) == rhi::FrameStatus::Ready);
        REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Skipped);
        REQUIRE(rhi::GetFrameInfo().Width == 0);
        REQUIRE(rhi::SetFrameTarget({ .Width = 64, .Height = 96 }) == rhi::FrameStatus::Ready);
        REQUIRE(rhi::BeginFrameStatus() == rhi::FrameStatus::Ready);
        REQUIRE(rhi::DrawFullscreen(pipeline) == rhi::ResourceStatus::Ready);
        // Abort a recorded draw before submission; restart must not retain it.
        rhi::Shutdown();
        REQUIRE(rhi::GetStatus(pipeline) == rhi::ResourceStatus::InvalidHandle);
        REQUIRE(rhi::GetStatus(replacement) == rhi::ResourceStatus::InvalidHandle);
    }
}
