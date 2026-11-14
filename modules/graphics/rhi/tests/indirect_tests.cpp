#include "internal/lifecycle.h"
#include "internal/raster.h"
#include "reference_raster.h"

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

// R12a: expanded indirect draws and dispatches on the reference backend. These cases check
// argument ABI, alignment, counts, strides, span overflow, capability gates, graph declarations
// and producer-to-command hazards before any native work; GPU journeys check the pixels.
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
constexpr usize ARGUMENT_BUFFER_BYTES = 64;
constexpr usize STORAGE_BYTES = 16;
constexpr usize VERTEX_BYTES = 36;
constexpr usize INDEX_BYTES = 6;
constexpr uint32 TRIANGLE_VERTICES = 3;
constexpr usize UNIFORM_BYTES = 16;
constexpr uint32 FLOAT3_STRIDE = 12;
constexpr uint32 STORAGE_ELEMENT_STRIDE = 4;
constexpr uint32 TEST_SURFACE_WIDTH = 4096;
constexpr uint32 TEST_SURFACE_HEIGHT = 16384;

struct IndirectSession final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    explicit IndirectSession(Backend activeBackend = Backend::WebGPU,
                             const IndirectCapabilities* indirectOverrideOrNull = nullptr)
    {
        Shutdown();
        reference::Reset();
        backend::ImmediateError = StartupError::None;
        backend::NextFrame = FrameStatus::Ready;
        backend::ActiveKind = activeBackend;
        if (indirectOverrideOrNull != nullptr)
        {
            reference::HasIndirectOverride = true;
            reference::IndirectOverride = *indirectOverrideOrNull;
        }
        REQUIRE(CreateDevice({}, {}, {}, Device, Surface) == DeviceStatus::Pending);
        internal::Complete(backend::PendingToken, StartupError::None, {TEST_SURFACE_WIDTH, TEST_SURFACE_HEIGHT});
        DeviceInfo info;
        REQUIRE(GetDeviceInfo(Device, info) == DeviceStatus::Ready);
    }
    IndirectSession(const IndirectSession&) = delete;
    IndirectSession& operator=(const IndirectSession&) = delete;
    ~IndirectSession() noexcept
    {
        Shutdown();
    }
};
// One float3 stream, a uniform binding, one triangle of indices and a 64-byte argument buffer.
struct IndirectScene final
{
    RasterShaderHandle Vertex;
    RasterShaderHandle Fragment;
    BindingLayoutHandle Layout;
    BindingSetHandle Set;
    RasterPipelineHandle Pipeline;
    BufferHandle Uniform;
    BufferHandle Vertices;
    BufferHandle Indices;
    BufferHandle Arguments;
    StaticArray<RasterVertexSlice, 1> Slices{};
    explicit IndirectScene(DeviceHandle device)
    {
        const RasterBinding binding{0, RasterBindingKind::UniformBuffer, RasterVisibility::Both, UNIFORM_BYTES};
        const RasterShaderInput input{0, RasterVertexFormat::Float3};
        RasterShaderDescription vertex;
        vertex.Artifact.Stage = ShaderStage::Vertex;
        vertex.Artifact.Wgsl = "fixture vertex";
        vertex.Artifact.WgslEntry = "vertexMain";
        vertex.Inputs = {&input, 1};
        vertex.Bindings = {&binding, 1};
        REQUIRE(CreateRasterShader(device, vertex, Vertex) == RasterStatus::Ready);
        RasterShaderDescription fragment = vertex;
        fragment.Inputs = {};
        fragment.Artifact.Stage = ShaderStage::Fragment;
        fragment.Artifact.WgslEntry = "fragmentMain";
        REQUIRE(CreateRasterShader(device, fragment, Fragment) == RasterStatus::Ready);
        REQUIRE(CreateBindingLayout(device, {&binding, 1}, Layout) == RasterStatus::Ready);
        const StaticArray<uint8, UNIFORM_BYTES> uniform{};
        REQUIRE(CreateBuffer(device,
                             {BufferRole::Uniform, UNIFORM_BYTES},
                             {uniform.GetData(), uniform.GetSize()},
                             Uniform) == RasterStatus::Ready);
        const RasterBindingResource resource{ .Buffer = Uniform, .Size = UNIFORM_BYTES };
        REQUIRE(CreateBindingSet(device, Layout, {&resource, 1}, Set) == RasterStatus::Ready);
        const RasterVertexStream stream{FLOAT3_STRIDE, false};
        const RasterVertexAttribute attribute{0, 0, 0, RasterVertexFormat::Float3};
        REQUIRE(CreateRasterPipeline(device,
                                     {Vertex, Fragment, Layout, {&stream, 1}, {&attribute, 1}, true},
                                     Pipeline) == RasterStatus::Ready);
        const StaticArray<uint8, VERTEX_BYTES> vertices{};
        const StaticArray<uint16, TRIANGLE_VERTICES> indices{0, 1, 2};
        const StaticArray<uint8, ARGUMENT_BUFFER_BYTES> arguments{};
        REQUIRE(CreateBuffer(device,
                             {BufferRole::Vertex, VERTEX_BYTES},
                             {vertices.GetData(), vertices.GetSize()},
                             Vertices) == RasterStatus::Ready);
        REQUIRE(CreateBuffer(device,
                             {BufferRole::Index16, INDEX_BYTES},
                             {reinterpret_cast<const uint8*>(indices.GetData()), INDEX_BYTES},
                             Indices) == RasterStatus::Ready);
        REQUIRE(CreateBuffer(device,
                             {BufferRole::StorageIndirect, ARGUMENT_BUFFER_BYTES},
                             {arguments.GetData(), arguments.GetSize()},
                             Arguments) == RasterStatus::Ready);
        Slices[0] = {Vertices, 0, VERTEX_BYTES};
    }
    [[nodiscard]] RasterDraw MakeIndexedDraw() const noexcept
    {
        RasterDraw draw{Pipeline,
                        Set,
                        {Slices.GetData(), Slices.GetSize()},
                        Indices,
                        0,
                        TRIANGLE_VERTICES,
                        TRIANGLE_VERTICES,
                        1};
        draw.Indirect = Arguments;
        return draw;
    }
    [[nodiscard]] RasterDraw MakeNonIndexedDraw() const noexcept
    {
        RasterDraw draw = MakeIndexedDraw();
        draw.Indices = {};
        draw.IndexCount = 0;
        return draw;
    }
};
// A compute kernel writing a 16-byte storage range of an existing or new buffer.
struct IndirectKernel final
{
    BufferHandle Buffer;
    RasterShaderHandle Shader;
    BindingLayoutHandle Layout;
    BindingSetHandle Set;
    ComputePipelineHandle Pipeline;
    explicit IndirectKernel(DeviceHandle device, BufferHandle existing = {})
    {
        Buffer = existing;
        if (GetStatus(device, Buffer) == RasterStatus::InvalidHandle)
        {
            const StaticArray<uint8, STORAGE_BYTES> bytes{};
            REQUIRE(CreateBuffer(device,
                                 {BufferRole::Storage, STORAGE_BYTES},
                                 {bytes.GetData(), bytes.GetSize()},
                                 Buffer) == RasterStatus::Ready);
        }
        const RasterBinding binding{0,
                                    RasterBindingKind::StorageReadWrite,
                                    RasterVisibility::Compute,
                                    STORAGE_ELEMENT_STRIDE};
        RasterShaderDescription description;
        description.Artifact.Stage = ShaderStage::Compute;
        description.Artifact.Wgsl = "reference-only compute metadata";
        description.Artifact.WgslEntry = "computeMain";
        description.Bindings = {&binding, 1};
        REQUIRE(CreateRasterShader(device, description, Shader) == RasterStatus::Ready);
        REQUIRE(CreateBindingLayout(device, {&binding, 1}, Layout) == RasterStatus::Ready);
        const RasterBindingResource resource{ .Buffer = Buffer, .Size = STORAGE_BYTES };
        REQUIRE(CreateBindingSet(device, Layout, {&resource, 1}, Set) == RasterStatus::Ready);
        REQUIRE(CreateComputePipeline(device, {Shader, Layout}, Pipeline) == RasterStatus::Ready);
    }
    [[nodiscard]] ComputeDispatch MakeDispatch() const noexcept
    {
        return {Pipeline, Set};
    }
};
bool HasDependency(const GraphReport& report, GraphHazard hazard, GraphAccessMode destination) noexcept
{
    constexpr uint32 REPORTED_DEPENDENCIES = sizeof(report.Dependencies) / sizeof(report.Dependencies[0]);
    const uint32 count =
        report.DependencyCount < REPORTED_DEPENDENCIES ? report.DependencyCount : REPORTED_DEPENDENCIES;
    for (uint32 i = 0; i < count; ++i)
    {
        if (report.Dependencies[i].Hazard == hazard && report.Dependencies[i].Destination == destination)
        {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("R12a indirect profile follows compute availability and is absent on WebGL 2", "[rhi][indirect]")
{
    {
        IndirectSession session;
        DeviceInfo info;
        REQUIRE(GetDeviceInfo(session.Device, info) == DeviceStatus::Ready);
        CHECK(info.Enabled.IndirectRendering);
        CHECK(info.Profile.Indirect.ArgumentAlignment == internal::INDIRECT_ARGUMENT_ALIGNMENT_BYTES);
        CHECK(info.Profile.Indirect.MaxCommandsPerDraw == internal::MAX_INDIRECT_COMMANDS_PER_DRAW);
        CHECK(info.Profile.Indirect.HasNonIndexedDraws);
        CHECK(info.Profile.Indirect.HasDrawOffsets);
        CHECK(info.Profile.Indirect.HasFirstInstance);
        CHECK(info.Profile.Indirect.HasIndirectDispatch);
    }
    IndirectSession webgl(Backend::WebGL2);
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(webgl.Device, info) == DeviceStatus::Ready);
    CHECK(info.Enabled.IndirectRendering == false);
    CHECK(info.Profile.Indirect.ArgumentAlignment == 0);
    CHECK(info.Profile.Indirect.MaxCommandsPerDraw == 0);
    CHECK(info.Profile.Indirect.HasNonIndexedDraws == false);
    CHECK(info.Profile.Indirect.HasIndirectDispatch == false);
}

TEST_CASE("R12a indirect draws validate non-indexed packets, counts, strides and spans", "[rhi][indirect]")
{
    IndirectSession session;
    IndirectScene scene(session.Device);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);

    RasterDraw draw = scene.MakeNonIndexedDraw();
    REQUIRE(DrawIndexed(session.Device, draw) == RasterStatus::Ready);
    CHECK(reference::Packet.Indices == internal::RASTER_INVALID);
    CHECK(reference::Packet.IndirectCount == 1);
    CHECK(reference::Packet.IndirectStride == sizeof(DrawIndirectArguments));
    CHECK(internal::GetIndirectArgumentBytes(reference::Packet) == sizeof(DrawIndirectArguments));

    SECTION("Packets without indices require indirect commands and no index range")
    {
        RasterDraw direct = draw;
        direct.Indirect = {};
        CHECK(DrawIndexed(session.Device, direct) == RasterStatus::InvalidHandle);
        RasterDraw ranged = draw;
        ranged.IndexCount = TRIANGLE_VERTICES;
        CHECK(DrawIndexed(session.Device, ranged) == RasterStatus::InvalidDescription);
        ranged.IndexCount = 0;
        ranged.IndexOffset = sizeof(uint16);
        CHECK(DrawIndexed(session.Device, ranged) == RasterStatus::InvalidDescription);
    }
    SECTION("Command counts and strides stay bounded, aligned and inside the buffer")
    {
        constexpr uint32 FULL_BUFFER_COMMANDS = ARGUMENT_BUFFER_BYTES / sizeof(DrawIndirectArguments);
        RasterDraw counted = draw;
        counted.IndirectCount = 0;
        CHECK(DrawIndexed(session.Device, counted) == RasterStatus::InvalidDescription);
        counted.IndirectCount = internal::MAX_INDIRECT_COMMANDS_PER_DRAW + 1;
        CHECK(DrawIndexed(session.Device, counted) == RasterStatus::InvalidDescription);
        counted.IndirectCount = FULL_BUFFER_COMMANDS;
        REQUIRE(DrawIndexed(session.Device, counted) == RasterStatus::Ready);
        CHECK(reference::Packet.IndirectCount == FULL_BUFFER_COMMANDS);
        counted.IndirectOffset = internal::INDIRECT_ARGUMENT_ALIGNMENT_BYTES;
        CHECK(DrawIndexed(session.Device, counted) == RasterStatus::InvalidDescription);
        counted.IndirectOffset = 0;
        counted.IndirectCount = 2;
        counted.IndirectStride = sizeof(DrawIndirectArguments) + 2;
        CHECK(DrawIndexed(session.Device, counted) == RasterStatus::InvalidDescription);
        counted.IndirectStride = sizeof(DrawIndirectArguments) - internal::INDIRECT_ARGUMENT_ALIGNMENT_BYTES;
        CHECK(DrawIndexed(session.Device, counted) == RasterStatus::InvalidDescription);
        counted.IndirectStride = ~usize{0} - (internal::INDIRECT_ARGUMENT_ALIGNMENT_BYTES - 1);
        CHECK(DrawIndexed(session.Device, counted) == RasterStatus::InvalidDescription);
        counted.IndirectStride = ARGUMENT_BUFFER_BYTES / 2;
        REQUIRE(DrawIndexed(session.Device, counted) == RasterStatus::Ready);
        CHECK(reference::Packet.IndirectStride == ARGUMENT_BUFFER_BYTES / 2);
    }
    SECTION("Indexed commands use the twenty-byte layout and direct packets keep one command")
    {
        constexpr uint32 INDEXED_COMMANDS = 3;
        RasterDraw indexed = scene.MakeIndexedDraw();
        indexed.IndirectCount = INDEXED_COMMANDS;
        REQUIRE(DrawIndexed(session.Device, indexed) == RasterStatus::Ready);
        CHECK(reference::Packet.Indices != internal::RASTER_INVALID);
        CHECK(reference::Packet.IndirectStride == sizeof(IndexedIndirectArguments));
        CHECK(internal::GetIndirectArgumentBytes(reference::Packet) == sizeof(IndexedIndirectArguments));
        indexed.IndirectCount = INDEXED_COMMANDS + 1;
        CHECK(DrawIndexed(session.Device, indexed) == RasterStatus::InvalidDescription);
        RasterDraw direct = scene.MakeIndexedDraw();
        direct.Indirect = {};
        direct.IndirectCount = 2;
        CHECK(DrawIndexed(session.Device, direct) == RasterStatus::InvalidDescription);
        direct.IndirectCount = 1;
        direct.IndirectStride = sizeof(IndexedIndirectArguments);
        CHECK(DrawIndexed(session.Device, direct) == RasterStatus::InvalidDescription);
        direct.IndirectStride = 0;
        REQUIRE(DrawIndexed(session.Device, direct) == RasterStatus::Ready);
        CHECK(reference::Packet.Indirect == internal::RASTER_INVALID);
    }
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}

TEST_CASE("R12a span arithmetic rejects zero counts and overflow", "[rhi][indirect]")
{
    usize span = 0;
    CHECK(internal::ComputeIndirectSpan(0, sizeof(DrawIndirectArguments), sizeof(DrawIndirectArguments), span) ==
          false);
    REQUIRE(internal::ComputeIndirectSpan(1, ~usize{0}, sizeof(DrawIndirectArguments), span));
    CHECK(span == sizeof(DrawIndirectArguments));
    constexpr uint32 COMMANDS = 4;
    constexpr usize STRIDE = 32;
    REQUIRE(internal::ComputeIndirectSpan(COMMANDS, STRIDE, sizeof(IndexedIndirectArguments), span));
    CHECK(span == (COMMANDS - 1) * STRIDE + sizeof(IndexedIndirectArguments));
    CHECK(internal::ComputeIndirectSpan(2, ~usize{0} - 2, sizeof(DrawIndirectArguments), span) == false);
}

TEST_CASE("R12a disabled indirect features reject before native work", "[rhi][indirect]")
{
    const IndirectCapabilities indexedOnly
    {
        .ArgumentAlignment = internal::INDIRECT_ARGUMENT_ALIGNMENT_BYTES,
        .MaxCommandsPerDraw = 1,
    };
    IndirectSession session(Backend::WebGPU, &indexedOnly);
    IndirectScene scene(session.Device);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    CHECK(DrawIndexed(session.Device, scene.MakeNonIndexedDraw()) == RasterStatus::Unsupported);
    RasterDraw indexed = scene.MakeIndexedDraw();
    indexed.IndirectCount = 2;
    CHECK(DrawIndexed(session.Device, indexed) == RasterStatus::InvalidDescription);
    CHECK(reference::Draws == 0);
    indexed.IndirectCount = 1;
    REQUIRE(DrawIndexed(session.Device, indexed) == RasterStatus::Ready);
    CHECK(reference::Draws == 1);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);

    IndirectKernel kernel(session.Device);
    ComputeDispatch dispatch = kernel.MakeDispatch();
    dispatch.Indirect = scene.Arguments;
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphPassDescription pass;
    pass.Name = "unsupported indirect dispatch";
    pass.Dispatch = &dispatch;
    CHECK(AddGraphPass(session.Device, graph, pass) == RasterStatus::Unsupported);
    CHECK(reference::Dispatches == 0);
}

TEST_CASE("R12a graph indirect declarations cover every command and order the producer", "[rhi][indirect][graph]")
{
    IndirectSession session;
    IndirectScene scene(session.Device);
    IndirectKernel producer(session.Device, scene.Arguments);
    constexpr uint32 COMMANDS = 2;
    constexpr usize SPAN = sizeof(IndexedIndirectArguments) * COMMANDS;
    const bool shouldDeclareSpan = GENERATE(true, false);

    OrderedGraph graph;
    GraphVersion vertex;
    GraphVersion index;
    GraphVersion uniform;
    GraphVersion argumentsInput;
    GraphVersion argumentsOutput;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Vertices, vertex) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Indices, index) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Uniform, uniform) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Arguments, argumentsInput) == RasterStatus::Ready);
    REQUIRE(NextGraphVersion(session.Device, graph, argumentsInput, argumentsOutput) == RasterStatus::Ready);
    const ComputeDispatch dispatch = producer.MakeDispatch();
    const GraphUse produce{argumentsOutput,
                           GraphAccessMode::StorageReadWrite,
                           0,
                           STORAGE_BYTES,
                           RasterVisibility::Compute};
    GraphPassDescription computePass;
    computePass.Name = "write commands";
    computePass.Dispatch = &dispatch;
    computePass.Uses = &produce;
    computePass.UseCount = 1;
    REQUIRE(AddGraphPass(session.Device, graph, computePass) == RasterStatus::Ready);

    RasterDraw draw = scene.MakeIndexedDraw();
    draw.IndirectCount = COMMANDS;
    const StaticArray<GraphUse, 4> uses{{
        {vertex, GraphAccessMode::Vertex, 0, VERTEX_BYTES},
        {index, GraphAccessMode::Index, 0, INDEX_BYTES},
        {uniform, GraphAccessMode::Uniform, 0, UNIFORM_BYTES},
        {argumentsOutput,
         GraphAccessMode::Indirect,
         0,
         shouldDeclareSpan == true ? SPAN : sizeof(IndexedIndirectArguments)},
    }};
    GraphPassDescription rasterPass;
    rasterPass.Name = "draw commands";
    rasterPass.Uses = uses.GetData();
    rasterPass.UseCount = uses.GetSize();
    rasterPass.Draws = &draw;
    rasterPass.DrawCount = 1;
    REQUIRE(AddGraphPass(session.Device, graph, rasterPass) == RasterStatus::Ready);
    GraphReport report;
    if (shouldDeclareSpan == false)
    {
        CHECK(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::InvalidDescription);
        CHECK(report.ErrorPass == 1);
        return;
    }
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    CHECK(report.ComputePasses == 1);
    CHECK(HasDependency(report, GraphHazard::ReadAfterWrite, GraphAccessMode::Indirect));
    SubmissionToken token;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::Ready);
    CHECK(reference::Dispatches == 1);
    CHECK(reference::Draws == 1);
    CHECK(reference::Packet.IndirectCount == COMMANDS);
}

TEST_CASE("R12a indirect dispatch validates arguments and requires its declared use", "[rhi][indirect][compute]")
{
    IndirectSession session;
    IndirectScene scene(session.Device);
    IndirectKernel producer(session.Device, scene.Arguments);
    IndirectKernel consumer(session.Device);
    constexpr usize DISPATCH_OFFSET = STORAGE_BYTES;

    OrderedGraph graph;
    GraphVersion argumentsInput;
    GraphVersion argumentsOutput;
    GraphVersion storageInput;
    GraphVersion storageOutput;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Arguments, argumentsInput) == RasterStatus::Ready);
    REQUIRE(NextGraphVersion(session.Device, graph, argumentsInput, argumentsOutput) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, consumer.Buffer, storageInput) == RasterStatus::Ready);
    REQUIRE(NextGraphVersion(session.Device, graph, storageInput, storageOutput) == RasterStatus::Ready);

    ComputeDispatch dispatch = consumer.MakeDispatch();
    dispatch.Indirect = scene.Arguments;
    dispatch.IndirectOffset = DISPATCH_OFFSET;
    const GraphUse storageUse{storageOutput,
                              GraphAccessMode::StorageReadWrite,
                              0,
                              STORAGE_BYTES,
                              RasterVisibility::Compute};
    GraphPassDescription pass;
    pass.Name = "indirect dispatch";
    pass.Dispatch = &dispatch;
    pass.Uses = &storageUse;
    pass.UseCount = 1;
    pass.SideEffect = true;

    SECTION("Misaligned, truncated, wrongly typed and unpaired offsets reject at admission")
    {
        ComputeDispatch invalid = dispatch;
        pass.Dispatch = &invalid;
        invalid.IndirectOffset = DISPATCH_OFFSET + 2;
        CHECK(AddGraphPass(session.Device, graph, pass) == RasterStatus::InvalidDescription);
        invalid.IndirectOffset = ARGUMENT_BUFFER_BYTES - sizeof(DispatchIndirectArguments) + STORAGE_ELEMENT_STRIDE;
        CHECK(AddGraphPass(session.Device, graph, pass) == RasterStatus::InvalidDescription);
        invalid.IndirectOffset = 0;
        invalid.Indirect = consumer.Buffer;
        CHECK(AddGraphPass(session.Device, graph, pass) == RasterStatus::InvalidDescription);
        invalid.Indirect = {};
        invalid.IndirectOffset = DISPATCH_OFFSET;
        CHECK(AddGraphPass(session.Device, graph, pass) == RasterStatus::InvalidDescription);
        invalid = dispatch;
        invalid.Groups[0] = 0;
        CHECK(AddGraphPass(session.Device, graph, pass) == RasterStatus::InvalidDescription);
    }
    SECTION("A missing Indirect use rejects the compiled plan")
    {
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
        GraphReport report;
        CHECK(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::InvalidDescription);
        CHECK(report.ErrorPass == 0);
    }
    SECTION("An Indirect use on a direct dispatch rejects the compiled plan")
    {
        const ComputeDispatch direct = consumer.MakeDispatch();
        const StaticArray<GraphUse, 2> uses{{
            storageUse,
            {argumentsInput, GraphAccessMode::Indirect, 0, sizeof(DispatchIndirectArguments)},
        }};
        pass.Dispatch = &direct;
        pass.Uses = uses.GetData();
        pass.UseCount = uses.GetSize();
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
        GraphReport report;
        CHECK(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::InvalidDescription);
    }
    SECTION("A GPU producer orders before the declared dispatch read")
    {
        const ComputeDispatch produce = producer.MakeDispatch();
        const GraphUse produceUse{argumentsOutput,
                                  GraphAccessMode::StorageReadWrite,
                                  0,
                                  STORAGE_BYTES,
                                  RasterVisibility::Compute};
        GraphPassDescription producerPass;
        producerPass.Name = "write group counts";
        producerPass.Dispatch = &produce;
        producerPass.Uses = &produceUse;
        producerPass.UseCount = 1;
        REQUIRE(AddGraphPass(session.Device, graph, producerPass) == RasterStatus::Ready);
        const StaticArray<GraphUse, 2> uses{{
            storageUse,
            {argumentsOutput, GraphAccessMode::Indirect, DISPATCH_OFFSET, sizeof(DispatchIndirectArguments)},
        }};
        pass.Uses = uses.GetData();
        pass.UseCount = uses.GetSize();
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
        GraphReport report;
        REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
        CHECK(report.ComputePasses == 2);
        CHECK(HasDependency(report, GraphHazard::ReadAfterWrite, GraphAccessMode::Indirect));
        SubmissionToken token;
        REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::Ready);
        CHECK(reference::Dispatches == 2);
        CHECK(reference::DispatchPacket.Indirect != internal::RASTER_INVALID);
        CHECK(reference::DispatchPacket.IndirectOffset == DISPATCH_OFFSET);
    }
}
