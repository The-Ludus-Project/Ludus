#include "internal/lifecycle.h"
#include "reference_raster.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

// R12b raster-stage storage buffers and storage textures on the reference backend:
// stage/format admission, capability gates and the WebGL fallback, view/binding
// contracts, ordered-graph hazards and rejection of undeclared writes.
namespace ludus::graphics::rhi::backend
{
extern foundation::uint32 PendingToken;
extern StartupError ImmediateError;
extern FrameStatus NextFrame;
extern Backend ActiveKind;
} // namespace ludus::graphics::rhi::backend
using namespace ludus::foundation;
using namespace ludus::graphics::rhi;
namespace
{
constexpr uint32 IMAGE_EXTENT = 4;
constexpr usize RGBA8_BYTES = 4;
constexpr usize RGBA16_BYTES = 8;
constexpr usize STORAGE_BYTES = 16;
constexpr uint32 INDEX_COUNT = 3;
constexpr uint32 POSITION_SLOT = 0;
constexpr uint32 COVERAGE_SLOT = 1;
constexpr uint32 IMAGE_SLOT = 2;
constexpr uint32 SCENE_BINDING_COUNT = 3;
constexpr uint32 TARGET_WIDTH = 16;
constexpr uint32 TARGET_HEIGHT = 8;
// Reference startup completion arguments shared with the other reference-backend suites.
constexpr uint32 COMPLETION_EXTENT = 4096;
constexpr uint32 COMPLETION_LIMIT = 16384;

struct StorageSession final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    explicit StorageSession(Backend activeBackend = Backend::WebGPU,
                            const RasterStorageCapabilities* storageOverrideOrNull = nullptr)
    {
        Shutdown();
        reference::Reset();
        if (storageOverrideOrNull != nullptr)
        {
            reference::HasRasterStorageOverride = true;
            reference::RasterStorageOverride = *storageOverrideOrNull;
        }
        backend::ImmediateError = StartupError::None;
        backend::NextFrame = FrameStatus::Ready;
        backend::ActiveKind = activeBackend;
        DeviceDescription description;
        description.Required.Compute = activeBackend != Backend::WebGL2;
        REQUIRE(CreateDevice({}, {}, description, Device, Surface) == DeviceStatus::Pending);
        internal::Complete(backend::PendingToken, StartupError::None, {COMPLETION_EXTENT, COMPLETION_LIMIT});
    }
    ~StorageSession() noexcept
    {
        Shutdown();
    }
};
RasterStatus TryLayout(DeviceHandle device, const RasterBinding& binding)
{
    BindingLayoutHandle layout;
    const RasterStatus status = CreateBindingLayout(device, {&binding, 1}, layout);
    if (status == RasterStatus::Ready)
    {
        REQUIRE(Destroy(device, layout) == RasterStatus::Ready);
    }
    return status;
}
RasterStatus TryStorageTexture(DeviceHandle device, const TextureDescription& description, TextureHandle& outTexture)
{
    const usize pixel = description.Format == RasterFormat::Rgba16Float ? RGBA16_BYTES : RGBA8_BYTES;
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(usize{description.Width} * description.Height * pixel) == true);
    return CreateTexture(device,
                         description,
                         {{bytes.GetData(), bytes.GetSize()}, description.Width * pixel},
                         outTexture);
}
TextureDescription StorageImage(RasterFormat format = RasterFormat::Rgba8Unorm)
{
    return { .Width = IMAGE_EXTENT, .Height = IMAGE_EXTENT, .Format = format, .IsStorage = true };
}
// Vertex storage read, fragment storage write and fragment storage texture in one layout.
struct StorageScene final
{
    BufferHandle Positions;
    BufferHandle Coverage;
    BufferHandle Indices;
    TextureHandle Image;
    TextureViewHandle StorageView;
    TextureViewHandle SampledView;
    TextureHandle Target;
    BindingLayoutHandle Layout;
    BindingSetHandle Set;
    RasterPipelineHandle Pipeline;
    StaticArray<RasterBinding, SCENE_BINDING_COUNT> Bindings{{
        {POSITION_SLOT, RasterBindingKind::StorageRead, RasterVisibility::Vertex, sizeof(uint32)},
        {COVERAGE_SLOT, RasterBindingKind::StorageReadWrite, RasterVisibility::Fragment, sizeof(uint32)},
        {
            .Binding = IMAGE_SLOT,
            .Kind = RasterBindingKind::StorageTexture2D,
            .Visibility = RasterVisibility::Fragment,
        },
    }};
    explicit StorageScene(DeviceHandle device)
    {
        const uint8 storage[STORAGE_BYTES]{};
        const uint16 indices[INDEX_COUNT + 1]{0, 1, 2, 0};
        REQUIRE(CreateBuffer(device, {BufferRole::Storage, STORAGE_BYTES}, storage, Positions) == RasterStatus::Ready);
        REQUIRE(CreateBuffer(device, {BufferRole::Storage, STORAGE_BYTES}, storage, Coverage) == RasterStatus::Ready);
        REQUIRE(CreateBuffer(device,
                             {BufferRole::Index16, sizeof(indices)},
                             {reinterpret_cast<const uint8*>(indices), sizeof(indices)},
                             Indices) == RasterStatus::Ready);
        REQUIRE(TryStorageTexture(device, StorageImage(), Image) == RasterStatus::Ready);
        REQUIRE(CreateTextureView(device, Image, { .IsStorage = true }, StorageView) == RasterStatus::Ready);
        REQUIRE(CreateTextureView(device, Image, SampledView) == RasterStatus::Ready);
        REQUIRE(
            CreateTexture(device, { .Width = TARGET_WIDTH, .Height = TARGET_HEIGHT, .Attachment = true }, {}, Target) ==
            RasterStatus::Ready);
        RasterShaderDescription vertex;
        vertex.Artifact.Stage = ShaderStage::Vertex;
        vertex.Artifact.Wgsl = "fixture vertex";
        vertex.Artifact.WgslEntry = "vertexMain";
        vertex.Bindings = {Bindings.GetData(), 1};
        RasterShaderDescription fragment = vertex;
        fragment.Artifact.Stage = ShaderStage::Fragment;
        fragment.Artifact.WgslEntry = "fragmentMain";
        fragment.Bindings = {Bindings.GetData() + 1, 2};
        RasterShaderHandle vertexShader;
        RasterShaderHandle fragmentShader;
        REQUIRE(CreateRasterShader(device, vertex, vertexShader) == RasterStatus::Ready);
        REQUIRE(CreateRasterShader(device, fragment, fragmentShader) == RasterStatus::Ready);
        REQUIRE(CreateBindingLayout(device, {Bindings.GetData(), Bindings.GetSize()}, Layout) == RasterStatus::Ready);
        const StaticArray<RasterBindingResource, SCENE_BINDING_COUNT> resources{{
            { .Binding = POSITION_SLOT, .Buffer = Positions, .Size = STORAGE_BYTES },
            { .Binding = COVERAGE_SLOT, .Buffer = Coverage, .Size = STORAGE_BYTES },
            { .Binding = IMAGE_SLOT, .Texture = StorageView },
        }};
        REQUIRE(CreateBindingSet(device, Layout, {resources.GetData(), resources.GetSize()}, Set) ==
                RasterStatus::Ready);
        REQUIRE(CreateRasterPipeline(device,
                                     {
                                         .Vertex = vertexShader,
                                         .Fragment = fragmentShader,
                                         .Layout = Layout,
                                         .Streams = {},
                                         .Attributes = {},
                                         .Target = RasterTarget::Rgba8Unorm,
                                     },
                                     Pipeline) == RasterStatus::Ready);
        REQUIRE(Destroy(device, vertexShader) == RasterStatus::Ready);
        REQUIRE(Destroy(device, fragmentShader) == RasterStatus::Ready);
    }
    [[nodiscard]] RasterDraw Draw() const noexcept
    {
        return
        {
            .Pipeline = Pipeline,
            .Bindings = Set,
            .Vertices = {},
            .Indices = Indices,
            .IndexCount = INDEX_COUNT,
            .VertexCount = INDEX_COUNT,
            .InstanceCount = 1,
        };
    }
};
// Compute pass reading the coverage buffer and sampling the fragment-written image.
struct StorageReader final
{
    ComputeDispatch Dispatch;
    StorageReader(DeviceHandle device, const StorageScene& scene)
    {
        const StaticArray<RasterBinding, 2> bindings{{
            {0, RasterBindingKind::StorageRead, RasterVisibility::Compute, sizeof(uint32)},
            { .Binding = 1, .Kind = RasterBindingKind::Texture2D, .Visibility = RasterVisibility::Compute },
        }};
        RasterShaderDescription description;
        description.Artifact.Stage = ShaderStage::Compute;
        description.Artifact.Wgsl = "fixture compute";
        description.Artifact.WgslEntry = "computeMain";
        description.Bindings = {bindings.GetData(), bindings.GetSize()};
        RasterShaderHandle shader;
        BindingLayoutHandle layout;
        REQUIRE(CreateRasterShader(device, description, shader) == RasterStatus::Ready);
        REQUIRE(CreateBindingLayout(device, description.Bindings, layout) == RasterStatus::Ready);
        const StaticArray<RasterBindingResource, 2> resources{{
            { .Binding = 0, .Buffer = scene.Coverage, .Size = STORAGE_BYTES },
            { .Binding = 1, .Texture = scene.SampledView },
        }};
        REQUIRE(CreateBindingSet(device, layout, {resources.GetData(), resources.GetSize()}, Dispatch.Bindings) ==
                RasterStatus::Ready);
        REQUIRE(CreateComputePipeline(device, {shader, layout}, Dispatch.Pipeline) == RasterStatus::Ready);
    }
};
struct StorageGraph final
{
    OrderedGraph Graph;
    GraphVersion Positions;
    GraphVersion Coverage;
    GraphVersion Indices;
    GraphVersion Image;
    GraphVersion Target;
    StorageGraph(DeviceHandle device, const StorageScene& scene)
    {
        REQUIRE(CreateOrderedGraph(device, Graph) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(device, Graph, scene.Positions, Positions) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(device, Graph, scene.Coverage, Coverage) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(device, Graph, scene.Indices, Indices) == RasterStatus::Ready);
        RasterTextureState state;
        REQUIRE(GetTextureState(device, scene.Image, state) == RasterStatus::Ready);
        REQUIRE(ImportGraphTexture(device, Graph, scene.Image, state, RasterTextureUse::SampledBoth, Image) ==
                RasterStatus::Ready);
        REQUIRE(GetTextureState(device, scene.Target, state) == RasterStatus::Ready);
        REQUIRE(ImportGraphTexture(device, Graph, scene.Target, state, RasterTextureUse::ColorAttachment, Target) ==
                RasterStatus::Ready);
    }
    [[nodiscard]] GraphVersion Next(DeviceHandle device, GraphVersion& inoutCurrent) const
    {
        GraphVersion next;
        REQUIRE(NextGraphVersion(device, Graph, inoutCurrent, next) == RasterStatus::Ready);
        inoutCurrent = next;
        return next;
    }
};
constexpr usize WRITE_USE_COUNT = 5;
StaticArray<GraphUse, WRITE_USE_COUNT> WriteUses(DeviceHandle device, StorageGraph& inoutGraph)
{
    return {{
        {inoutGraph.Positions, GraphAccessMode::StorageRead, 0, STORAGE_BYTES, RasterVisibility::Vertex},
        {inoutGraph.Next(device, inoutGraph.Coverage),
         GraphAccessMode::StorageReadWrite,
         0,
         STORAGE_BYTES,
         RasterVisibility::Fragment},
        {
            .Resource = inoutGraph.Next(device, inoutGraph.Image),
            .Access = GraphAccessMode::StorageTextureWrite,
            .Visibility = RasterVisibility::Fragment,
        },
        {inoutGraph.Indices, GraphAccessMode::Index, 0, INDEX_COUNT * sizeof(uint16)},
        { .Resource = inoutGraph.Next(device, inoutGraph.Target), .Access = GraphAccessMode::ColorWrite },
    }};
}
RasterStatus AddWritePass(DeviceHandle device,
                          const StorageGraph& graph,
                          const StorageScene& scene,
                          const StaticArray<GraphUse, WRITE_USE_COUNT>& uses,
                          const RasterDraw& draw)
{
    return AddGraphPass(device,
                        graph.Graph,
                        {
                            .Name = "fragment storage",
                            .Attachment = { .Color = scene.Target, .ColorLoad = RasterLoad::Clear },
                            .Uses = uses.GetData(),
                            .UseCount = uses.GetSize(),
                            .Draws = &draw,
                            .DrawCount = 1,
                        });
}
} // namespace

TEST_CASE("R12b layouts admit raster storage by stage, access and format", "[rhi][raster-storage]")
{
    StorageSession session;
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(session.Device, info) == DeviceStatus::Ready);
    CHECK(info.Profile.RasterStorage.HasFragmentStorageWrites);
    CHECK(info.Profile.Formats[static_cast<usize>(RasterFormat::Rgba8Unorm)].Uses.Storage);
    CHECK(info.Profile.Formats[static_cast<usize>(RasterFormat::Rgba16Float)].Uses.Storage);
    CHECK_FALSE(info.Profile.Formats[static_cast<usize>(RasterFormat::Rgba8Srgb)].Uses.Storage);
    CHECK_FALSE(info.Profile.Formats[static_cast<usize>(RasterFormat::Bc1RgbaUnorm)].Uses.Storage);

    RasterBinding read{0, RasterBindingKind::StorageRead, RasterVisibility::Vertex, sizeof(uint32)};
    for (const RasterVisibility visibility :
         {RasterVisibility::Vertex, RasterVisibility::Fragment, RasterVisibility::Both, RasterVisibility::Compute})
    {
        read.Visibility = visibility;
        CHECK(TryLayout(session.Device, read) == RasterStatus::Ready);
    }
    RasterBinding write = read;
    write.Kind = RasterBindingKind::StorageReadWrite;
    for (const RasterVisibility visibility : {RasterVisibility::Fragment, RasterVisibility::Compute})
    {
        write.Visibility = visibility;
        CHECK(TryLayout(session.Device, write) == RasterStatus::Ready);
    }
    for (const RasterVisibility visibility : {RasterVisibility::Vertex, RasterVisibility::Both})
    {
        write.Visibility = visibility;
        CHECK(TryLayout(session.Device, write) == RasterStatus::InvalidDescription);
    }

    RasterBinding image{ .Binding = 0, .Kind = RasterBindingKind::StorageTexture2D };
    for (const RasterVisibility visibility : {RasterVisibility::Fragment, RasterVisibility::Compute})
    {
        image.Visibility = visibility;
        CHECK(TryLayout(session.Device, image) == RasterStatus::Ready);
    }
    image.StorageFormat = RasterFormat::Rgba16Float;
    CHECK(TryLayout(session.Device, image) == RasterStatus::Ready);
    SECTION("vertex and shared raster visibility exclude storage textures")
    {
        image.Visibility = GENERATE(RasterVisibility::Vertex, RasterVisibility::Both);
        CHECK(TryLayout(session.Device, image) == RasterStatus::InvalidDescription);
    }
    SECTION("formats outside the portable pair reject")
    {
        image.StorageFormat = GENERATE(RasterFormat::Rgba8Srgb, RasterFormat::R8Unorm, RasterFormat::Bc5RgUnorm);
        CHECK(TryLayout(session.Device, image) == RasterStatus::InvalidDescription);
    }
    SECTION("storage textures are scalar, fixed and non-dynamic")
    {
        image.Count = 2;
        CHECK(TryLayout(session.Device, image) == RasterStatus::InvalidDescription);
        image.Count = 1;
        image.HasDynamicOffset = true;
        CHECK(TryLayout(session.Device, image) == RasterStatus::InvalidDescription);
        image.HasDynamicOffset = false;
        image.MinSize = sizeof(uint32);
        CHECK(TryLayout(session.Device, image) == RasterStatus::InvalidDescription);
    }
}

TEST_CASE("R12b capability gates and the WebGL fallback reject before native creation", "[rhi][raster-storage]")
{
    SECTION("each negotiated per-stage limit is independent")
    {
        StorageSession session;
        reference::HasRasterStorageOverride = true;
        reference::RasterStorageOverride =
        {
            .MaxVertexStorageBuffers = 1,
            .MaxFragmentStorageBuffers = 1,
            .HasFragmentStorageWrites = false,
            .MaxFragmentStorageTextures = 1,
            .MaxComputeStorageTextures = 0,
        };
        const usize layouts = reference::Creates[static_cast<usize>(internal::RasterKind::Layout)];
        CHECK(
            TryLayout(session.Device, {0, RasterBindingKind::StorageRead, RasterVisibility::Vertex, sizeof(uint32)}) ==
            RasterStatus::Ready);
        CHECK(TryLayout(session.Device,
                        {0, RasterBindingKind::StorageReadWrite, RasterVisibility::Fragment, sizeof(uint32)}) ==
              RasterStatus::Unsupported);
        CHECK(TryLayout(session.Device,
                        {
                            .Binding = 0,
                            .Kind = RasterBindingKind::StorageTexture2D,
                            .Visibility = RasterVisibility::Compute,
                        }) == RasterStatus::Unsupported);
        const StaticArray<RasterBinding, 2> twoVertexReads{{
            {0, RasterBindingKind::StorageRead, RasterVisibility::Vertex, sizeof(uint32)},
            {1, RasterBindingKind::StorageRead, RasterVisibility::Both, sizeof(uint32)},
        }};
        BindingLayoutHandle layout;
        CHECK(CreateBindingLayout(session.Device, {twoVertexReads.GetData(), twoVertexReads.GetSize()}, layout) ==
              RasterStatus::Unsupported);
        CHECK(reference::Creates[static_cast<usize>(internal::RasterKind::Layout)] == layouts + 1);
    }
    SECTION("storage texture allocations require an enabled storage stage")
    {
        const RasterStorageCapabilities buffersOnly{ .MaxFragmentStorageBuffers = 1 };
        StorageSession session(Backend::WebGPU, &buffersOnly);
        DeviceInfo info;
        REQUIRE(GetDeviceInfo(session.Device, info) == DeviceStatus::Ready);
        CHECK_FALSE(info.Profile.Formats[static_cast<usize>(RasterFormat::Rgba8Unorm)].Uses.Storage);
        TextureHandle texture;
        CHECK(TryStorageTexture(session.Device, StorageImage(), texture) == RasterStatus::Unsupported);
    }
    SECTION("WebGL 2 exposes no raster storage and keeps its authored alternative")
    {
        StorageSession session(Backend::WebGL2);
        DeviceInfo info;
        REQUIRE(GetDeviceInfo(session.Device, info) == DeviceStatus::Ready);
        CHECK(info.Profile.RasterStorage.MaxVertexStorageBuffers == 0);
        CHECK(info.Profile.RasterStorage.MaxFragmentStorageBuffers == 0);
        CHECK_FALSE(info.Profile.RasterStorage.HasFragmentStorageWrites);
        CHECK(info.Profile.RasterStorage.MaxFragmentStorageTextures == 0);
        CHECK_FALSE(info.Profile.Formats[static_cast<usize>(RasterFormat::Rgba8Unorm)].Uses.Storage);
        CHECK(TryLayout(session.Device,
                        {0, RasterBindingKind::StorageRead, RasterVisibility::Fragment, sizeof(uint32)}) ==
              RasterStatus::Unsupported);
        CHECK(TryLayout(session.Device,
                        {
                            .Binding = 0,
                            .Kind = RasterBindingKind::StorageTexture2D,
                            .Visibility = RasterVisibility::Fragment,
                        }) == RasterStatus::Unsupported);
        TextureHandle texture;
        CHECK(TryStorageTexture(session.Device, StorageImage(), texture) == RasterStatus::Unsupported);
        // The sampled alternative of the same allocation remains available.
        TextureDescription sampled = StorageImage();
        sampled.IsStorage = false;
        CHECK(TryStorageTexture(session.Device, sampled, texture) == RasterStatus::Ready);
    }
}

TEST_CASE("R12b storage textures validate allocation, views and binding format", "[rhi][raster-storage]")
{
    StorageSession session;
    TextureHandle texture;
    SECTION("allocations are uploaded single-mip D2 storage formats")
    {
        TextureDescription description = StorageImage(RasterFormat::Rgba8Srgb);
        CHECK(TryStorageTexture(session.Device, description, texture) == RasterStatus::InvalidDescription);
        description = StorageImage();
        description.MipLevels = 2;
        CHECK(TryStorageTexture(session.Device, description, texture) == RasterStatus::InvalidDescription);
        description = StorageImage();
        description.RetainCpuContent = true;
        CHECK(TryStorageTexture(session.Device, description, texture) == RasterStatus::InvalidDescription);
        description = StorageImage();
        description.Attachment = true;
        CHECK(CreateTexture(session.Device, description, {}, texture) == RasterStatus::InvalidDescription);
        description = StorageImage();
        description.Dimension = TextureDimension::D2Array;
        description.DepthOrLayers = 2;
        CHECK(TryStorageTexture(session.Device, description, texture) == RasterStatus::InvalidDescription);
        CHECK(TryStorageTexture(session.Device, StorageImage(RasterFormat::Rgba16Float), texture) ==
              RasterStatus::Ready);
    }
    SECTION("storage views select only storage allocations")
    {
        TextureHandle sampled;
        TextureDescription description = StorageImage();
        description.IsStorage = false;
        REQUIRE(TryStorageTexture(session.Device, description, sampled) == RasterStatus::Ready);
        TextureViewHandle view;
        CHECK(CreateTextureView(session.Device,
                                sampled,
                                {
                                    .IsStorage = true,
                                },
                                view) == RasterStatus::InvalidDescription);
        REQUIRE(TryStorageTexture(session.Device, StorageImage(), texture) == RasterStatus::Ready);
        CHECK(CreateTextureView(session.Device, texture, { .Attachment = true, .IsStorage = true }, view) ==
              RasterStatus::InvalidDescription);
        CHECK(CreateTextureView(session.Device, texture, { .Aspect = TextureAspect::Depth, .IsStorage = true }, view) ==
              RasterStatus::InvalidDescription);
        CHECK(CreateTextureView(session.Device, texture, { .IsStorage = true }, view) == RasterStatus::Ready);
    }
    SECTION("bindings require a storage view of the reflected format")
    {
        REQUIRE(TryStorageTexture(session.Device, StorageImage(RasterFormat::Rgba16Float), texture) ==
                RasterStatus::Ready);
        TextureViewHandle storageView;
        TextureViewHandle sampledView;
        REQUIRE(CreateTextureView(session.Device, texture, { .IsStorage = true }, storageView) == RasterStatus::Ready);
        REQUIRE(CreateTextureView(session.Device, texture, sampledView) == RasterStatus::Ready);
        RasterBinding binding
        {
            .Binding = 0,
            .Kind = RasterBindingKind::StorageTexture2D,
            .Visibility = RasterVisibility::Fragment,
        };
        BindingLayoutHandle rgba8;
        BindingLayoutHandle rgba16;
        REQUIRE(CreateBindingLayout(session.Device, {&binding, 1}, rgba8) == RasterStatus::Ready);
        binding.StorageFormat = RasterFormat::Rgba16Float;
        REQUIRE(CreateBindingLayout(session.Device, {&binding, 1}, rgba16) == RasterStatus::Ready);
        BindingSetHandle set;
        RasterBindingResource resource{ .Binding = 0, .Texture = storageView };
        CHECK(CreateBindingSet(session.Device, rgba8, {&resource, 1}, set) == RasterStatus::InvalidDescription);
        resource.Texture = sampledView;
        CHECK(CreateBindingSet(session.Device, rgba16, {&resource, 1}, set) == RasterStatus::InvalidDescription);
        resource.Texture = storageView;
        REQUIRE(CreateBindingSet(session.Device, rgba16, {&resource, 1}, set) == RasterStatus::Ready);
        const RasterBinding sampledBinding
        {
            .Binding = 0,
            .Kind = RasterBindingKind::Texture2D,
            .Visibility = RasterVisibility::Fragment,
        };
        BindingLayoutHandle sampledLayout;
        REQUIRE(CreateBindingLayout(session.Device, {&sampledBinding, 1}, sampledLayout) == RasterStatus::Ready);
        BindingSetHandle sampledSet;
        CHECK(CreateBindingSet(session.Device, sampledLayout, {&resource, 1}, sampledSet) ==
              RasterStatus::InvalidDescription);
    }
}

TEST_CASE("R12b ordered graphs order raster storage writes before later reads", "[rhi][raster-storage]")
{
    StorageSession session;
    StorageScene scene(session.Device);
    StorageReader reader(session.Device, scene);
    StorageGraph graph(session.Device, scene);
    const RasterDraw draw = scene.Draw();
    const StaticArray<GraphUse, WRITE_USE_COUNT> uses = WriteUses(session.Device, graph);
    REQUIRE(AddWritePass(session.Device, graph, scene, uses, draw) == RasterStatus::Ready);
    const StaticArray<GraphUse, 2> readUses{{
        {graph.Coverage, GraphAccessMode::StorageRead, 0, STORAGE_BYTES, RasterVisibility::Compute},
        { .Resource = graph.Image, .Access = GraphAccessMode::Sampled, .Visibility = RasterVisibility::Compute },
    }};
    REQUIRE(AddGraphPass(session.Device,
                         graph.Graph,
                         {
                             .Name = "compute reads",
                             .Attachment = {},
                             .Uses = readUses.GetData(),
                             .UseCount = readUses.GetSize(),
                             .SideEffect = true,
                             .Dispatch = &reader.Dispatch,
                         }) == RasterStatus::Ready);
    REQUIRE(AddGraphRoot(session.Device, graph.Graph, graph.Target, GraphRoot::Readback) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device,
                                graph.Graph,
                                report,
                                {
                                    .CullPasses = true,
                                    .MergeRasterPasses = true,
                                }) == RasterStatus::Ready);
    CHECK(report.LivePasses == 2);
    bool hasBufferReadAfterWrite = false;
    bool hasTextureReadAfterWrite = false;
    for (usize i = 0; i < report.DependencyCount; ++i)
    {
        const GraphDependency& edge = report.Dependencies[i];
        hasBufferReadAfterWrite |= edge.Before == 0 && edge.After == 1 && edge.Hazard == GraphHazard::ReadAfterWrite &&
                                   edge.Source == GraphAccessMode::StorageReadWrite &&
                                   edge.Destination == GraphAccessMode::StorageRead;
        hasTextureReadAfterWrite |= edge.Before == 0 && edge.After == 1 && edge.Hazard == GraphHazard::ReadAfterWrite &&
                                    edge.Source == GraphAccessMode::StorageTextureWrite &&
                                    edge.Destination == GraphAccessMode::Sampled;
    }
    CHECK(hasBufferReadAfterWrite);
    CHECK(hasTextureReadAfterWrite);

    RasterTextureState before;
    REQUIRE(GetTextureState(session.Device, scene.Image, before) == RasterStatus::Ready);
    SubmissionToken completion;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph.Graph, completion) == RasterStatus::Ready);
    CHECK(reference::Draws == 1);
    CHECK(reference::Dispatches == 1);
    // The final barrier returns the storage texture to its declared sampled use.
    CHECK(reference::LastTextureUse != RasterTextureUse::StorageFragment);
    RasterTextureState after;
    REQUIRE(GetTextureState(session.Device, scene.Image, after) == RasterStatus::Ready);
    CHECK(after.Use == RasterTextureUse::SampledBoth);
    CHECK(after.Revision > before.Revision);
}

TEST_CASE("R12b consecutive storage writes are ordered and never merged", "[rhi][raster-storage]")
{
    StorageSession session;
    StorageScene scene(session.Device);
    StorageGraph graph(session.Device, scene);
    const RasterDraw draw = scene.Draw();
    const StaticArray<GraphUse, WRITE_USE_COUNT> first = WriteUses(session.Device, graph);
    REQUIRE(AddWritePass(session.Device, graph, scene, first, draw) == RasterStatus::Ready);
    const StaticArray<GraphUse, WRITE_USE_COUNT> second = WriteUses(session.Device, graph);
    REQUIRE(AddWritePass(session.Device, graph, scene, second, draw) == RasterStatus::Ready);
    REQUIRE(AddGraphRoot(session.Device, graph.Graph, graph.Image, GraphRoot::Readback) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device,
                                graph.Graph,
                                report,
                                {
                                    .CullPasses = true,
                                    .MergeRasterPasses = true,
                                }) == RasterStatus::Ready);
    CHECK(report.MergedPasses == 0);
    CHECK(report.RasterPasses == 2);
    bool hasTextureWriteAfterWrite = false;
    bool hasBufferWriteAfterWrite = false;
    for (usize i = 0; i < report.DependencyCount; ++i)
    {
        const GraphDependency& edge = report.Dependencies[i];
        hasTextureWriteAfterWrite |= edge.Before == 0 && edge.After == 1 &&
                                     edge.Hazard == GraphHazard::WriteAfterWrite &&
                                     edge.Destination == GraphAccessMode::StorageTextureWrite;
        hasBufferWriteAfterWrite |= edge.Before == 0 && edge.After == 1 &&
                                    edge.Hazard == GraphHazard::WriteAfterWrite &&
                                    edge.Destination == GraphAccessMode::StorageReadWrite;
    }
    CHECK(hasTextureWriteAfterWrite);
    CHECK(hasBufferWriteAfterWrite);
}

TEST_CASE("R12b undeclared or misdeclared raster storage writes reject before effects", "[rhi][raster-storage]")
{
    StorageSession session;
    StorageScene scene(session.Device);
    StorageGraph graph(session.Device, scene);
    const RasterDraw draw = scene.Draw();
    StaticArray<GraphUse, WRITE_USE_COUNT> uses = WriteUses(session.Device, graph);
    SECTION("vertex-visible buffer write")
    {
        uses[1].Visibility = RasterVisibility::Vertex;
    }
    SECTION("buffer write declared as a read")
    {
        uses[1].Access = GraphAccessMode::StorageRead;
    }
    SECTION("storage texture declared as sampled")
    {
        uses[2].Access = GraphAccessMode::Sampled;
    }
    SECTION("storage texture write with compute visibility in a raster pass")
    {
        uses[2].Visibility = RasterVisibility::Compute;
    }
    SECTION("storage texture write with a byte range")
    {
        uses[2].Size = STORAGE_BYTES;
    }
    SECTION("storage texture write that is not the next version")
    {
        static_cast<void>(graph.Next(session.Device, graph.Image));
        uses[2].Resource = graph.Image;
    }
    REQUIRE(AddWritePass(session.Device, graph, scene, uses, draw) == RasterStatus::Ready);
    REQUIRE(AddGraphRoot(session.Device, graph.Graph, graph.Target, GraphRoot::Readback) == RasterStatus::Ready);
    GraphReport report;
    CHECK(CompileOrderedGraph(session.Device, graph.Graph, report) == RasterStatus::InvalidDescription);
    CHECK(report.ErrorPass == 0);
    CHECK(reference::Draws == 0);
    CHECK(reference::Submitted == 0);
}

TEST_CASE("R12b direct and attachment uses cannot write raster storage", "[rhi][raster-storage]")
{
    StorageSession session;
    StorageScene scene(session.Device);
    SECTION("direct draws have no declared versions")
    {
        REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
        REQUIRE(BeginRasterPass(session.Device, { .Color = scene.Target }) == RasterStatus::Ready);
        CHECK(DrawIndexed(session.Device, scene.Draw()) == RasterStatus::InvalidDescription);
        CHECK(reference::Draws == 0);
        REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    }
    SECTION("an attachment cannot also be written as storage")
    {
        OrderedGraph graph;
        REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
        RasterTextureState state;
        REQUIRE(GetTextureState(session.Device, scene.Target, state) == RasterStatus::Ready);
        GraphVersion target;
        GraphVersion next;
        REQUIRE(
            ImportGraphTexture(session.Device, graph, scene.Target, state, RasterTextureUse::ColorAttachment, target) ==
            RasterStatus::Ready);
        REQUIRE(NextGraphVersion(session.Device, graph, target, next) == RasterStatus::Ready);
        const GraphUse use
        {
            .Resource = next,
            .Access = GraphAccessMode::StorageTextureWrite,
            .Visibility = RasterVisibility::Fragment,
        };
        REQUIRE(AddGraphPass(session.Device,
                             graph,
                             {
                                 .Name = "attachment storage",
                                 .Attachment = { .Color = scene.Target },
                                 .Uses = &use,
                                 .UseCount = 1,
                             }) == RasterStatus::Ready);
        REQUIRE(AddGraphRoot(session.Device, graph, next, GraphRoot::Readback) == RasterStatus::Ready);
        GraphReport report;
        CHECK(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::InvalidDescription);
    }
}
