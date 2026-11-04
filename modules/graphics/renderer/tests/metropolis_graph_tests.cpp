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
constexpr uint32 ITERATIONS = 64;
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
    namespace format = ludus::content::scene_format;
    bool isJumpRestoreOnly = false;
    bool hasSurface = false;
    bool hasIncompatibleSampler = false;
    SECTION("MH and Jump Restore RB alongside") {}
    SECTION("Jump Restore estimator comparison")
    {
        isJumpRestoreOnly = true;
    }
    SECTION("Fixed-dimension reflection accepts a Mix")
    {
        hasSurface = true;
    }
    SECTION("An internal walk fails the material requirement")
    {
        hasSurface = true;
        hasIncompatibleSampler = true;
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
    constexpr usize VERTEX_COUNT = 4;
    constexpr usize INDEX_COUNT = 6;
    const StaticArray<math::Vector3, VERTEX_COUNT> positionsWorld{
        {{-2, -2, 0.5F}, {2, -2, 0.5F}, {-2, 2, 0.5F}, {2, 2, 0.5F}}};
    const StaticArray<uint32, INDEX_COUNT> indices{{0, 2, 1, 1, 2, 3}};
    const uint32 materialIndex = 0;
    renderer::SceneShaderMaterial material;
    material.BaseColor = {0.5F, 0.5F, 0.5F};
    material.Metallic = 0.25F;
    material.Scattering.Model = hasIncompatibleSampler == true ? format::MaterialModel::CoatedDiffuse
                                                               : format::MaterialModel::DiffuseConductorMix;
    renderer::RenderGraphMesh mesh;
    mesh.Geometry = {positionsWorld.GetData(), positionsWorld.GetSize(), indices.GetData(), indices.GetSize()};
    renderer::RenderGraphItem item;
    item.SourceId = 1;
    item.Material.Color = {1, 1, 1, 1};
    item.MaterialOverrides = {&materialIndex, 1};
    if (hasSurface == true)
        inputs.Materials = {&material, 1};
    for (uint32 pass = 0; pass < plan.Passes.GetSize(); ++pass)
    {
        const uint32 warmup = programs.CaptureWarmupSamples[plan.Passes[pass].RayProgram];
        REQUIRE(warmup > ITERATIONS);
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
        const usize surfaceCount = hasSurface == true ? 1 : 0;
        const RasterStatus submitted = capture.SubmitScene({&mesh, surfaceCount},
                                                           {&item, surfaceCount},
                                                           {cameras.GetData(), cameras.GetSize()},
                                                           1,
                                                           &inputs);
        CAPTURE(submitted, capture.GetProblem());
        REQUIRE((submitted == RasterStatus::Ready || submitted == RasterStatus::Pending));
        while (capture.GetPhase() != renderer::RenderGraphCapturePhase::Complete &&
               capture.GetPhase() != renderer::RenderGraphCapturePhase::Failed &&
               time::NowTicks() - beginning < TIMEOUT)
            capture.Service();
        CAPTURE(pass, capture.GetStatus(), capture.GetProblem());
        if (hasIncompatibleSampler == true)
        {
            REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Failed);
            renderer::RenderGraphRayFailures failures;
            RasterStatus status = capture.PollRayFailures(pass, failures);
            while (status == RasterStatus::Pending && time::NowTicks() - beginning < TIMEOUT)
            {
                status = capture.PollRayFailures(pass, failures);
            }
            REQUIRE(status == RasterStatus::Ready);
            CHECK(failures.MainReason ==
                  static_cast<uint32>(renderer::RenderGraphFailureReason::FixedDimensionMaterial));
            return;
        }
        REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Complete);
        REQUIRE(capture.GetPixels().GetSize() == (EXTENT / 2) * (EXTENT / 2));
        const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
        for (usize index = 0; index < pixels.GetSize(); ++index)
        {
            const renderer::RenderGraphCapturePixel& pixel = pixels[index];
            CHECK(pixel.Samples == ITERATIONS + warmup);
            // A short requested capture must run past the declared warm-up. Its
            // latest ratio must not average black warm-up outputs into the environment.
            if (hasSurface == true)
            {
                CHECK(pixel.Mean.X > 0);
                CHECK(pixel.Mean.X < ENVIRONMENT.X);
                continue;
            }
            CHECK(math::Abs(pixel.Mean.X - ENVIRONMENT.X) < 0.0001F);
            CHECK(math::Abs(pixel.Mean.Y - ENVIRONMENT.Y) < 0.0001F);
            CHECK(math::Abs(pixel.Mean.Z - ENVIRONMENT.Z) < 0.0001F);
        }
    }
}
