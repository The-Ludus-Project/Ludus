// Native shader-worker reload journey: tools/shader-worker/test_worker.py publishes real compiler
// output (ludus_shader_reload_gpu), and this case replaces the running renderer's generation.

#include "render_graph_gpu_fixture.h"
#include "renderer_composite.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/runtime/shader_development/reload.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <thread>

using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
namespace format = ludus::content::render_graph_format;
TEST_CASE("Native compiler publications reload a running renderer without losing its drawable generation",
          "[shader][reload][gpu][.native-reload]")
{
    const char* rootOrNull = std::getenv("LUDUS_NATIVE_RELOAD_ROOT");
    if (rootOrNull == nullptr)
    {
        SKIP("Run through ludus_shader_reload_gpu with real native-worker publications");
    }
    GraphGpuFixture fixture;
    filesystem::Directory directory;
    filesystem::WriteDirectory publication;
    REQUIRE(directory.Open(rootOrNull).Succeeded() == true);
    REQUIRE(publication.Open(rootOrNull).Succeeded() == true);
    const auto readBytes = [&](const char* name) {
        filesystem::File file;
        REQUIRE(directory.OpenRead(name, file).Succeeded() == true);
        Array<uint8> bytes;
        REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
        REQUIRE(file.ReadAt(0, bytes.AsSpan()).Outcome.Succeeded() == true);
        return bytes;
    };
    const Array<uint8> initial = readBytes("initial.lurgrt");
    const Array<uint8> changed = readBytes("changed.lurgrt");
    renderer::RenderGraphPackage identity;
    REQUIRE(identity.Decode({initial.GetData(), initial.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    renderer::RenderGraphActivation activation;
    REQUIRE(activation.Initialize(fixture.Device,
                                  identity.GetInterfaceIdentity(),
                                  shaders::renderer_composite::Vertex(),
                                  shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    REQUIRE(publication.Publish("main.lurgrt", initial.AsSpan()).Published == true);
    ludus::runtime::shader_development::ReloadSession reload;
    REQUIRE(reload.Initialize({rootOrNull, std::strlen(rootOrNull)},
                              {"main.lurgrt", sizeof("main.lurgrt") - 1},
                              activation) == rhi::RasterStatus::Ready);
    using ReloadState = ludus::runtime::shader_development::ReloadState;
    constexpr usize MAX_ATTEMPTS = 5000;
    uint64 maximumServiceNanoseconds = 0;
    const auto awaitDelivery = [&](ReloadState expected) {
        for (usize attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
        {
            const uint64 start = time::NowTicks();
            const ludus::runtime::shader_development::ReloadReport report = reload.Service(start);
            const uint64 elapsed = time::NowTicks() - start;
            maximumServiceNanoseconds = elapsed > maximumServiceNanoseconds ? elapsed : maximumServiceNanoseconds;
            if (report.State == expected)
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        FAIL("Timed out waiting for runtime publication delivery");
    };
    const auto activate = [&](uint64 generation) {
        renderer::RenderGraphReport sceneReport;
        for (usize attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
        {
            static_cast<void>(activation.Poll());
            const renderer::RenderGraphActivationReport report = activation.GetReport();
            REQUIRE(report.Candidate != renderer::RenderGraphCandidateState::Failed);
            if (report.Candidate == renderer::RenderGraphCandidateState::NeedsUpdate ||
                (report.ActiveGeneration == generation &&
                 activation.GetActiveOrNull()->GetDrawableSceneRevision() == 0))
            {
                const rhi::RasterStatus updated =
                    activation.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                      {fixture.Items.GetData(), fixture.Items.GetSize()},
                                      {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                      generation,
                                      renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                      sceneReport);
                REQUIRE((updated == rhi::RasterStatus::Ready || updated == rhi::RasterStatus::Pending ||
                         updated == rhi::RasterStatus::NotReady));
            }
            if (report.ActiveGeneration == generation && activation.GetActiveOrNull()->GetDrawableSceneRevision() != 0)
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        FAIL("Timed out waiting for ready pipelines and scene activation");
    };
    const auto render = [&]() {
        REQUIRE(activation.GetActiveOrNull() != nullptr);
        rhi::SubmissionToken completion;
        REQUIRE(activation.GetActiveOrNull()->Render(fixture.Surface, WIDTH, HEIGHT, completion) ==
                rhi::RasterStatus::Ready);
        REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    };
    awaitDelivery(ReloadState::Staged);
    activate(1);
    render();
    const usize centerPixel = (usize{32} * WIDTH + 48) * 4;
    REQUIRE(fixture.Pixels[centerPixel] == 255);
    REQUIRE(fixture.Pixels[centerPixel + 1] == 0);
    const StaticArray<uint8, PIXEL_BYTES> firstPixels = fixture.Pixels;
    // A malformed publication fails before Renderer sees a candidate.
    REQUIRE(publication.Publish("main.lurgrt", {initial.GetData(), sizeof(uint32)}).Published == true);
    // Detect the producer's atomic replacement through the watcher itself.
    awaitDelivery(ReloadState::Failed);
    CHECK(activation.GetReport().ActiveGeneration == 1);
    render();
    CHECK(std::memcmp(firstPixels.GetData(), fixture.Pixels.GetData(), PIXEL_BYTES) == 0);
    // Supersede a read before collecting it; the stale payload must never stage.
    REQUIRE(publication.Publish("main.lurgrt", initial.AsSpan()).Published == true);
    reload.RequestReload();
    static_cast<void>(reload.Service(time::NowTicks()));
    CHECK(activation.GetReport().ActiveGeneration == 1);
    render();
    CHECK(std::memcmp(firstPixels.GetData(), fixture.Pixels.GetData(), PIXEL_BYTES) == 0);
    REQUIRE(publication.Publish("main.lurgrt", changed.AsSpan()).Published == true);
    reload.RequestReload();
    const uint64 pipelineStart = time::NowTicks();
    // Driver work is deliberately held on its worker. Neither delivery nor
    // renderer polling/drawing may wait for that work on the owner thread.
    struct PreparationPause final
    {
        PreparationPause()
        {
            rhi::backend::SetNativePipelinePreparationPaused(true);
        }
        ~PreparationPause()
        {
            rhi::backend::SetNativePipelinePreparationPaused(false);
        }
    };
    {
        PreparationPause pause;
        const uint32 started = rhi::backend::GetNativePreparationStarted();
        awaitDelivery(ReloadState::Staged);
        CHECK(activation.GetReport().Candidate == renderer::RenderGraphCandidateState::Preparing);
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (rhi::backend::GetNativePreparationStarted() == started && std::chrono::steady_clock::now() < deadline)
        {
            static_cast<void>(activation.Poll());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(rhi::backend::GetNativePreparationStarted() > started);
        for (usize frame = 0; frame < 8; ++frame)
        {
            CHECK(activation.Poll() == rhi::RasterStatus::Ready);
            CHECK(activation.GetReport().ActiveGeneration == 1);
            render();
            CHECK(std::memcmp(firstPixels.GetData(), fixture.Pixels.GetData(), PIXEL_BYTES) == 0);
        }
        // A newer candidate may detach this blocked pipeline immediately;
        // late completion is retired, never published as the active graph.
        reload.RequestReload();
        awaitDelivery(ReloadState::Staged);
        CHECK(activation.GetReport().CandidateGeneration == 3);
        CHECK(activation.GetReport().ActiveGeneration == 1);
    }
    activate(3);
    const uint64 replacementNanoseconds = time::NowTicks() - pipelineStart;
    render();
    REQUIRE(fixture.Pixels[centerPixel] == 0);
    REQUIRE(fixture.Pixels[centerPixel + 1] == 255);
#if defined(LUDUS_TEST_METAL)
    // Preserve the active green graph when the native compiler rejects an
    // otherwise structurally admitted runtime artifact after asynchronous enqueue.
    renderer::RenderGraphPackage changedDescription;
    REQUIRE(changedDescription.Decode({changed.GetData(), changed.GetSize()}) ==
            renderer::RenderGraphPackageStatus::Ready);
    const renderer::RenderGraphPrograms programs =
        changedDescription.GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    const std::string_view payload{reinterpret_cast<const char*>(changed.GetData()), changed.GetSize()};
    const usize fragmentOffset = payload.find(programs.ColorFragment.Artifact.Msl);
    REQUIRE(fragmentOffset != std::string_view::npos);
    Array<uint8> rejected = changed;
    rejected[fragmentOffset] = '?';
    REQUIRE(publication.Publish("main.lurgrt", rejected.AsSpan()).Published == true);
    reload.RequestReload();
    awaitDelivery(ReloadState::Staged);
    const std::chrono::steady_clock::time_point failureDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (activation.GetReport().Candidate != renderer::RenderGraphCandidateState::Failed &&
           std::chrono::steady_clock::now() < failureDeadline)
    {
        CHECK(activation.Poll() == rhi::RasterStatus::Ready);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(activation.GetReport().Candidate == renderer::RenderGraphCandidateState::Failed);
    CHECK(activation.GetReport().ActiveGeneration == 3);
    render();
    REQUIRE(fixture.Pixels[centerPixel + 1] == 255);
#endif
    // CPU ABI changes require a restart and preserve the green generation.
    Array<uint8> incompatible = changed;
    incompatible[format::HEADER_FIELD_COUNT * sizeof(uint32)] ^= 1;
    REQUIRE(publication.Publish("main.lurgrt", incompatible.AsSpan()).Published == true);
    reload.RequestReload();
    awaitDelivery(ReloadState::RestartRequired);
    CHECK(activation.GetReport().ActiveGeneration == 3);
    render();
    REQUIRE(fixture.Pixels[centerPixel + 1] == 255);
    const char* metricsOrNull = std::getenv("LUDUS_NATIVE_RELOAD_METRICS");
    if (metricsOrNull != nullptr)
    {
        std::ofstream metrics(metricsOrNull);
        metrics << "{\"replacement_read_and_pipeline_ns\":" << replacementNanoseconds
                << ",\"maximum_delivery_service_ns\":" << maximumServiceNanoseconds << "}\n";
    }
    activation.Reset();
}
