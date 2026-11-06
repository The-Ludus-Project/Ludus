// Windows render-graph DXIL evidence (D6c): rg2 graphs authored for the dxil profile compile
// through the pinned DXC, cook into runtime packages and decode into Direct3D 12 shader
// descriptions, and the compiled graph draws extracted geometry on Direct3D 12 (WARP in CI).

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>

#include "internal/readback.h"
#include "renderer_composite.h"
#include "rg2_dxil_bound.h"
#include "rg2_dxil_surface.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <fstream>
#include <span>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace
{
constexpr uint32 WIDTH = 96;
constexpr uint32 HEIGHT = 64;
constexpr usize PIXEL_BYTES = static_cast<usize>(WIDTH) * HEIGHT * 4;
constexpr usize VERTEX_COUNT = 3;
constexpr usize INDEX_COUNT = 3;
constexpr usize TABLE_COUNT = 1;
struct RhiGuard final
{
    RhiGuard() noexcept
    {
        rhi::Shutdown();
    }
    ~RhiGuard() noexcept
    {
        rhi::Shutdown();
    }
};
/// One red triangle drawn headlessly through a render graph, as the portable RG2 GPU suite does.
struct GraphGpuFixture final
{
    RhiGuard Session;
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    renderer::RenderGraph Graph;
    StaticArray<math::Vector3, VERTEX_COUNT> Positions{{{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}}};
    StaticArray<uint32, INDEX_COUNT> Indices{{0, 1, 2}};
    StaticArray<renderer::RenderGraphMesh, TABLE_COUNT> Meshes{};
    StaticArray<renderer::RenderGraphItem, TABLE_COUNT> Items{};
    StaticArray<renderer::ViewDescription, TABLE_COUNT> Cameras{};
    StaticArray<uint8, PIXEL_BYTES> Pixels{};
    GraphGpuFixture()
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus started =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface, &failure);
        REQUIRE(started == rhi::DeviceStatus::Ready);
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Items[0].SourceId = 1;
        Items[0].Material.Color = {1, 0, 0, 1};
    }
    void Update(uint64 revision)
    {
        renderer::RenderGraphReport report;
        REQUIRE(Graph.Update({Meshes.GetData(), Meshes.GetSize()},
                             {Items.GetData(), Items.GetSize()},
                             {Cameras.GetData(), Cameras.GetSize()},
                             revision,
                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                             report) == rhi::RasterStatus::Ready);
    }
    void Render()
    {
        rhi::SubmissionToken completion;
        REQUIRE(Graph.Render(Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::backend::ReadHeadlessPixels(Pixels.AsSpan()) == true);
    }
};
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
void CheckPackage(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const std::streamoff size = file.tellg();
    REQUIRE(size > 0);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(size)));
    file.seekg(0);
    REQUIRE(file.read(reinterpret_cast<char*>(bytes.GetData()), size).good());
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
    CheckPackage(LUDUS_RG2_DXIL_SURFACE_PACKAGE);
    CheckPackage(LUDUS_RG2_DXIL_BOUND_PACKAGE);
    // The compiled headers carry the same plans; their programs need the composite stages.
    CHECK(packages::rg2_dxil_surface::MakePlan().Passes.GetSize() != 0);
    CHECK(packages::rg2_dxil_bound::MakePlan().Passes.GetSize() != 0);
}

TEST_CASE("A DXIL render graph draws extracted geometry on Direct3D 12 and follows edits", "[render-graph][dxil][gpu]")
{
    GraphGpuFixture fixture;
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     packages::rg2_dxil_surface::MakePlan(),
                                     packages::rg2_dxil_surface::MakePrograms(
                                         shaders::renderer_composite::Vertex(),
                                         shaders::renderer_composite::Fragment())) == rhi::RasterStatus::Ready);
    fixture.Update(1);
    fixture.Render();
    const usize centerPixel = (usize{32} * WIDTH + 48) * 4;
    CHECK(fixture.Pixels[centerPixel] == 255);
    CHECK(fixture.Pixels[centerPixel + 1] == 0);
    // Moving the instance and then the camera moves the triangle off and back over the center.
    fixture.Items[0].MeshToWorld.Translation.X = 1;
    fixture.Update(2);
    fixture.Render();
    CHECK(fixture.Pixels[centerPixel] == 0);
    fixture.Cameras[0].WorldToClip.Columns[3].X = -1;
    fixture.Update(3);
    fixture.Render();
    CHECK(fixture.Pixels[centerPixel] == 255);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 3);
}
