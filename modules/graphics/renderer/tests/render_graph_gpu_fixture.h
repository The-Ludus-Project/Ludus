#pragma once

// Shared headless GPU fixture of the render-graph pixel suites: one red triangle drawn through a
// cooked graph into a 96x64 RGBA8 surface and read back after each frame, cooked-package loading,
// and a depth-aspect display pipeline. Test-only.

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>

#include "internal/readback.h"
#include "rg2_depth_display.h"

#include <catch2/catch_test_macros.hpp>

#include <new>
#include <string_view>

namespace ludus::graphics::renderer::testing
{
using namespace foundation;
inline constexpr uint32 WIDTH = 96;
inline constexpr uint32 HEIGHT = 64;
inline constexpr usize PIXEL_BYTES = static_cast<usize>(WIDTH) * HEIGHT * 4;
inline constexpr usize VERTEX_COUNT = 3;
inline constexpr usize TEXEL_BYTES = 4;
inline constexpr usize INDEX_COUNT = 3;
inline constexpr usize TABLE_COUNT = 1;
inline constexpr usize STREAM_COUNT = 1;
inline constexpr usize ATTRIBUTE_COUNT = 1;
struct RhiGuard final
{
    LUDUS_INLINE RhiGuard() noexcept
    {
        rhi::Shutdown();
    }
    LUDUS_INLINE ~RhiGuard() noexcept
    {
        rhi::Shutdown();
    }
};
struct GraphGpuFixture final
{
    RhiGuard Session;
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    RenderGraph Graph;
    StaticArray<math::Vector3, VERTEX_COUNT> Positions{{{-.5F, -.5F, .5F}, {.5F, -.5F, .5F}, {0, .5F, .5F}}};
    StaticArray<uint32, INDEX_COUNT> Indices{{0, 1, 2}};
    StaticArray<RenderGraphMesh, TABLE_COUNT> Meshes{};
    StaticArray<RenderGraphItem, TABLE_COUNT> Items{};
    StaticArray<ViewDescription, TABLE_COUNT> Cameras{};
    StaticArray<uint8, PIXEL_BYTES> Pixels{};
    GraphGpuFixture()
    {
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus started =
            rhi::CreateDevice({}, { .Width = WIDTH, .Height = HEIGHT }, description, Device, Surface, &failure);
        if (started != rhi::DeviceStatus::Ready && failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            SKIP("No GPU adapter on this host");
        }
        REQUIRE(started == rhi::DeviceStatus::Ready);
        Meshes[0].Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Items[0].SourceId = 1;
        Items[0].Material.Color = {1, 0, 0, 1};
    }
    void Update(uint64 revision)
    {
        RenderGraphReport report;
        REQUIRE(Graph.Update({Meshes.GetData(), Meshes.GetSize()},
                             {Items.GetData(), Items.GetSize()},
                             {Cameras.GetData(), Cameras.GetSize()},
                             revision,
                             RenderGraphCoveragePolicy::RejectIncomplete,
                             report) == rhi::RasterStatus::Ready);
    }
    void Render()
    {
        rhi::SubmissionToken completion;
        REQUIRE(Graph.Render(Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::backend::ReadHeadlessPixels(foundation::MakeSpan(Pixels)) == true);
    }
};
/// Build-tree location of one runtime generation cooked by ludus_compile_render_graph(... RUNTIME_PACKAGE).
struct PackageLocation final
{
    std::string_view Directory; ///< Absolute output directory of the graph.
    std::string_view File;      ///< `<NAME>.lurgrt` inside Directory.
};
inline core::UniquePtr<RenderGraphPackage> LoadPackage(const PackageLocation& location)
{
    filesystem::Directory directory;
    REQUIRE(directory.Open(location.Directory).Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead(location.File, file).Succeeded() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
    const filesystem::ReadResult read = file.ReadAt(0, foundation::MakeSpan(bytes));
    REQUIRE(read.Outcome.Succeeded() == true);
    REQUIRE(read.BytesRead == bytes.GetSize());
    RenderGraphPackage* allocationOrNull = new (std::nothrow) RenderGraphPackage;
    core::UniquePtr<RenderGraphPackage> package{allocationOrNull};
    REQUIRE(package.Get() != nullptr);
    REQUIRE(package->Decode({bytes.GetData(), bytes.GetSize()}) == RenderGraphPackageStatus::Ready);
    return package;
}
/// Draw a sampled depth aspect into the surface with the depth display recipe and read it back.
struct DepthDisplay final
{
    rhi::DeviceHandle Device{};
    rhi::BindingLayoutHandle Layout{};
    rhi::RasterPipelineHandle Pipeline{};
    rhi::BufferHandle Vertices{};
    rhi::BufferHandle Indices{};
    explicit DepthDisplay(rhi::DeviceHandle device) : Device(device)
    {
        const rhi::RasterShaderDescription vertex = shaders::rg2_depth_display::Vertex();
        const rhi::RasterShaderDescription fragment = shaders::rg2_depth_display::Fragment();
        rhi::RasterShaderHandle vertexHandle;
        rhi::RasterShaderHandle fragmentHandle;
        REQUIRE(rhi::CreateRasterShader(Device, vertex, vertexHandle) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateRasterShader(Device, fragment, fragmentHandle) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateBindingLayout(Device, fragment.Bindings, Layout) == rhi::RasterStatus::Ready);
        const StaticArray<rhi::RasterVertexStream, STREAM_COUNT> streams{{{sizeof(math::Vector4), false}}};
        const StaticArray<rhi::RasterVertexAttribute, ATTRIBUTE_COUNT> attributes{
            {{0, 0, 0, rhi::RasterVertexFormat::Float4}}};
        rhi::RasterPipelineDescription description;
        description.Vertex = vertexHandle;
        description.Fragment = fragmentHandle;
        description.Layout = Layout;
        description.Streams = {streams.GetData(), streams.GetSize()};
        description.Attributes = {attributes.GetData(), attributes.GetSize()};
        REQUIRE(rhi::CreateRasterPipeline(Device, description, Pipeline) == rhi::RasterStatus::Ready);
        const StaticArray<math::Vector4, VERTEX_COUNT> positions{{{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}};
        const StaticArray<uint32, INDEX_COUNT> indices{{0, 1, 2}};
        REQUIRE(rhi::CreateBuffer(Device,
                                  {rhi::BufferRole::Vertex, sizeof(positions)},
                                  {reinterpret_cast<const uint8*>(positions.GetData()), sizeof(positions)},
                                  Vertices) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::CreateBuffer(Device,
                                  {rhi::BufferRole::Index32, sizeof(indices)},
                                  {reinterpret_cast<const uint8*>(indices.GetData()), sizeof(indices)},
                                  Indices) == rhi::RasterStatus::Ready);
    }
    void Show(rhi::SurfaceHandle surface, rhi::TextureHandle texture, StaticArray<uint8, PIXEL_BYTES>& outPixels)
    {
        rhi::TextureViewHandle depth;
        REQUIRE(rhi::CreateTextureView(Device, texture, { .Aspect = rhi::TextureAspect::Depth }, depth) ==
                rhi::RasterStatus::Ready);
        const rhi::RasterBindingResource resource{ .Texture = depth };
        rhi::BindingSetHandle set;
        REQUIRE(rhi::CreateBindingSet(Device, Layout, {&resource, 1}, set) == rhi::RasterStatus::Ready);
        const rhi::RasterVertexSlice slice{Vertices, 0, sizeof(math::Vector4) * VERTEX_COUNT};
        rhi::RasterDraw drawing;
        drawing.Pipeline = Pipeline;
        drawing.Bindings = set;
        drawing.Vertices = {&slice, 1};
        drawing.Indices = Indices;
        drawing.IndexCount = INDEX_COUNT;
        drawing.VertexCount = VERTEX_COUNT;
        REQUIRE(rhi::BeginFrame(Device, surface) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::BeginRasterPass(Device, {}) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::DrawIndexed(Device, drawing) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(Device, completion) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::backend::ReadHeadlessPixels(foundation::MakeSpan(outPixels)) == true);
        static_cast<void>(rhi::Destroy(Device, set));
        static_cast<void>(rhi::Destroy(Device, depth));
    }
};
inline constexpr usize ROW_PIXELS = WIDTH;
/// Byte offset of pixel (column, row) in the RGBA8 readback.
constexpr usize GetPixel(usize column, usize row)
{
    return (row * ROW_PIXELS + column) * 4;
}
} // namespace ludus::graphics::renderer::testing
