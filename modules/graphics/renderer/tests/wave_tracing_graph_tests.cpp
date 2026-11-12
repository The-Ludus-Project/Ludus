#include "renderer_composite.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/threading/thread.hpp>
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
namespace threading = ludus::foundation::threading;
using rhi::RasterStatus;
constexpr uint32 EXTENT = 4;
constexpr usize QUAD_VERTICES = 4;
constexpr usize QUAD_INDICES = 6;
constexpr usize TABLE_COUNT = 2;
constexpr usize CAMERA_COUNT = 1;
constexpr uint64 COLD_CAPTURE_TIMEOUT_NANOSECONDS =
    LUDUS_RENDERER_TEST_COLD_GPU_TIMEOUT_SECONDS * time::NANOSECONDS_PER_SECOND;
constexpr time::Duration COMPLETION_POLL_INTERVAL = time::Milliseconds(1);
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
void LoadPackage(renderer::RenderGraphPackage& outPackage)
{
    filesystem::Directory directory;
    REQUIRE(directory.Open(LUDUS_WAVE_TEMPLATE_DIRECTORY).Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead("wave_graph.lurgrt", file).Succeeded() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
    REQUIRE(file.ReadAt(0, MakeSpan(bytes)).BytesRead == bytes.GetSize());
    REQUIRE(outPackage.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
}
} // namespace

TEST_CASE("Wave template transports through the current World's screen and diffracts into shadow",
          "[renderer][gpu][wave-tracing][graph]")
{
    uint32 screen = 0;
    SECTION("Unobstructed beam") {}
    SECTION("Opaque screen")
    {
        screen = 1;
    }
    SECTION("Half screen casts a geometric shadow with nonzero diffracted light")
    {
        screen = 2;
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
    {
        SKIP("No compute adapter");
    }
    REQUIRE(created == rhi::DeviceStatus::Ready);
    renderer::RenderGraphPackage package;
    LoadPackage(package);
    const renderer::RenderGraphPlan plan = package.GetPlan();
    const renderer::RenderGraphPrograms programs = package.GetPrograms(ludus::shaders::renderer_composite::Vertex(),
                                                                       ludus::shaders::renderer_composite::Fragment());
    REQUIRE(renderer::RequiresHardwareRayTracing(plan) == false);
    REQUIRE(plan.Passes.GetSize() == 4);
    const StaticArray<math::Vector3, QUAD_VERTICES> receiverWorld{{{-1, -1, 1}, {1, -1, 1}, {-1, 1, 1}, {1, 1, 1}}};
    const float32 screenRightWorld = screen == 1 ? 1.0F : 0.0F;
    const StaticArray<math::Vector3, QUAD_VERTICES> screenWorld{
        {{-1, -1, 0}, {screenRightWorld, -1, 0}, {-1, 1, 0}, {screenRightWorld, 1, 0}}};
    const StaticArray<uint32, QUAD_INDICES> indices{{0, 1, 2, 1, 3, 2}};
    StaticArray<renderer::RenderGraphMesh, TABLE_COUNT> meshes;
    meshes[0].Geometry = {receiverWorld.GetData(), receiverWorld.GetSize(), indices.GetData(), indices.GetSize()};
    meshes[1].Geometry = {screenWorld.GetData(), screenWorld.GetSize(), indices.GetData(), indices.GetSize()};
    StaticArray<renderer::RenderGraphItem, TABLE_COUNT> items;
    items[0].SourceId = 1;
    items[1].SourceId = 2;
    items[1].MeshIndex = 1;
    items[0].Material.Color = {1, 1, 1, 1};
    items[1].Material.Color = {1, 1, 1, 1};
    StaticArray<renderer::ViewDescription, CAMERA_COUNT> cameras;
    cameras[0].OrthographicCamera = true;
    cameras[0].WorldToClip.Columns[0].X = 10;
    cameras[0].WorldToClip.Columns[1].Y = 10;
    cameras[0].WorldToClip.Columns[2].Z = -0.5F;
    cameras[0].WorldToClip.Columns[3].Z = 1;
    renderer::SceneShaderInputs inputs;
    // Primary pass index zero only writes shared hits. The following passes export wave, geometric and aperture.
    for (uint32 pass = 1; pass < plan.Passes.GetSize(); ++pass)
    {
        renderer::RenderGraphCapture capture;
        renderer::RenderGraphCaptureSettings settings;
        settings.Output = plan.Passes[pass].Color;
        settings.TargetSamples = 1;
        const RasterStatus started = capture.Start(device, surface, plan, programs, EXTENT, EXTENT, settings);
        REQUIRE((started == RasterStatus::Ready || started == RasterStatus::Pending));
        const uint64 beginning = time::NowTicks();
        while (capture.GetPhase() == renderer::RenderGraphCapturePhase::Initializing &&
               time::NowTicks() - beginning < COLD_CAPTURE_TIMEOUT_NANOSECONDS)
        {
            capture.Service();
            threading::SleepFor(COMPLETION_POLL_INTERVAL);
        }
        REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::AwaitingScene);
        const usize itemCount = screen == 0 ? 1 : TABLE_COUNT;
        const uint64 captureBeginning = time::NowTicks();
        const RasterStatus submitted = capture.SubmitScene({meshes.GetData(), itemCount},
                                                           {items.GetData(), itemCount},
                                                           {cameras.GetData(), CAMERA_COUNT},
                                                           1,
                                                           &inputs);
        REQUIRE((submitted == RasterStatus::Ready || submitted == RasterStatus::Pending));
        while (capture.GetPhase() != renderer::RenderGraphCapturePhase::Complete &&
               capture.GetPhase() != renderer::RenderGraphCapturePhase::Failed &&
               time::NowTicks() - captureBeginning < COLD_CAPTURE_TIMEOUT_NANOSECONDS)
        {
            capture.Service();
            threading::SleepFor(COMPLETION_POLL_INTERVAL);
        }
        const uint64 elapsedNanoseconds = time::NowTicks() - captureBeginning;
        CAPTURE(pass,
                screen,
                capture.GetPhase(),
                capture.GetStatus(),
                capture.GetProblem(),
                elapsedNanoseconds,
                COLD_CAPTURE_TIMEOUT_NANOSECONDS);
        REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Complete);
        const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
        REQUIRE(pixels.GetSize() == usize{EXTENT} * EXTENT);
        for (usize index = 0; index < pixels.GetSize(); ++index)
        {
            const float32 value = pixels[index].Mean.X;
            REQUIRE(math::IsFinite(value) == true);
            if (screen == 1)
            {
                CHECK(value == 0);
            }
            else if (screen == 0 || pass == 1)
            {
                CHECK(value > 0);
            }
            else if (index % EXTENT < EXTENT / 2)
            {
                CHECK(value == 0);
            }
            else
            {
                CHECK(value > 0);
            }
        }
    }
}
