// Windows render-graph DXIL evidence (D6c): rg2 graphs authored for the dxil profile compile
// through the pinned DXC and cook into runtime packages of signed DXIL stages. The portable RG2
// pixel suites draw the same packages on Direct3D 12 (WARP in CI) in this executable, and shader
// file delivery (Ludus::ShaderDevelopment) reloads an edited generation here.

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/runtime/shader_development/reload.hpp>

#include "render_graph_gpu_fixture.h"
#include "renderer_composite.h"
#include "rg2_bound.h"
#include "rg2_surface.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>

using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace
{
// DXBC header: code, 16-byte hash, version and total size; unsigned output has a constant hash.
bool IsSignedDxil(std::span<const uint8> bytes)
{
    constexpr usize HEADER_BYTES = 32;
    constexpr usize HASH_OFFSET = 4;
    constexpr usize HASH_BYTES = 16;
    constexpr usize SIZE_OFFSET = 24;
    if (bytes.size() < HEADER_BYTES || std::memcmp(bytes.data(), "DXBC", HASH_OFFSET) != 0)
    {
        return false;
    }
    uint32 size = 0;
    std::memcpy(&size, bytes.data() + SIZE_OFFSET, sizeof(size));
    bool isConstant = true;
    for (usize i = 1; i < HASH_BYTES; ++i)
    {
        isConstant = isConstant == true && bytes[HASH_OFFSET + i] == bytes[HASH_OFFSET];
    }
    return size == bytes.size() && isConstant == false;
}
Array<uint8> ReadPackage(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const std::streamoff size = file.tellg();
    REQUIRE(size > 0);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(size)));
    file.seekg(0);
    REQUIRE(file.read(reinterpret_cast<char*>(bytes.GetData()), size).good());
    return bytes;
}
void CheckPackage(const char* path)
{
    const Array<uint8> bytes = ReadPackage(path);
    renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    const renderer::RenderGraphPrograms programs = package.GetPrograms({}, {});
    for (const rhi::RasterShaderDescription* stage : {&programs.ColorVertex, &programs.ColorFragment})
    {
        INFO(path);
        CHECK(IsSignedDxil(stage->Artifact.Dxil) == true);
        CHECK(stage->Artifact.DxilEntry.empty() == false);
        // A dxil-only package carries no other representation.
        CHECK(stage->Artifact.Spirv.empty() == true);
        CHECK(stage->Artifact.Wgsl.empty() == true);
    }
}
} // namespace

TEST_CASE("Render graphs built for the dxil profile cook signed DXIL stages", "[render-graph][dxil]")
{
    CheckPackage(LUDUS_RG2_SURFACE_PACKAGE);
    CheckPackage(LUDUS_RG2_BOUND_PACKAGE);
    // The compiled headers carry the same plans; their programs need the composite stages.
    CHECK(packages::rg2_surface::MakePlan().Passes.GetSize() != 0);
    CHECK(packages::rg2_bound::MakePlan().Passes.GetSize() != 0);
}

TEST_CASE("The Windows shader worker reloads a running Direct3D 12 graph", "[.native-dxil-reload]")
{
    using ReloadState = ludus::runtime::shader_development::ReloadState;
    GraphGpuFixture fixture;
    // The producer runs this case in its publication directory after the first build.
    const std::filesystem::path root = std::filesystem::current_path();
    const Array<uint8> initial = ReadPackage(root / "main.lurgrt");
    const std::u8string rootUtf8 = root.u8string();
    const std::string rootPath(reinterpret_cast<const char*>(rootUtf8.data()), rootUtf8.size());
    filesystem::WriteDirectory publication;
    REQUIRE(publication.Open(rootPath).Succeeded() == true);
    renderer::RenderGraphPackage identity;
    REQUIRE(identity.Decode({initial.GetData(), initial.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    renderer::RenderGraphActivation activation;
    REQUIRE(activation.Initialize(fixture.Device,
                                  identity.GetInterfaceIdentity(),
                                  shaders::renderer_composite::Vertex(),
                                  shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    ludus::runtime::shader_development::ReloadSession reload;
    REQUIRE(reload.Initialize({rootPath.data(), rootPath.size()},
                              {"main.lurgrt", sizeof("main.lurgrt") - 1},
                              activation) == rhi::RasterStatus::Ready);
    constexpr std::chrono::seconds DELIVERY_TIMEOUT{60};
    uint64 observedRequest = 0;
    const auto awaitDelivery = [&](ReloadState expected) {
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + DELIVERY_TIMEOUT;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const ludus::runtime::shader_development::ReloadReport report = reload.Service(time::NowTicks());
            if (report.State == expected && report.Request > observedRequest)
            {
                observedRequest = report.Request;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        FAIL("Timed out waiting for runtime publication delivery");
    };
    constexpr usize MAX_ATTEMPTS = 5000;
    const auto activate = [&]() {
        const uint64 generation = activation.GetReport().CandidateGeneration;
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
    const usize centerPixel = (usize{32} * WIDTH + 48) * 4;
    awaitDelivery(ReloadState::Staged);
    activate();
    render();
    REQUIRE(fixture.Pixels[centerPixel] == 255);
    REQUIRE(fixture.Pixels[centerPixel + 1] == 0);
    const uint64 firstGeneration = activation.GetReport().ActiveGeneration;
    // Release the producer after the original red generation is drawable.
    REQUIRE(publication.Publish("ready", {initial.GetData(), 1}).Published == true);
    awaitDelivery(ReloadState::Staged);
    activate();
    CHECK(activation.GetReport().ActiveGeneration != firstGeneration);
    render();
    REQUIRE(fixture.Pixels[centerPixel] == 0);
    REQUIRE(fixture.Pixels[centerPixel + 1] == 255);
    const uint64 greenGeneration = activation.GetReport().ActiveGeneration;
    const StaticArray<uint8, PIXEL_BYTES> greenPixels = fixture.Pixels;

    // A truncated replacement must fail delivery without erasing the green frame.
    REQUIRE(publication.Publish("main.lurgrt", {initial.GetData(), sizeof(uint32)}).Published == true);
    awaitDelivery(ReloadState::Failed);
    CHECK(activation.GetReport().ActiveGeneration == greenGeneration);
    render();
    CHECK(std::memcmp(greenPixels.GetData(), fixture.Pixels.GetData(), PIXEL_BYTES) == 0);
}
