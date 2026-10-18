#include "internal/readback.h"
#include "renderer_composite.h"
#include "rg2_bound.h"
#include "rg2_depth.h"
#include "rg2_depth_display.h"
#include "rg2_masked_color.h"
#include "rg2_masked_depth.h"
#include "rg2_offscreen.h"
#include "rg2_surface.h"

#include <ludus/foundation/base/compiler.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/views.hpp>
#include <ludus/graphics/rhi/device.h>

#include <catch2/catch_test_macros.hpp>

#include <new>
#include <string_view>

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
