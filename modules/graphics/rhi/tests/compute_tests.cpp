#include "internal/lifecycle.h"
#include "reference_raster.h"

#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>

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
struct ComputeSession final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    ComputeSession()
    {
        Shutdown();
        reference::Reset();
        backend::ImmediateError = StartupError::None;
        backend::NextFrame = FrameStatus::Ready;
        backend::ActiveKind = Backend::WebGPU;
        backend::SelectionSupported = true;
        DeviceDescription description;
        description.Required.Compute = true;
        REQUIRE(CreateDevice({}, {}, description, Device, Surface) == DeviceStatus::Pending);
        internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
        DeviceInfo info;
        REQUIRE(GetDeviceInfo(Device, info) == DeviceStatus::Ready);
        REQUIRE(info.Enabled.Compute);
        REQUIRE(info.Enabled.IndirectRendering);
    }
    ~ComputeSession() noexcept
    {
        Shutdown();
    }
};
struct Kernel final
{
    BufferHandle Buffer;
    RasterShaderHandle Shader;
    BindingLayoutHandle Layout;
    BindingSetHandle Set;
    ComputePipelineHandle Pipeline;
    Kernel(DeviceHandle device, bool write = true, BufferHandle existing = {})
    {
        const uint8 bytes[16]{};
        Buffer = existing;
        if (GetStatus(device, Buffer) == RasterStatus::InvalidHandle)
        {
            REQUIRE(CreateBuffer(device, {BufferRole::Storage, 16}, bytes, Buffer) == RasterStatus::Ready);
        }
        const RasterBinding binding{0,
                                    write ? RasterBindingKind::StorageReadWrite : RasterBindingKind::StorageRead,
                                    RasterVisibility::Compute,
                                    4};
        RasterShaderDescription description;
        description.Artifact.Stage = ShaderStage::Compute;
        description.Artifact.Wgsl = "reference-only compute metadata";
        description.Artifact.WgslEntry = "computeMain";
        description.Bindings = {&binding, 1};
        REQUIRE(CreateRasterShader(device, description, Shader) == RasterStatus::Ready);
        REQUIRE(CreateBindingLayout(device, {&binding, 1}, Layout) == RasterStatus::Ready);
        const RasterBindingResource resource{ .Buffer = Buffer, .Size = 16 };
        REQUIRE(CreateBindingSet(device, Layout, {&resource, 1}, Set) == RasterStatus::Ready);
        REQUIRE(CreateComputePipeline(device, {Shader, Layout}, Pipeline) == RasterStatus::Ready);
    }
    [[nodiscard]] ComputeDispatch Dispatch() const noexcept
    {
        return {Pipeline, Set};
    }
};
void AddDispatch(DeviceHandle device,
                 OrderedGraph graph,
                 const Kernel& kernel,
                 GraphVersion version,
                 bool write,
                 bool sideEffect = false)
{
    const auto dispatch = kernel.Dispatch();
    const GraphUse use{version,
                       write ? GraphAccessMode::StorageReadWrite : GraphAccessMode::StorageRead,
                       0,
                       16,
                       RasterVisibility::Compute};
    GraphPassDescription pass;
    pass.Name = write ? "compute write" : "compute read";
    pass.Dispatch = &dispatch;
    pass.Uses = &use;
    pass.UseCount = 1;
    pass.SideEffect = sideEffect;
    REQUIRE(AddGraphPass(device, graph, pass) == RasterStatus::Ready);
}
OrderedGraph Plan(DeviceHandle device, const Kernel& kernel)
{
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(device, graph) == RasterStatus::Ready);
    GraphVersion input, output;
    REQUIRE(ImportGraphBuffer(device, graph, kernel.Buffer, input) == RasterStatus::Ready);
    REQUIRE(NextGraphVersion(device, graph, input, output) == RasterStatus::Ready);
    AddDispatch(device, graph, kernel, output, true, true);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(device, graph, report) == RasterStatus::Ready);
    REQUIRE(report.LivePasses == 1);
    return graph;
}
} // namespace
TEST_CASE("Compute admission negotiates support and rejects the WebGL workload before startup", "[rhi][compute]")
{
    ComputeSession session;
    ComputeCapabilities limits;
    REQUIRE(GetComputeCapabilities(session.Device, limits) == RasterStatus::Ready);
    CHECK(limits.MaxWorkgroupInvocations == 256);
    CHECK(limits.MaxDispatch[0] == 65535);
    const auto preserved = limits.MaxStorageRange;
    CHECK(GetComputeCapabilities({}, limits) == RasterStatus::InvalidHandle);
    CHECK(limits.MaxStorageRange == preserved);
    REQUIRE(DestroyDevice(session.Device) == DeviceStatus::Ready);
    session.Surface = {};
    DeviceDescription required;
    required.Required.Compute = true;
    required.Selection = BackendSelection::WebGL2;
    CHECK(CreateDevice({}, {}, required, session.Device, session.Surface) == DeviceStatus::Unsupported);
    required.Selection = BackendSelection::Auto;
    reference::ComputeAvailable = false;
    REQUIRE(CreateDevice({}, {}, required, session.Device, session.Surface) == DeviceStatus::Pending);
    internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
    DeviceInfo info;
    CHECK(GetDeviceInfo(session.Device, info) == DeviceStatus::Failed);
    CHECK(info.Startup.UnmetRequirement == RequirementFailure::Compute);
    CHECK(reference::Dispatches == 0);
}
TEST_CASE("Compute layouts and workgroup limits reject unsupported contracts without allocation", "[rhi][compute]")
{
    ComputeSession session;
    RasterShaderHandle shader;
    RasterShaderDescription description;
    description.Artifact.Stage = ShaderStage::Compute;
    description.Artifact.Wgsl = "fixture";
    description.Artifact.WgslEntry = "computeMain";
    description.WorkgroupSize[0] = 257;
    CHECK(CreateRasterShader(session.Device, description, shader) == RasterStatus::InvalidDescription);
    description.WorkgroupSize[0] = 16;
    description.WorkgroupSize[1] = 17;
    CHECK(CreateRasterShader(session.Device, description, shader) == RasterStatus::InvalidDescription);
    description.WorkgroupSize[1] = 0;
    CHECK(CreateRasterShader(session.Device, description, shader) == RasterStatus::InvalidDescription);
    CHECK(reference::Creates[static_cast<usize>(internal::RasterKind::Shader)] == 0);
    RasterBinding bindings[5];
    for (uint32 i = 0; i < 5; ++i)
    {
        bindings[i] = {i, RasterBindingKind::StorageReadWrite, RasterVisibility::Compute, 4};
    }
    BindingLayoutHandle layout;
    REQUIRE(CreateBindingLayout(session.Device, bindings, layout) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, layout) == RasterStatus::Ready);
    bindings[0].Visibility = RasterVisibility::Fragment;
    CHECK(CreateBindingLayout(session.Device, {bindings, 1}, layout) == RasterStatus::InvalidDescription);
    bindings[0].Kind = RasterBindingKind::StorageRead;
    REQUIRE(CreateBindingLayout(session.Device, {bindings, 1}, layout) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, layout) == RasterStatus::Ready);
    bindings[0].Visibility = RasterVisibility::Vertex;
    CHECK(CreateBindingLayout(session.Device, {bindings, 1}, layout) == RasterStatus::InvalidDescription);
    Kernel kernel(session.Device);
    const RasterBindingResource wrong{ .Buffer = kernel.Buffer, .Offset = 4, .Size = 4 };
    BindingSetHandle set;
    CHECK(CreateBindingSet(session.Device, kernel.Layout, {&wrong, 1}, set) == RasterStatus::InvalidDescription);
}
TEST_CASE("Compute buffer versions expose RAW WAR WAW and cull an unobservable reader", "[rhi][compute]")
{
    ComputeSession session;
    Kernel writer(session.Device);
    Kernel reader(session.Device, false, writer.Buffer);
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphVersion input, first, second;
    REQUIRE(ImportGraphBuffer(session.Device, graph, writer.Buffer, input) == RasterStatus::Ready);
    REQUIRE(NextGraphVersion(session.Device, graph, input, first) == RasterStatus::Ready);
    REQUIRE(NextGraphVersion(session.Device, graph, first, second) == RasterStatus::Ready);
    AddDispatch(session.Device, graph, writer, first, true);
    AddDispatch(session.Device, graph, reader, first, false);
    AddDispatch(session.Device, graph, writer, second, true);
    REQUIRE(AddGraphRoot(session.Device, graph, second, GraphRoot::History) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    CHECK(report.LivePasses == 2);
    CHECK(report.Culled[1]);
    bool raw = false, war = false, waw = false;
    for (uint32 i = 0; i < report.DependencyCount; ++i)
    {
        raw |= report.Dependencies[i].Hazard == GraphHazard::ReadAfterWrite;
        war |= report.Dependencies[i].Hazard == GraphHazard::WriteAfterRead;
        waw |= report.Dependencies[i].Hazard == GraphHazard::WriteAfterWrite;
    }
    CHECK(raw);
    CHECK(war);
    CHECK(waw);
    SubmissionToken token;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::Ready);
    CHECK(reference::Dispatches == 2);
}
TEST_CASE("Compute preflight rejects missing declarations and excessive dispatch before GPU work", "[rhi][compute]")
{
    ComputeSession session;
    Kernel kernel(session.Device);
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    auto dispatch = kernel.Dispatch();
    GraphPassDescription pass;
    pass.Name = "undeclared dispatch";
    pass.Dispatch = &dispatch;
    pass.SideEffect = true;
    dispatch.Groups[0] = 65536;
    CHECK(AddGraphPass(session.Device, graph, pass) == RasterStatus::InvalidDescription);
    dispatch.Groups[0] = 1;
    REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    GraphReport report;
    CHECK(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::InvalidDescription);
    CHECK(report.ErrorPass == 0);
    CHECK(reference::Dispatches == 0);
    CHECK(reference::Passes == 0);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
}
TEST_CASE("Compute imported revisions freeze accepted writes while skip and capacity preserve retry", "[rhi][compute]")
{
    ComputeSession session;
    Kernel kernel(session.Device);
    auto first = Plan(session.Device, kernel);
    auto stale = Plan(session.Device, kernel);
    SubmissionToken token;
    backend::NextFrame = FrameStatus::Skipped;
    CHECK(ExecuteOrderedGraph(session.Device, session.Surface, first, token) == RasterStatus::NotReady);
    CHECK(reference::Dispatches == 0);
    backend::NextFrame = FrameStatus::Ready;
    reference::Submission = RasterStatus::CapacityExceeded;
    CHECK(ExecuteOrderedGraph(session.Device, session.Surface, first, token) == RasterStatus::CapacityExceeded);
    CHECK(reference::Dispatches == 0);
    reference::Submission = RasterStatus::Ready;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, first, token) == RasterStatus::Ready);
    token = {};
    CHECK(ExecuteOrderedGraph(session.Device, session.Surface, stale, token) == RasterStatus::InvalidState);
    CHECK(reference::Dispatches == 1);
    REQUIRE(DiscardOrderedGraph(session.Device, stale) == RasterStatus::Ready);
}
TEST_CASE("Compute leases survive public destruction and partial encoding faults the session", "[rhi][compute]")
{
    ComputeSession session;
    Kernel kernel(session.Device);
    auto graph = Plan(session.Device, kernel);
    const auto stale = kernel.Pipeline;
    REQUIRE(Destroy(session.Device, kernel.Pipeline) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, kernel.Set) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, kernel.Buffer) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
    SubmissionToken token;
    SECTION("Completion protects a detached compute pipeline")
    {
        REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::Ready);
        CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::ComputePipeline)] == 0);
        reference::Completed = reference::Submitted;
        REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
        CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::ComputePipeline)] == 1);
    }
    SECTION("Backend failure cannot masquerade as rollback")
    {
        reference::ComputeResult = RasterStatus::Failed;
        CHECK(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::Failed);
        DeviceInfo info;
        CHECK(GetDeviceInfo(session.Device, info) == DeviceStatus::Failed);
        CHECK(reference::Dispatches == 1);
        CHECK(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Failed);
    }
}

TEST_CASE("R11 compute offsets are copied and exact storage ranges are declared", "[rhi][compute][binding]")
{
    ComputeSession session;
    constexpr usize BUFFER_BYTES = 512;
    const uint8 bytes[BUFFER_BYTES]{};
    BufferHandle buffer;
    REQUIRE(CreateBuffer(session.Device, { .Role = BufferRole::Storage, .Size = sizeof(bytes) }, bytes, buffer) ==
            RasterStatus::Ready);
    Kernel kernel(session.Device, true, buffer);
    REQUIRE(Destroy(session.Device, kernel.Pipeline) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, kernel.Set) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, kernel.Layout) == RasterStatus::Ready);
    const RasterBinding binding
    {
        .Kind = RasterBindingKind::StorageReadWrite,
        .Visibility = RasterVisibility::Compute,
        .MinSize = 4,
        .HasDynamicOffset = true,
    };
    REQUIRE(CreateBindingLayout(session.Device, {&binding, 1}, kernel.Layout) == RasterStatus::Ready);
    const RasterBindingResource resource{ .Buffer = buffer, .Size = 16 };
    REQUIRE(CreateBindingSet(session.Device, kernel.Layout, {&resource, 1}, kernel.Set) == RasterStatus::Ready);
    REQUIRE(CreateComputePipeline(session.Device,
                                  { .Shader = kernel.Shader, .Layout = kernel.Layout },
                                  kernel.Pipeline) == RasterStatus::Ready);
    OrderedGraph graph;
    GraphVersion input;
    GraphVersion output;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, buffer, input) == RasterStatus::Ready);
    REQUIRE(NextGraphVersion(session.Device, graph, input, output) == RasterStatus::Ready);
    ComputeDispatch dispatch = kernel.Dispatch();
    dispatch.BindingOffsets.Bytes[0] = 1;
    const GraphUse use
    {
        .Resource = output,
        .Access = GraphAccessMode::StorageReadWrite,
        .Offset = 256,
        .Size = 16,
        .Visibility = RasterVisibility::Compute,
    };
    const GraphPassDescription pass
    {
        .Name = "dynamic storage",
        .Attachment = {},
        .Uses = &use,
        .UseCount = 1,
        .SideEffect = true,
        .Dispatch = &dispatch,
    };
    CHECK(AddGraphPass(session.Device, graph, pass) == RasterStatus::InvalidDescription);
    dispatch.BindingOffsets.Bytes[0] = 256;
    REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    dispatch.BindingOffsets.Bytes[0] = 0;
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    SubmissionToken submission;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, submission) == RasterStatus::Ready);
    CHECK(reference::DispatchPacket.DynamicOffsetCount == 1);
    CHECK(reference::DispatchPacket.DynamicOffsets[0] == 256);
}

TEST_CASE("R18c layout entry and per-stage descriptor limits reject before publication", "[rhi][compute][r18c]")
{
    ComputeSession session;
    constexpr usize ENTRY_COUNT = 16;
    // Elements use their default member initializers; the check misreports them as zero enums.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    ludus::foundation::StaticArray<RasterBinding, ENTRY_COUNT> bindings{};
    for (usize entry = 0; entry < ENTRY_COUNT; ++entry)
    {
        bindings[entry] =
        {
            .Binding = static_cast<uint32>(entry),
            .Kind = RasterBindingKind::StorageRead,
            .Visibility = RasterVisibility::Compute,
            .MinSize = 4,
        };
    }
    BindingLayoutHandle layout;
    REQUIRE(CreateBindingLayout(session.Device, {bindings.GetData(), 8}, layout) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, layout) == RasterStatus::Ready);
    StorageUsage before;
    REQUIRE(GetStorageUsage(session.Device, before) == RasterStatus::Ready);
    CHECK(CreateBindingLayout(session.Device, {bindings.GetData(), 9}, layout) == RasterStatus::Unsupported);
    CHECK(GetStatus(session.Device, layout) == RasterStatus::InvalidHandle);
    for (RasterBinding& binding : bindings)
    {
        binding.Kind = RasterBindingKind::UniformBuffer;
        binding.MinSize = 16;
    }
    CHECK(CreateBindingLayout(session.Device, {bindings.GetData(), 13}, layout) == RasterStatus::Unsupported);
    CHECK(GetStatus(session.Device, layout) == RasterStatus::InvalidHandle);
    // Stage visibility is counted independently: twelve fragment and four vertex uniforms.
    for (usize entry = 0; entry < ENTRY_COUNT; ++entry)
    {
        bindings[entry].Visibility = entry < 12 ? RasterVisibility::Fragment : RasterVisibility::Vertex;
    }
    REQUIRE(CreateBindingLayout(session.Device, bindings, layout) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, layout) == RasterStatus::Ready);
    reference::BindingLimitOverride = backend::BindingLimits();
    reference::BindingLimitOverride.MaxUniformBuffersPerSet = 12;
    reference::HasBindingLimitOverride = true;
    CHECK(CreateBindingLayout(session.Device, bindings, layout) == RasterStatus::Unsupported);
    CHECK(GetStatus(session.Device, layout) == RasterStatus::InvalidHandle);
    reference::HasBindingLimitOverride = false;
    bindings[15].Visibility = RasterVisibility::Both;
    CHECK(CreateBindingLayout(session.Device, bindings, layout) == RasterStatus::Unsupported);
    CHECK(GetStatus(session.Device, layout) == RasterStatus::InvalidHandle);
    bindings[15].Binding = 16;
    CHECK(CreateBindingLayout(session.Device, bindings, layout) == RasterStatus::InvalidDescription);
    CHECK(GetStatus(session.Device, layout) == RasterStatus::InvalidHandle);
    StorageUsage after;
    REQUIRE(GetStorageUsage(session.Device, after) == RasterStatus::Ready);
    CHECK(after.ResourceBytes == before.ResourceBytes);
    CHECK(reference::Creates[static_cast<usize>(internal::RasterKind::Layout)] == 2);
}
