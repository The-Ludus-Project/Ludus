#include "renderer_composite.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/render_graph_capture.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
using RasterStatus = rhi::RasterStatus;
constexpr uint32 EXTENT = 4;
constexpr usize CAMERA_COUNT = 1;
constexpr uint32 ITERATIONS = 256;
constexpr uint64 TIMEOUT = 60 * time::NANOSECONDS_PER_SECOND;
constexpr math::Vector3 ENVIRONMENT{0.25F, 0.5F, 1};
struct Session final
{
    LUDUS_INLINE Session() noexcept
    {
        rhi::Shutdown();
    }
    LUDUS_INLINE ~Session() noexcept
    {
        rhi::Shutdown();
    }
};

void LoadPackage(bool isJumpRestoreOnly, renderer::RenderGraphPackage& outPackage)
{
    filesystem::Directory directory;
    REQUIRE(directory.Open(isJumpRestoreOnly == true ? LUDUS_JR_TEMPLATE_DIRECTORY : LUDUS_MH_TEMPLATE_DIRECTORY)
                .Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead(isJumpRestoreOnly == true ? "jump_restore_graph.lurgrt" : "mh_graph.lurgrt", file)
                .Succeeded() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
    const filesystem::ReadResult read = file.ReadAt(0, bytes.AsSpan());
    REQUIRE(read.Outcome.Succeeded() == true);
    REQUIRE(read.BytesRead == bytes.GetSize());
    REQUIRE(outPackage.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
}
} // namespace

TEST_CASE("MH graph captures resolve the latest weighted estimate after burn-in", "[renderer][gpu][metropolis-graph]")
{
    bool isJumpRestoreOnly = false;
    SECTION("MH and Jump Restore RB alongside") {}
    SECTION("Jump Restore estimator comparison")
    {
        isJumpRestoreOnly = true;
    }
    Session session;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    description.Required.PortableRaster = true;
    const rhi::DeviceStatus created =
        rhi::CreateDevice({}, { .Width = EXTENT, .Height = EXTENT }, description, device, surface);
    if (created != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
        SKIP("No compute adapter");
    REQUIRE(created == rhi::DeviceStatus::Ready);
    renderer::RenderGraphPackage package;
    LoadPackage(isJumpRestoreOnly, package);
    const renderer::RenderGraphPlan plan = package.GetPlan();
    const renderer::RenderGraphPrograms programs = package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                       ludus::shaders::renderer_composite::Fragment());
    REQUIRE(renderer::RequiresHardwareRayTracing(plan) == false);
    REQUIRE(plan.Passes.GetSize() == (isJumpRestoreOnly == true ? 3 : 4));
    renderer::SceneShaderInputs inputs;
    inputs.Environment = ENVIRONMENT;
    StaticArray<renderer::ViewDescription, CAMERA_COUNT> cameras;
    cameras[0].OrthographicCamera = true;
    for (uint32 pass = 0; pass < plan.Passes.GetSize(); ++pass)
    {
        renderer::RenderGraphCapture capture;
        renderer::RenderGraphCaptureSettings settings;
        settings.Output = plan.Passes[pass].Color;
        settings.TargetSamples = ITERATIONS;
        const RasterStatus started = capture.Start(device, surface, plan, programs, EXTENT, EXTENT, settings);
        REQUIRE((started == RasterStatus::Ready || started == RasterStatus::Pending));
        const uint64 beginning = time::NowTicks();
        while (capture.GetPhase() == renderer::RenderGraphCapturePhase::Initializing &&
               time::NowTicks() - beginning < TIMEOUT)
            capture.Service();
        REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::AwaitingScene);
        const RasterStatus submitted = capture.SubmitScene({}, {}, {cameras.GetData(), cameras.GetSize()}, 1, &inputs);
        REQUIRE((submitted == RasterStatus::Ready || submitted == RasterStatus::Pending));
        while (capture.GetPhase() != renderer::RenderGraphCapturePhase::Complete &&
               capture.GetPhase() != renderer::RenderGraphCapturePhase::Failed &&
               time::NowTicks() - beginning < TIMEOUT)
            capture.Service();
        CAPTURE(pass, capture.GetStatus(), capture.GetProblem());
        REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Complete);
        REQUIRE(capture.GetPixels().GetSize() == (EXTENT / 2) * (EXTENT / 2));
        const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
        for (usize index = 0; index < pixels.GetSize(); ++index)
        {
            const renderer::RenderGraphCapturePixel& pixel = pixels[index];
            CHECK(pixel.Samples == ITERATIONS);
            // The first 128 outputs are black burn-in. Averaging successive ratios
            // would return half the environment instead of this current estimate.
            CHECK(math::Abs(pixel.Mean.X - ENVIRONMENT.X) < 0.0001F);
            CHECK(math::Abs(pixel.Mean.Y - ENVIRONMENT.Y) < 0.0001F);
            CHECK(math::Abs(pixel.Mean.Z - ENVIRONMENT.Z) < 0.0001F);
        }
    }
}
