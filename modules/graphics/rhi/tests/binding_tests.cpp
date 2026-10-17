#include "internal/lifecycle.h"
#include "reference_raster.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/measurement.h>

#include <catch2/catch_test_macros.hpp>

namespace ludus::graphics::rhi::backend
{
extern foundation::uint32 PendingToken;
extern StartupError ImmediateError;
extern FrameStatus NextFrame;
extern Backend ActiveKind;
extern bool SelectionSupported;
} // namespace ludus::graphics::rhi::backend
using namespace ludus::foundation;
using namespace ludus::graphics::rhi;
namespace
{
constexpr usize UNIFORM_BYTES = 512;
constexpr usize TEXTURE_COUNT = 4;
constexpr usize SPIRV_HEADER_WORDS = 5;
// Registry-only fixture; the reference backend owns execution, not this header.
constexpr StaticArray<uint32, SPIRV_HEADER_WORDS> REFERENCE_SPIRV{0x07230203U, 0x00010300U, 0, 1, 0};
struct BindingSession final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    explicit BindingSession(uint32 descriptorLimit = 512, Backend backendKind = Backend::Metal)
    {
        Shutdown();
        reference::Reset();
        backend::ImmediateError = StartupError::None;
        backend::NextFrame = FrameStatus::Ready;
        backend::ActiveKind = backendKind;
        backend::SelectionSupported = true;
        DeviceDescription description
        {
            .Limits = {},
            .Required = { .PortableRaster = true },
            .Preferred = { .BindlessTextureIndexing = true, .HasRuntimeTextureArrays = true },
            .Storage = { .BindingDescriptors = descriptorLimit },
            .RequiredProfile = {},
            .PreferredProfile = {},
        };
        REQUIRE(CreateDevice({}, {}, description, Device, Surface) == DeviceStatus::Pending);
        internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
    }
    ~BindingSession() noexcept
    {
        Shutdown();
    }
};
struct BindingScene final
{
    BufferHandle Uniform;
    BufferHandle Vertices;
    BufferHandle Indices;
    RasterShaderHandle Vertex;
    RasterShaderHandle Fragment;
    BindingLayoutHandle Layout;
    BindingSetHandle Set;
    RasterPipelineHandle Pipeline;
    RasterVertexSlice Slice;
    explicit BindingScene(DeviceHandle device)
    {
        const uint8 bytes[UNIFORM_BYTES]{};
        REQUIRE(CreateBuffer(device, { .Role = BufferRole::Uniform, .Size = sizeof(bytes) }, bytes, Uniform) ==
                RasterStatus::Ready);
        const RasterBinding binding
        {
            .Kind = RasterBindingKind::UniformBuffer,
            .Visibility = RasterVisibility::Both,
            .MinSize = 16,
            .HasDynamicOffset = true,
        };
        const RasterShaderInput input{ .Format = RasterVertexFormat::Float3 };
        RasterShaderDescription vertex
        {
            .Artifact =
            {
                .Stage = ShaderStage::Vertex,
                .Spirv = {REFERENCE_SPIRV.GetData(), SPIRV_HEADER_WORDS},
                .Wgsl = "fixture",
                .SpirvEntry = "VertexMain",
                .WgslEntry = "VertexMain",
                .Msl = "fixture",
                .MslEntry = "VertexMain",
            },
            .Inputs = {&input, 1},
            .Bindings = {&binding, 1},
        };
        REQUIRE(CreateRasterShader(device, vertex, Vertex) == RasterStatus::Ready);
        const RasterShaderDescription fragment
        {
            .Artifact =
            {
                .Stage = ShaderStage::Fragment,
                .Spirv = {REFERENCE_SPIRV.GetData(), SPIRV_HEADER_WORDS},
                .Wgsl = "fixture",
                .SpirvEntry = "FragmentMain",
                .WgslEntry = "FragmentMain",
                .Msl = "fixture",
                .MslEntry = "FragmentMain",
            },
            .Inputs = {},
            .Bindings = {&binding, 1},
        };
        REQUIRE(CreateRasterShader(device, fragment, Fragment) == RasterStatus::Ready);
        REQUIRE(CreateBindingLayout(device, {&binding, 1}, Layout) == RasterStatus::Ready);
        const RasterBindingResource resource{ .Buffer = Uniform, .Size = 16 };
        REQUIRE(CreateBindingSet(device, Layout, {&resource, 1}, Set) == RasterStatus::Ready);
        const RasterVertexStream stream{ .Stride = 12 };
        const RasterVertexAttribute attribute{ .Format = RasterVertexFormat::Float3 };
        REQUIRE(CreateRasterPipeline(device,
                                     {
                                         .Vertex = Vertex,
                                         .Fragment = Fragment,
                                         .Layout = Layout,
                                         .Streams = {&stream, 1},
                                         .Attributes = {&attribute, 1},
                                     },
                                     Pipeline) == RasterStatus::Ready);
        const uint8 vertices[36]{};
        const uint16 indices[3]{0, 1, 2};
        REQUIRE(CreateBuffer(device, { .Role = BufferRole::Vertex, .Size = sizeof(vertices) }, vertices, Vertices) ==
                RasterStatus::Ready);
        REQUIRE(CreateBuffer(device,
                             { .Role = BufferRole::Index16, .Size = sizeof(indices) },
                             {reinterpret_cast<const uint8*>(indices), sizeof(indices)},
                             Indices) == RasterStatus::Ready);
        Slice = { .Buffer = Vertices, .Size = sizeof(vertices) };
    }
    [[nodiscard]] RasterDraw GetDraw() const noexcept
    {
        return
        {
            .Pipeline = Pipeline,
            .Bindings = Set,
            .Vertices = {&Slice, 1},
            .Indices = Indices,
            .IndexCount = 3,
            .VertexCount = 3,
        };
    }
};
struct TextureTable final
{
    TextureHandle Textures[TEXTURE_COUNT];
    TextureViewHandle Views[TEXTURE_COUNT];
    RasterBindingResource Entries[TEXTURE_COUNT];
    BindingLayoutHandle Layout;
    BindingSetHandle Set;
    explicit TextureTable(DeviceHandle device)
    {
        const RasterBinding binding
        {
            .Binding = 1,
            .Kind = RasterBindingKind::Texture2D,
            .Visibility = RasterVisibility::Fragment,
            .Count = static_cast<uint32>(TEXTURE_COUNT),
            .Indexing = RasterBindingIndexing::NonUniform,
        };
        REQUIRE(CreateBindingLayout(device, {&binding, 1}, Layout) == RasterStatus::Ready);
        for (usize element = 0; element < TEXTURE_COUNT; ++element)
        {
            const uint8 pixel[4]{255, 0, 0, 255};
            REQUIRE(CreateTexture(device,
                                  {
                                      .Width = 1,
                                      .Height = 1,
                                  },
                                  {
                                      .Bytes = pixel,
                                      .RowPitch = 4,
                                  },
                                  Textures[element]) == RasterStatus::Ready);
            REQUIRE(CreateTextureView(device, Textures[element], Views[element]) == RasterStatus::Ready);
            Entries[element] =
            {
                .Binding = 1,
                .Texture = Views[element],
                .ArrayElement = static_cast<uint32>(element),
            };
        }
        REQUIRE(CreateBindingSet(device, Layout, Entries, Set) == RasterStatus::Ready);
    }
};
} // namespace

TEST_CASE("R11 dynamic offsets reject alignment range and undeclared bindings before draws", "[rhi][binding]")
{
    BindingSession session;
    BindingScene scene(session.Device);
    RasterDraw draw = scene.GetDraw();
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    draw.BindingOffsets.Bytes[0] = 1;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    draw.BindingOffsets.Bytes[0] = static_cast<uint32>(UNIFORM_BYTES);
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    draw.BindingOffsets.Bytes[0] = 256;
    draw.BindingOffsets.Bytes[7] = 256;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    CHECK(reference::Draws == 0);
    draw.BindingOffsets.Bytes[7] = 0;
    REQUIRE(DrawIndexed(session.Device, draw) == RasterStatus::Ready);
    CHECK(reference::Packet.DynamicOffsetCount == 1);
    CHECK(reference::Packet.DynamicOffsets[0] == 256);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}

TEST_CASE("R11 graph declarations cover the effective dynamic range", "[rhi][binding][graph]")
{
    BindingSession session;
    BindingScene scene(session.Device);
    for (bool hasCorrectRange : {false, true})
    {
        OrderedGraph graph;
        GraphVersion vertices;
        GraphVersion indices;
        GraphVersion uniform;
        REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Vertices, vertices) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Indices, indices) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Uniform, uniform) == RasterStatus::Ready);
        const GraphUse uses[]{
            { .Resource = vertices, .Access = GraphAccessMode::Vertex, .Size = 36 },
            { .Resource = indices, .Access = GraphAccessMode::Index, .Size = 6 },
            {
                .Resource = uniform,
                .Access = GraphAccessMode::Uniform,
                .Offset = hasCorrectRange == true ? 256U : 0U,
                .Size = 16,
            },
        };
        RasterDraw draw = scene.GetDraw();
        draw.BindingOffsets.Bytes[0] = 256;
        const GraphPassDescription pass
        {
            .Name = "dynamic",
            .Attachment = {},
            .Uses = uses,
            .UseCount = 3,
            .Draws = &draw,
            .DrawCount = 1,
        };
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
        GraphReport report;
        CHECK(CompileOrderedGraph(session.Device, graph, report) ==
              (hasCorrectRange == true ? RasterStatus::Ready : RasterStatus::InvalidDescription));
        REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    }
}

TEST_CASE("R11 descriptor exhaustion survives failed creation and in-flight replacement", "[rhi][binding][lifetime]")
{
    BindingSession session(1);
    BindingScene scene(session.Device);
    const RasterBindingResource resource{ .Buffer = scene.Uniform, .Size = 16 };
    BindingSetHandle replacement;
    CHECK(CreateBindingSet(session.Device, scene.Layout, {&resource, 1}, replacement) ==
          RasterStatus::CapacityExceeded);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(DrawIndexed(session.Device, scene.GetDraw()) == RasterStatus::Ready);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    const BindingSetHandle stale = scene.Set;
    REQUIRE(Destroy(session.Device, scene.Set) == RasterStatus::Ready);
    AllocationTelemetry telemetry;
    REQUIRE(GetAllocationTelemetry(session.Device, telemetry) == RasterStatus::Ready);
    CHECK(telemetry.BindingDescriptors == 1);
    CHECK(telemetry.RetiringBindingDescriptors == 1);
    CHECK(CreateBindingSet(session.Device, scene.Layout, {&resource, 1}, replacement) ==
          RasterStatus::CapacityExceeded);
    reference::Completed = reference::Submitted;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    reference::Next = RasterStatus::OutOfMemory;
    CHECK(CreateBindingSet(session.Device, scene.Layout, {&resource, 1}, replacement) == RasterStatus::OutOfMemory);
    reference::Next = RasterStatus::Ready;
    REQUIRE(CreateBindingSet(session.Device, scene.Layout, {&resource, 1}, replacement) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
}

TEST_CASE("R11 complete texture tables retain every element and reject holes and stale indices", "[rhi][binding]")
{
    BindingSession session;
    TextureTable table(session.Device);
    RasterBindingIndex index;
    REQUIRE(GetBindingIndex(session.Device, table.Set, 1, 3, index) == RasterStatus::Ready);
    uint32 element = 99;
    REQUIRE(GetBindingArrayElement(session.Device, table.Set, index, element) == RasterStatus::Ready);
    CHECK(element == 3);
    CHECK(GetBindingIndex(session.Device, table.Set, 1, 4, index) == RasterStatus::InvalidDescription);
    BindingSetHandle replacement;
    CHECK(CreateBindingSet(session.Device, table.Layout, {table.Entries, 3}, replacement) ==
          RasterStatus::InvalidDescription);
    table.Entries[3].ArrayElement = 2;
    CHECK(CreateBindingSet(session.Device, table.Layout, table.Entries, replacement) ==
          RasterStatus::InvalidDescription);
    table.Entries[3].ArrayElement = 3;
    REQUIRE(CreateBindingSet(session.Device, table.Layout, table.Entries, replacement) == RasterStatus::Ready);
    element = 99;
    CHECK(GetBindingArrayElement(session.Device, replacement, index, element) == RasterStatus::InvalidHandle);
    CHECK(element == 99);
    for (usize item = 0; item < TEXTURE_COUNT; ++item)
    {
        REQUIRE(Destroy(session.Device, table.Views[item]) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, table.Textures[item]) == RasterStatus::Ready);
    }
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == 0);
    const BindingSetHandle stale = table.Set;
    REQUIRE(Destroy(session.Device, table.Set) == RasterStatus::Ready);
    CHECK(GetBindingArrayElement(session.Device, stale, index, element) == RasterStatus::InvalidHandle);
    REQUIRE(Destroy(session.Device, replacement) == RasterStatus::Ready);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == TEXTURE_COUNT);
}

TEST_CASE("R11 native indexing requires explicit admission and browser arrays reject", "[rhi][binding]")
{
    BindingSession session(512, Backend::WebGPU);
    const RasterBinding array
    {
        .Binding = 1,
        .Kind = RasterBindingKind::Texture2D,
        .Visibility = RasterVisibility::Fragment,
        .Count = 4,
    };
    BindingLayoutHandle layout;
    CHECK(CreateBindingLayout(session.Device, {&array, 1}, layout) == RasterStatus::Unsupported);
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(session.Device, info) == DeviceStatus::Ready);
    CHECK(info.Profile.Binding.HasBindlessTextureIndexing == false);
    CHECK(info.UnmetPreferences.BindlessTextureIndexing == true);
    CHECK(info.Profile.Binding.MaxTextureArrayElements == 1);
}

TEST_CASE("R11 descriptor payloads exceed 64 without changing configured limits", "[rhi][binding][storage]")
{
    BindingSession session;
    constexpr usize DESCRIPTORS = 72;
    const RasterBinding array
    {
        .Kind = RasterBindingKind::Texture2D,
        .Visibility = RasterVisibility::Fragment,
        .Count = static_cast<uint32>(DESCRIPTORS),
    };
    BindingLayoutHandle layout;
    REQUIRE(CreateBindingLayout(session.Device, {&array, 1}, layout) == RasterStatus::Ready);
    const uint8 pixel[4]{255, 0, 0, 255};
    TextureHandle texture;
    TextureViewHandle view;
    REQUIRE(CreateTexture(session.Device, { .Width = 1, .Height = 1 }, { .Bytes = pixel, .RowPitch = 4 }, texture) ==
            RasterStatus::Ready);
    REQUIRE(CreateTextureView(session.Device, texture, view) == RasterStatus::Ready);
    RasterBindingResource entries[DESCRIPTORS]{};
    for (usize element = 0; element < DESCRIPTORS; ++element)
    {
        entries[element] = { .Texture = view, .ArrayElement = static_cast<uint32>(element) };
    }
    BindingSetHandle set;
    REQUIRE(CreateBindingSet(session.Device, layout, entries, set) == RasterStatus::Ready);
    AllocationTelemetry telemetry;
    REQUIRE(GetAllocationTelemetry(session.Device, telemetry) == RasterStatus::Ready);
    CHECK(telemetry.BindingDescriptors == DESCRIPTORS);
    REQUIRE(Destroy(session.Device, view) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == 0);
    REQUIRE(Destroy(session.Device, set) == RasterStatus::Ready);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == 1);
}

TEST_CASE("R11 GDI rejects a missing reachable array texture before native execution", "[rhi][binding][graph]")
{
    BindingSession session;
    BindingScene scene(session.Device);
    TextureTable table(session.Device);
    const RasterShaderInput input{ .Format = RasterVertexFormat::Float3 };
    const RasterBinding binding
    {
        .Binding = 1,
        .Kind = RasterBindingKind::Texture2D,
        .Visibility = RasterVisibility::Fragment,
        .Count = static_cast<uint32>(TEXTURE_COUNT),
        .Indexing = RasterBindingIndexing::NonUniform,
    };
    const ShaderDescription vertexArtifact
    {
        .Stage = ShaderStage::Vertex,
        .Spirv = {REFERENCE_SPIRV.GetData(), SPIRV_HEADER_WORDS},
        .Wgsl = "reference-only array vertex",
        .SpirvEntry = "FragmentMain",
        .WgslEntry = "VertexMain",
        .Msl = "reference-only array vertex",
        .MslEntry = "VertexMain",
    };
    const ShaderDescription fragmentArtifact
    {
        .Stage = ShaderStage::Fragment,
        .Spirv = {REFERENCE_SPIRV.GetData(), SPIRV_HEADER_WORDS},
        .Wgsl = "reference-only bounded array load",
        .SpirvEntry = "FragmentMain",
        .WgslEntry = "FragmentMain",
        .Msl = "reference-only bounded array load",
        .MslEntry = "FragmentMain",
    };
    RasterShaderHandle vertex;
    RasterShaderHandle fragment;
    REQUIRE(CreateRasterShader(session.Device,
                               { .Artifact = vertexArtifact, .Inputs = {&input, 1}, .Bindings = {} },
                               vertex) == RasterStatus::Ready);
    CHECK(CreateRasterShader(session.Device,
                             { .Artifact = fragmentArtifact, .Inputs = {}, .Bindings = {&binding, 1} },
                             fragment) == RasterStatus::InvalidDescription);
    REQUIRE(CreateRasterShader(session.Device,
                               {
                                   .Artifact = fragmentArtifact,
                                   .Inputs = {},
                                   .Bindings = {&binding, 1},
                                   .HasVerifiedArrayBounds = true,
                               },
                               fragment) == RasterStatus::Ready);
    const RasterVertexStream stream{ .Stride = 12 };
    const RasterVertexAttribute attribute{ .Format = RasterVertexFormat::Float3 };
    RasterPipelineHandle pipeline;
    REQUIRE(CreateRasterPipeline(session.Device,
                                 {
                                     .Vertex = vertex,
                                     .Fragment = fragment,
                                     .Layout = table.Layout,
                                     .Streams = {&stream, 1},
                                     .Attributes = {&attribute, 1},
                                 },
                                 pipeline) == RasterStatus::Ready);
    RasterBindingIndex reachable[TEXTURE_COUNT];
    for (usize element = 0; element < TEXTURE_COUNT; ++element)
    {
        REQUIRE(GetBindingIndex(session.Device, table.Set, 1, static_cast<uint32>(element), reachable[element]) ==
                RasterStatus::Ready);
    }
    const RasterDraw draw
    {
        .Pipeline = pipeline,
        .Bindings = table.Set,
        .Vertices = {&scene.Slice, 1},
        .Indices = scene.Indices,
        .IndexCount = 3,
        .VertexCount = 3,
        .ReachableBindings = reachable,
    };
    for (bool hasCompleteUses : {false, true})
    {
        OrderedGraph graph;
        GraphVersion vertices;
        GraphVersion indices;
        GraphVersion textures[TEXTURE_COUNT];
        REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Vertices, vertices) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Indices, indices) == RasterStatus::Ready);
        // Elements use their default member initializers; the check misreports them as zero enums.
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        GraphUse uses[TEXTURE_COUNT + 2]{};
        uses[0] = { .Resource = vertices, .Access = GraphAccessMode::Vertex, .Size = 36 };
        uses[1] = { .Resource = indices, .Access = GraphAccessMode::Index, .Size = 6 };
        for (usize element = 0; element < TEXTURE_COUNT; ++element)
        {
            RasterTextureState state;
            REQUIRE(GetTextureState(session.Device, table.Textures[element], state) == RasterStatus::Ready);
            REQUIRE(ImportGraphTexture(session.Device,
                                       graph,
                                       table.Textures[element],
                                       state,
                                       RasterTextureUse::SampledFragment,
                                       textures[element]) == RasterStatus::Ready);
            uses[element + 2] =
            {
                .Resource = textures[element],
                .Access = GraphAccessMode::Sampled,
                .Visibility = RasterVisibility::Fragment,
            };
        }
        const GraphPassDescription pass
        {
            .Name = "reachable array",
            .Attachment = {},
            .Uses = uses,
            .UseCount = hasCompleteUses == true ? TEXTURE_COUNT + 2 : TEXTURE_COUNT + 1,
            .Draws = &draw,
            .DrawCount = 1,
        };
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
        GraphReport report;
        CHECK(CompileOrderedGraph(session.Device, graph, report) ==
              (hasCompleteUses == true ? RasterStatus::Ready : RasterStatus::InvalidDescription));
        CHECK(reference::Draws == 0);
        REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    }
}

TEST_CASE("R11c sparse tables retain fallback resources and reject hole tokens", "[rhi][binding][r11c]")
{
    BindingSession session;
    TextureTable source(session.Device);
    RasterBinding binding
    {
        .Binding = 1,
        .Kind = RasterBindingKind::Texture2D,
        .Visibility = RasterVisibility::Fragment,
        .Count = static_cast<uint32>(TEXTURE_COUNT),
        .HasPartialBinding = true,
    };
    BindingLayoutHandle layout;
    REQUIRE(CreateBindingLayout(session.Device, {&binding, 1}, layout) == RasterStatus::Ready);
    BindingSetHandle sparse;
    CHECK(CreateBindingSet(session.Device, layout, {source.Entries + 1, 1}, sparse) ==
          RasterStatus::InvalidDescription);
    REQUIRE(CreateBindingSet(session.Device, layout, {source.Entries, 1}, sparse) == RasterStatus::Ready);
    RasterBindingIndex index;
    REQUIRE(GetBindingIndex(session.Device, sparse, 1, 0, index) == RasterStatus::Ready);
    CHECK(GetBindingIndex(session.Device, sparse, 1, 1, index) == RasterStatus::InvalidDescription);
    uint32 element = 99;
    REQUIRE(GetBindingArrayElement(session.Device, sparse, index, element) == RasterStatus::Ready);
    CHECK(element == 0);
    REQUIRE(Destroy(session.Device, source.Set) == RasterStatus::Ready);
    for (usize texture = 0; texture < TEXTURE_COUNT; ++texture)
    {
        REQUIRE(Destroy(session.Device, source.Views[texture]) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, source.Textures[texture]) == RasterStatus::Ready);
    }
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == TEXTURE_COUNT - 1);
    AllocationTelemetry telemetry;
    REQUIRE(GetAllocationTelemetry(session.Device, telemetry) == RasterStatus::Ready);
    CHECK(telemetry.BindingDescriptors == TEXTURE_COUNT);
    REQUIRE(Destroy(session.Device, sparse) == RasterStatus::Ready);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == TEXTURE_COUNT);
}

TEST_CASE("R11c snapshot updates preserve submitted work and reject incompatible publication", "[rhi][binding][r11c]")
{
    BindingSession session;
    BindingScene scene(session.Device);
    const RasterBindingResource resource{ .Buffer = scene.Uniform, .Offset = 256, .Size = 16 };
    BindingSetHandle replacement;
    REQUIRE(CreateBindingSet(session.Device, scene.Layout, {&resource, 1}, replacement) == RasterStatus::Ready);
    const BindingSetHandle previous = scene.Set;
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(DrawIndexed(session.Device, scene.GetDraw()) == RasterStatus::Ready);
    REQUIRE(PublishBindingSetUpdate(session.Device, scene.Set, replacement) == RasterStatus::Ready);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    CHECK(GetStatus(session.Device, previous) == RasterStatus::InvalidHandle);
    CHECK(GetStatus(session.Device, replacement) == RasterStatus::InvalidHandle);
    CHECK(GetStatus(session.Device, scene.Set) == RasterStatus::Ready);
    AllocationTelemetry telemetry;
    REQUIRE(GetAllocationTelemetry(session.Device, telemetry) == RasterStatus::Ready);
    CHECK(telemetry.BindingDescriptors == 2);
    CHECK(telemetry.RetiringBindingDescriptors == 1);
    reference::Completed = reference::Submitted;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    REQUIRE(GetAllocationTelemetry(session.Device, telemetry) == RasterStatus::Ready);
    CHECK(telemetry.BindingDescriptors == 1);
    BindingSetHandle alias = scene.Set;
    CHECK(PublishBindingSetUpdate(session.Device, scene.Set, alias) == RasterStatus::InvalidDescription);
    TextureTable incompatible(session.Device);
    CHECK(PublishBindingSetUpdate(session.Device, scene.Set, incompatible.Set) == RasterStatus::InvalidDescription);
    CHECK(GetStatus(session.Device, scene.Set) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, incompatible.Set) == RasterStatus::Ready);
}

TEST_CASE("R11c runtime allocations have independent extents and charge actual descriptor counts",
          "[rhi][binding][r11c]")
{
    BindingSession session(512, Backend::Vulkan);
    TextureTable source(session.Device);
    for (RasterBindingResource& resource : source.Entries)
    {
        resource.Binding = RUNTIME_BINDING_ARRAY_SLOT;
    }
    const RasterBinding binding
    {
        .Binding = RUNTIME_BINDING_ARRAY_SLOT,
        .Kind = RasterBindingKind::Texture2D,
        .Visibility = RasterVisibility::Fragment,
        .Count = 16,
        .IsRuntimeSized = true,
        .HasPartialBinding = true,
    };
    BindingLayoutHandle layout;
    REQUIRE(CreateBindingLayout(session.Device, {&binding, 1}, layout) == RasterStatus::Ready);
    BindingSetHandle small;
    BindingSetHandle large;
    CHECK(CreateRuntimeBindingSet(session.Device, layout, {source.Entries, 1}, 0, small) ==
          RasterStatus::InvalidDescription);
    CHECK(CreateRuntimeBindingSet(session.Device, layout, {source.Entries, 1}, 17, small) ==
          RasterStatus::InvalidDescription);
    CHECK(CreateRuntimeBindingSet(session.Device, layout, {nullptr, 1}, 1, small) == RasterStatus::InvalidDescription);
    const RasterBindingResource forgedLength
    {
        .Binding = RUNTIME_BINDING_LENGTH_SLOT,
        .Texture = source.Views[0],
    };
    CHECK(CreateRuntimeBindingSet(session.Device, layout, {&forgedLength, 1}, 1, small) ==
          RasterStatus::InvalidDescription);
    REQUIRE(CreateRuntimeBindingSet(session.Device, layout, {source.Entries, 1}, 1, small) == RasterStatus::Ready);
    REQUIRE(CreateRuntimeBindingSet(session.Device, layout, source.Entries, 8, large) == RasterStatus::Ready);
    RasterBindingIndex token;
    REQUIRE(GetBindingIndex(session.Device, small, RUNTIME_BINDING_ARRAY_SLOT, 0, token) == RasterStatus::Ready);
    CHECK(GetBindingIndex(session.Device, small, RUNTIME_BINDING_ARRAY_SLOT, 1, token) ==
          RasterStatus::InvalidDescription);
    REQUIRE(GetBindingIndex(session.Device, large, RUNTIME_BINDING_ARRAY_SLOT, 3, token) == RasterStatus::Ready);
    AllocationTelemetry telemetry;
    REQUIRE(GetAllocationTelemetry(session.Device, telemetry) == RasterStatus::Ready);
    CHECK(telemetry.BindingDescriptors == TEXTURE_COUNT + 1 + 8 + 2);
    REQUIRE(PublishBindingSetUpdate(session.Device, small, large) == RasterStatus::Ready);
    uint32 element = 99;
    REQUIRE(GetBindingArrayElement(session.Device, small, token, element) == RasterStatus::Ready);
    CHECK(element == 3);
    CHECK(CreateRuntimeBindingSet(session.Device, source.Layout, source.Entries, 4, large) ==
          RasterStatus::InvalidDescription);
    const RasterBinding invalid[]{binding,
                                  { .Binding = RUNTIME_BINDING_LENGTH_SLOT, .Kind = RasterBindingKind::Sampler }};
    BindingLayoutHandle invalidLayout;
    CHECK(CreateBindingLayout(session.Device, invalid, invalidLayout) == RasterStatus::InvalidDescription);
}

TEST_CASE("R11c wider descriptor arrays count every stage and preserve capability boundaries", "[rhi][binding][r11c]")
{
    BindingSession session(512, Backend::Vulkan);
    RasterBinding binding
    {
        .Kind = RasterBindingKind::UniformBuffer,
        .Visibility = RasterVisibility::Fragment,
        .MinSize = 16,
        .Count = 12,
    };
    BindingLayoutHandle layout;
    REQUIRE(CreateBindingLayout(session.Device, {&binding, 1}, layout) == RasterStatus::Ready);
    binding.Count = 13;
    BindingLayoutHandle rejected;
    CHECK(CreateBindingLayout(session.Device, {&binding, 1}, rejected) == RasterStatus::Unsupported);
    binding.Count = 2;
    binding.HasDynamicOffset = true;
    CHECK(CreateBindingLayout(session.Device, {&binding, 1}, rejected) == RasterStatus::InvalidDescription);
    binding.HasDynamicOffset = false;
    binding.Indexing = RasterBindingIndexing::Uniform;
    CHECK(CreateBindingLayout(session.Device, {&binding, 1}, rejected) == RasterStatus::InvalidDescription);
    binding.Indexing = RasterBindingIndexing::Constant;
    reference::BindingLimitOverride = backend::BindingLimits();
    reference::HasBindingLimitOverride = true;
    reference::BindingLimitOverride.HasBufferDescriptorArrays = false;
    CHECK(CreateBindingLayout(session.Device, {&binding, 1}, rejected) == RasterStatus::Unsupported);
}

TEST_CASE("R11c pending and failed replacements preserve the drawable snapshot", "[rhi][binding][r11c]")
{
    BindingSession session;
    BindingScene scene(session.Device);
    const RasterBindingResource resource{ .Buffer = scene.Uniform, .Size = 16 };
    BindingSetHandle replacement;
    reference::Next = RasterStatus::Pending;
    REQUIRE(CreateBindingSet(session.Device, scene.Layout, {&resource, 1}, replacement) == RasterStatus::Pending);
    reference::Next = RasterStatus::Ready;
    const uint32 request = reference::LastRequest;
    CHECK(PublishBindingSetUpdate(session.Device, scene.Set, replacement) == RasterStatus::NotReady);
    CHECK(GetStatus(session.Device, scene.Set) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, replacement) == RasterStatus::Pending);
    internal::RasterComplete(request, RasterStatus::Failed);
    CHECK(PublishBindingSetUpdate(session.Device, scene.Set, replacement) == RasterStatus::Failed);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(DrawIndexed(session.Device, scene.GetDraw()) == RasterStatus::Ready);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    CHECK(GetStatus(session.Device, scene.Set) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, replacement) == RasterStatus::Ready);
}

TEST_CASE("R11c a runtime allocation shorter than shader bounds rejects before encoding", "[rhi][binding][r11c]")
{
    BindingSession session(512, Backend::Vulkan);
    BindingScene scene(session.Device);
    TextureTable source(session.Device);
    const RasterBinding entries[]{
        { .Kind = RasterBindingKind::UniformBuffer, .MinSize = 16, .HasDynamicOffset = true },
        {
            .Binding = RUNTIME_BINDING_ARRAY_SLOT,
            .Kind = RasterBindingKind::Texture2D,
            .Visibility = RasterVisibility::Fragment,
            .Count = static_cast<uint32>(TEXTURE_COUNT),
            .IsRuntimeSized = true,
            .HasPartialBinding = true,
        },
    };
    BindingLayoutHandle layout;
    REQUIRE(CreateBindingLayout(session.Device, entries, layout) == RasterStatus::Ready);
    RasterShaderHandle fragment;
    REQUIRE(CreateRasterShader(session.Device,
                               {
                                   .Artifact =
                                   {
                                       .Stage = ShaderStage::Fragment,
                                       .Spirv = {REFERENCE_SPIRV.GetData(), SPIRV_HEADER_WORDS},
                                       .Wgsl = "fixture",
                                       .SpirvEntry = "FragmentMain",
                                       .WgslEntry = "FragmentMain",
                                       .Msl = "fixture",
                                       .MslEntry = "FragmentMain",
                                   },
                                   .Inputs = {},
                                   .Bindings = {entries + 1, 1},
                                   .HasVerifiedArrayBounds = true,
                               },
                               fragment) == RasterStatus::Ready);
    const RasterVertexStream stream{ .Stride = 12 };
    const RasterVertexAttribute attribute{ .Format = RasterVertexFormat::Float3 };
    RasterPipelineHandle pipeline;
    REQUIRE(CreateRasterPipeline(session.Device,
                                 {
                                     .Vertex = scene.Vertex,
                                     .Fragment = fragment,
                                     .Layout = layout,
                                     .Streams = {&stream, 1},
                                     .Attributes = {&attribute, 1},
                                 },
                                 pipeline) == RasterStatus::Ready);
    const RasterBindingResource resources[]{
        { .Buffer = scene.Uniform, .Size = 16 },
        { .Binding = RUNTIME_BINDING_ARRAY_SLOT, .Texture = source.Views[0] },
    };
    BindingSetHandle small;
    REQUIRE(CreateRuntimeBindingSet(session.Device, layout, resources, 1, small) == RasterStatus::Ready);
    RasterDraw draw = scene.GetDraw();
    draw.Pipeline = pipeline;
    draw.Bindings = small;
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    CHECK(reference::Draws == 0);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}

TEST_CASE("R11c GDI owns runtime length reads without a consumer metadata import", "[rhi][binding][r11c][graph]")
{
    BindingSession session(512, Backend::Vulkan);
    BindingScene scene(session.Device);
    TextureTable source(session.Device);
    const RasterBinding bindings[]{
        { .Kind = RasterBindingKind::UniformBuffer, .MinSize = 16, .HasDynamicOffset = true },
        {
            .Binding = RUNTIME_BINDING_ARRAY_SLOT,
            .Kind = RasterBindingKind::Texture2D,
            .Visibility = RasterVisibility::Fragment,
            .Count = 4,
            .IsRuntimeSized = true,
            .HasPartialBinding = true,
        },
    };
    BindingLayoutHandle layout;
    REQUIRE(CreateBindingLayout(session.Device, bindings, layout) == RasterStatus::Ready);
    RasterShaderHandle fragment;
    REQUIRE(CreateRasterShader(session.Device,
                               {
                                   .Artifact =
                                   {
                                       .Stage = ShaderStage::Fragment,
                                       .Spirv = {REFERENCE_SPIRV.GetData(), SPIRV_HEADER_WORDS},
                                       .Wgsl = "fixture",
                                       .SpirvEntry = "FragmentMain",
                                       .WgslEntry = "FragmentMain",
                                       .Msl = "fixture",
                                       .MslEntry = "FragmentMain",
                                   },
                                   .Inputs = {},
                                   .Bindings = {bindings + 1, 1},
                                   .HasVerifiedArrayBounds = true,
                               },
                               fragment) == RasterStatus::Ready);
    const RasterVertexStream stream{ .Stride = 12 };
    const RasterVertexAttribute attribute{ .Format = RasterVertexFormat::Float3 };
    RasterPipelineHandle pipeline;
    REQUIRE(CreateRasterPipeline(session.Device,
                                 {
                                     .Vertex = scene.Vertex,
                                     .Fragment = fragment,
                                     .Layout = layout,
                                     .Streams = {&stream, 1},
                                     .Attributes = {&attribute, 1},
                                 },
                                 pipeline) == RasterStatus::Ready);
    const RasterBindingResource resources[]{
        { .Buffer = scene.Uniform, .Size = 16 },
        { .Binding = RUNTIME_BINDING_ARRAY_SLOT, .Texture = source.Views[0] },
    };
    BindingSetHandle set;
    REQUIRE(CreateRuntimeBindingSet(session.Device, layout, resources, 4, set) == RasterStatus::Ready);
    OrderedGraph graph;
    GraphVersion vertices;
    GraphVersion indices;
    GraphVersion uniform;
    GraphVersion sampled;
    RasterTextureState state;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Vertices, vertices) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Indices, indices) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Uniform, uniform) == RasterStatus::Ready);
    REQUIRE(GetTextureState(session.Device, source.Textures[0], state) == RasterStatus::Ready);
    REQUIRE(ImportGraphTexture(session.Device,
                               graph,
                               source.Textures[0],
                               state,
                               RasterTextureUse::SampledFragment,
                               sampled) == RasterStatus::Ready);
    const GraphUse uses[]{
        { .Resource = vertices, .Access = GraphAccessMode::Vertex, .Size = 36 },
        { .Resource = indices, .Access = GraphAccessMode::Index, .Size = 6 },
        { .Resource = uniform, .Access = GraphAccessMode::Uniform, .Size = 16 },
        { .Resource = sampled, .Access = GraphAccessMode::Sampled, .Visibility = RasterVisibility::Fragment },
    };
    RasterDraw draw = scene.GetDraw();
    draw.Pipeline = pipeline;
    draw.Bindings = set;
    REQUIRE(AddGraphPass(session.Device,
                         graph,
                         {
                             .Name = "owned runtime control",
                             .Attachment = {},
                             .Uses = uses,
                             .UseCount = 4,
                             .Draws = &draw,
                             .DrawCount = 1,
                         }) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
}
