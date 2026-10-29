#include "../src/internal/raster_mesh_lights.h"
#include "../src/internal/render_graph_metalfx.h"
#include "../src/internal/render_graph_upscale.h"
#include "internal/readback.h"
#include "renderer_composite.h"
#include "rg2_bound.h"
#include "rg2_depth.h"
#include "rg2_depth_display.h"
#include "rg2_forward_lit.h"
#include "rg2_independent.h"
#include "rg2_masked_color.h"
#include "rg2_masked_depth.h"
#include "rg2_offscreen.h"
#include "rg2_surface.h"

#include <ludus/foundation/base/compiler.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/runtime/shader_development/reload.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <ludus/graphics/rhi/spatial_upscale.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string_view>
#include <thread>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace packages = ludus::render_graph_packages;
namespace shaders = ludus::shaders;
namespace format = ludus::content::render_graph_format;
namespace
{
constexpr uint32 WIDTH = 96;
constexpr uint32 HEIGHT = 64;
constexpr usize PIXEL_BYTES = static_cast<usize>(WIDTH) * HEIGHT * 4;
constexpr usize VERTEX_COUNT = 3;
constexpr usize TEXEL_BYTES = 4;
constexpr usize INDEX_COUNT = 3;
constexpr usize TABLE_COUNT = 1;
constexpr usize STREAM_COUNT = 1;
constexpr usize ATTRIBUTE_COUNT = 1;
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
/// Build-tree location of one runtime generation cooked by ludus_compile_render_graph(... RUNTIME_PACKAGE).
struct PackageLocation final
{
    std::string_view Directory; ///< Absolute output directory of the graph.
    std::string_view File;      ///< `<NAME>.lurgrt` inside Directory.
};
constexpr PackageLocation SURFACE_PACKAGE{LUDUS_RG3_SURFACE_DIRECTORY, "rg2_surface.lurgrt"};
constexpr PackageLocation BOUND_PACKAGE{LUDUS_RG3_BOUND_DIRECTORY, "rg2_bound.lurgrt"};
core::UniquePtr<renderer::RenderGraphPackage> LoadPackage(const PackageLocation& location)
{
    filesystem::Directory directory;
    REQUIRE(directory.Open(location.Directory).Succeeded() == true);
    filesystem::File file;
    REQUIRE(directory.OpenRead(location.File, file).Succeeded() == true);
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(static_cast<usize>(file.Size())) == true);
    const filesystem::ReadResult read = file.ReadAt(0, bytes.AsSpan());
    REQUIRE(read.Outcome.Succeeded() == true);
    REQUIRE(read.BytesRead == bytes.GetSize());
    renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
    REQUIRE(package.Get() != nullptr);
    REQUIRE(package->Decode({bytes.GetData(), bytes.GetSize()}) == renderer::RenderGraphPackageStatus::Ready);
    return package;
}
core::UniquePtr<renderer::RenderGraphPackage> LoadSurfacePackage()
{
    return LoadPackage(SURFACE_PACKAGE);
}
/// Compare one runtime stage description with the compiled header's, target by target.
void CheckSameStage(const rhi::RasterShaderDescription& runtime, const rhi::RasterShaderDescription& compiled)
{
    CHECK(runtime.Artifact.Stage == compiled.Artifact.Stage);
    REQUIRE(runtime.Artifact.Spirv.size() == compiled.Artifact.Spirv.size());
    for (usize word = 0; word < compiled.Artifact.Spirv.size(); ++word)
    {
        REQUIRE(runtime.Artifact.Spirv[word] == compiled.Artifact.Spirv[word]);
    }
    CHECK(runtime.Artifact.SpirvEntry == compiled.Artifact.SpirvEntry);
    CHECK(runtime.Artifact.Msl == compiled.Artifact.Msl);
    CHECK(runtime.Artifact.MslEntry == compiled.Artifact.MslEntry);
    CHECK(runtime.Artifact.Wgsl == compiled.Artifact.Wgsl);
    CHECK(runtime.Artifact.WgslEntry == compiled.Artifact.WgslEntry);
    CHECK(runtime.Artifact.GlslEs == compiled.Artifact.GlslEs);
    CHECK(runtime.Artifact.GlslEsEntry == compiled.Artifact.GlslEsEntry);
    REQUIRE(runtime.Inputs.size() == compiled.Inputs.size());
    for (usize input = 0; input < compiled.Inputs.size(); ++input)
    {
        CHECK(runtime.Inputs[input].Location == compiled.Inputs[input].Location);
        CHECK(runtime.Inputs[input].Format == compiled.Inputs[input].Format);
    }
    REQUIRE(runtime.Bindings.size() == compiled.Bindings.size());
    for (usize binding = 0; binding < compiled.Bindings.size(); ++binding)
    {
        CHECK(runtime.Bindings[binding].Binding == compiled.Bindings[binding].Binding);
        CHECK(runtime.Bindings[binding].Kind == compiled.Bindings[binding].Kind);
        CHECK(runtime.Bindings[binding].Visibility == compiled.Bindings[binding].Visibility);
        CHECK(runtime.Bindings[binding].MinSize == compiled.Bindings[binding].MinSize);
    }
    for (usize slot = 0; slot < format::UNIFORM_SLOT_COUNT; ++slot)
    {
        CHECK(runtime.UniformBlocks[slot] == compiled.UniformBlocks[slot]);
    }
}
/// Compare every numeric uniform record of a decoded generation with its compiled header.
void CheckSameUniforms(const renderer::RenderGraphPlan& runtime, const renderer::RenderGraphPlan& compiled)
{
    CHECK(runtime.Description.UniformValueCount == compiled.Description.UniformValueCount);
    REQUIRE(runtime.UniformBindings.GetSize() == compiled.UniformBindings.GetSize());
    for (usize i = 0; i < compiled.UniformBindings.GetSize(); ++i)
    {
        CHECK(runtime.UniformBindings[i].Program == compiled.UniformBindings[i].Program);
        CHECK(runtime.UniformBindings[i].Slot == compiled.UniformBindings[i].Slot);
        CHECK(runtime.UniformBindings[i].Stages == compiled.UniformBindings[i].Stages);
        CHECK(runtime.UniformBindings[i].ValueOffset == compiled.UniformBindings[i].ValueOffset);
        CHECK(runtime.UniformBindings[i].ValueCount == compiled.UniformBindings[i].ValueCount);
    }
    REQUIRE(runtime.UniformBlocks.GetSize() == compiled.UniformBlocks.GetSize());
    for (usize i = 0; i < compiled.UniformBlocks.GetSize(); ++i)
    {
        CHECK(runtime.UniformBlocks[i].Binding == compiled.UniformBlocks[i].Binding);
        CHECK(runtime.UniformBlocks[i].Target == compiled.UniformBlocks[i].Target);
        CHECK(runtime.UniformBlocks[i].Size == compiled.UniformBlocks[i].Size);
    }
    REQUIRE(runtime.UniformFields.GetSize() == compiled.UniformFields.GetSize());
    for (usize i = 0; i < compiled.UniformFields.GetSize(); ++i)
    {
        CHECK(runtime.UniformFields[i].Binding == compiled.UniformFields[i].Binding);
        CHECK(runtime.UniformFields[i].Target == compiled.UniformFields[i].Target);
        CHECK(runtime.UniformFields[i].ValueOffset == compiled.UniformFields[i].ValueOffset);
        CHECK(runtime.UniformFields[i].Components == compiled.UniformFields[i].Components);
        CHECK(runtime.UniformFields[i].ByteOffset == compiled.UniformFields[i].ByteOffset);
    }
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
        description.Streams = streams.AsSpan();
        description.Attributes = attributes.AsSpan();
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
        REQUIRE(rhi::backend::ReadHeadlessPixels(outPixels.AsSpan()) == true);
        static_cast<void>(rhi::Destroy(Device, set));
        static_cast<void>(rhi::Destroy(Device, depth));
    }
};
constexpr usize ROW_PIXELS = WIDTH;
/// Byte offset of pixel (column, row) in the RGBA8 readback.
constexpr usize GetPixel(usize column, usize row)
{
    return (row * ROW_PIXELS + column) * 4;
}
} // namespace
TEST_CASE("RG2 cooked graph renders extracted geometry and follows camera/instance changes", "[render-graph][gpu]")
{
    GraphGpuFixture fixture;
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     packages::rg2_surface::MakePlan(),
                                     packages::rg2_surface::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                         shaders::renderer_composite::Fragment())) ==
            rhi::RasterStatus::Ready);
    fixture.Update(1);
    fixture.Render();
    const usize centerPixel = (usize{32} * WIDTH + 48) * 4;
    CHECK(fixture.Pixels[centerPixel] == 255);
    CHECK(fixture.Pixels[centerPixel + 1] == 0);
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
TEST_CASE("RG2 exported offscreen color matches the same direct raster graph", "[render-graph][gpu]")
{
    GraphGpuFixture fixture;
    fixture.Items[0].Material.Color = {.25F, .5F, .75F, 1};
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     packages::rg2_surface::MakePlan(),
                                     packages::rg2_surface::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                         shaders::renderer_composite::Fragment())) ==
            rhi::RasterStatus::Ready);
    fixture.Update(1);
    fixture.Render();
    const StaticArray<uint8, PIXEL_BYTES> direct = fixture.Pixels;
    fixture.Graph.Reset();
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     packages::rg2_offscreen::MakePlan(),
                                     packages::rg2_offscreen::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                           shaders::renderer_composite::Fragment())) ==
            rhi::RasterStatus::Ready);
    fixture.Update(2);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(fixture.Graph.GetExportAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
    REQUIRE(aspect == rhi::TextureAspect::Color);
    renderer::Renderer compositor;
    REQUIRE(compositor.Initialize(fixture.Device,
                                  packages::rg2_offscreen::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                        shaders::renderer_composite::Fragment())
                                      .ColorVertex,
                                  packages::rg2_offscreen::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                        shaders::renderer_composite::Fragment())
                                      .ColorFragment) == rhi::RasterStatus::Ready);
    REQUIRE(compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                       shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    renderer::ViewMapping mapping;
    REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                         {0, 0, WIDTH, HEIGHT},
                                         WIDTH,
                                         HEIGHT,
                                         mapping) == rhi::RasterStatus::Ready);
    renderer::Presentation presentation;
    REQUIRE(compositor.PreparePresentation(texture, mapping, presentation) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
    REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
    const rhi::RasterStatus confirmed = fixture.Graph.ConfirmSubmission(completion);
    REQUIRE((confirmed == rhi::RasterStatus::Ready || confirmed == rhi::RasterStatus::Pending));
    REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    for (usize i = 0; i < PIXEL_BYTES; ++i)
    {
        REQUIRE(fixture.Pixels[i] == direct[i]);
    }
}
TEST_CASE("RG2 depth-only export samples actual opaque geometry depth", "[render-graph][gpu]")
{
    GraphGpuFixture fixture;
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     packages::rg2_depth::MakePlan(),
                                     packages::rg2_depth::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                       shaders::renderer_composite::Fragment())) ==
            rhi::RasterStatus::Ready);
    fixture.Update(1);
    fixture.Render();
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(fixture.Graph.GetExport(0, texture, aspect) == rhi::RasterStatus::Ready);
    REQUIRE(aspect == rhi::TextureAspect::Depth);
    rhi::TextureViewHandle depth;
    REQUIRE(rhi::CreateTextureView(fixture.Device, texture, { .Aspect = aspect }, depth) == rhi::RasterStatus::Ready);
    const rhi::RasterShaderDescription vertex = shaders::rg2_depth_display::Vertex();
    const rhi::RasterShaderDescription fragment = shaders::rg2_depth_display::Fragment();
    rhi::RasterShaderHandle vertexHandle;
    rhi::RasterShaderHandle fragmentHandle;
    REQUIRE(rhi::CreateRasterShader(fixture.Device, vertex, vertexHandle) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateRasterShader(fixture.Device, fragment, fragmentHandle) == rhi::RasterStatus::Ready);
    rhi::BindingLayoutHandle layout;
    REQUIRE(rhi::CreateBindingLayout(fixture.Device, fragment.Bindings, layout) == rhi::RasterStatus::Ready);
    const rhi::RasterBindingResource resource{ .Texture = depth };
    rhi::BindingSetHandle set;
    REQUIRE(rhi::CreateBindingSet(fixture.Device, layout, {&resource, 1}, set) == rhi::RasterStatus::Ready);
    const StaticArray<rhi::RasterVertexStream, STREAM_COUNT> streams{{{sizeof(math::Vector4), false}}};
    const StaticArray<rhi::RasterVertexAttribute, ATTRIBUTE_COUNT> attributes{
        {{0, 0, 0, rhi::RasterVertexFormat::Float4}}};
    rhi::RasterPipelineDescription description;
    description.Vertex = vertexHandle;
    description.Fragment = fragmentHandle;
    description.Layout = layout;
    description.Streams = streams.AsSpan();
    description.Attributes = attributes.AsSpan();
    rhi::RasterPipelineHandle pipeline;
    REQUIRE(rhi::CreateRasterPipeline(fixture.Device, description, pipeline) == rhi::RasterStatus::Ready);
    const StaticArray<math::Vector4, VERTEX_COUNT> positions{{{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}};
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    REQUIRE(rhi::CreateBuffer(fixture.Device,
                              {rhi::BufferRole::Vertex, sizeof(positions)},
                              {reinterpret_cast<const uint8*>(positions.GetData()), sizeof(positions)},
                              vertices) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::CreateBuffer(fixture.Device,
                              {rhi::BufferRole::Index32, sizeof(fixture.Indices)},
                              {reinterpret_cast<const uint8*>(fixture.Indices.GetData()), sizeof(fixture.Indices)},
                              indices) == rhi::RasterStatus::Ready);
    const rhi::RasterVertexSlice slice{vertices, 0, sizeof(positions)};
    rhi::RasterDraw drawing;
    drawing.Pipeline = pipeline;
    drawing.Bindings = set;
    drawing.Vertices = {&slice, 1};
    drawing.Indices = indices;
    drawing.IndexCount = INDEX_COUNT;
    drawing.VertexCount = VERTEX_COUNT;
    const auto displayDepth = [&]() {
        REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
        REQUIRE(rhi::BeginRasterPass(fixture.Device, {}) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::DrawIndexed(fixture.Device, drawing) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
        REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    };
    displayDepth();
    const usize centerPixel = (usize{32} * WIDTH + 48) * 4;
    CHECK(fixture.Pixels[centerPixel] >= 127);
    CHECK(fixture.Pixels[centerPixel] <= 128);
    CHECK(fixture.Pixels[0] == 255);
    fixture.Items[0].MeshToWorld.Translation.X = 1;
    fixture.Update(2);
    fixture.Render();
    displayDepth();
    CHECK(fixture.Pixels[centerPixel] == 255);
    fixture.Cameras[0].WorldToClip.Columns[3].X = -1;
    fixture.Update(3);
    fixture.Render();
    displayDepth();
    CHECK(fixture.Pixels[centerPixel] >= 127);
    CHECK(fixture.Pixels[centerPixel] <= 128);
}
TEST_CASE("RG3 cooked runtime generation decodes the compiled header's plan and stage artifacts",
          "[render-graph][activation]")
{
    const core::UniquePtr<renderer::RenderGraphPackage> package = LoadSurfacePackage();
    const BorrowedView<const uint8> identity = package->GetInterfaceIdentity();
    REQUIRE(identity.GetSize() == packages::rg2_surface::INTERFACE_IDENTITY.GetSize());
    for (usize i = 0; i < identity.GetSize(); ++i)
    {
        CHECK(identity[i] == packages::rg2_surface::INTERFACE_IDENTITY[i]);
    }
    const renderer::RenderGraphPlan runtimePlan = package->GetPlan();
    const renderer::RenderGraphPlan compiledPlan = packages::rg2_surface::MakePlan();
    CHECK(runtimePlan.Description.Version == compiledPlan.Description.Version);
    CHECK(runtimePlan.Description.CameraCount == compiledPlan.Description.CameraCount);
    REQUIRE(runtimePlan.Resources.GetSize() == compiledPlan.Resources.GetSize());
    for (usize i = 0; i < compiledPlan.Resources.GetSize(); ++i)
    {
        const format::Resource& runtime = runtimePlan.Resources[i];
        const format::Resource& compiled = compiledPlan.Resources[i];
        CHECK(runtime.Width == compiled.Width);
        CHECK(runtime.Height == compiled.Height);
        CHECK(runtime.Kind == compiled.Kind);
        CHECK(runtime.Backing == compiled.Backing);
        CHECK(runtime.IsSurface == compiled.IsSurface);
        CHECK(runtime.IsExported == compiled.IsExported);
    }
    REQUIRE(runtimePlan.Cameras.GetSize() == compiledPlan.Cameras.GetSize());
    for (usize i = 0; i < compiledPlan.Cameras.GetSize(); ++i)
    {
        CHECK(runtimePlan.Cameras[i].Target == compiledPlan.Cameras[i].Target);
    }
    REQUIRE(runtimePlan.Passes.GetSize() == compiledPlan.Passes.GetSize());
    for (usize i = 0; i < compiledPlan.Passes.GetSize(); ++i)
    {
        const format::Pass& runtime = runtimePlan.Passes[i];
        const format::Pass& compiled = compiledPlan.Passes[i];
        CHECK(runtime.Program == compiled.Program);
        CHECK(runtime.Camera == compiled.Camera);
        CHECK(runtime.Color == compiled.Color);
        CHECK(runtime.Depth == compiled.Depth);
        CHECK(runtime.ColorLoad == compiled.ColorLoad);
        CHECK(runtime.ColorStore == compiled.ColorStore);
        CHECK(runtime.DepthLoad == compiled.DepthLoad);
        CHECK(runtime.DepthStore == compiled.DepthStore);
        CHECK(runtime.AllowsRepeatedCoverage == compiled.AllowsRepeatedCoverage);
    }
    const renderer::RenderGraphPrograms runtime = package->GetPrograms({}, {});
    const renderer::RenderGraphPrograms compiled = packages::rg2_surface::MakePrograms({}, {});
    CheckSameStage(runtime.ColorVertex, compiled.ColorVertex);
    CheckSameStage(runtime.ColorFragment, compiled.ColorFragment);
    CheckSameStage(runtime.DepthVertex, compiled.DepthVertex);
    CheckSameStage(runtime.DepthFragment, compiled.DepthFragment);
    // The fixture is cooked for this host's native target and WGSL; neither may be silently absent.
#if defined(LUDUS_TEST_METAL)
    CHECK(runtime.ColorVertex.Artifact.Msl.empty() == false);
    CHECK(runtime.ColorFragment.Artifact.Msl.empty() == false);
#else
    CHECK(runtime.ColorVertex.Artifact.Spirv.empty() == false);
    CHECK(runtime.ColorFragment.Artifact.Spirv.empty() == false);
#endif
    CHECK(runtime.ColorVertex.Artifact.Wgsl.empty() == false);
    CHECK(runtime.ColorFragment.Artifact.Wgsl.empty() == false);
}
TEST_CASE("RG3 runtime generation renders the same pixels as its compiled header across activation",
          "[render-graph][gpu][activation]")
{
    GraphGpuFixture fixture;
    fixture.Items[0].Material.Color = {.25F, .5F, .75F, 1};
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     packages::rg2_surface::MakePlan(),
                                     packages::rg2_surface::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                         shaders::renderer_composite::Fragment())) ==
            rhi::RasterStatus::Ready);
    fixture.Update(1);
    fixture.Render();
    const StaticArray<uint8, PIXEL_BYTES> compiled = fixture.Pixels;
    fixture.Graph.Reset();
    renderer::RenderGraphActivation activation;
    REQUIRE(activation.Initialize(fixture.Device,
                                  {packages::rg2_surface::INTERFACE_IDENTITY.GetData(),
                                   packages::rg2_surface::INTERFACE_IDENTITY.GetSize()},
                                  shaders::renderer_composite::Vertex(),
                                  shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    const rhi::RasterStatus staged = activation.Stage(LoadSurfacePackage());
    REQUIRE((staged == rhi::RasterStatus::Ready || staged == rhi::RasterStatus::Pending));
    rhi::RasterStatus polled = activation.Poll();
    for (usize attempt = 0; polled == rhi::RasterStatus::Pending && attempt < 1000; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        polled = activation.Poll();
    }
    REQUIRE(polled == rhi::RasterStatus::Ready);
    REQUIRE(activation.GetReport().ActiveGeneration == 1);
    // Stage a second decoded generation, then activate it at a frame boundary.
    renderer::RenderGraphReport report;
    REQUIRE(activation.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                              {fixture.Items.GetData(), fixture.Items.GetSize()},
                              {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                              1,
                              renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                              report) == rhi::RasterStatus::Ready);
    const rhi::RasterStatus restaged = activation.Stage(LoadSurfacePackage());
    REQUIRE((restaged == rhi::RasterStatus::Ready || restaged == rhi::RasterStatus::Pending));
    for (usize attempt = 0; activation.GetReport().ActiveGeneration != 2 && attempt < 1000; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(activation.Poll() == rhi::RasterStatus::Ready);
        if (activation.GetReport().Candidate == renderer::RenderGraphCandidateState::NeedsUpdate)
        {
            REQUIRE(activation.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                      {fixture.Items.GetData(), fixture.Items.GetSize()},
                                      {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                      1,
                                      renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                      report) == rhi::RasterStatus::Ready);
        }
    }
    REQUIRE(activation.GetReport().ActiveGeneration == 2);
    renderer::RenderGraph* activeOrNull = activation.GetActiveOrNull();
    REQUIRE(activeOrNull != nullptr);
    rhi::SubmissionToken completion;
    REQUIRE(activeOrNull->Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    for (usize i = 0; i < PIXEL_BYTES; ++i)
    {
        REQUIRE(fixture.Pixels[i] == compiled[i]);
    }
    activation.Reset();
}
namespace
{
constexpr usize BOUND_ITEM_COUNT = 2;
constexpr usize BOUND_VALUE_COUNT = packages::rg2_bound::UNIFORM_VALUE_COUNT;
constexpr usize MASK_VALUE_COUNT = packages::rg2_masked_depth::UNIFORM_VALUE_COUNT;
/// Two items left and right of center, each with its own per-draw Material values.
struct BoundScene final
{
    StaticArray<renderer::RenderGraphItem, BOUND_ITEM_COUNT> Items{};
    StaticArray<StaticArray<float32, BOUND_VALUE_COUNT>, BOUND_ITEM_COUNT> Values{};
    explicit BoundScene(const renderer::RenderGraphItem& prototype)
    {
        for (usize i = 0; i < BOUND_ITEM_COUNT; ++i)
        {
            Items[i] = prototype;
            Items[i].SourceId = i + 1;
            Items[i].MeshToWorld.Translation.X = i == 0 ? -.5F : .5F;
            Items[i].Uniforms = {Values[i].GetData(), BOUND_VALUE_COUNT};
        }
        constexpr usize TINT = packages::rg2_bound::UNIFORM_VALUE_MATERIAL_TINT;
        constexpr usize GAIN = packages::rg2_bound::UNIFORM_VALUE_MATERIAL_GAIN;
        Values[0][TINT] = 1;
        Values[0][GAIN] = 1;
        Values[1][TINT + 1] = 1;
        Values[1][GAIN] = .5F;
    }
    rhi::RasterStatus Update(renderer::RenderGraph& inoutGraph, GraphGpuFixture& inoutFixture, uint64 revision)
    {
        renderer::RenderGraphReport report;
        return inoutGraph.Update({inoutFixture.Meshes.GetData(), inoutFixture.Meshes.GetSize()},
                                 {Items.GetData(), Items.GetSize()},
                                 {inoutFixture.Cameras.GetData(), inoutFixture.Cameras.GetSize()},
                                 revision,
                                 renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                 report);
    }
};
renderer::RenderGraphPrograms MakeBoundPrograms()
{
    return packages::rg2_bound::MakePrograms(shaders::renderer_composite::Vertex(),
                                             shaders::renderer_composite::Fragment());
}
} // namespace
TEST_CASE("RG2 bound uniforms render per-draw material values without shader permutations", "[render-graph][gpu]")
{
    GraphGpuFixture fixture;
    REQUIRE(fixture.Graph.Initialize(fixture.Device, packages::rg2_bound::MakePlan(), MakeBoundPrograms()) ==
            rhi::RasterStatus::Ready);
    BoundScene scene{fixture.Items[0]};
    REQUIRE(scene.Update(fixture.Graph, fixture, 1) == rhi::RasterStatus::Ready);
    fixture.Render();
    const usize left = GetPixel(24, 32);
    const usize right = GetPixel(72, 32);
    CHECK(fixture.Pixels[left] == 255);
    CHECK(fixture.Pixels[left + 1] == 0);
    CHECK(fixture.Pixels[right] == 0);
    CHECK(fixture.Pixels[right + 1] >= 127);
    CHECK(fixture.Pixels[right + 1] <= 128);
    // Changing only runtime values republishes the extraction with the same cooked programs.
    scene.Values[0][packages::rg2_bound::UNIFORM_VALUE_MATERIAL_GAIN] = .5F;
    REQUIRE(scene.Update(fixture.Graph, fixture, 2) == rhi::RasterStatus::Ready);
    fixture.Render();
    CHECK(fixture.Pixels[left] >= 127);
    CHECK(fixture.Pixels[left] <= 128);
    CHECK(fixture.Pixels[right + 1] >= 127);
}
TEST_CASE("RG3 bound runtime generation carries the compiled uniform tables and renders the same pixels",
          "[render-graph][gpu][activation]")
{
    GraphGpuFixture fixture;
    const core::UniquePtr<renderer::RenderGraphPackage> decoded = LoadPackage(BOUND_PACKAGE);
    const BorrowedView<const uint8> identity = decoded->GetInterfaceIdentity();
    REQUIRE(identity.GetSize() == packages::rg2_bound::INTERFACE_IDENTITY.GetSize());
    for (usize i = 0; i < identity.GetSize(); ++i)
    {
        CHECK(identity[i] == packages::rg2_bound::INTERFACE_IDENTITY[i]);
    }
    CheckSameUniforms(decoded->GetPlan(), packages::rg2_bound::MakePlan());
    const renderer::RenderGraphPrograms runtime = decoded->GetPrograms({}, {});
    const renderer::RenderGraphPrograms compiled = packages::rg2_bound::MakePrograms({}, {});
    CheckSameStage(runtime.ColorVertex, compiled.ColorVertex);
    CheckSameStage(runtime.ColorFragment, compiled.ColorFragment);
    CHECK(runtime.ColorFragment.Bindings.empty() == false);
    REQUIRE(fixture.Graph.Initialize(fixture.Device, packages::rg2_bound::MakePlan(), MakeBoundPrograms()) ==
            rhi::RasterStatus::Ready);
    BoundScene scene{fixture.Items[0]};
    REQUIRE(scene.Update(fixture.Graph, fixture, 1) == rhi::RasterStatus::Ready);
    fixture.Render();
    const StaticArray<uint8, PIXEL_BYTES> header = fixture.Pixels;
    fixture.Graph.Reset();
    renderer::RenderGraphActivation activation;
    REQUIRE(activation.Initialize(
                fixture.Device,
                {packages::rg2_bound::INTERFACE_IDENTITY.GetData(), packages::rg2_bound::INTERFACE_IDENTITY.GetSize()},
                shaders::renderer_composite::Vertex(),
                shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    const rhi::RasterStatus staged = activation.Stage(LoadPackage(BOUND_PACKAGE));
    REQUIRE((staged == rhi::RasterStatus::Ready || staged == rhi::RasterStatus::Pending));
    rhi::RasterStatus polled = activation.Poll();
    for (usize attempt = 0; polled == rhi::RasterStatus::Pending && attempt < 1000; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        polled = activation.Poll();
    }
    REQUIRE(polled == rhi::RasterStatus::Ready);
    renderer::RenderGraph* activeOrNull = activation.GetActiveOrNull();
    REQUIRE(activeOrNull != nullptr);
    REQUIRE(scene.Update(*activeOrNull, fixture, 1) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(activeOrNull->Render(fixture.Surface, WIDTH, HEIGHT, completion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    for (usize i = 0; i < PIXEL_BYTES; ++i)
    {
        REQUIRE(fixture.Pixels[i] == header[i]);
    }
    activation.Reset();
}
TEST_CASE("RG2 masked color and separately authored masked depth agree on authored coverage", "[render-graph][gpu]")
{
    GraphGpuFixture fixture;
    StaticArray<float32, MASK_VALUE_COUNT> mask{};
    mask[packages::rg2_masked_depth::UNIFORM_VALUE_MASK_THRESHOLD] = 0;
    fixture.Items[0].Coverage = renderer::RenderGraphMaterial::Masked;
    fixture.Items[0].Uniforms = {mask.GetData(), mask.GetSize()};
    DepthDisplay display{fixture.Device};
    // Masked depth-only export, the pre-Z technique's authored coverage.
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     packages::rg2_masked_depth::MakePlan(),
                                     packages::rg2_masked_depth::MakePrograms(
                                         shaders::renderer_composite::Vertex(),
                                         shaders::renderer_composite::Fragment())) == rhi::RasterStatus::Ready);
    fixture.Update(1);
    fixture.Render();
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(fixture.Graph.GetExport(0, texture, aspect) == rhi::RasterStatus::Ready);
    REQUIRE(aspect == rhi::TextureAspect::Depth);
    StaticArray<uint8, PIXEL_BYTES> depthOnly{};
    display.Show(fixture.Surface, texture, depthOnly);
    // Mesh x >= threshold keeps the right half of the triangle at its 0.5 depth.
    const usize kept = GetPixel(55, 32);
    const usize discarded = GetPixel(40, 32);
    CHECK(depthOnly[kept] >= 127);
    CHECK(depthOnly[kept] <= 128);
    CHECK(depthOnly[discarded] == 255);
    CHECK(depthOnly[0] == 255);
    fixture.Graph.Reset();
    // Masked color technique, exporting its color and its depth aspect.
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     packages::rg2_masked_color::MakePlan(),
                                     packages::rg2_masked_color::MakePrograms(
                                         shaders::renderer_composite::Vertex(),
                                         shaders::renderer_composite::Fragment())) == rhi::RasterStatus::Ready);
    fixture.Update(2);
    fixture.Render();
    rhi::TextureHandle colorTexture;
    REQUIRE(fixture.Graph.GetExport(0, colorTexture, aspect) == rhi::RasterStatus::Ready);
    REQUIRE(aspect == rhi::TextureAspect::Color);
    REQUIRE(fixture.Graph.GetExport(1, texture, aspect) == rhi::RasterStatus::Ready);
    REQUIRE(aspect == rhi::TextureAspect::Depth);
    StaticArray<uint8, PIXEL_BYTES> colorDepth{};
    display.Show(fixture.Surface, texture, colorDepth);
    // Depth parity: the same authored coverage writes byte-identical depth in both techniques.
    for (usize i = 0; i < PIXEL_BYTES; ++i)
    {
        REQUIRE(colorDepth[i] == depthOnly[i]);
    }
    // Color parity: covered pixels are exactly those with geometry depth.
    renderer::Renderer compositor;
    const renderer::RenderGraphPrograms flat =
        packages::rg2_surface::MakePrograms(shaders::renderer_composite::Vertex(),
                                            shaders::renderer_composite::Fragment());
    REQUIRE(compositor.Initialize(fixture.Device, flat.ColorVertex, flat.ColorFragment) == rhi::RasterStatus::Ready);
    REQUIRE(compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                       shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    renderer::ViewMapping mapping;
    REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                         {0, 0, WIDTH, HEIGHT},
                                         WIDTH,
                                         HEIGHT,
                                         mapping) == rhi::RasterStatus::Ready);
    renderer::Presentation presentation;
    REQUIRE(compositor.PreparePresentation(colorTexture, mapping, presentation) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    for (usize row = 0; row < HEIGHT; ++row)
    {
        for (usize column = 0; column < WIDTH; ++column)
        {
            const usize pixel = GetPixel(column, row);
            const bool hasGeometry = depthOnly[pixel] != 255;
            const bool isRed = fixture.Pixels[pixel] == 255 && fixture.Pixels[pixel + 1] == 0;
            REQUIRE(hasGeometry == isRed);
        }
    }
    static_cast<void>(compositor.Release(presentation));
    compositor.Reset();
    fixture.Graph.Reset();
    // A per-draw threshold beyond the mesh removes all coverage; no program is rebuilt.
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     packages::rg2_masked_depth::MakePlan(),
                                     packages::rg2_masked_depth::MakePrograms(
                                         shaders::renderer_composite::Vertex(),
                                         shaders::renderer_composite::Fragment())) == rhi::RasterStatus::Ready);
    mask[packages::rg2_masked_depth::UNIFORM_VALUE_MASK_THRESHOLD] = 1;
    fixture.Update(3);
    fixture.Render();
    REQUIRE(fixture.Graph.GetExport(0, texture, aspect) == rhi::RasterStatus::Ready);
    display.Show(fixture.Surface, texture, depthOnly);
    CHECK(depthOnly[kept] == 255);
}
TEST_CASE("RG2 masked techniques keep rejecting opaque and transparent items", "[render-graph][gpu]")
{
    GraphGpuFixture fixture;
    StaticArray<float32, MASK_VALUE_COUNT> mask{};
    fixture.Items[0].Uniforms = {mask.GetData(), mask.GetSize()};
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     packages::rg2_masked_depth::MakePlan(),
                                     packages::rg2_masked_depth::MakePrograms(
                                         shaders::renderer_composite::Vertex(),
                                         shaders::renderer_composite::Fragment())) == rhi::RasterStatus::Ready);
    for (const renderer::RenderGraphMaterial material :
         {renderer::RenderGraphMaterial::Opaque, renderer::RenderGraphMaterial::Transparent})
    {
        fixture.Items[0].Coverage = material;
        renderer::RenderGraphReport report;
        CHECK(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                   {fixture.Items.GetData(), fixture.Items.GetSize()},
                                   {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                   1,
                                   renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                   report) == rhi::RasterStatus::Unsupported);
        CHECK(report.Unsupported == 1);
    }
}

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

TEST_CASE("independent entry points render different colors in one graph", "[render-graph][gpu][independent]")
{
    GraphGpuFixture fixture;
    core::UniquePtr<renderer::RenderGraphPackage> package;
    renderer::RenderGraphPlan plan = packages::rg2_independent::MakePlan();
    renderer::RenderGraphPrograms programs =
        packages::rg2_independent::MakePrograms(shaders::renderer_composite::Vertex(),
                                                shaders::renderer_composite::Fragment());
    SECTION("compiled header") {}
    SECTION("runtime package")
    {
        constexpr PackageLocation LOCATION{LUDUS_RG3_INDEPENDENT_DIRECTORY, "rg2_independent.lurgrt"};
        package = LoadPackage(LOCATION);
        plan = package->GetPlan();
        programs = package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    }
    rhi::RasterStatus initialized = fixture.Graph.Initialize(fixture.Device, plan, programs);
    for (usize attempt = 0; attempt < 10000 && initialized == rhi::RasterStatus::Pending; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        initialized = fixture.Graph.Poll();
    }
    REQUIRE(initialized == rhi::RasterStatus::Ready);
    fixture.Update(1);
    fixture.Render();
    constexpr usize CHANNEL_COUNT = 4;
    constexpr usize SAMPLE_OFFSET_PIXEL = WIDTH / 16;
    constexpr usize LEFT = (HEIGHT / 2 * WIDTH + WIDTH / 2 - SAMPLE_OFFSET_PIXEL) * CHANNEL_COUNT;
    constexpr usize RIGHT = (HEIGHT / 2 * WIDTH + WIDTH / 2 + SAMPLE_OFFSET_PIXEL) * CHANNEL_COUNT;
    CHECK(fixture.Pixels[LEFT] > 0);
    CHECK(fixture.Pixels[LEFT + 1] == 0);
    CHECK(fixture.Pixels[RIGHT] == 0);
    CHECK(fixture.Pixels[RIGHT + 1] > 0);
}

TEST_CASE("default clustered deferred matches forward material shading and preserves optional prepass pixels",
          "[render-graph][clustered-deferred][gpu]")
{
    constexpr usize LIGHT_COUNT = 40;
    constexpr usize LIGHTING_CASE_COUNT = 4;
    constexpr usize PROFILE_COUNT = 3;
    StaticArray<StaticArray<uint8, PIXEL_BYTES>, LIGHTING_CASE_COUNT> forwardPixels{};
    StaticArray<StaticArray<uint8, PIXEL_BYTES>, LIGHTING_CASE_COUNT> forwardDepthPixels{};
    constexpr usize POLL_ATTEMPTS = 10000;
    StaticArray<StaticArray<uint8, PIXEL_BYTES>, LIGHTING_CASE_COUNT> ordinaryPixels{};
    for (usize profile = 0; profile < PROFILE_COUNT; ++profile)
    {
        const bool shouldUsePrepass = profile == 2;
        GraphGpuFixture fixture;
        const PackageLocation location =
            profile == 0
                ? PackageLocation{LUDUS_RG3_CLUSTERED_FORWARD_DIRECTORY, "rg2_clustered_forward_control.lurgrt"}
            : shouldUsePrepass == true
                ? PackageLocation{LUDUS_RG3_CLUSTERED_ZPREPASS_DIRECTORY, "rg2_clustered_ZPrepass.lurgrt"}
                : PackageLocation{LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"};
        const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(location);
        const renderer::RenderGraphPrograms programs =
            package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
        REQUIRE(programs.HasClusteredForward == true);
        REQUIRE(programs.HasClusteredDeferred == (profile != 0));
        CHECK(package->GetPlan().Passes.GetSize() == (shouldUsePrepass == true ? 2 : 1));
        if (shouldUsePrepass == true)
        {
            CHECK(package->GetPlan().Passes[0].AllowsRepeatedCoverage == 1);
            CHECK(package->GetPlan().Passes[1].AllowsRepeatedCoverage == 1);
        }
        const auto poll = [&](rhi::RasterStatus status) {
            for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                status = fixture.Graph.Poll();
            }
            return status;
        };
        REQUIRE(poll(fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
                rhi::RasterStatus::Ready);
        const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
        fixture.Meshes[0].Geometry.Normals = normals.GetData();
        const uint32 materialIndex = 0;
        fixture.Meshes[0].MaterialBindings = {&materialIndex, 1};
        constexpr usize MATERIAL_COUNT = 2;
        StaticArray<renderer::SceneShaderMaterial, MATERIAL_COUNT> materials{};
        materials[1].BaseColor = {.2F, .6F, .8F};
        materials[1].Metallic = .5F;
        materials[1].Roughness = .3F;
        materials[1].Emission = {.1F, .02F, .03F};
        const StaticArray<uint8, TEXEL_BYTES> surfaceMap{{140, 120, 150, 200}};
        materials[1].SurfaceTexture =
            {surfaceMap.GetData(), surfaceMap.GetSize(), surfaceMap.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Unorm};
        const StaticArray<uint8, TEXEL_BYTES> emissionMap{{128, 255, 64, 255}};
        materials[1].EmissionTexture =
            {emissionMap.GetData(), emissionMap.GetSize(), emissionMap.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Unorm};
        StaticArray<renderer::RenderGraphItem, MATERIAL_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
        items[1].SourceId = 2;
        items[1].MeshToWorld.Translation = {.2F, 0, -.1F};
        const uint32 secondMaterial = 1;
        items[1].MaterialOverrides = {&secondMaterial, 1};
        renderer::SceneShaderInputs inputs;
        inputs.Materials = {materials.GetData(), materials.GetSize()};
        inputs.Environment = {.02F, .03F, .01F};
        StaticArray<renderer::DirectLight, LIGHT_COUNT> lights{};
        inputs.Lights = {lights.GetData(), lights.GetSize()};
        uint64 revision = 0;
        uint64 contentRevision = 0;
        const auto update = [&]() {
            renderer::RenderGraphReport report;
            ++revision;
            ++contentRevision;
            return poll(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                             {items.GetData(), items.GetSize()},
                                             {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                             revision,
                                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                             report,
                                             &inputs,
                                             contentRevision));
        };
        const auto updateCamera = [&]() {
            renderer::RenderGraphReport report;
            ++revision;
            return poll(fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                             {items.GetData(), items.GetSize()},
                                             {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                             revision,
                                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                             report,
                                             &inputs,
                                             contentRevision));
        };
        rhi::TextureHandle texture;
        rhi::TextureAspect aspect;
        REQUIRE(fixture.Graph.GetExportAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
        renderer::Renderer compositor;
        REQUIRE(compositor.Initialize(fixture.Device, programs.ColorVertex, programs.ColorFragment) ==
                rhi::RasterStatus::Ready);
        REQUIRE(compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                           shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
        renderer::ViewMapping mapping;
        REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                             {0, 0, WIDTH, HEIGHT},
                                             WIDTH,
                                             HEIGHT,
                                             mapping) == rhi::RasterStatus::Ready);
        DepthDisplay depthDisplay(fixture.Device);
        renderer::Presentation presentation;
        REQUIRE(compositor.PreparePresentation(texture, mapping, presentation) == rhi::RasterStatus::Ready);
        const auto render = [&]() {
            REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
            REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
            REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
            rhi::SubmissionToken completion;
            REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
            const rhi::RasterStatus confirmed = fixture.Graph.ConfirmSubmission(completion);
            REQUIRE((confirmed == rhi::RasterStatus::Ready || confirmed == rhi::RasterStatus::Pending));
            REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
        };
        for (usize lightingCase = 0; lightingCase < LIGHTING_CASE_COUNT; ++lightingCase)
        {
            const bool areDirectional = lightingCase % 2 == 0;
            fixture.Cameras[0].OrthographicCamera = lightingCase >= 2;
            fixture.Cameras[0].WorldToClip.Columns[2].W = lightingCase >= 2 ? 0 : .2F;
            fixture.Cameras[0].WorldToClip.Columns[3].X = .1F;
            for (usize light = 0; light < LIGHT_COUNT; ++light)
            {
                lights[light].SourceId = light + 1;
                lights[light].Color = light + 1 == LIGHT_COUNT ? math::Vector3{0, 0, 1} : math::Vector3{1, 0, 0};
                lights[light].Direction = {0, 0, -1};
                lights[light].Intensity = light + 1 == LIGHT_COUNT ? 0.5F : 0.02F;
                lights[light].Kind =
                    areDirectional == true ? renderer::LightKind::Directional : renderer::LightKind::Point;
                lights[light].Position = {100 + static_cast<float32>(light), 0, 1};
                lights[light].Range = 0.1F;
            }
            lights[LIGHT_COUNT - 1].Position = {0, 0, 1};
            lights[LIGHT_COUNT - 1].Range = 2;
            fixture.Cameras[0].DirectLighting = false;
            REQUIRE(update() == rhi::RasterStatus::Ready);
            const renderer::RenderGraphSceneResourceReport report = fixture.Graph.GetReport().Resources;
            CHECK(report.DroppedLights == 0);
            CHECK(report.OverflowClusters == (areDirectional == true ? 512 : 0));
            render();
            CHECK(fixture.Pixels[GetPixel(WIDTH / 2, HEIGHT / 2) + 2] > 10);
            const StaticArray<uint8, PIXEL_BYTES> clustered = fixture.Pixels;
            StaticArray<uint8, PIXEL_BYTES> depthPixels{};
            depthDisplay.Show(fixture.Surface, texture, depthPixels);
            if (profile == 0)
            {
                forwardDepthPixels[lightingCase] = depthPixels;
            }
            else
            {
                CHECK(depthPixels == forwardDepthPixels[lightingCase]);
            }
            if (profile == 0)
            {
                forwardPixels[lightingCase] = clustered;
            }
            else
            {
                // RGBA16F attributes and depth reconstruction may move a final linear RGBA8 channel by two codes.
                for (usize channel = 0; channel < PIXEL_BYTES; ++channel)
                {
                    CHECK(math::Abs(static_cast<float32>(clustered[channel]) -
                                    static_cast<float32>(forwardPixels[lightingCase][channel])) <= 2);
                }
                const renderer::RenderGraphAllocationReport allocation = fixture.Graph.GetAllocationReport();
                CHECK(allocation.DeferredAllocations == 3);
                CHECK(allocation.DeferredAttachmentBytes == uint64{WIDTH} * HEIGHT * 36);
            }
            if (profile == 1)
            {
                ordinaryPixels[lightingCase] = clustered;
            }
            else if (shouldUsePrepass == true)
            {
                CHECK(clustered == ordinaryPixels[lightingCase]);
            }
            fixture.Cameras[0].DirectLighting = true;
            REQUIRE(updateCamera() == rhi::RasterStatus::Ready);
            render();
            for (usize channel = 0; channel < PIXEL_BYTES; ++channel)
            {
                CHECK(math::Abs(static_cast<float32>(clustered[channel]) -
                                static_cast<float32>(fixture.Pixels[channel])) <= 1);
            }
            // Failed replacement retains the last successfully published scene and pixels.
            const StaticArray<uint8, PIXEL_BYTES> lastDrawable = fixture.Pixels;
            const uint64 lastDrawableRevision = fixture.Graph.GetDrawableSceneRevision();
            if (profile != 0)
            {
                const math::Matrix4 validWorldToClip = fixture.Cameras[0].WorldToClip;
                fixture.Cameras[0].WorldToClip.Columns[0] = {};
                REQUIRE(updateCamera() != rhi::RasterStatus::Ready);
                CHECK(fixture.Graph.GetDrawableSceneRevision() == lastDrawableRevision);
                render();
                CHECK(fixture.Pixels == lastDrawable);
                fixture.Cameras[0].WorldToClip = validWorldToClip;
            }
            lights[LIGHT_COUNT - 1].Color = {0, 1, 0};
            lights[LIGHT_COUNT - 1].SourceId = lights[0].SourceId;
            REQUIRE(update() != rhi::RasterStatus::Ready);
            CHECK(fixture.Graph.GetDrawableSceneRevision() == lastDrawableRevision);
            render();
            CHECK(fixture.Pixels == lastDrawable);
        }
        if (profile == 1)
        {
            lights[LIGHT_COUNT - 1].SourceId = LIGHT_COUNT;
            lights[LIGHT_COUNT - 1].Color = {0, 0, 1};
            REQUIRE(update() == rhi::RasterStatus::Ready);
            render();
            const StaticArray<uint8, PIXEL_BYTES> beforeResize = fixture.Pixels;
            REQUIRE(fixture.Graph.Resize(WIDTH / 2, HEIGHT / 2) == rhi::RasterStatus::Pending);
            render();
            CHECK(fixture.Pixels == beforeResize);
            REQUIRE(update() == rhi::RasterStatus::Ready);
            CHECK(fixture.Graph.HasPendingResize() == false);
            REQUIRE(fixture.Graph.GetExportAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
            REQUIRE(compositor.Release(presentation) == rhi::RasterStatus::Ready);
            REQUIRE(renderer::ResolveViewMapping({WIDTH / 2, HEIGHT / 2, renderer::ScreenFit::Stretch},
                                                 {0, 0, WIDTH, HEIGHT},
                                                 WIDTH,
                                                 HEIGHT,
                                                 mapping) == rhi::RasterStatus::Ready);
            REQUIRE(compositor.PreparePresentation(texture, mapping, presentation) == rhi::RasterStatus::Ready);
            render();
            CHECK(fixture.Pixels[GetPixel(WIDTH / 2, HEIGHT / 2) + 2] > 10);
            CHECK(fixture.Graph.GetAllocationReport().DeferredAttachmentBytes == uint64{WIDTH / 2} * (HEIGHT / 2) * 36);
        }
    }
}

TEST_CASE("forward-lit reuses immutable assets and preserves the drawable scene on rejection",
          "[render-graph][forward-lit][gpu]")
{
    GraphGpuFixture fixture;
    const renderer::RenderGraphPrograms programs =
        packages::rg2_forward_lit::MakePrograms(shaders::renderer_composite::Vertex(),
                                                shaders::renderer_composite::Fragment());
    const renderer::RenderGraphPlan plan = packages::rg2_forward_lit::MakePlan();
    rhi::RasterStatus initialized = fixture.Graph.Initialize(fixture.Device, plan, programs, nullptr, WIDTH, HEIGHT);
    for (usize attempt = 0; attempt < 10000 && initialized == rhi::RasterStatus::Pending; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        initialized = fixture.Graph.Poll();
    }
    REQUIRE(initialized == rhi::RasterStatus::Ready);
    const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    fixture.Meshes[0].Geometry.Normals = normals.GetData();
    const uint32 materialIndex = 0;
    fixture.Meshes[0].MaterialBindings = {&materialIndex, 1};
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 0, 0};
    const StaticArray<uint8, TEXEL_BYTES> white{{255, 255, 255, 255}};
    material.ColorTexture = {white.GetData(), white.GetSize(), white.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Unorm};
    renderer::SceneShaderInputs inputs;
    inputs.Materials = {&material, 1};
    renderer::RenderGraphReport report;
    const auto update = [&](uint64 revision) {
        rhi::RasterStatus status = fixture.Graph.Update({fixture.Meshes.GetData(), fixture.Meshes.GetSize()},
                                                        {fixture.Items.GetData(), fixture.Items.GetSize()},
                                                        {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                                        revision,
                                                        renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                                        report,
                                                        &inputs,
                                                        revision);
        for (usize attempt = 0; attempt < 10000 && status == rhi::RasterStatus::Pending; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            status = fixture.Graph.Poll();
        }
        return status;
    };
    REQUIRE(update(1) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Resources.CreatedMeshes == 1);
    CHECK(fixture.Graph.GetReport().Resources.CreatedTextures == 1);
    rhi::TextureHandle texture;
    rhi::TextureAspect aspect;
    REQUIRE(fixture.Graph.GetExportAllocation(0, texture, aspect) == rhi::RasterStatus::Ready);
    renderer::Renderer compositor;
    REQUIRE(compositor.Initialize(fixture.Device, programs.ColorVertex, programs.ColorFragment) ==
            rhi::RasterStatus::Ready);
    REQUIRE(compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                       shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
    renderer::ViewMapping mapping;
    REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                         {0, 0, WIDTH, HEIGHT},
                                         WIDTH,
                                         HEIGHT,
                                         mapping) == rhi::RasterStatus::Ready);
    renderer::Presentation presentation;
    REQUIRE(compositor.PreparePresentation(texture, mapping, presentation) == rhi::RasterStatus::Ready);
    const auto render = [&]() {
        REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
        REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
        REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
        rhi::SubmissionToken completion;
        REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
        const rhi::RasterStatus confirmed = fixture.Graph.ConfirmSubmission(completion);
        REQUIRE((confirmed == rhi::RasterStatus::Ready || confirmed == rhi::RasterStatus::Pending));
        REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    };
    render();
    const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
    CHECK(fixture.Pixels[center] > 200);
    CHECK(fixture.Pixels[center + 1] < 10);
    material.Emission = {0, 1, 0};
    REQUIRE(update(2) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetReport().Resources.CreatedMeshes == 1);
    CHECK(fixture.Graph.GetReport().Resources.CreatedTextures == 1);
    CHECK(fixture.Graph.GetReport().Resources.ReusedMeshes == 1);
    CHECK(fixture.Graph.GetReport().Resources.ReusedTextures == 1);
    render();
    CHECK(fixture.Pixels[center] < 10);
    CHECK(fixture.Pixels[center + 1] > 200);
    fixture.Meshes[0].Geometry.Normals = nullptr;
    CHECK(update(3) != rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetDrawableSceneRevision() == 2);
    render();
    CHECK(fixture.Pixels[center + 1] > 200);
    fixture.Meshes[0].Geometry.Normals = normals.GetData();
    material.Emission = {};
    renderer::DirectLight light;
    light.SourceId = 1;
    light.Color = {0, 0, 1};
    light.Direction = {0, 0, -1};
    light.Intensity = 1;
    inputs.Lights = {&light, 1};
    REQUIRE(update(4) == rhi::RasterStatus::Ready);
    render();
    CHECK(fixture.Pixels[center] < 10);
    CHECK(fixture.Pixels[center + 2] > 10);
    CHECK(fixture.Graph.GetReport().Resources.CreatedMeshes == 1);
    CHECK(fixture.Graph.GetReport().Resources.CreatedTextures == 1);
    const core::UniquePtr<renderer::RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_FORWARD_DIRECTORY, "rg2_forward_lit.lurgrt"});
    const renderer::RenderGraphPrograms decoded =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    CHECK(decoded.ForwardVertex.Inputs.size() == format::FORWARD_INPUT_COUNT);
    CHECK(decoded.ForwardFragment.Bindings.size() == programs.ForwardFragment.Bindings.size());
}

TEST_CASE("bilinear GPU sampling matches pixel centers and clamps source edges", "[render-graph][gpu][bilinear]")
{
    GraphGpuFixture fixture;
    constexpr usize SOURCE_BYTES = 16;
    constexpr usize CHANNEL_COUNT = 4;
    const StaticArray<uint8, SOURCE_BYTES> pixels{{255, 0, 0, 0, 0, 255, 0, 64, 0, 0, 255, 128, 255, 255, 255, 255}};
    rhi::TextureDescription sourceDescription;
    sourceDescription.Width = 2;
    sourceDescription.Height = 2;
    rhi::TextureHandle source;
    REQUIRE(rhi::CreateTexture(fixture.Device, sourceDescription, {pixels.AsSpan(), 2 * CHANNEL_COUNT}, source) ==
            rhi::RasterStatus::Ready);
    renderer::internal::RenderGraphUpscaleProgram program;
    REQUIRE(program.Initialize(fixture.Device,
                               shaders::renderer_composite::Vertex(),
                               shaders::renderer_composite::Fragment(),
                               true) == rhi::RasterStatus::Ready);
    renderer::internal::RenderGraphUpscalePass upscale;
    REQUIRE(upscale.Initialize(program, source) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::Destroy(fixture.Device, source) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(upscale.Draw({}) == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    // Independent CPU reference: pixel centers map to source texel coordinates u*2-0.5,
    // with edge clamping. The four source colors make red/green/blue orientation observable.
    for (uint32 y = 0; y < HEIGHT; ++y)
    {
        const float32 positionY = (static_cast<float32>(y) + .5F) * 2 / static_cast<float32>(HEIGHT) - .5F;
        const float32 weightY = positionY < 0 ? 0 : positionY > 1 ? 1 : positionY;
        for (uint32 x = 0; x < WIDTH; ++x)
        {
            const float32 positionX = (static_cast<float32>(x) + .5F) * 2 / static_cast<float32>(WIDTH) - .5F;
            const float32 weightX = positionX < 0 ? 0 : positionX > 1 ? 1 : positionX;
            for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
            {
                const float32 top = static_cast<float32>(pixels[channel]) * (1 - weightX) +
                                    static_cast<float32>(pixels[CHANNEL_COUNT + channel]) * weightX;
                const float32 bottom = static_cast<float32>(pixels[2 * CHANNEL_COUNT + channel]) * (1 - weightX) +
                                       static_cast<float32>(pixels[3 * CHANNEL_COUNT + channel]) * weightX;
                const int32 expected = static_cast<int32>(top * (1 - weightY) + bottom * weightY + .5F);
                const int32 actual = fixture.Pixels[(static_cast<usize>(y) * WIDTH + x) * CHANNEL_COUNT + channel];
                REQUIRE(actual >= expected - 1);
                REQUIRE(actual <= expected + 1);
            }
        }
    }
}

TEST_CASE("spatial graph executes rendered scene color and responds to camera edits", "[render-graph][gpu][bilinear]")
{
    const format::Technique technique = GENERATE(format::Technique::BilinearUpscale,
                                                 format::Technique::MetalFxSpatialUpscale,
                                                 format::Technique::MetalFxSpatialPerceptualUpscale);
    GraphGpuFixture fixture;
    if (technique != format::Technique::BilinearUpscale)
    {
        rhi::SpatialUpscaleCapabilities capabilities;
        REQUIRE(rhi::GetSpatialUpscaleCapabilities(fixture.Device, capabilities) == rhi::RasterStatus::Ready);
        if (capabilities.IsMetalFxSpatialSupported == false)
        {
            SKIP("MetalFX Spatial unsupported on this device");
        }
    }
    constexpr usize UPSCALE_RESOURCE_COUNT = 3;
    constexpr usize UPSCALE_PASS_COUNT = 2;
    const StaticArray<format::Resource, UPSCALE_RESOURCE_COUNT> resources{
        {{WIDTH / 2, HEIGHT / 2, format::Aspect::Color, 0, 0, 0},
         {WIDTH / 2, HEIGHT / 2, format::Aspect::Depth, 0, 0, 0},
         {WIDTH, HEIGHT, format::Aspect::Color, 2, 1, 0}}};
    StaticArray<format::Pass, UPSCALE_PASS_COUNT> passes{{packages::rg2_offscreen::PASSES[0],
                                                          {technique,
                                                           0,
                                                           2,
                                                           format::ABSENT,
                                                           format::Load::Clear,
                                                           format::Store::Store,
                                                           format::Load::Discard,
                                                           format::Store::Discard,
                                                           0}}};
    passes[1].SceneData = 0;
    passes[1].SourceColor = 0;
    const StaticArray<format::Camera, TABLE_COUNT> cameras{{{2}}};
    renderer::RenderGraphPlan plan = packages::rg2_offscreen::MakePlan();
    plan.Description.ResourceCount = UPSCALE_RESOURCE_COUNT;
    plan.Description.PassCount = UPSCALE_PASS_COUNT;
    plan.Resources = {resources.GetData(), resources.GetSize()};
    plan.Passes = {passes.GetData(), passes.GetSize()};
    plan.Cameras = {cameras.GetData(), cameras.GetSize()};
    plan.PassNames = {};
    fixture.Items[0].Material.Color = {.25F, .5F, .75F, 1};
    REQUIRE(fixture.Graph.Initialize(fixture.Device,
                                     plan,
                                     packages::rg2_offscreen::MakePrograms(shaders::renderer_composite::Vertex(),
                                                                           shaders::renderer_composite::Fragment())) ==
            rhi::RasterStatus::Ready);
    fixture.Update(1);
    fixture.Render();
    const StaticArray<uint8, PIXEL_BYTES> before = fixture.Pixels;
    bool hasInterpolatedEdge = false;
    for (usize pixel = 0; pixel < PIXEL_BYTES; pixel += TEXEL_BYTES)
    {
        hasInterpolatedEdge = hasInterpolatedEdge == true || (fixture.Pixels[pixel] > 0 && fixture.Pixels[pixel] < 64);
    }
    CHECK(hasInterpolatedEdge == true);
    fixture.Cameras[0].WorldToClip.Columns[3].X = .2F;
    fixture.Update(2);
    fixture.Render();
    bool hasChanged = false;
    for (usize index = 0; index < PIXEL_BYTES; ++index)
    {
        hasChanged = hasChanged == true || before[index] != fixture.Pixels[index];
    }
    CHECK(hasChanged == true);
}

TEST_CASE("MetalFX preserves constant RGB through its private output and surface copy", "[render-graph][gpu][metalfx]")
{
    const rhi::SpatialUpscaleColorProcessing mode =
        GENERATE(rhi::SpatialUpscaleColorProcessing::Linear, rhi::SpatialUpscaleColorProcessing::Perceptual);
    GraphGpuFixture fixture;
    rhi::SpatialUpscaleCapabilities capabilities;
    REQUIRE(rhi::GetSpatialUpscaleCapabilities(fixture.Device, capabilities) == rhi::RasterStatus::Ready);
    if (capabilities.IsMetalFxSpatialSupported == false)
    {
        SKIP("MetalFX Spatial unsupported on this device");
    }
    constexpr uint32 SOURCE_WIDTH = WIDTH / 2;
    constexpr uint32 SOURCE_HEIGHT = HEIGHT / 2;
    constexpr usize SOURCE_BYTES = static_cast<usize>(SOURCE_WIDTH) * SOURCE_HEIGHT * TEXEL_BYTES;
    StaticArray<uint8, SOURCE_BYTES> pixels{};
    for (usize pixel = 0; pixel < SOURCE_BYTES; pixel += TEXEL_BYTES)
    {
        pixels[pixel] = 64;
        pixels[pixel + 1] = 128;
        pixels[pixel + 2] = 192;
        pixels[pixel + TEXEL_BYTES - 1] = 255;
    }
    rhi::TextureDescription sourceDescription;
    sourceDescription.Width = SOURCE_WIDTH;
    sourceDescription.Height = SOURCE_HEIGHT;
    rhi::TextureHandle source;
    REQUIRE(
        rhi::CreateTexture(fixture.Device, sourceDescription, {pixels.AsSpan(), SOURCE_WIDTH * TEXEL_BYTES}, source) ==
        rhi::RasterStatus::Ready);
    renderer::internal::RenderGraphUpscaleProgram program;
    REQUIRE(program.Initialize(fixture.Device,
                               shaders::renderer_composite::Vertex(),
                               shaders::renderer_composite::Fragment(),
                               true) == rhi::RasterStatus::Ready);
    renderer::internal::RenderGraphMetalFxPass pass;
    REQUIRE(pass.Initialize(fixture.Device, {SOURCE_WIDTH, SOURCE_HEIGHT, WIDTH, HEIGHT, mode}, source, {}, &program) ==
            rhi::RasterStatus::Ready);
    REQUIRE(pass.Poll() == rhi::RasterStatus::Ready);
    REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
    REQUIRE(pass.Draw() == rhi::RasterStatus::Ready);
    rhi::SubmissionToken completion;
    REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
    for (usize pixel = 0; pixel < PIXEL_BYTES; pixel += TEXEL_BYTES)
    {
        for (usize channel = 0; channel < TEXEL_BYTES - 1; ++channel)
        {
            const int32 actual = fixture.Pixels[pixel + channel];
            const int32 expected = pixels[channel];
            CHECK(actual >= expected - 2);
            CHECK(actual <= expected + 2);
        }
    }
    REQUIRE(rhi::Destroy(fixture.Device, source) == rhi::RasterStatus::Ready);
}

TEST_CASE("default deferred mesh area lights illuminate a receiver with CPU checked radiance", "[mesh-area][gpu]")
{
    constexpr usize SCENE_COUNT = 2;
    constexpr usize POLL_ATTEMPTS = 10000;
    StaticArray<uint8, PIXEL_BYTES> ordinary{};
    for (const bool shouldUsePrepass : {false, true})
    {
        GraphGpuFixture fixture;
        const PackageLocation location =
            shouldUsePrepass == true
                ? PackageLocation{LUDUS_RG3_CLUSTERED_ZPREPASS_DIRECTORY, "rg2_clustered_ZPrepass.lurgrt"}
                : PackageLocation{LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"};
        const core::UniquePtr<renderer::RenderGraphPackage> package = LoadPackage(location);
        const renderer::RenderGraphPrograms programs =
            package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
        const auto poll = [&](rhi::RasterStatus status) {
            for (usize attempt = 0; attempt < POLL_ATTEMPTS && status == rhi::RasterStatus::Pending; ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                status = fixture.Graph.Poll();
            }
            return status;
        };
        REQUIRE(poll(fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
                rhi::RasterStatus::Ready);
        // This package exports an offscreen target; explicitly present it before reading the surface.
        rhi::TextureHandle outputTexture;
        rhi::TextureAspect outputAspect;
        REQUIRE(fixture.Graph.GetExportAllocation(0, outputTexture, outputAspect) == rhi::RasterStatus::Ready);
        renderer::Renderer compositor;
        REQUIRE(compositor.Initialize(fixture.Device, programs.ColorVertex, programs.ColorFragment) ==
                rhi::RasterStatus::Ready);
        REQUIRE(compositor.InitializeViews(shaders::renderer_composite::Vertex(),
                                           shaders::renderer_composite::Fragment()) == rhi::RasterStatus::Ready);
        renderer::ViewMapping mapping;
        REQUIRE(renderer::ResolveViewMapping({WIDTH, HEIGHT, renderer::ScreenFit::Stretch},
                                             {0, 0, WIDTH, HEIGHT},
                                             WIDTH,
                                             HEIGHT,
                                             mapping) == rhi::RasterStatus::Ready);
        renderer::Presentation presentation;
        REQUIRE(compositor.PreparePresentation(outputTexture, mapping, presentation) == rhi::RasterStatus::Ready);
        const auto render = [&]() {
            REQUIRE(rhi::BeginFrame(fixture.Device, fixture.Surface) == rhi::DeviceStatus::Ready);
            REQUIRE(fixture.Graph.Encode(fixture.Surface) == rhi::RasterStatus::Ready);
            REQUIRE(compositor.DrawPresentation(presentation) == rhi::RasterStatus::Ready);
            rhi::SubmissionToken completion;
            REQUIRE(rhi::EndFrame(fixture.Device, completion) == rhi::RasterStatus::Ready);
            const rhi::RasterStatus status = fixture.Graph.ConfirmSubmission(completion);
            REQUIRE((status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending));
            REQUIRE(rhi::backend::ReadHeadlessPixels(fixture.Pixels.AsSpan()) == true);
        };
        const StaticArray<math::Vector3, VERTEX_COUNT> normals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
        const StaticArray<uint32, INDEX_COUNT> reversed{{0, 2, 1}};
        StaticArray<renderer::RenderGraphMesh, SCENE_COUNT> meshes{{fixture.Meshes[0], fixture.Meshes[0]}};
        const uint32 receiverMaterial = 0;
        const uint32 emitterMaterial = 1;
        meshes[0].Geometry.Normals = normals.GetData();
        meshes[1].Geometry.Normals = normals.GetData();
        meshes[1].Geometry.Indices = reversed.GetData();
        meshes[0].MaterialBindings = {&receiverMaterial, 1};
        meshes[1].MaterialBindings = {&emitterMaterial, 1};
        StaticArray<renderer::RenderGraphItem, SCENE_COUNT> items{{fixture.Items[0], fixture.Items[0]}};
        items[1].SourceId = 2;
        items[1].MeshIndex = 1;
        items[1].MeshToWorld.Translation = {.8F, 0, .3F};
        StaticArray<renderer::SceneShaderMaterial, SCENE_COUNT> materials{};
        materials[1].BaseColor = {};
        materials[1].Emission = {.7F, .2F, 0};
        renderer::SceneShaderInputs inputs;
        inputs.Materials = {materials.GetData(), materials.GetSize()};
        fixture.Cameras[0].OrthographicCamera = true;
        uint64 revision = 0;
        uint64 contentRevision = 0;
        const auto update = [&](bool hasSceneChanges = true) {
            renderer::RenderGraphReport report;
            ++revision;
            if (hasSceneChanges == true)
            {
                ++contentRevision;
            }
            return poll(fixture.Graph.Update({meshes.GetData(), meshes.GetSize()},
                                             {items.GetData(), items.GetSize()},
                                             {fixture.Cameras.GetData(), fixture.Cameras.GetSize()},
                                             revision,
                                             renderer::RenderGraphCoveragePolicy::RejectIncomplete,
                                             report,
                                             &inputs,
                                             contentRevision));
        };
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Graph.GetReport().Resources.MeshLightTriangles == 1);
        const usize center = GetPixel(WIDTH / 2, HEIGHT / 2);
        CHECK(fixture.Pixels[center] > 0);
        CHECK(fixture.Pixels[center + 1] > 0);
        CHECK(fixture.Pixels[center + 2] == 0);
        Array<math::Vector4> words;
        REQUIRE(
            renderer::internal::AppendRasterMeshLights(meshes[1].Geometry, items[1].MeshToWorld, materials[1], words) ==
            rhi::RasterStatus::Ready);
        const math::Vector3 positionWorld{1.F / WIDTH, -1.F / HEIGHT, .5F};
        const math::Vector3 expected = renderer::internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                                                                   positionWorld,
                                                                                   {0, 0, 1},
                                                                                   {0, 0, 1},
                                                                                   {.5F, .5F, .5F},
                                                                                   0,
                                                                                   1);
        CHECK(math::Abs(float32(fixture.Pixels[center]) - expected.X * 255) <= 2);
        CHECK(math::Abs(float32(fixture.Pixels[center + 1]) - expected.Y * 255) <= 2);
        if (shouldUsePrepass == true)
        {
            CHECK(fixture.Pixels == ordinary);
        }
        else
        {
            ordinary = fixture.Pixels;
        }
        // Exercise the specular table on rough and smooth metals, then change only the camera.
        for (const float32 roughness : {.2F, .8F})
        {
            materials[0].Metallic = 1;
            materials[0].Roughness = roughness;
            REQUIRE(update() == rhi::RasterStatus::Ready);
            render();
            const math::Vector3 metalExpected =
                renderer::internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                                            positionWorld,
                                                            {0, 0, 1},
                                                            {0, 0, 1},
                                                            {.5F, .5F, .5F},
                                                            1,
                                                            roughness);
            CHECK(math::Abs(float32(fixture.Pixels[center]) - metalExpected.X * 255) <= 2);
            CHECK(math::Abs(float32(fixture.Pixels[center + 1]) - metalExpected.Y * 255) <= 2);
        }
        materials[0] = {};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        const renderer::RenderGraphSceneResourceReport beforeCamera = fixture.Graph.GetReport().Resources;
        fixture.Cameras[0].CameraForward = {.3F, 0, -1};
        REQUIRE(update(false) == rhi::RasterStatus::Ready);
        render();
        math::Vector3 viewWorld;
        REQUIRE(math::TryNormalize(-fixture.Cameras[0].CameraForward, viewWorld) == math::MathStatus::Success);
        const math::Vector3 cameraExpected =
            renderer::internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                                        positionWorld,
                                                        {0, 0, 1},
                                                        viewWorld,
                                                        {.5F, .5F, .5F},
                                                        0,
                                                        1);
        CHECK(math::Abs(float32(fixture.Pixels[center]) - cameraExpected.X * 255) <= 2);
        CHECK(fixture.Graph.GetReport().Resources.CreatedMeshes == beforeCamera.CreatedMeshes);
        CHECK(fixture.Graph.GetReport().Resources.MeshLightBytes == beforeCamera.MeshLightBytes);
        fixture.Cameras[0].CameraForward = {0, 0, -1};
        // Emission edits replace the immutable light generation, with no invented analytic source.
        materials[1].Emission = {};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center] == 0);
        CHECK(fixture.Pixels[center + 1] == 0);
        // Back-side emission contributes only when explicitly authored two-sided.
        materials[1].Emission = {.7F, .2F, 0};
        meshes[1].Geometry.Indices = fixture.Indices.GetData();
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center] == 0);
        materials[1].IsEmissionTwoSided = true;
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Pixels[center] > 0);
        // Retain every emitter contribution at the Staircase2 triangle count. Splitting
        // constant radiance across coincident triangles leaves the receiver integral unchanged.
        constexpr usize EMITTER_TRIANGLES = 336;
        Array<uint32> repeatedIndices;
        REQUIRE(repeatedIndices.TryResize(EMITTER_TRIANGLES * INDEX_COUNT) == true);
        for (usize index = 0; index < repeatedIndices.GetSize(); ++index)
        {
            repeatedIndices[index] = fixture.Indices[index % INDEX_COUNT];
        }
        const StaticArray<uint8, PIXEL_BYTES> singleEmitter = fixture.Pixels;
        meshes[1].Geometry.Indices = repeatedIndices.GetData();
        meshes[1].Geometry.IndexCount = repeatedIndices.GetSize();
        materials[1].Emission = materials[1].Emission / static_cast<float32>(EMITTER_TRIANGLES);
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        CHECK(fixture.Graph.GetReport().Resources.MeshLightTriangles == EMITTER_TRIANGLES);
        CHECK(math::Abs(float32(fixture.Pixels[center]) - float32(singleEmitter[center])) <= 2);
        CHECK(math::Abs(float32(fixture.Pixels[center + 1]) - float32(singleEmitter[center + 1])) <= 2);
        meshes[1].Geometry.Indices = fixture.Indices.GetData();
        meshes[1].Geometry.IndexCount = fixture.Indices.GetSize();
        materials[1].Emission = {.7F, .2F, 0};
        REQUIRE(update() == rhi::RasterStatus::Ready);
        render();
        const StaticArray<uint8, PIXEL_BYTES> lastDrawable = fixture.Pixels;
        const uint64 lastRevision = fixture.Graph.GetDrawableSceneRevision();
        // Too many emitter triangles rejects preparation and preserves the drawable generation.
        constexpr usize EXCESS_TRIANGLES = renderer::MAX_RASTER_MESH_LIGHT_TRIANGLES + 1;
        Array<uint32> excessIndices;
        REQUIRE(excessIndices.TryResize(EXCESS_TRIANGLES * INDEX_COUNT) == true);
        for (usize index = 0; index < excessIndices.GetSize(); ++index)
        {
            excessIndices[index] = reversed[index % INDEX_COUNT];
        }
        meshes[1].Geometry.Indices = excessIndices.GetData();
        meshes[1].Geometry.IndexCount = excessIndices.GetSize();
        CHECK(update() == rhi::RasterStatus::CapacityExceeded);
        CHECK(fixture.Graph.GetDrawableSceneRevision() == lastRevision);
        render();
        CHECK(fixture.Pixels == lastDrawable);
        meshes[1].Geometry.Indices = reversed.GetData();
        meshes[1].Geometry.IndexCount = reversed.GetSize();
        renderer::DirectLight proxy;
        proxy.SourceId = 1;
        proxy.Kind = renderer::LightKind::AreaOneSided;
        inputs.Lights = {&proxy, 1};
        CHECK(update() == rhi::RasterStatus::Unsupported);
        CHECK(fixture.Graph.GetDrawableSceneRevision() == lastRevision);
        render();
        CHECK(fixture.Pixels == lastDrawable);
    }
}
