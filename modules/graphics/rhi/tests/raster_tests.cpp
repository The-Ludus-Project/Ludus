#include "internal/lifecycle.h"
#include "internal/profile.h"
#include "reference_raster.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <cstring>
#include <span>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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
struct Session final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    explicit Session(const StorageLimits& storage = {},
                     uint32 maxTextureDimension = 4096,
                     Backend activeBackend = Backend::WebGPU)
    {
        Shutdown();
        reference::Reset();
        backend::ImmediateError = StartupError::None;
        backend::NextFrame = FrameStatus::Ready;
        backend::ActiveKind = activeBackend;
        backend::SelectionSupported = true;
        if (maxTextureDimension != 4096)
        {
            reference::RasterLimitOverride = backend::RasterLimits();
            reference::RasterLimitOverride.MaxTextureDimension2D = maxTextureDimension;
            reference::OverrideRasterLimits = true;
        }
        DeviceDescription description;
        description.Storage = storage;
        REQUIRE(CreateDevice({}, {}, description, Device, Surface) == DeviceStatus::Pending);
        internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
    }
    ~Session() noexcept
    {
        Shutdown();
    }
};
template <typename T, usize N>
std::span<const uint8> Bytes(const T (&data)[N]) noexcept
{
    return {reinterpret_cast<const uint8*>(data), sizeof(data)};
}
struct Scene final
{
    RasterShaderHandle Vertex, Fragment;
    BindingLayoutHandle Layout;
    BindingSetHandle Set;
    RasterPipelineHandle Pipeline;
    BufferHandle Uniform, Vertices, Indices;
    RasterVertexSlice Slices[1];
    Scene(DeviceHandle device)
    {
        const RasterBinding binding{0, RasterBindingKind::UniformBuffer, RasterVisibility::Both, 16};
        const RasterShaderInput input{0, RasterVertexFormat::Float3};
        RasterShaderDescription vertex;
        vertex.Artifact.Stage = ShaderStage::Vertex;
        vertex.Artifact.Wgsl = "fixture vertex";
        vertex.Artifact.WgslEntry = "vertexMain";
        vertex.Inputs = {&input, 1};
        vertex.Bindings = {&binding, 1};
        REQUIRE(CreateRasterShader(device, vertex, Vertex) == RasterStatus::Ready);
        auto fragment = vertex;
        fragment.Inputs = {};
        fragment.Artifact.Stage = ShaderStage::Fragment;
        fragment.Artifact.WgslEntry = "fragmentMain";
        REQUIRE(CreateRasterShader(device, fragment, Fragment) == RasterStatus::Ready);
        REQUIRE(CreateBindingLayout(device, {&binding, 1}, Layout) == RasterStatus::Ready);
        const uint8 uniform[16]{};
        REQUIRE(CreateBuffer(device, {BufferRole::Uniform, sizeof(uniform)}, uniform, Uniform) == RasterStatus::Ready);
        const RasterBindingResource resource{ .Buffer = Uniform, .Size = sizeof(uniform) };
        REQUIRE(CreateBindingSet(device, Layout, {&resource, 1}, Set) == RasterStatus::Ready);
        const RasterVertexStream stream{12, false};
        const RasterVertexAttribute attribute{0, 0, 0, RasterVertexFormat::Float3};
        REQUIRE(CreateRasterPipeline(device,
                                     {Vertex, Fragment, Layout, {&stream, 1}, {&attribute, 1}, true},
                                     Pipeline) == RasterStatus::Ready);
        const float32 vertices[9]{};
        const uint16 indices[3]{0, 1, 2};
        REQUIRE(CreateBuffer(device, {BufferRole::Vertex, sizeof(vertices)}, Bytes(vertices), Vertices) ==
                RasterStatus::Ready);
        REQUIRE(CreateBuffer(device, {BufferRole::Index16, sizeof(indices)}, Bytes(indices), Indices) ==
                RasterStatus::Ready);
        Slices[0] = {Vertices, 0, sizeof(vertices)};
    }
    [[nodiscard]] RasterDraw Draw() const noexcept
    {
        return {Pipeline, Set, Slices, Indices, 0, 3, 3, 1};
    }
};
} // namespace
TEST_CASE("Portable resource admission preserves outputs and checks packing before backend calls", "[rhi][raster]")
{
    Session session;
    const uint8 bytes[16]{};
    BufferHandle buffer;
    CHECK(CreateBuffer({}, {BufferRole::Uniform, 16}, bytes, buffer) == RasterStatus::InvalidHandle);
    CHECK(CreateBuffer(session.Device, {BufferRole::Uniform, 17}, bytes, buffer) == RasterStatus::InvalidDescription);
    CHECK(reference::Creates[0] == 0);
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, buffer) == RasterStatus::Ready);
    CHECK(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, buffer) == RasterStatus::InvalidState);
    CHECK(reference::Creates[0] == 1);
    TextureHandle texture;
    CHECK(CreateTexture(session.Device, {2, 2}, {bytes, 7}, texture) == RasterStatus::InvalidDescription);
    CHECK(CreateTexture(session.Device, {2, 2}, {bytes, ~usize{0} - 3}, texture) == RasterStatus::InvalidDescription);
    REQUIRE(CreateTexture(session.Device, {2, 2}, {bytes, 8}, texture) == RasterStatus::Ready);
    TextureViewHandle view;
    REQUIRE(CreateTextureView(session.Device, texture, view) == RasterStatus::Ready);
    auto stale = texture;
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
    CHECK(reference::Destroys[1] == 0);
    REQUIRE(Destroy(session.Device, view) == RasterStatus::Ready);
    CHECK(reference::Destroys[1] == 1);
    CHECK(reference::Destroys[2] == 1);
}
TEST_CASE("Portable bindings retain detached resources and reject range/layout mismatch", "[rhi][raster]")
{
    Session session;
    Scene scene(session.Device);
    const auto stale = scene.Uniform;
    REQUIRE(Destroy(session.Device, scene.Uniform) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
    CHECK(reference::Destroys[0] == 0);
    const RasterBinding duplicate[2]{{0, RasterBindingKind::UniformBuffer, RasterVisibility::Both, 16},
                                     {0, RasterBindingKind::Sampler, RasterVisibility::Both, 0}};
    BindingLayoutHandle layout;
    CHECK(CreateBindingLayout(session.Device, duplicate, layout) == RasterStatus::InvalidDescription);
    const RasterBinding different{0, RasterBindingKind::UniformBuffer, RasterVisibility::Both, 16};
    REQUIRE(CreateBindingLayout(session.Device, {&different, 1}, layout) == RasterStatus::Ready);
    BindingSetHandle set;
    RasterBindingResource bad{ .Buffer = scene.Vertices, .Offset = 1, .Size = 16 };
    CHECK(CreateBindingSet(session.Device, layout, {&bad, 1}, set) == RasterStatus::InvalidDescription);
    bad.Buffer = stale;
    CHECK(CreateBindingSet(session.Device, layout, {&bad, 1}, set) == RasterStatus::InvalidHandle);
    REQUIRE(Destroy(session.Device, scene.Set) == RasterStatus::Ready);
    CHECK(reference::Destroys[0] == 1);
}
TEST_CASE("Portable pending callbacks cannot resurrect cancelled or old-session resources", "[rhi][raster]")
{
    Session session;
    const uint8 bytes[16]{};
    BufferHandle buffer;
    reference::Next = RasterStatus::Pending;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, buffer) == RasterStatus::Pending);
    const auto oldRequest = reference::LastRequest;
    const auto stale = buffer;
    REQUIRE(Destroy(session.Device, buffer) == RasterStatus::Ready);
    CHECK(reference::Destroys[0] == 0);
    internal::RasterComplete(oldRequest, RasterStatus::Failed);
    reference::Next = RasterStatus::Ready;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, buffer) == RasterStatus::Ready);
    CHECK(reference::Destroys[0] == 1);
    internal::RasterComplete(oldRequest, RasterStatus::Ready);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
    CHECK(GetStatus(session.Device, buffer) == RasterStatus::Ready);
}
TEST_CASE("Portable draw preflight rejects invalid packets before native calls", "[rhi][raster]")
{
    Session session;
    Scene scene(session.Device);
    auto draw = scene.Draw();
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidState);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    draw.VertexCount = 2;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    draw = scene.Draw();
    draw.IndexOffset = 1;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    draw = scene.Draw();
    draw.InstanceCount = 0;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    draw = scene.Draw();
    scene.Slices[0].Size = 35;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    scene.Slices[0].Size = 36;
    reference::Submission = RasterStatus::CapacityExceeded;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::CapacityExceeded);
    CHECK(reference::Draws == 0);
    reference::Submission = RasterStatus::Ready;
    REQUIRE(DrawIndexed(session.Device, draw) == RasterStatus::Ready);
    CHECK(reference::Draws == 1);
    CHECK(Destroy(session.Device, scene.Indices) == RasterStatus::InvalidState);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}
TEST_CASE("Portable logical destruction waits for CPU leases and true GPU completion", "[rhi][raster]")
{
    Session session;
    Scene scene(session.Device);
    const auto draw = scene.Draw();
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(DrawIndexed(session.Device, draw) == RasterStatus::Ready);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    REQUIRE(reference::Submitted != 0);
    REQUIRE(Destroy(session.Device, scene.Pipeline) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Set) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Uniform) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Vertices) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Indices) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Vertex) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Fragment) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Layout) == RasterStatus::Ready);
    for (auto count : reference::Destroys)
    {
        CHECK(count == 0);
    }
    reference::Completed = reference::Submitted;
    SamplerHandle sampler;
    REQUIRE(CreateSampler(session.Device, {}, sampler) == RasterStatus::Ready);
    CHECK(reference::Destroys[0] == 3);
    CHECK(reference::Destroys[4] == 2);
    CHECK(reference::Destroys[5] == 1);
    CHECK(reference::Destroys[6] == 1);
    CHECK(reference::Destroys[7] == 1);
}

TEST_CASE("Portable identities exhaust without wrapping or becoming live again", "[rhi][raster]")
{
    internal::RasterSequence<uint32> requests{~uint32{0} - 1};
    CHECK(requests.Take() == ~uint32{0} - 1);
    CHECK(requests.Take() == ~uint32{0});
    CHECK(requests.Take() == 0);
    CHECK(requests.Take() == 0);
    internal::RasterSequence<uint64> submissions{~uint64{0}};
    CHECK(submissions.Take() == ~uint64{0});
    CHECK(submissions.Take() == 0);
    CHECK(internal::RasterNextGeneration(~uint64{0} - 1) == ~uint64{0});
    CHECK(internal::RasterNextGeneration(~uint64{0}) == 0);
    CHECK(internal::RasterNextGeneration(0) == 0);
}
TEST_CASE("Failed asynchronous pipeline dependencies preserve their failure result", "[rhi][raster]")
{
    Session session;
    Scene scene(session.Device);
    reference::Next = RasterStatus::Pending;
    const RasterVertexStream stream{12, false};
    const RasterVertexAttribute attribute{0, 0, 0, RasterVertexFormat::Float3};
    RasterPipelineHandle pipeline;
    REQUIRE(CreateRasterPipeline(session.Device,
                                 {scene.Vertex, scene.Fragment, scene.Layout, {&stream, 1}, {&attribute, 1}, true},
                                 pipeline) == RasterStatus::Pending);
    const auto request = reference::LastRequest;
    internal::RasterExpect(request, internal::RasterCallbacks::Two);
    internal::RasterFail(request, RasterStatus::OutOfMemory);
    REQUIRE(GetStatus(session.Device, pipeline) == RasterStatus::OutOfMemory);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    auto draw = scene.Draw();
    draw.Pipeline = pipeline;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::OutOfMemory);
    CHECK(reference::Draws == 0);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    REQUIRE(Destroy(session.Device, pipeline) == RasterStatus::Ready);
    CHECK(reference::Destroys[7] == 0);
    internal::RasterComplete(request, RasterStatus::Ready);
    internal::RasterComplete(request, RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Pipeline) == RasterStatus::Ready);
    CHECK(reference::Destroys[7] == 2);
}
TEST_CASE("Portable admission verifies negotiated profile before readiness", "[rhi][raster]")
{
    Session session;
    REQUIRE(DestroyDevice(session.Device) == DeviceStatus::Ready);
    session.Surface = {};
    reference::ProfileAvailable = false;
    DeviceDescription description;
    description.Required.PortableRaster = true;
    REQUIRE(ValidateDeviceDescription(description) == DeviceStatus::Ready);
    REQUIRE(CreateDevice({}, {}, description, session.Device, session.Surface) == DeviceStatus::Pending);
    internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
    DeviceInfo info;
    CHECK(GetDeviceInfo(session.Device, info) == DeviceStatus::Failed);
    CHECK(info.Startup.Error == StartupError::RequirementsUnsatisfied);
    CHECK(info.Startup.UnmetRequirement == RequirementFailure::PortableRaster);
    CHECK_FALSE(info.Enabled.PortableRaster);
}
TEST_CASE("Pending dependencies publish no handles and multiple validation scopes must drain", "[rhi][raster]")
{
    Session session;
    const uint8 image[4]{};
    TextureHandle texture;
    TextureViewHandle view;
    reference::Next = RasterStatus::Pending;
    REQUIRE(CreateTexture(session.Device, {1, 1}, {image, 4}, texture) == RasterStatus::Pending);
    const auto request = reference::LastRequest;
    internal::RasterExpect(request, internal::RasterCallbacks::Two);
    CHECK(CreateTextureView(session.Device, texture, view) == RasterStatus::NotReady);
    internal::RasterComplete(request, RasterStatus::Ready);
    CHECK(GetStatus(session.Device, texture) == RasterStatus::Pending);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    SamplerHandle sampler;
    reference::Next = RasterStatus::Ready;
    REQUIRE(CreateSampler(session.Device, {}, sampler) == RasterStatus::Ready);
    CHECK(reference::Destroys[1] == 0);
    internal::RasterComplete(request, RasterStatus::OutOfMemory);
    REQUIRE(Destroy(session.Device, sampler) == RasterStatus::Ready);
    CHECK(reference::Destroys[1] == 1);
}
TEST_CASE("Portable pool capacity and owner restart preserve stale handle rejection", "[rhi][raster]")
{
    Session session;
    SamplerHandle samplers[internal::RASTER_CAPACITY];
    for (auto& sampler : samplers)
    {
        REQUIRE(CreateSampler(session.Device, {}, sampler) == RasterStatus::Ready);
    }
    SamplerHandle extra;
    CHECK(CreateSampler(session.Device, {}, extra) == RasterStatus::CapacityExceeded);
    const auto stale = samplers[0];
    const auto owner = session.Device;
    REQUIRE(Destroy(session.Device, samplers[0]) == RasterStatus::Ready);
    REQUIRE(CreateSampler(session.Device, {}, extra) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
    REQUIRE(DestroyDevice(session.Device) == DeviceStatus::Ready);
    session.Surface = {};
    REQUIRE(CreateDevice({}, {}, {}, session.Device, session.Surface) == DeviceStatus::Pending);
    internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
    CHECK(GetStatus(owner, extra) == RasterStatus::InvalidHandle);
    CHECK(GetStatus(session.Device, extra) == RasterStatus::InvalidHandle);
}
TEST_CASE("Fixed restart indices and forged layout substitution issue no native draw", "[rhi][raster]")
{
    Session session;
    Scene scene(session.Device);
    const uint16 values[3]{0, 1, 65535};
    BufferHandle indices;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Index16, sizeof(values)}, Bytes(values), indices) ==
            RasterStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    auto draw = scene.Draw();
    draw.Indices = indices;
    draw.VertexCount = 65536;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    CHECK(reference::Draws == 0);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    const RasterBinding binding{0, RasterBindingKind::UniformBuffer, RasterVisibility::Both, 16};
    BindingLayoutHandle equivalent;
    BindingSetHandle other;
    REQUIRE(CreateBindingLayout(session.Device, {&binding, 1}, equivalent) == RasterStatus::Ready);
    const RasterBindingResource resource{ .Buffer = scene.Uniform, .Size = 16 };
    REQUIRE(CreateBindingSet(session.Device, equivalent, {&resource, 1}, other) == RasterStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    draw = scene.Draw();
    draw.Bindings = other;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    CHECK(reference::Draws == 0);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}

TEST_CASE("Indexed indirect packets reject wrong roles, misalignment, truncation and stale owners", "[rhi][compute]")
{
    Session session;
    Scene scene(session.Device);
    const uint8 bytes[20]{};
    BufferHandle arguments;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::StorageIndirect, sizeof(bytes)}, bytes, arguments) ==
            RasterStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    auto draw = scene.Draw();
    draw.Indirect = scene.Uniform;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    draw.Indirect = arguments;
    draw.IndirectOffset = 1;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    draw.IndirectOffset = 4;
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    CHECK(reference::Draws == 0);
    draw.IndirectOffset = 0;
    REQUIRE(DrawIndexed(session.Device, draw) == RasterStatus::Ready);
    CHECK(reference::Draws == 1);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    REQUIRE(Destroy(session.Device, arguments) == RasterStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidHandle);
    CHECK(reference::Draws == 1);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}

TEST_CASE("Completion-observed device loss closes resource admission before native work", "[rhi][raster]")
{
    Session session;
    const uint8 bytes[16]{};
    BufferHandle buffer;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, buffer) == RasterStatus::Ready);
    reference::LossOnCompletion = backend::PendingToken;
    SECTION("Polling returns the owning device loss")
    {
        CHECK(GetStatus(session.Device, buffer) == RasterStatus::DeviceLost);
    }
    SECTION("Creation cannot call the torn-down backend")
    {
        SamplerHandle sampler;
        CHECK(CreateSampler(session.Device, {}, sampler) == RasterStatus::DeviceLost);
        CHECK(reference::Creates[3] == 0);
    }
    DeviceInfo info;
    CHECK(GetDeviceInfo(session.Device, info) == DeviceStatus::DeviceLost);
    CHECK(reference::Destroys[0] == 1);
    REQUIRE(DestroyDevice(session.Device) == DeviceStatus::Ready);
}
TEST_CASE("Retained batches survive detached resources until discard or GPU completion", "[rhi][lifetime]")
{
    Session session;
    Scene scene(session.Device);
    CommandBatch batch;
    REQUIRE(BeginCommands(session.Device, batch) == RasterStatus::Ready);
    REQUIRE(RecordDraw(session.Device, batch, scene.Draw()) == RasterStatus::Ready);
    REQUIRE(reference::Draws == 0);
    REQUIRE(Destroy(session.Device, scene.Pipeline) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Set) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Uniform) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Vertices) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Indices) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Vertex) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Fragment) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Layout) == RasterStatus::Ready);
    for (auto count : reference::Destroys)
    {
        CHECK(count == 0);
    }
    REQUIRE(FinishCommands(session.Device, batch) == RasterStatus::Ready);
    CHECK(FinishCommands(session.Device, batch) == RasterStatus::InvalidState);
    CHECK(RecordDraw(session.Device, batch, scene.Draw()) == RasterStatus::InvalidState);
    auto stale = batch;
    SECTION("Discard releases the dependency graph without GPU work")
    {
        REQUIRE(DiscardCommands(session.Device, batch) == RasterStatus::Ready);
        CHECK(reference::Draws == 0);
        CHECK(reference::Submitted == 0);
    }
    SECTION("Skipped target preserves work and submission retains it until completion")
    {
        SubmissionToken completion;
        backend::NextFrame = FrameStatus::Skipped;
        CHECK(SubmitCommands(session.Device, session.Surface, batch, completion) == RasterStatus::NotReady);
        CHECK(GetStatus(session.Device, completion) == RasterStatus::InvalidHandle);
        backend::NextFrame = FrameStatus::Ready;
        REQUIRE(SubmitCommands(session.Device, session.Surface, batch, completion) == RasterStatus::Ready);
        CHECK(reference::Draws == 1);
        CHECK(GetStatus(session.Device, completion) == RasterStatus::Pending);
        for (auto count : reference::Destroys)
        {
            CHECK(count == 0);
        }
        reference::Completed = reference::Submitted;
        CHECK(GetStatus(session.Device, completion) == RasterStatus::Ready);
    }
    CHECK(DiscardCommands(session.Device, stale) == RasterStatus::InvalidHandle);
    CHECK(reference::Destroys[0] == 3);
    CHECK(reference::Destroys[4] == 2);
    CHECK(reference::Destroys[5] == 1);
    CHECK(reference::Destroys[6] == 1);
    CHECK(reference::Destroys[7] == 1);
}
TEST_CASE("Batch budgets and clear-only completion reservation preserve retry state", "[rhi][lifetime]")
{
    Session session;
    Scene scene(session.Device);
    LifetimeCapabilities limits;
    REQUIRE(GetLifetimeCapabilities(session.Device, limits) == RasterStatus::Ready);
    CommandBatch batches[4];
    for (auto& batch : batches)
    {
        REQUIRE(BeginCommands(session.Device, batch) == RasterStatus::Ready);
    }
    CommandBatch extra;
    CHECK(BeginCommands(session.Device, extra) == RasterStatus::CapacityExceeded);
    for (uint32 i = 0; i < limits.DrawsPerBatch; ++i)
    {
        REQUIRE(RecordDraw(session.Device, batches[0], scene.Draw()) == RasterStatus::Ready);
    }
    CHECK(RecordDraw(session.Device, batches[0], scene.Draw()) == RasterStatus::CapacityExceeded);
    for (auto& batch : batches)
    {
        REQUIRE(DiscardCommands(session.Device, batch) == RasterStatus::Ready);
    }
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    SubmissionToken first;
    reference::Submission = RasterStatus::CapacityExceeded;
    CHECK(EndFrame(session.Device, first) == RasterStatus::CapacityExceeded);
    CHECK(GetStatus(session.Device, first) == RasterStatus::InvalidHandle);
    reference::Submission = RasterStatus::Ready;
    REQUIRE(EndFrame(session.Device, first) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, first) == RasterStatus::Pending);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    SubmissionToken second;
    REQUIRE(EndFrame(session.Device, second) == RasterStatus::Ready);
    reference::Completed = reference::Submitted;
    CHECK(GetStatus(session.Device, first) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, second) == RasterStatus::Ready);
    CHECK(GetStatus({}, first) == RasterStatus::InvalidHandle);
}
TEST_CASE("Upload rings copy caller bytes and cancelled slots await true completion", "[rhi][lifetime]")
{
    Session session;
    uint8 bytes[16]{1, 2, 3, 4};
    UploadTicket tickets[4];
    for (auto& ticket : tickets)
    {
        REQUIRE(
            RequestBufferUpload(session.Device, {BufferRole::Uniform, sizeof(bytes)}, bytes, sizeof(bytes), ticket) ==
            RasterStatus::Pending);
    }
    bytes[0] = 99;
    UploadTicket extra;
    CHECK(RequestBufferUpload(session.Device, {BufferRole::Uniform, sizeof(bytes)}, bytes, sizeof(bytes), extra) ==
          RasterStatus::CapacityExceeded);
    auto stale = tickets[0];
    REQUIRE(Release(session.Device, tickets[0]) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
    CHECK(reference::Destroys[0] == 0);
    CHECK(RequestBufferUpload(session.Device, {BufferRole::Uniform, sizeof(bytes)}, bytes, sizeof(bytes), extra) ==
          RasterStatus::CapacityExceeded);
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    CHECK(reference::Destroys[0] == 1);
    REQUIRE(RequestBufferUpload(session.Device, {BufferRole::Uniform, sizeof(bytes)}, bytes, sizeof(bytes), extra) ==
            RasterStatus::Pending);
    reference::Transfers[0][1] = RasterStatus::Ready;
    REQUIRE(GetStatus(session.Device, tickets[1]) == RasterStatus::Ready);
    BufferHandle buffer;
    REQUIRE(TakeUploadedBuffer(session.Device, tickets[1], buffer) == RasterStatus::Ready);
    ReadbackTicket readback;
    REQUIRE(RequestBufferReadback(session.Device, buffer, 0, 16, readback) == RasterStatus::Pending);
    reference::Transfers[1][0] = RasterStatus::Ready;
    uint8 result[16]{};
    REQUIRE(CopyReadback(session.Device, readback, result, sizeof(result)) == RasterStatus::Ready);
    CHECK(result[0] == 1);
    REQUIRE(Release(session.Device, readback) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, buffer) == RasterStatus::Ready);
}
TEST_CASE("Upload validation cancellation and memory failures retain the right obligations", "[rhi][lifetime]")
{
    Session session;
    const uint16 indices[3]{0, 1, 2};
    UploadTicket ticket;
    const auto* bytes = reinterpret_cast<const uint8*>(indices);
    CHECK(RequestBufferUpload(session.Device, {BufferRole::Vertex, 6}, bytes, 6, ticket) ==
          RasterStatus::InvalidDescription);
    reference::TransferStart = RasterStatus::OutOfMemory;
    CHECK(RequestBufferUpload(session.Device, {BufferRole::Index16, 6}, bytes, 6, ticket) == RasterStatus::OutOfMemory);
    CHECK(GetStatus(session.Device, ticket) == RasterStatus::InvalidHandle);
    reference::TransferStart = RasterStatus::Ready;
    reference::Next = RasterStatus::Pending;
    REQUIRE(RequestBufferUpload(session.Device, {BufferRole::Index16, 6}, bytes, 6, ticket) == RasterStatus::Pending);
    const auto request = reference::LastRequest;
    REQUIRE(Release(session.Device, ticket) == RasterStatus::Ready);
    const auto destroyed = reference::Destroys[0];
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    CHECK(reference::Destroys[0] == destroyed);
    internal::RasterComplete(request, RasterStatus::OutOfMemory);
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    CHECK(reference::Destroys[0] == destroyed + 1);
    reference::Next = RasterStatus::Ready;
    REQUIRE(RequestBufferUpload(session.Device, {BufferRole::Index16, 6}, bytes, 6, ticket) == RasterStatus::Pending);
    reference::Transfers[0][0] = RasterStatus::Ready;
    BufferHandle buffer;
    REQUIRE(TakeUploadedBuffer(session.Device, ticket, buffer) == RasterStatus::Ready);
    CHECK(reference::BufferSizes[0] == 8);
    CHECK(reference::BufferBytes[0][6] == 0);
    CHECK(reference::BufferBytes[0][7] == 0);
}
TEST_CASE("Readback owns detached sources and preserves output on pending and failed copies", "[rhi][lifetime]")
{
    Session session;
    const uint8 bytes[16]{11, 22, 33, 44};
    BufferHandle source;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, source) == RasterStatus::Ready);
    ReadbackTicket tickets[4];
    CHECK(RequestBufferReadback(session.Device, source, 1, 4, tickets[0]) == RasterStatus::InvalidDescription);
    CHECK(RequestBufferReadback(session.Device, source, 0, 20, tickets[0]) == RasterStatus::InvalidDescription);
    for (auto& ticket : tickets)
    {
        REQUIRE(RequestBufferReadback(session.Device, source, 0, 16, ticket) == RasterStatus::Pending);
    }
    ReadbackTicket extra;
    CHECK(RequestBufferReadback(session.Device, source, 0, 16, extra) == RasterStatus::CapacityExceeded);
    REQUIRE(Destroy(session.Device, source) == RasterStatus::Ready);
    CHECK(reference::Destroys[0] == 0);
    uint8 output[16]{99};
    CHECK(CopyReadback(session.Device, tickets[0], output, sizeof(output)) == RasterStatus::Pending);
    CHECK(output[0] == 99);
    reference::Transfers[1][0] = RasterStatus::Ready;
    CHECK(CopyReadback(session.Device, tickets[0], output, 15) == RasterStatus::InvalidDescription);
    reference::ReadbackCopy = RasterStatus::Failed;
    CHECK(CopyReadback(session.Device, tickets[0], output, 16) == RasterStatus::Failed);
    CHECK(output[0] == 99);
    reference::ReadbackCopy = RasterStatus::Ready;
    REQUIRE(CopyReadback(session.Device, tickets[0], output, 16) == RasterStatus::Ready);
    CHECK(output[0] == 11);
    for (auto& ticket : tickets)
    {
        REQUIRE(Release(session.Device, ticket) == RasterStatus::Ready);
    }
    CHECK(reference::Destroys[0] == 0);
    for (auto& status : reference::Transfers[1])
    {
        status = RasterStatus::Ready;
    }
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    CHECK(reference::Destroys[0] == 1);
}
TEST_CASE("Pipeline requests deduplicate fixed state and independently retain borrowed results", "[rhi][lifetime]")
{
    Session session;
    Scene scene(session.Device);
    const RasterVertexStream stream{12, false};
    const RasterVertexAttribute attribute{0, 0, 0, RasterVertexFormat::Float3};
    const RasterPipelineDescription description{scene.Vertex,
                                                scene.Fragment,
                                                scene.Layout,
                                                {&stream, 1},
                                                {&attribute, 1},
                                                true};
    const auto created = reference::Creates[7];
    PipelineRequest first, second;
    REQUIRE(RequestPipeline(session.Device, description, first) == RasterStatus::Pending);
    REQUIRE(RequestPipeline(session.Device, description, second) == RasterStatus::Pending);
    CHECK(reference::Creates[7] == created);
    REQUIRE(GetStatus(session.Device, first) == RasterStatus::Ready);
    REQUIRE(GetStatus(session.Device, second) == RasterStatus::Ready);
    CHECK(reference::Creates[7] == created + 1);
    RasterPipelineHandle pipeline;
    REQUIRE(GetRequestedPipeline(session.Device, first, pipeline) == RasterStatus::Ready);
    CHECK(Destroy(session.Device, pipeline) == RasterStatus::InvalidState);
    auto stale = first;
    REQUIRE(Release(session.Device, first) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
    CHECK(GetStatus(session.Device, pipeline) == RasterStatus::Ready);
    auto draw = scene.Draw();
    draw.Pipeline = pipeline;
    CommandBatch batch;
    REQUIRE(BeginCommands(session.Device, batch) == RasterStatus::Ready);
    REQUIRE(RecordDraw(session.Device, batch, draw) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, second) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, pipeline) == RasterStatus::InvalidHandle);
    CHECK(reference::Destroys[7] == 0);
    REQUIRE(FinishCommands(session.Device, batch) == RasterStatus::Ready);
    SubmissionToken token;
    REQUIRE(SubmitCommands(session.Device, session.Surface, batch, token) == RasterStatus::Ready);
    reference::Completed = reference::Submitted;
    REQUIRE(GetStatus(session.Device, token) == RasterStatus::Ready);
    CHECK(reference::Destroys[7] == 1);
}
TEST_CASE("Pipeline pending failure cancellation and request capacity remain explicit", "[rhi][lifetime]")
{
    Session session;
    Scene scene(session.Device);
    const RasterVertexStream stream{12, false};
    const RasterVertexAttribute attribute{0, 0, 0, RasterVertexFormat::Float3};
    RasterPipelineDescription description{scene.Vertex,
                                          scene.Fragment,
                                          scene.Layout,
                                          {&stream, 1},
                                          {&attribute, 1},
                                          true};
    PipelineRequest first;
    REQUIRE(RequestPipeline(session.Device, description, first) == RasterStatus::Pending);
    const auto created = reference::Creates[7];
    REQUIRE(Release(session.Device, first) == RasterStatus::Ready);
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    CHECK(reference::Creates[7] == created);
    PipelineRequest requests[internal::RASTER_CAPACITY];
    for (auto& request : requests)
    {
        REQUIRE(RequestPipeline(session.Device, description, request) == RasterStatus::Pending);
    }
    PipelineRequest extra;
    CHECK(RequestPipeline(session.Device, description, extra) == RasterStatus::CapacityExceeded);
    reference::Next = RasterStatus::Pending;
    REQUIRE(GetStatus(session.Device, requests[0]) == RasterStatus::Pending);
    const auto callback = reference::LastRequest;
    internal::RasterComplete(callback, RasterStatus::OutOfMemory);
    CHECK(GetStatus(session.Device, requests[1]) == RasterStatus::OutOfMemory);
    RasterPipelineHandle pipeline;
    CHECK(GetRequestedPipeline(session.Device, requests[1], pipeline) == RasterStatus::OutOfMemory);
    for (auto& request : requests)
    {
        REQUIRE(Release(session.Device, request) == RasterStatus::Ready);
    }
    reference::Next = RasterStatus::Ready;
    description.Blend = true;
    REQUIRE(RequestPipeline(session.Device, description, first) == RasterStatus::Pending);
    internal::RasterComplete(callback, RasterStatus::Ready);
    CHECK(GetStatus(session.Device, first) == RasterStatus::Ready);
}
TEST_CASE("Loss and restart invalidate every lifetime identity", "[rhi][lifetime]")
{
    Session session;
    const uint8 bytes[16]{};
    UploadTicket upload;
    REQUIRE(RequestBufferUpload(session.Device, {BufferRole::Uniform, 16}, bytes, 16, upload) == RasterStatus::Pending);
    CommandBatch batch;
    REQUIRE(BeginCommands(session.Device, batch) == RasterStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    SubmissionToken completion;
    REQUIRE(EndFrame(session.Device, completion) == RasterStatus::Ready);
    internal::Fail(backend::PendingToken, StartupError::DeviceLost);
    CHECK(GetStatus(session.Device, upload) == RasterStatus::DeviceLost);
    CHECK(GetStatus(session.Device, completion) == RasterStatus::DeviceLost);
    Shutdown();
    Session replacement;
    CHECK(GetStatus(replacement.Device, upload) == RasterStatus::InvalidHandle);
    CHECK(GetStatus(replacement.Device, completion) == RasterStatus::InvalidHandle);
    CHECK(DiscardCommands(replacement.Device, batch) == RasterStatus::InvalidHandle);
}
TEST_CASE("Restart admits free transfer slices while old backend callbacks retain other cells", "[rhi][lifetime]")
{
    Session session;
    bool readback = false;
    SECTION("Upload")
    {
        readback = false;
    }
    SECTION("Readback")
    {
        readback = true;
    }
    const uint8 bytes[16]{1, 2, 3, 4};
    BufferHandle source;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, source) == RasterStatus::Ready);
    const auto request = [&](UploadTicket& upload, ReadbackTicket& read) {
        return readback ? RequestBufferReadback(session.Device, source, 0, 16, read)
                        : RequestBufferUpload(session.Device, {BufferRole::Uniform, 16}, bytes, 16, upload);
    };
    UploadTicket oldUpload;
    ReadbackTicket oldReadback;
    REQUIRE(request(oldUpload, oldReadback) == RasterStatus::Pending);
    reference::RetainTransfersOnReset = true;
    REQUIRE(DestroyDevice(session.Device) == DeviceStatus::Ready);
    session.Surface = {};
    REQUIRE(CreateDevice({}, {}, {}, session.Device, session.Surface) == DeviceStatus::Pending);
    internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
    CHECK((readback ? GetStatus(session.Device, oldReadback) : GetStatus(session.Device, oldUpload)) ==
          RasterStatus::InvalidHandle);
    source = {};
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, source) == RasterStatus::Ready);
    UploadTicket uploads[4];
    ReadbackTicket reads[4];
    for (usize i = 0; i < 3; ++i)
    {
        REQUIRE(request(uploads[i], reads[i]) == RasterStatus::Pending);
    }
    CHECK(reference::Copies == 4);
    CHECK(request(uploads[3], reads[3]) == RasterStatus::CapacityExceeded);
    const usize direction = readback ? 1 : 0;
    // An old-session callback drains only its own physical cell. Slot zero can
    // now accept new work without changing the three new-session requests.
    reference::TransferOccupied[direction][0] = false;
    REQUIRE(request(uploads[3], reads[3]) == RasterStatus::Pending);
    CHECK(reference::Copies == 5);
    for (auto& status : reference::Transfers[direction])
    {
        status = RasterStatus::Ready;
    }
    if (readback)
    {
        uint8 actual[16]{};
        REQUIRE(CopyReadback(session.Device, reads[0], actual, sizeof(actual)) == RasterStatus::Ready);
        CHECK(std::memcmp(actual, bytes, sizeof(bytes)) == 0);
    }
    for (usize i = 0; i < 4; ++i)
    {
        REQUIRE((readback ? Release(session.Device, reads[i]) : Release(session.Device, uploads[i])) ==
                RasterStatus::Ready);
    }
    reference::RetainTransfersOnReset = false;
}
TEST_CASE("Pipeline cache canonicalizes attributes and distinguishes immutable fixed state", "[rhi][lifetime]")
{
    Session session;
    Scene scene(session.Device);
    const RasterBinding binding{0, RasterBindingKind::UniformBuffer, RasterVisibility::Both, 16};
    const RasterShaderInput inputs[2]{{0, RasterVertexFormat::Float3}, {1, RasterVertexFormat::Float2}};
    RasterShaderDescription shader;
    shader.Artifact.Stage = ShaderStage::Vertex;
    shader.Artifact.Wgsl = "two input fixture";
    shader.Artifact.WgslEntry = "vertexMain";
    shader.Inputs = inputs;
    shader.Bindings = {&binding, 1};
    RasterShaderHandle vertex;
    REQUIRE(CreateRasterShader(session.Device, shader, vertex) == RasterStatus::Ready);
    const RasterVertexStream stream{20, false};
    const RasterVertexAttribute forward[2]{{0, 0, 0, RasterVertexFormat::Float3},
                                           {1, 0, 12, RasterVertexFormat::Float2}};
    const RasterVertexAttribute reverse[2]{forward[1], forward[0]};
    RasterPipelineDescription description{vertex, scene.Fragment, scene.Layout, {&stream, 1}, forward, true};
    PipelineRequest a, b, c, d;
    const auto created = reference::Creates[7];
    REQUIRE(RequestPipeline(session.Device, description, a) == RasterStatus::Pending);
    description.Attributes = reverse;
    REQUIRE(RequestPipeline(session.Device, description, b) == RasterStatus::Pending);
    REQUIRE(GetStatus(session.Device, a) == RasterStatus::Ready);
    CHECK(reference::Creates[7] == created + 1);
    description.Depth = false;
    REQUIRE(RequestPipeline(session.Device, description, c) == RasterStatus::Pending);
    REQUIRE(GetStatus(session.Device, c) == RasterStatus::Ready);
    description.Blend = true;
    REQUIRE(RequestPipeline(session.Device, description, d) == RasterStatus::Pending);
    REQUIRE(GetStatus(session.Device, d) == RasterStatus::Ready);
    CHECK(reference::Creates[7] == created + 3);
    REQUIRE(Release(session.Device, a) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, b) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, c) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, d) == RasterStatus::Ready);
}
TEST_CASE("Cancelled pipeline callbacks retain detached shader and layout dependencies", "[rhi][lifetime]")
{
    Session session;
    Scene scene(session.Device);
    const RasterVertexStream stream{12, false};
    const RasterVertexAttribute attribute{0, 0, 0, RasterVertexFormat::Float3};
    PipelineRequest request;
    REQUIRE(RequestPipeline(session.Device,
                            {scene.Vertex, scene.Fragment, scene.Layout, {&stream, 1}, {&attribute, 1}, true},
                            request) == RasterStatus::Pending);
    reference::Next = RasterStatus::Pending;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    const auto callback = reference::LastRequest;
    REQUIRE(Release(session.Device, request) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Pipeline) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Set) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Vertex) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Fragment) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Layout) == RasterStatus::Ready);
    CHECK(reference::Destroys[4] == 0);
    CHECK(reference::Destroys[5] == 0);
    CHECK(reference::Destroys[7] == 1);
    internal::RasterComplete(callback, RasterStatus::Failed);
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    CHECK(reference::Destroys[4] == 2);
    CHECK(reference::Destroys[5] == 1);
    CHECK(reference::Destroys[7] == 2);
}

namespace
{
struct GraphScene final
{
    OrderedGraph Graph;
    GraphVersion Vertex, Index, Uniform;
    GraphScene(DeviceHandle device, const Scene& scene)
    {
        REQUIRE(CreateOrderedGraph(device, Graph) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(device, Graph, scene.Vertices, Vertex) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(device, Graph, scene.Indices, Index) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(device, Graph, scene.Uniform, Uniform) == RasterStatus::Ready);
    }
    void Add(DeviceHandle device, const Scene& scene, bool uniform = true)
    {
        const GraphUse uses[]{{Vertex, GraphAccessMode::Vertex, 0, 36},
                              {Index, GraphAccessMode::Index, 0, 6},
                              {Uniform, GraphAccessMode::Uniform, 0, 16}};
        const auto draw = scene.Draw();
        GraphPassDescription pass;
        pass.Name = "scene";
        pass.Source = "fixture.cpp";
        pass.Line = 17;
        pass.Uses = uses;
        pass.UseCount = uniform ? 3 : 2;
        pass.Draws = &draw;
        pass.DrawCount = 1;
        REQUIRE(AddGraphPass(device, Graph, pass) == RasterStatus::Ready);
    }
};
struct GraphTexture final
{
    TextureHandle Texture;
    GraphVersion Initial, Output;
    GraphTexture(DeviceHandle device, OrderedGraph graph, bool pooled = false)
    {
        if (pooled)
        {
            REQUIRE(CreateGraphTexture(device, graph, {16, 8, RasterFormat::Rgba8Unorm, true}, Initial, Texture) ==
                    RasterStatus::Ready);
        }
        else
        {
            REQUIRE(CreateTexture(device, {16, 8, RasterFormat::Rgba8Unorm, true}, {}, Texture) == RasterStatus::Ready);
            RasterTextureState state;
            REQUIRE(GetTextureState(device, Texture, state) == RasterStatus::Ready);
            REQUIRE_FALSE(state.ColorDefined);
            REQUIRE(ImportGraphTexture(device, graph, Texture, state, RasterTextureUse::SampledFragment, Initial) ==
                    RasterStatus::Ready);
        }
        REQUIRE(NextGraphVersion(device, graph, Initial, Output) == RasterStatus::Ready);
    }
    void Add(DeviceHandle device,
             OrderedGraph graph,
             RasterLoad load = RasterLoad::Clear,
             RasterStore store = RasterStore::Store,
             bool sideEffect = false) const
    {
        const GraphUse use{Output,
                           load == RasterLoad::Load ? GraphAccessMode::ColorReadWrite : GraphAccessMode::ColorWrite};
        GraphPassDescription pass;
        pass.Name = "offscreen";
        pass.Attachment.Color = Texture;
        pass.Attachment.ColorLoad = load;
        pass.Attachment.ColorStore = store;
        pass.Uses = &use;
        pass.UseCount = 1;
        pass.SideEffect = sideEffect;
        REQUIRE(AddGraphPass(device, graph, pass) == RasterStatus::Ready);
    }
};
} // namespace
TEST_CASE("Ordered graph checks complete packet declarations before effects", "[rhi][graph]")
{
    Session session;
    Scene scene(session.Device);
    GraphScene graph(session.Device, scene);
    SECTION("Missing reflected uniform use")
    {
        graph.Add(session.Device, scene, false);
        GraphReport report;
        CHECK(CompileOrderedGraph(session.Device, graph.Graph, report) == RasterStatus::InvalidDescription);
        CHECK(report.ErrorPass == 0);
        CHECK(reference::Draws == 0);
        CHECK(reference::Submitted == 0);
    }
    SECTION("Accepted packets retain destroyed public owners")
    {
        graph.Add(session.Device, scene);
        REQUIRE(Destroy(session.Device, scene.Pipeline) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, scene.Set) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, scene.Vertices) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, scene.Indices) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, scene.Uniform) == RasterStatus::Ready);
        GraphReport report;
        REQUIRE(CompileOrderedGraph(session.Device, graph.Graph, report) == RasterStatus::Ready);
        CHECK(report.LivePasses == 1);
        CHECK(std::strcmp(report.Names[0], "scene") == 0);
        CHECK(std::strcmp(report.Sources[0], "fixture.cpp") == 0);
        CHECK(report.Lines[0] == 17);
        CHECK(report.WholeBufferHazards);
        SubmissionToken completed;
        REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph.Graph, completed) == RasterStatus::Ready);
        CHECK(reference::Draws == 1);
        CHECK(GetStatus(session.Device, completed) == RasterStatus::Pending);
        CHECK(DiscardOrderedGraph(session.Device, graph.Graph) == RasterStatus::InvalidHandle);
        reference::Completed = reference::Submitted;
        CHECK(GetStatus(session.Device, completed) == RasterStatus::Ready);
    }
}
TEST_CASE("Ordered graph versions produce RAW WAR WAW without reordering", "[rhi][graph]")
{
    Session session;
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphTexture texture(session.Device, graph);
    texture.Add(session.Device, graph);
    const GraphUse sampled{texture.Output, GraphAccessMode::Sampled, 0, 0, RasterVisibility::Fragment};
    GraphPassDescription composite;
    composite.Name = "composite";
    composite.Uses = &sampled;
    composite.UseCount = 1;
    REQUIRE(AddGraphPass(session.Device, graph, composite) == RasterStatus::Ready);
    GraphVersion overwritten;
    REQUIRE(NextGraphVersion(session.Device, graph, texture.Output, overwritten) == RasterStatus::Ready);
    texture.Output = overwritten;
    texture.Add(session.Device, graph);
    REQUIRE(AddGraphRoot(session.Device, graph, overwritten, GraphRoot::History) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    CHECK(report.LivePasses == 3);
    bool raw = false, war = false, waw = false;
    for (usize i = 0; i < report.DependencyCount; ++i)
    {
        const auto& edge = report.Dependencies[i];
        raw |= edge.Before == 0 && edge.After == 1 && edge.Hazard == GraphHazard::ReadAfterWrite;
        war |= edge.Before == 1 && edge.After == 2 && edge.Hazard == GraphHazard::WriteAfterRead;
        waw |= edge.Before == 0 && edge.After == 2 && edge.Hazard == GraphHazard::WriteAfterWrite;
    }
    CHECK(raw);
    CHECK(war);
    CHECK(waw);
    CHECK(report.FirstUse[0] == 0);
    CHECK(report.LastUse[0] == 2);
}
TEST_CASE("Ordered graph rejects forward reads stale versions undefined loads and roots", "[rhi][graph]")
{
    Session session;
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphTexture texture(session.Device, graph);
    SECTION("Forward read cannot move the producer")
    {
        const GraphUse use{texture.Output, GraphAccessMode::Sampled};
        GraphPassDescription pass;
        pass.Name = "forward";
        pass.Uses = &use;
        pass.UseCount = 1;
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
        texture.Add(session.Device, graph);
    }
    SECTION("Load needs defined prior contents")
    {
        texture.Add(session.Device, graph, RasterLoad::Load);
    }
    SECTION("Partial draw after Discard never establishes initialization")
    {
        texture.Add(session.Device, graph, RasterLoad::Discard);
        REQUIRE(AddGraphRoot(session.Device, graph, texture.Output, GraphRoot::History) == RasterStatus::Ready);
    }
    SECTION("Store Discard invalidates a cleared root")
    {
        texture.Add(session.Device, graph, RasterLoad::Clear, RasterStore::Discard);
        REQUIRE(AddGraphRoot(session.Device, graph, texture.Output, GraphRoot::Export) == RasterStatus::Ready);
    }
    SECTION("Overwritten versions cannot be read later")
    {
        texture.Add(session.Device, graph);
        const auto first = texture.Output;
        GraphVersion second;
        REQUIRE(NextGraphVersion(session.Device, graph, first, second) == RasterStatus::Ready);
        texture.Output = second;
        texture.Add(session.Device, graph);
        const GraphUse use{first, GraphAccessMode::Sampled};
        GraphPassDescription pass;
        pass.Name = "stale";
        pass.Uses = &use;
        pass.UseCount = 1;
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    }
    SECTION("Feedback is a conflicting duplicate physical declaration")
    {
        const GraphUse uses[]{{texture.Output, GraphAccessMode::ColorWrite},
                              {texture.Initial, GraphAccessMode::Sampled}};
        GraphPassDescription pass;
        pass.Name = "feedback";
        pass.Attachment.Color = texture.Texture;
        pass.Uses = uses;
        pass.UseCount = 2;
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    }
    GraphReport report;
    CHECK(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::InvalidDescription);
    CHECK(reference::Passes == 0);
    CHECK(reference::Draws == 0);
    CHECK(reference::Submitted == 0);
}
TEST_CASE("Ordered graph culls only unobservable work and reference mode retains it", "[rhi][graph]")
{
    Session session;
    const bool referenceMode = GENERATE(false, true);
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphTexture unused(session.Device, graph);
    unused.Add(session.Device, graph);
    GraphTexture history(session.Device, graph);
    history.Add(session.Device, graph);
    REQUIRE(AddGraphRoot(session.Device, graph, history.Output, GraphRoot::History) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report, referenceMode) == RasterStatus::Ready);
    CHECK(report.LivePasses == (referenceMode ? 2 : 1));
    CHECK(report.Culled[0] == (referenceMode == false));
    CHECK_FALSE(report.Culled[1]);
    CHECK(report.FirstUse[0] == (referenceMode ? 0U : GRAPH_EXTERNAL_INDEX));
    SubmissionToken token;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::Ready);
    CHECK(reference::Passes == (referenceMode ? 2 : 1));
    RasterTextureState historyState, unusedState;
    REQUIRE(GetTextureState(session.Device, history.Texture, historyState) == RasterStatus::Ready);
    REQUIRE(GetTextureState(session.Device, unused.Texture, unusedState) == RasterStatus::Ready);
    CHECK(historyState.ColorDefined);
    CHECK(unusedState.ColorDefined == referenceMode);
    CHECK(historyState.Use == RasterTextureUse::SampledFragment);
}
TEST_CASE("Ordered graph preflight and skipped acquisition preserve retry while partial failure faults", "[rhi][graph]")
{
    Session session;
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphTexture texture(session.Device, graph);
    texture.Add(session.Device, graph, RasterLoad::Clear, RasterStore::Store, true);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    SubmissionToken token;
    reference::PassPrepare = RasterStatus::OutOfMemory;
    CHECK(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::OutOfMemory);
    CHECK(reference::Passes == 0);
    CHECK(reference::Submitted == 0);
    reference::PassPrepare = RasterStatus::Ready;
    backend::NextFrame = FrameStatus::Skipped;
    CHECK(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::NotReady);
    backend::NextFrame = FrameStatus::Ready;
    reference::PassStart = RasterStatus::Failed;
    CHECK(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::Failed);
    CHECK(GetStartup().State == StartupState::Failed);
    CHECK(reference::Submitted == 0);
}
TEST_CASE("Pending history imports share direct-submission revisions without host waits", "[rhi][graph]")
{
    Session session;
    OrderedGraph first;
    REQUIRE(CreateOrderedGraph(session.Device, first) == RasterStatus::Ready);
    GraphTexture texture(session.Device, first);
    texture.Add(session.Device, first);
    REQUIRE(AddGraphRoot(session.Device, first, texture.Output, GraphRoot::History) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, first, report) == RasterStatus::Ready);
    SubmissionToken dependency;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, first, dependency) == RasterStatus::Ready);
    REQUIRE(GetStatus(session.Device, dependency) == RasterStatus::Pending);
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, texture.Texture, state) == RasterStatus::Ready);
    OrderedGraph second;
    GraphVersion imported;
    REQUIRE(CreateOrderedGraph(session.Device, second) == RasterStatus::Ready);
    REQUIRE(ImportGraphTexture(session.Device,
                               second,
                               texture.Texture,
                               state,
                               RasterTextureUse::SampledFragment,
                               imported) == RasterStatus::Ready);
    const GraphUse use{imported, GraphAccessMode::Sampled};
    GraphPassDescription pass;
    pass.Name = "history";
    pass.Uses = &use;
    pass.UseCount = 1;
    REQUIRE(AddGraphPass(session.Device, second, pass) == RasterStatus::Ready);
    REQUIRE(CompileOrderedGraph(session.Device, second, report) == RasterStatus::Ready);
    SECTION("Same-queue pending data is ordered without waiting")
    {
        SubmissionToken token;
        REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, second, token) == RasterStatus::Ready);
        CHECK(reference::Completed == 0);
    }
    SECTION("Direct accepted work invalidates a frozen import snapshot")
    {
        REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
        RasterPassDescription direct;
        direct.Color = texture.Texture;
        REQUIRE(BeginRasterPass(session.Device, direct) == RasterStatus::Ready);
        REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
        SubmissionToken token;
        CHECK(ExecuteOrderedGraph(session.Device, session.Surface, second, token) == RasterStatus::InvalidState);
        REQUIRE(DiscardOrderedGraph(session.Device, second) == RasterStatus::Ready);
    }
}
TEST_CASE("Transient object pooling waits for views leases and GPU completion and invalidates borrowed handles",
          "[rhi][graph]")
{
    Session session;
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphTexture first(session.Device, graph, true);
    TextureViewHandle view;
    REQUIRE(CreateTextureView(session.Device, first.Texture, view) == RasterStatus::Ready);
    const auto old = first.Texture;
    CHECK(Destroy(session.Device, first.Texture) == RasterStatus::InvalidState);
    first.Add(session.Device, graph, RasterLoad::Clear, RasterStore::Store, true);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    SubmissionToken token;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, old) == RasterStatus::InvalidHandle);
    OrderedGraph held;
    REQUIRE(CreateOrderedGraph(session.Device, held) == RasterStatus::Ready);
    for (usize i = 0; i < 7; ++i)
    {
        GraphTexture other(session.Device, held, true);
    }
    GraphVersion extra;
    TextureHandle extraTexture;
    CHECK(CreateGraphTexture(session.Device, held, {16, 8, RasterFormat::Rgba8Unorm, true}, extra, extraTexture) ==
          RasterStatus::CapacityExceeded);
    reference::Completed = reference::Submitted;
    CHECK(CreateGraphTexture(session.Device, held, {16, 8, RasterFormat::Rgba8Unorm, true}, extra, extraTexture) ==
          RasterStatus::CapacityExceeded);
    REQUIRE(Destroy(session.Device, view) == RasterStatus::Ready);
    REQUIRE(CreateGraphTexture(session.Device, held, {16, 8, RasterFormat::Rgba8Unorm, true}, extra, extraTexture) ==
            RasterStatus::Ready);
    CHECK(reference::Creates[static_cast<usize>(internal::RasterKind::Texture)] == 8);
    CHECK(GetStatus(session.Device, old) == RasterStatus::InvalidHandle);
    REQUIRE(DiscardOrderedGraph(session.Device, held) == RasterStatus::Ready);
}
TEST_CASE("Graph owner identities and capacity failures never issue effects", "[rhi][graph]")
{
    Session session;
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphTexture texture(session.Device, graph);
    OrderedGraph foreign;
    REQUIRE(CreateOrderedGraph(session.Device, foreign) == RasterStatus::Ready);
    GraphVersion next;
    CHECK(NextGraphVersion(session.Device, foreign, texture.Initial, next) == RasterStatus::InvalidHandle);
    for (usize i = 0; i < 32; ++i)
    {
        GraphPassDescription pass;
        pass.Name = "surface";
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    }
    GraphPassDescription extra;
    extra.Name = "overflow";
    CHECK(AddGraphPass(session.Device, graph, extra) == RasterStatus::CapacityExceeded);
    CHECK(reference::Passes == 0);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    CHECK(AddGraphPass(session.Device, graph, extra) == RasterStatus::InvalidHandle);
}

TEST_CASE("Idle transient pools evict whole objects when attachment dimensions change", "[rhi][graph]")
{
    Session session;
    OrderedGraph initial;
    REQUIRE(CreateOrderedGraph(session.Device, initial) == RasterStatus::Ready);
    for (usize i = 0; i < 8; ++i)
    {
        GraphTexture texture(session.Device, initial, true);
    }
    REQUIRE(DiscardOrderedGraph(session.Device, initial) == RasterStatus::Ready);
    OrderedGraph resized;
    REQUIRE(CreateOrderedGraph(session.Device, resized) == RasterStatus::Ready);
    GraphVersion version;
    TextureHandle texture;
    REQUIRE(CreateGraphTexture(session.Device, resized, {32, 16, RasterFormat::Rgba8Unorm, true}, version, texture) ==
            RasterStatus::Ready);
    CHECK(reference::Creates[static_cast<usize>(internal::RasterKind::Texture)] == 9);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::Texture)] == 1);
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, texture, state) == RasterStatus::Ready);
    CHECK_FALSE(state.ColorDefined);
    CHECK_FALSE(state.DepthDefined);
    REQUIRE(DiscardOrderedGraph(session.Device, resized) == RasterStatus::Ready);
}

TEST_CASE("Direct draws and R2 batches reject graph imports without compatible sampled visibility", "[rhi][graph]")
{
    Session session;
    Scene scene(session.Device);
    const auto mode = GENERATE(0, 1, 2, 3);
    TextureHandle texture;
    REQUIRE(CreateTexture(session.Device, {16, 8, RasterFormat::Rgba8Unorm, true}, {}, texture) == RasterStatus::Ready);
    if (mode != 0)
    {
        OrderedGraph graph;
        GraphVersion initial, written;
        RasterTextureState state;
        REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
        REQUIRE(GetTextureState(session.Device, texture, state) == RasterStatus::Ready);
        const auto finalUse = mode == 1   ? RasterTextureUse::ColorAttachment
                              : mode == 2 ? RasterTextureUse::SampledFragment
                                          : RasterTextureUse::SampledBoth;
        REQUIRE(ImportGraphTexture(session.Device, graph, texture, state, finalUse, initial) == RasterStatus::Ready);
        REQUIRE(NextGraphVersion(session.Device, graph, initial, written) == RasterStatus::Ready);
        const GraphUse write{written, GraphAccessMode::ColorWrite};
        GraphPassDescription pass;
        pass.Name = "external state";
        pass.Attachment.Color = texture;
        pass.Uses = &write;
        pass.UseCount = 1;
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
        REQUIRE(AddGraphRoot(session.Device, graph, written, GraphRoot::History) == RasterStatus::Ready);
        GraphReport report;
        REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
        SubmissionToken completion;
        REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, completion) == RasterStatus::Ready);
    }
    TextureViewHandle view;
    REQUIRE(CreateTextureView(session.Device, texture, view) == RasterStatus::Ready);
    const RasterBinding entries[]{{0, RasterBindingKind::UniformBuffer, RasterVisibility::Both, 16},
                                  {1, RasterBindingKind::Texture2D, RasterVisibility::Both, 0}};
    BindingLayoutHandle layout;
    REQUIRE(CreateBindingLayout(session.Device, entries, layout) == RasterStatus::Ready);
    const RasterBindingResource resources[]{{ .Buffer = scene.Uniform, .Size = 16 }, { .Binding = 1, .Texture = view }};
    BindingSetHandle set;
    REQUIRE(CreateBindingSet(session.Device, layout, resources, set) == RasterStatus::Ready);
    const RasterVertexStream stream{12, false};
    const RasterVertexAttribute attribute{0, 0, 0, RasterVertexFormat::Float3};
    RasterPipelineHandle pipeline;
    REQUIRE(CreateRasterPipeline(session.Device,
                                 {scene.Vertex, scene.Fragment, layout, {&stream, 1}, {&attribute, 1}, true},
                                 pipeline) == RasterStatus::Ready);
    const RasterDraw draw{pipeline, set, scene.Slices, scene.Indices, 0, 3, 3, 1};
    CommandBatch batch;
    REQUIRE(BeginCommands(session.Device, batch) == RasterStatus::Ready);
    REQUIRE(RecordDraw(session.Device, batch, draw) == RasterStatus::Ready);
    REQUIRE(FinishCommands(session.Device, batch) == RasterStatus::Ready);
    const auto before = reference::Submitted;
    const auto expected = mode == 0   ? RasterStatus::InvalidDescription
                          : mode == 3 ? RasterStatus::Ready
                                      : RasterStatus::InvalidState;
    SubmissionToken completion;
    CHECK(SubmitCommands(session.Device, session.Surface, batch, completion) == expected);
    if (mode != 3)
    {
        CHECK(reference::Submitted == before);
        CHECK(reference::Draws == 0);
        REQUIRE(DiscardCommands(session.Device, batch) == RasterStatus::Ready);
    }
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    CHECK(DrawIndexed(session.Device, draw) == expected);
    CHECK(reference::Draws == (mode == 3 ? 2 : 0));
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}
TEST_CASE("L1 depth comparison participates in pipeline request identity", "[rhi][raster][lifetime]")
{
    Session session;
    Scene scene(session.Device);
    const RasterVertexStream stream{12, false};
    const RasterVertexAttribute attribute{0, 0, 0, RasterVertexFormat::Float3};
    RasterPipelineDescription description{scene.Vertex,
                                          scene.Fragment,
                                          scene.Layout,
                                          {&stream, 1},
                                          {&attribute, 1},
                                          true};
    PipelineRequest conventional, reverse, same;
    REQUIRE(RequestPipeline(session.Device, description, conventional) == RasterStatus::Pending);
    REQUIRE(GetStatus(session.Device, conventional) == RasterStatus::Ready);
    const auto created = reference::Creates[7];
    description.DepthCompare = RasterDepthCompare::Greater;
    REQUIRE(RequestPipeline(session.Device, description, reverse) == RasterStatus::Pending);
    REQUIRE(GetStatus(session.Device, reverse) == RasterStatus::Ready);
    CHECK(reference::Creates[7] == created + 1);
    REQUIRE(RequestPipeline(session.Device, description, same) == RasterStatus::Ready);
    CHECK(reference::Creates[7] == created + 1);
    description.DepthCompare = static_cast<RasterDepthCompare>(255);
    PipelineRequest rejected;
    CHECK(RequestPipeline(session.Device, description, rejected) == RasterStatus::InvalidDescription);
    CHECK(reference::Creates[7] == created + 1);
    REQUIRE(Release(session.Device, conventional) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, reverse) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, same) == RasterStatus::Ready);
}
TEST_CASE("L1 frozen surface viewport checks actual acquisition without faulting the device", "[rhi][graph]")
{
    Session session;
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphPassDescription pass;
    pass.Name = "stale surface extent";
    pass.Attachment.UseViewport = true;
    pass.Attachment.Viewport = {0, 0, 97, 64};
    pass.Attachment.ClearDepth = 0;
    REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    SubmissionToken completion;
    CHECK(ExecuteOrderedGraph(session.Device, session.Surface, graph, completion) == RasterStatus::InvalidDescription);
    CHECK(GetStatus(session.Device, completion) == RasterStatus::Pending);
    CHECK(reference::Draws == 0);
    CHECK(DiscardOrderedGraph(session.Device, graph) == RasterStatus::InvalidHandle);
    DeviceInfo info;
    CHECK(GetDeviceInfo(session.Device, info) == DeviceStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    RasterPassDescription invalid;
    invalid.ClearDepth = 2;
    CHECK(BeginRasterPass(session.Device, invalid) == RasterStatus::InvalidDescription);
    SubmissionToken next;
    CHECK(EndFrame(session.Device, next) == RasterStatus::Ready);
}
TEST_CASE("L1 rectangle intersection handles empty and overflowing scissors without wrap", "[rhi][raster]")
{
    internal::RasterArea area;
    RasterPassDescription pass;
    pass.UseScissor = true;
    pass.Scissor = {90, 60, ~uint32{0}, ~uint32{0}};
    REQUIRE(internal::RasterResolveArea(pass, 96, 64, area));
    CHECK(area.Scissor.Width == 6);
    CHECK(area.Scissor.Height == 4);
    CHECK_FALSE(area.Empty);
    pass.Scissor = {~uint32{0}, 0, ~uint32{0}, 1};
    REQUIRE(internal::RasterResolveArea(pass, 96, 64, area));
    CHECK(area.Empty);
    pass.UseViewport = true;
    pass.Viewport = {95, 0, ~uint32{0}, 1};
    CHECK_FALSE(internal::RasterResolveArea(pass, 96, 64, area));
}
TEST_CASE("L3 complete immutable mip uploads reject holes and overflow; R10 enables attachment chains",
          "[rhi][raster][l3]")
{
    Session session;
    const uint8 bytes[32]{};
    TextureMipUpload mips[]{{0, 4}, {20, 4}, {28, 4}};
    TextureDescription description{3, 5, RasterFormat::R8Unorm, false, 3, true};
    TextureHandle texture;
    CHECK(CreateTexture(session.Device, description, {bytes, 0, {mips, 2}}, texture) ==
          RasterStatus::InvalidDescription);
    mips[1].Offset = 21;
    CHECK(CreateTexture(session.Device, description, {bytes, 0, mips}, texture) == RasterStatus::InvalidDescription);
    mips[1].Offset = 20;
    mips[2].RowPitch = ~usize{0} - 3;
    CHECK(CreateTexture(session.Device, description, {bytes, 0, mips}, texture) == RasterStatus::InvalidDescription);
    mips[2].RowPitch = 4;
    REQUIRE(CreateTexture(session.Device, description, {bytes, 0, mips}, texture) == RasterStatus::Ready);
    TextureDescription copy;
    REQUIRE(GetTextureDescription(session.Device, texture, copy) == RasterStatus::Ready);
    CHECK(copy.MipLevels == 3);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    description.MipLevels = 4;
    CHECK(CreateTexture(session.Device, description, {bytes, 0, mips}, texture) == RasterStatus::InvalidDescription);
    description = {2, 2, RasterFormat::Rgba8Unorm, true, 2};
    REQUIRE(CreateTexture(session.Device, description, {}, texture) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    description = {2, 2, RasterFormat::Rg8Unorm};
    const uint8 rg[8]{};
    REQUIRE(CreateTexture(session.Device, description, {rg, 4}, texture) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    description.Attachment = true;
    CHECK(CreateTexture(session.Device, description, {}, texture) == RasterStatus::InvalidDescription);
    SamplerHandle sampler;
    SamplerDescription invalid;
    invalid.MinLod = 1;
    CHECK(CreateSampler(session.Device, invalid, sampler) == RasterStatus::InvalidDescription);
    invalid.MipFilter = RasterMipFilter::Linear;
    invalid.MaxLod = 0;
    CHECK(CreateSampler(session.Device, invalid, sampler) == RasterStatus::InvalidDescription);
    invalid.MaxLod = 15;
    REQUIRE(CreateSampler(session.Device, invalid, sampler) == RasterStatus::Ready);
}

TEST_CASE("R5 merges only compatible raster scopes and retains completion leases", "[rhi][graph][merge]")
{
    Session session;
    Scene scene(session.Device);
    GraphScene graph(session.Device, scene);
    const auto variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    GraphUse uses[]{{graph.Vertex, GraphAccessMode::Vertex, 0, 36},
                    {graph.Index, GraphAccessMode::Index, 0, 6},
                    {graph.Uniform, GraphAccessMode::Uniform, 0, 16}};
    const auto draw = scene.Draw();
    GraphPassDescription pass;
    pass.Name = "first";
    pass.Attachment.DepthStore = RasterStore::Store;
    pass.Uses = uses;
    pass.UseCount = 3;
    pass.Draws = &draw;
    pass.DrawCount = 1;
    REQUIRE(AddGraphPass(session.Device, graph.Graph, pass) == RasterStatus::Ready);
    pass.Name = "second";
    pass.Attachment.ColorLoad = pass.Attachment.DepthLoad = RasterLoad::Load;
    if (variant == 2)
    {
        pass.SideEffect = true;
    }
    if (variant == 3)
    {
        pass.Attachment.ColorLoad = RasterLoad::Clear;
    }
    if (variant == 4)
    {
        pass.Attachment.DepthLoad = RasterLoad::Clear;
    }
    if (variant == 5)
    {
        pass.Attachment.DepthStore = RasterStore::Discard;
    }
    if (variant == 6)
    {
        pass.Attachment.UseScissor = true;
        pass.Attachment.Scissor = {0, 0, 1, 1};
    }
    if (variant == 7)
    {
        const auto temp = uses[0];
        uses[0] = uses[1];
        uses[1] = temp;
    }
    REQUIRE(AddGraphPass(session.Device, graph.Graph, pass) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph.Graph, report, GraphCompileOptions{true, variant != 1}) ==
            RasterStatus::Ready);
    CHECK(report.LivePasses == 2);
    CHECK(report.RasterPasses == (variant == 0 ? 1 : 2));
    CHECK(report.MergedPasses == (variant == 0 ? 1 : 0));
    CHECK(report.DrawCount == 2);
    CHECK(report.ComputePasses == 0);
    CHECK(report.BufferBytes == 58);
    CHECK(report.TextureBytes == 0);
    CHECK(report.PlanStorageBytes > sizeof(GraphReport));
    CHECK(report.RasterGroup[0] == 0);
    CHECK(report.RasterGroup[1] == (variant == 0 ? 0 : 1));
    REQUIRE(Destroy(session.Device, scene.Vertices) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, scene.Pipeline) == RasterStatus::Ready);
    SubmissionToken token;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph.Graph, token) == RasterStatus::Ready);
    CHECK(reference::Passes == report.RasterPasses);
    CHECK(reference::Draws == 2);
    CHECK(reference::Destroys[7] == 0);
    CHECK(GetStatus(session.Device, token) == RasterStatus::Pending);
    reference::Completed = reference::Submitted;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    CHECK(reference::Destroys[7] == 1);
}

TEST_CASE("R5 attachment chains keep versions, barriers and final depth validity", "[rhi][graph][merge]")
{
    Session session;
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphTexture texture(session.Device, graph);
    const auto variant = GENERATE(0, 1, 2);
    GraphVersion previous = texture.Initial;
    for (uint32 i = 0; i < 3; ++i)
    {
        GraphVersion next;
        if (i == 0)
        {
            next = texture.Output;
        }
        else
        {
            REQUIRE(NextGraphVersion(session.Device, graph, previous, next) == RasterStatus::Ready);
        }
        const GraphUse use{next, i == 0 ? GraphAccessMode::ColorWrite : GraphAccessMode::ColorReadWrite};
        GraphPassDescription pass;
        pass.Name = "versioned attachment";
        pass.Attachment.Color = texture.Texture;
        pass.Attachment.ColorLoad = pass.Attachment.DepthLoad = i == 0 ? RasterLoad::Clear : RasterLoad::Load;
        pass.Attachment.DepthStore = RasterStore::Store;
        pass.Attachment.DepthReadOnly = variant == 1 && i == 2;
        pass.Attachment.ColorStore = variant == 2 && i == 2 ? RasterStore::Discard : RasterStore::Store;
        pass.Uses = &use;
        pass.UseCount = 1;
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
        previous = next;
    }
    REQUIRE(AddGraphRoot(session.Device, graph, previous, GraphRoot::History) == RasterStatus::Ready);
    GraphReport report;
    const auto status = CompileOrderedGraph(session.Device, graph, report, GraphCompileOptions{true, true});
    if (variant == 2)
    {
        CHECK(status == RasterStatus::InvalidDescription);
        CHECK(report.RasterPasses == 0);
        CHECK(report.PlanStorageBytes == 0);
        CHECK(reference::Passes == 0);
        REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
        return;
    }
    REQUIRE(status == RasterStatus::Ready);
    CHECK(report.RasterPasses == (variant == 0 ? 1 : 2));
    CHECK(report.TextureBytes == uint64{16} * 8 * 8);
    CHECK(report.LastUse[0] == 2);
    CHECK(report.DependencyCount >= 3);
    // Target skips retain the frozen group plan; retry issues one submission.
    backend::NextFrame = FrameStatus::Skipped;
    SubmissionToken token;
    CHECK(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::NotReady);
    CHECK(reference::Passes == 0);
    backend::NextFrame = FrameStatus::Ready;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::Ready);
    CHECK(reference::Passes == report.RasterPasses);
    CHECK(reference::Submitted != 0);
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, texture.Texture, state) == RasterStatus::Ready);
    CHECK(state.ColorDefined);
    CHECK(state.DepthDefined);
    CHECK(state.Use == RasterTextureUse::SampledFragment);
    REQUIRE(Destroy(session.Device, texture.Texture) == RasterStatus::Ready);
    CHECK(reference::Destroys[1] == 0);
    reference::Completed = reference::Submitted;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    CHECK(reference::Destroys[1] == 1);
}

TEST_CASE("R6 pages grow every general resource kind without invalidating earlier handles", "[rhi][r6]")
{
    StorageLimits limits;
    limits.ResourcesPerKind = 96;
    Session session(limits);
    Scene scene(session.Device);
    const RasterBinding binding{0, RasterBindingKind::UniformBuffer, RasterVisibility::Both, 16};
    const RasterBinding computeBinding{0, RasterBindingKind::UniformBuffer, RasterVisibility::Compute, 16};
    BindingLayoutHandle computeLayout;
    REQUIRE(CreateBindingLayout(session.Device, {&computeBinding, 1}, computeLayout) == RasterStatus::Ready);
    RasterShaderDescription vertex;
    vertex.Artifact.Stage = ShaderStage::Vertex;
    vertex.Artifact.Wgsl = "fixture vertex";
    vertex.Artifact.WgslEntry = "vertexMain";
    const RasterShaderInput input{0, RasterVertexFormat::Float3};
    vertex.Inputs = {&input, 1};
    vertex.Bindings = {&binding, 1};
    auto compute = vertex;
    compute.Artifact.Stage = ShaderStage::Compute;
    compute.Artifact.WgslEntry = "computeMain";
    compute.Inputs = {};
    compute.Bindings = {&computeBinding, 1};
    RasterShaderHandle computeShader;
    REQUIRE(CreateRasterShader(session.Device, compute, computeShader) == RasterStatus::Ready);
    const RasterVertexStream stream{12, false};
    const RasterVertexAttribute attribute{0, 0, 0, RasterVertexFormat::Float3};
    const RasterBindingResource resource{ .Buffer = scene.Uniform, .Size = 16 };
    Array<BufferHandle> buffers(80);
    Array<TextureHandle> textures(80);
    Array<TextureViewHandle> views(80);
    Array<SamplerHandle> samplers(80);
    Array<RasterShaderHandle> shaders(80);
    Array<BindingLayoutHandle> layouts(80);
    Array<BindingSetHandle> sets(80);
    Array<RasterPipelineHandle> pipelines(80);
    Array<ComputePipelineHandle> kernels(80);
    const uint8 bytes[16]{};
    const uint8 texel[4]{1, 2, 3, 255};
    for (usize i = 0; i < 80; ++i)
    {
        REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, buffers[i]) == RasterStatus::Ready);
        REQUIRE(CreateTexture(session.Device, {1, 1}, {texel, 4}, textures[i]) == RasterStatus::Ready);
        REQUIRE(CreateTextureView(session.Device, textures[i], views[i]) == RasterStatus::Ready);
        REQUIRE(CreateSampler(session.Device, {}, samplers[i]) == RasterStatus::Ready);
        REQUIRE(CreateRasterShader(session.Device, vertex, shaders[i]) == RasterStatus::Ready);
        REQUIRE(CreateBindingLayout(session.Device, {&binding, 1}, layouts[i]) == RasterStatus::Ready);
        REQUIRE(CreateBindingSet(session.Device, scene.Layout, {&resource, 1}, sets[i]) == RasterStatus::Ready);
        REQUIRE(CreateRasterPipeline(session.Device,
                                     {scene.Vertex, scene.Fragment, scene.Layout, {&stream, 1}, {&attribute, 1}, true},
                                     pipelines[i]) == RasterStatus::Ready);
        REQUIRE(CreateComputePipeline(session.Device, {computeShader, computeLayout}, kernels[i]) ==
                RasterStatus::Ready);
    }
    limits.ResourcesPerKind = 1;
    DeviceInfo info;
    REQUIRE(GetDeviceInfo(session.Device, info) == DeviceStatus::Ready);
    CHECK(info.Profile.Raster.ResourcesPerKind == 96);
    CHECK(info.Profile.Storage.ResourcesPerKind == 96);
    CHECK(GetStatus(session.Device, scene.Vertex) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, textures[0]) == RasterStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(DrawIndexed(session.Device, scene.Draw()) == RasterStatus::Ready);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    CHECK(reference::Draws == 1);
    for (usize kind = 0; kind < static_cast<usize>(internal::RasterKind::RayGeometry); ++kind)
    {
        CHECK(reference::Creates[kind] >= 80);
    }
    StorageUsage usage;
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    CHECK(usage.PublishedRecords >= 720);
    CHECK(usage.HostBytes <= info.Profile.Storage.HostBytes);
    CHECK(usage.PeakHostBytes <= info.Profile.Storage.HostBytes);
}
TEST_CASE("R6 texture slots remain occupied by retained views and reject stale reuse", "[rhi][r6][lifetime]")
{
    StorageLimits limits;
    limits.ResourcesPerKind = 72;
    Session session(limits);
    Array<TextureHandle> textures(72);
    const uint8 texel[4]{};
    for (auto& texture : textures)
    {
        REQUIRE(CreateTexture(session.Device, {1, 1}, {texel, 4}, texture) == RasterStatus::Ready);
    }
    TextureViewHandle view;
    REQUIRE(CreateTextureView(session.Device, textures[0], view) == RasterStatus::Ready);
    const auto stale = textures[0];
    REQUIRE(Destroy(session.Device, textures[0]) == RasterStatus::Ready);
    TextureHandle replacement;
    CHECK(CreateTexture(session.Device, {1, 1}, {texel, 4}, replacement) == RasterStatus::CapacityExceeded);
    REQUIRE(Destroy(session.Device, view) == RasterStatus::Ready);
    REQUIRE(CreateTexture(session.Device, {1, 1}, {texel, 4}, replacement) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
    CHECK(GetStatus(session.Device, replacement) == RasterStatus::Ready);
}
TEST_CASE("R6 byte budgets include detached pending resources and recover after callbacks", "[rhi][r6][budget]")
{
    StorageLimits limits;
    limits.ResourceBytes = 16;
    Session session(limits);
    reference::Next = RasterStatus::Pending;
    const uint8 bytes[16]{};
    BufferHandle buffer;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, buffer) == RasterStatus::Pending);
    const auto request = reference::LastRequest;
    REQUIRE(Destroy(session.Device, buffer) == RasterStatus::Ready);
    StorageUsage usage;
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    CHECK(usage.ResourceBytes == 16);
    CHECK(usage.DetachedRecords == 1);
    CHECK(usage.PendingRecords == 1);
    BufferHandle next;
    CHECK(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, next) == RasterStatus::CapacityExceeded);
    CHECK(reference::Creates[0] == 1);
    internal::RasterComplete(request, RasterStatus::Ready);
    reference::Next = RasterStatus::Ready;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, next) == RasterStatus::Ready);
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    CHECK(usage.ResourceBytes == 16);
    REQUIRE(Destroy(session.Device, next) == RasterStatus::Ready);
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    CHECK(usage.ResourceBytes == 0);
}
TEST_CASE("R6 allocation failure rolls back a resource reservation", "[rhi][r6][allocation]")
{
    Session session;
    const uint32 indices[3]{0, 1, 2};
    BufferHandle buffer;
    internal::FailNextStorageAllocation();
    CHECK(CreateBuffer(session.Device, {BufferRole::Index32, sizeof(indices)}, Bytes(indices), buffer) ==
          RasterStatus::OutOfMemory);
    CHECK(reference::Creates[0] == 0);
    StorageUsage usage;
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    CHECK(usage.ResourceBytes == 0);
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Index32, sizeof(indices)}, Bytes(indices), buffer) ==
            RasterStatus::Ready);
    CHECK(GetStatus(session.Device, buffer) == RasterStatus::Ready);
}
TEST_CASE("R6 retained batches exceed original draw limits and preflight whole frames", "[rhi][r6][batch]")
{
    const bool admit = GENERATE(false, true);
    StorageLimits limits;
    limits.DrawsPerPlan = 1200;
    limits.DrawsPerFrame = admit ? 1500U : 1000U;
    Session session(limits);
    Scene scene(session.Device);
    CommandBatch batch;
    REQUIRE(BeginCommands(session.Device, batch) == RasterStatus::Ready);
    for (usize i = 0; i < 1100; ++i)
    {
        REQUIRE(RecordDraw(session.Device, batch, scene.Draw()) == RasterStatus::Ready);
    }
    REQUIRE(FinishCommands(session.Device, batch) == RasterStatus::Ready);
    SubmissionToken token;
    const auto status = SubmitCommands(session.Device, session.Surface, batch, token);
    CHECK(status == (admit ? RasterStatus::Ready : RasterStatus::CapacityExceeded));
    CHECK(reference::Draws == (admit ? 1100U : 0U));
    if (admit == false)
    {
        REQUIRE(DiscardCommands(session.Device, batch) == RasterStatus::Ready);
    }
}
TEST_CASE("R6 plans admit 72 textures and 405 draws with complete indexed diagnostics", "[rhi][r6][graph]")
{
    StorageLimits limits;
    limits.ResourcesPerKind = 128;
    limits.ResourcesPerGraph = 80;
    limits.PassesPerGraph = 64;
    limits.DrawsPerPlan = 512;
    Session session(limits);
    Scene scene(session.Device);
    GraphScene graph(session.Device, scene);
    Array<TextureHandle> textures(72);
    Array<GraphUse> uses(75);
    uses[0] = {graph.Vertex, GraphAccessMode::Vertex, 0, 36};
    uses[1] = {graph.Index, GraphAccessMode::Index, 0, 6};
    uses[2] = {graph.Uniform, GraphAccessMode::Uniform, 0, 16};
    const uint8 texel[4]{};
    for (usize i = 0; i < 72; ++i)
    {
        REQUIRE(CreateTexture(session.Device, {1, 1}, {texel, 4}, textures[i]) == RasterStatus::Ready);
        GraphVersion texture;
        RasterTextureState state;
        REQUIRE(GetTextureState(session.Device, textures[i], state) == RasterStatus::Ready);
        REQUIRE(ImportGraphTexture(session.Device,
                                   graph.Graph,
                                   textures[i],
                                   state,
                                   RasterTextureUse::SampledBoth,
                                   texture) == RasterStatus::Ready);
        uses[i + 3] = {texture, GraphAccessMode::Sampled};
    }
    const auto draw = scene.Draw();
    Array<RasterDraw> draws(15, draw);
    for (uint32 i = 0; i < 40; ++i)
    {
        GraphPassDescription pass;
        pass.Name = "scaled scene";
        pass.Source = "r6_fixture.cpp";
        pass.Line = i + 100;
        pass.Uses = uses.GetData();
        pass.UseCount = uses.GetSize();
        pass.Draws = draws.GetData();
        pass.DrawCount = i == 39 ? 15U : 10U;
        REQUIRE(AddGraphPass(session.Device, graph.Graph, pass) == RasterStatus::Ready);
    }
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph.Graph, report) == RasterStatus::Ready);
    CHECK(report.PassCount == 40);
    CHECK(report.ResourceCount == 75);
    CHECK(report.DrawCount == 405);
    CHECK(report.ReportedPasses == 32);
    CHECK(report.ReportedResources == 16);
    CHECK(report.DiagnosticsTruncated);
    GraphPassDiagnostic pass;
    REQUIRE(GetGraphPassDiagnostic(session.Device, graph.Graph, 39, pass) == RasterStatus::Ready);
    CHECK(pass.Line == 139);
    CHECK_FALSE(pass.Culled);
    GraphResourceDiagnostic resource;
    REQUIRE(GetGraphResourceDiagnostic(session.Device, graph.Graph, 74, resource) == RasterStatus::Ready);
    CHECK(resource.FirstUse == 0);
    CHECK(resource.LastUse == 39);
    SubmissionToken token;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph.Graph, token) == RasterStatus::Ready);
    CHECK(reference::Draws == 405);
    CHECK(reference::Passes == 40);
    CHECK(GetGraphPassDiagnostic(session.Device, graph.Graph, 39, pass) == RasterStatus::InvalidHandle);
}
TEST_CASE("R6 dependencies and versions exceed fixed report prefixes without colliding with external indices",
          "[rhi][r6][graph]")
{
    StorageLimits limits;
    limits.PassesPerGraph = 650;
    limits.DependenciesPerGraph = 700;
    Session session(limits);
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    TextureHandle texture;
    REQUIRE(CreateTexture(session.Device, {1, 1, RasterFormat::Rgba8Unorm, true}, {}, texture) == RasterStatus::Ready);
    GraphVersion version;
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, texture, state) == RasterStatus::Ready);
    REQUIRE(ImportGraphTexture(session.Device, graph, texture, state, RasterTextureUse::SampledBoth, version) ==
            RasterStatus::Ready);
    for (uint32 i = 0; i < 600; ++i)
    {
        GraphVersion next;
        REQUIRE(NextGraphVersion(session.Device, graph, version, next) == RasterStatus::Ready);
        const GraphUse use{next, GraphAccessMode::ColorWrite};
        GraphPassDescription pass;
        pass.Name = "versioned clear";
        pass.Attachment.Color = texture;
        pass.Uses = &use;
        pass.UseCount = 1;
        pass.SideEffect = true;
        REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
        version = next;
    }
    REQUIRE(AddGraphRoot(session.Device, graph, version, GraphRoot::History) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    CHECK(report.PassCount == 600);
    CHECK(report.LivePasses == 600);
    REQUIRE(report.DependencyCount > 512);
    CHECK(report.ReportedDependencies == 512);
    CHECK(report.ErrorPass == GRAPH_EXTERNAL_INDEX);
    GraphDependency edge;
    REQUIRE(GetGraphDependency(session.Device, graph, 512, edge) == RasterStatus::Ready);
    CHECK(edge.Before == 512);
    CHECK(edge.After == 513);
    REQUIRE(GetGraphDependency(session.Device, graph, report.DependencyCount - 1, edge) == RasterStatus::Ready);
    CHECK(edge.After == GRAPH_EXTERNAL_INDEX);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
}

TEST_CASE("R6 readers above pass 32 retain WAR dependencies", "[rhi][r6][graph]")
{
    StorageLimits limits;
    limits.PassesPerGraph = 64;
    Session session(limits);
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphTexture texture(session.Device, graph);
    texture.Add(session.Device, graph);
    const GraphUse sampled{texture.Output, GraphAccessMode::Sampled, 0, 0, RasterVisibility::Fragment};
    GraphPassDescription reader;
    reader.Name = "many readers";
    reader.Uses = &sampled;
    reader.UseCount = 1;
    reader.SideEffect = true;
    for (uint32 i = 0; i < 40; ++i)
    {
        REQUIRE(AddGraphPass(session.Device, graph, reader) == RasterStatus::Ready);
    }
    GraphVersion overwritten;
    REQUIRE(NextGraphVersion(session.Device, graph, texture.Output, overwritten) == RasterStatus::Ready);
    texture.Output = overwritten;
    texture.Add(session.Device, graph);
    REQUIRE(AddGraphRoot(session.Device, graph, overwritten, GraphRoot::History) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    uint32 readers = 0;
    for (uint32 i = 0; i < report.DependencyCount; ++i)
    {
        GraphDependency edge;
        REQUIRE(GetGraphDependency(session.Device, graph, i, edge) == RasterStatus::Ready);
        if (edge.Hazard == GraphHazard::WriteAfterRead && edge.After == 41)
        {
            CHECK(edge.Before >= 1);
            CHECK(edge.Before <= 40);
            ++readers;
        }
    }
    CHECK(readers == 40);
}
TEST_CASE("R6 checked host growth and frame admission preserve existing storage", "[rhi][r6][storage]")
{
    Session session;
    internal::StorageArray<uint64> values;
    REQUIRE(values.TryResize(2));
    values[0] = 47;
    CHECK_FALSE(values.TryResize(~usize{0}));
    CHECK(internal::StorageFailure() == RasterStatus::CapacityExceeded);
    CHECK(values.GetSize() == 2);
    CHECK(values[0] == 47);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    CHECK_FALSE(values.TryResize(3));
    CHECK(internal::StorageFailure() == RasterStatus::InvalidState);
    CHECK(values.GetSize() == 2);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    StorageUsage usage;
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    auto limits = internal::CurrentStorageLimits();
    limits.HostBytes = usage.HostBytes;
    REQUIRE(internal::ConfigureStorage(limits));
    CHECK_FALSE(values.TryResize(3));
    CHECK(internal::StorageFailure() == RasterStatus::CapacityExceeded);
    CHECK(values[0] == 47);
    limits.HostBytes = StorageLimits{}.HostBytes;
    REQUIRE(internal::ConfigureStorage(limits));
    REQUIRE(values.TryResize(3));
    CHECK(values[0] == 47);
}
TEST_CASE("R6 rejects invalid startup counts and budgets", "[rhi][r6][storage]")
{
    Shutdown();
    DeviceDescription description;
    description.Storage.ResourcesPerKind = GENERATE(0U, ~uint32{0});
    CHECK(ValidateDeviceDescription(description) == DeviceStatus::InvalidDescription);
    description.Storage = {};
    description.Storage.RasterPassStates = GENERATE(0U, ~uint32{0});
    CHECK(ValidateDeviceDescription(description) == DeviceStatus::InvalidDescription);
    description.Storage = {};
    description.Storage.HostBytes = 0;
    CHECK(ValidateDeviceDescription(description) == DeviceStatus::InvalidDescription);
    description.Storage = {};
    description.Storage.ResourceBytes = 0;
    CHECK(ValidateDeviceDescription(description) == DeviceStatus::InvalidDescription);
}

TEST_CASE("R6 grown resource pages retire through GPU completion and device loss", "[rhi][r6][lifetime]")
{
    StorageLimits limits;
    limits.ResourcesPerKind = 128;
    Session session(limits);
    Scene scene(session.Device);
    const uint8 bytes[16]{};
    Array<BufferHandle> buffers(80);
    for (auto& buffer : buffers)
    {
        REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, buffer) == RasterStatus::Ready);
    }
    const auto stale = buffers[79];
    BindingSetHandle set;
    const RasterBindingResource resource{ .Buffer = buffers[79], .Size = 16 };
    REQUIRE(CreateBindingSet(session.Device, scene.Layout, {&resource, 1}, set) == RasterStatus::Ready);
    auto draw = scene.Draw();
    draw.Bindings = set;
    CommandBatch batch;
    REQUIRE(BeginCommands(session.Device, batch) == RasterStatus::Ready);
    REQUIRE(RecordDraw(session.Device, batch, draw) == RasterStatus::Ready);
    REQUIRE(FinishCommands(session.Device, batch) == RasterStatus::Ready);
    SubmissionToken completion;
    REQUIRE(SubmitCommands(session.Device, session.Surface, batch, completion) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, set) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, buffers[79]) == RasterStatus::Ready);
    StorageUsage usage;
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    CHECK(usage.DetachedRecords == 2);
    CHECK(usage.ResourceBytes >= uint64{80} * 16);
    reference::Completed = reference::Submitted;
    REQUIRE(GetStatus(session.Device, completion) == RasterStatus::Ready);
    SamplerHandle collect;
    REQUIRE(CreateSampler(session.Device, {}, collect) == RasterStatus::Ready);
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    CHECK(usage.DetachedRecords == 0);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
    reference::LossOnCompletion = backend::PendingToken;
    CHECK(GetStatus(session.Device, buffers[78]) == RasterStatus::DeviceLost);
    REQUIRE(DestroyDevice(session.Device) == DeviceStatus::Ready);
    session.Surface = {};
    DeviceDescription description;
    description.Storage = limits;
    REQUIRE(CreateDevice({}, {}, description, session.Device, session.Surface) == DeviceStatus::Pending);
    internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    CHECK(usage.ResourceBytes == 0);
    CHECK(usage.HostBytes > 0);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
}

TEST_CASE("R6 retained CPU texture copies roll back host allocation failure", "[rhi][r6][storage]")
{
    Session session;
    TextureHandle texture;
    const uint8 bytes[4]{1, 2, 3, 255};
    TextureDescription description{1, 1};
    description.RetainCpuContent = true;
    internal::FailNextStorageAllocation();
    CHECK(CreateTexture(session.Device, description, {bytes, 4}, texture) == RasterStatus::OutOfMemory);
    CHECK(reference::Creates[1] == 0);
    StorageUsage usage;
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    CHECK(usage.ResourceBytes == 0);
    REQUIRE(CreateTexture(session.Device, description, {bytes, 4}, texture) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, texture) == RasterStatus::Ready);
}

TEST_CASE("R6 recycled graph slots reject queries before replacement compilation", "[rhi][r6][graph]")
{
    Session session;
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphPassDescription pass;
    pass.Name = "recycled diagnostics";
    REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphPassDiagnostic diagnostic;
    diagnostic.Line = 911;
    CHECK(GetGraphPassDiagnostic(session.Device, graph, 0, diagnostic) == RasterStatus::NotReady);
    CHECK(diagnostic.Line == 911);
    GraphResourceDiagnostic resource;
    resource.LastUse = 911;
    CHECK(GetGraphResourceDiagnostic(session.Device, graph, 0, resource) == RasterStatus::NotReady);
    CHECK(resource.LastUse == 911);
    GraphDependency edge;
    edge.Before = 911;
    CHECK(GetGraphDependency(session.Device, graph, 0, edge) == RasterStatus::NotReady);
    CHECK(edge.Before == 911);
    REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    REQUIRE(GetGraphPassDiagnostic(session.Device, graph, 0, diagnostic) == RasterStatus::Ready);
    SubmissionToken completion;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, completion) == RasterStatus::Ready);
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    CHECK(GetGraphPassDiagnostic(session.Device, graph, 0, diagnostic) == RasterStatus::NotReady);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
}

TEST_CASE("R7 frame probes preserve owners, pending samples and unavailable output", "[rhi][measurement]")
{
    Session session;
    GpuTiming sample{ .State = GpuTimingState::Available, .DurationNanoseconds = 123 };
    CHECK(GetGpuTiming({}, sample) == RasterStatus::InvalidHandle);
    CHECK(sample.DurationNanoseconds == 123);
    CHECK(ArmGpuTiming(session.Device) == RasterStatus::Unsupported);
    REQUIRE(GetGpuTiming(session.Device, sample) == RasterStatus::Ready);
    CHECK(sample.State == GpuTimingState::Unavailable);
    REQUIRE(DestroyDevice(session.Device) == DeviceStatus::Ready);
    DeviceDescription description;
    description.EnableGpuTiming = true;
    session.Surface = {};
    REQUIRE(CreateDevice({}, {}, description, session.Device, session.Surface) == DeviceStatus::Pending);
    sample.DurationNanoseconds = 123;
    CHECK(GetGpuTiming(session.Device, sample) == RasterStatus::NotReady);
    CHECK(sample.DurationNanoseconds == 0);
    internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
    reference::TimingAdmission = RasterStatus::OutOfMemory;
    CHECK(ArmGpuTiming(session.Device) == RasterStatus::OutOfMemory);
    reference::TimingAdmission = RasterStatus::Ready;
    REQUIRE(ArmGpuTiming(session.Device) == RasterStatus::Ready);
    CHECK(ArmGpuTiming(session.Device) == RasterStatus::InvalidState);
    backend::NextFrame = FrameStatus::Skipped;
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Skipped);
    REQUIRE(GetGpuTiming(session.Device, sample) == RasterStatus::Ready);
    CHECK(sample.State == GpuTimingState::Armed);
    backend::NextFrame = FrameStatus::Ready;
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    CHECK(ArmGpuTiming(session.Device) == RasterStatus::InvalidState);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    reference::Timing.State = GpuTimingState::Pending;
    CHECK(ArmGpuTiming(session.Device) == RasterStatus::InvalidState);
    for (const auto terminal : {GpuTimingState::Available, GpuTimingState::Disjoint, GpuTimingState::Failed})
    {
        reference::Timing = { .State = terminal };
        REQUIRE(GetGpuTiming(session.Device, sample) == RasterStatus::Ready);
        CHECK(sample.State == terminal);
        CHECK(sample.DurationNanoseconds == 0);
        REQUIRE(ArmGpuTiming(session.Device) == RasterStatus::Ready);
    }
    reference::TimingLossToken = backend::PendingToken;
    sample.DurationNanoseconds = 123;
    CHECK(GetGpuTiming(session.Device, sample) == RasterStatus::DeviceLost);
    CHECK(sample.DurationNanoseconds == 0);
    CHECK(ArmGpuTiming(session.Device) == RasterStatus::DeviceLost);
}
TEST_CASE("R7 API allocation telemetry includes detached backing through retirement", "[rhi][measurement]")
{
    Session session;
    backend::ActiveKind = Backend::Metal;
    reference::AllocationAvailable = true;
    reference::AllocationBytes = 128;
    const uint8 data[16]{};
    BufferHandle buffer;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, data, buffer) == RasterStatus::Ready);
    AllocationTelemetry memory;
    REQUIRE(GetAllocationTelemetry(session.Device, memory) == RasterStatus::Ready);
    CHECK(memory.ApiBytesAvailable);
    CHECK(memory.LogicalBytes == 16);
    CHECK(memory.ApiBytes == 128);
    CHECK(memory.PeakObservedApiBytes == 128);
    CHECK_FALSE(memory.PhysicalBytesAvailable);
    // A binding set keeps the physical allocation alive after the buffer owner goes away.
    const RasterBinding binding{0, RasterBindingKind::UniformBuffer, RasterVisibility::Both, 16};
    BindingLayoutHandle layout;
    BindingSetHandle set;
    REQUIRE(CreateBindingLayout(session.Device, {&binding, 1}, layout) == RasterStatus::Ready);
    const RasterBindingResource resource{ .Buffer = buffer, .Size = 16 };
    REQUIRE(CreateBindingSet(session.Device, layout, {&resource, 1}, set) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, buffer) == RasterStatus::Ready);
    REQUIRE(GetAllocationTelemetry(session.Device, memory) == RasterStatus::Ready);
    CHECK(memory.RetiringApiBytes == 128);
    CHECK(memory.LogicalBytes == 16);
    reference::AllocationAvailable = false;
    REQUIRE(GetAllocationTelemetry(session.Device, memory) == RasterStatus::Ready);
    CHECK_FALSE(memory.ApiBytesAvailable);
    CHECK(memory.ApiBytes == 0);
    CHECK(memory.RetiringApiBytes == 0);
    reference::AllocationAvailable = true;
    REQUIRE(Destroy(session.Device, set) == RasterStatus::Ready);
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    REQUIRE(GetAllocationTelemetry(session.Device, memory) == RasterStatus::Ready);
    CHECK(memory.ApiBytes == 0);
    CHECK(memory.LogicalBytes == 0);
    CHECK(memory.PeakObservedApiBytes == 128);
    memory.LogicalBytes = 123;
    CHECK(GetAllocationTelemetry({}, memory) == RasterStatus::InvalidHandle);
    CHECK(memory.LogicalBytes == 123);
}

TEST_CASE("R7 timestamps reject ambiguous wraps and preserve failed conversion output", "[rhi][measurement]")
{
    float64 duration = 123;
    CHECK(internal::TimestampDuration(250, 4, {8, 2}, 40, duration));
    CHECK(duration == 20);
    CHECK_FALSE(internal::TimestampDuration(250, 4, {8, 2}, 512, duration));
    CHECK(duration == 20);
    CHECK_FALSE(internal::TimestampDuration(100, 90, {8, 1}, 20, duration));
    CHECK(duration == 20);
    CHECK_FALSE(internal::TimestampDuration(1, 2, {0, 1}, 1, duration));
    CHECK_FALSE(internal::TimestampDuration(1, 2, {65, 1}, 1, duration));
    CHECK_FALSE(internal::TimestampDuration(1, 2, {64, 0}, 1, duration));
    CHECK(internal::TimestampDuration(~uint64{0} - 1, 2, {64, .5}, 10, duration));
    CHECK(duration == 2);
    CHECK(internal::TimestampDuration(2, 2, {64, 1}, 10, duration));
    CHECK(duration == 0);
}

TEST_CASE("R7 allocation observations reject byte counter overflow", "[rhi][measurement]")
{
    Session session;
    backend::ActiveKind = Backend::Metal;
    const uint8 data[16]{};
    BufferHandle first, second;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, data, first) == RasterStatus::Ready);
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, data, second) == RasterStatus::Ready);
    reference::AllocationBytes = ~uint64{0};
    AllocationTelemetry memory{ .ApiBytes = 123 };
    CHECK(GetAllocationTelemetry(session.Device, memory) == RasterStatus::CapacityExceeded);
    CHECK(memory.ApiBytes == 0);
    CHECK(memory.LogicalBytes == 0);
    reference::AllocationBytes = 128;
}

TEST_CASE("R7 terminal device failure stays distinct from pending startup", "[rhi][measurement]")
{
    Session session;
    internal::Fail(backend::PendingToken, StartupError::RenderingUnavailable);
    GpuTiming timing{ .DurationNanoseconds = 123 };
    MeasurementAdapter adapter{ .VendorId = 123 };
    CHECK(GetGpuTiming(session.Device, timing) == RasterStatus::Failed);
    CHECK(timing.DurationNanoseconds == 0);
    CHECK(GetMeasurementAdapter(session.Device, adapter) == RasterStatus::Failed);
    CHECK(adapter.VendorId == 0);
    CHECK(ArmGpuTiming(session.Device) == RasterStatus::Failed);
}

TEST_CASE("R7 failed re-arm preserves every terminal sample", "[rhi][measurement]")
{
    Session session;
    for (const auto state : {GpuTimingState::Available, GpuTimingState::Disjoint, GpuTimingState::Failed})
    {
        GpuTiming timing
        {
            .State = state,
            .DurationNanoseconds = state == GpuTimingState::Available ? 123.0 : 0.0,
            .TickNanoseconds = 2,
            .ValidBits = 48,
        };
        for (const auto failure : {RasterStatus::OutOfMemory, RasterStatus::CapacityExceeded, RasterStatus::Failed})
        {
            const auto attempt = [&timing, failure]() noexcept {
                internal::TimingArmRollback rollback(timing);
                timing = {}; // The backend releases old native objects before attempting new ones.
                return failure;
            };
            CHECK(attempt() == failure);
            CHECK(timing.State == state);
            CHECK(timing.TickNanoseconds == 2);
            CHECK(timing.ValidBits == 48);
            CHECK(timing.DurationNanoseconds == (state == GpuTimingState::Available ? 123 : 0));
        }
        {
            internal::TimingArmRollback rollback(timing);
            timing = { .State = GpuTimingState::Armed };
            rollback.Accept();
        }
        CHECK(timing.State == GpuTimingState::Armed);
    }
    GpuTiming lost{ .State = GpuTimingState::Available, .DurationNanoseconds = 123 };
    {
        internal::TimingArmRollback rollback(lost);
        lost = {};
        internal::Fail(backend::PendingToken, StartupError::DeviceLost);
    }
    CHECK(lost.State == GpuTimingState::Empty);
}

TEST_CASE("R8 mutations copy bytes and reject retained or unfinished destinations", "[rhi][r8]")
{
    Session session;
    uint8 initial[16]{};
    uint8 patch[4]{9, 8, 7, 6};
    BufferHandle buffer;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Vertex, sizeof(initial)}, initial, buffer) ==
            RasterStatus::Ready);
    TransferTicket update;
    CHECK(RequestBufferUpdate(session.Device, buffer, 13, patch, 4, update) == RasterStatus::InvalidDescription);
    reference::TransferStart = RasterStatus::OutOfMemory;
    CHECK(RequestBufferUpdate(session.Device, buffer, 4, patch, 4, update) == RasterStatus::OutOfMemory);
    CHECK(GetStatus(session.Device, buffer) == RasterStatus::Ready);
    reference::TransferStart = RasterStatus::Ready;
    REQUIRE(RequestBufferUpdate(session.Device, buffer, 4, patch, 4, update) == RasterStatus::Pending);
    patch[0] = 0;
    CHECK(GetStatus(session.Device, buffer) == RasterStatus::Pending);
    TransferTicket second;
    CHECK(RequestBufferUpdate(session.Device, buffer, 4, patch, 4, second) == RasterStatus::NotReady);
    const auto stale = update;
    REQUIRE(Release(session.Device, update) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
    CHECK(reference::TransferOccupied[0][0]);
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    ReadbackTicket readback;
    REQUIRE(RequestBufferReadback(session.Device, buffer, 0, 16, readback) == RasterStatus::Pending);
    CHECK(RequestBufferUpdate(session.Device, buffer, 4, patch, 4, second) == RasterStatus::InvalidState);
    reference::Transfers[1][0] = RasterStatus::Ready;
    uint8 actual[16]{};
    REQUIRE(CopyReadback(session.Device, readback, actual, 16) == RasterStatus::Ready);
    CHECK(actual[4] == 9);
    CHECK(actual[7] == 6);
    CHECK(actual[0] == 0);
    REQUIRE(Release(session.Device, readback) == RasterStatus::Ready);
}

TEST_CASE("R8 incremental mip initialization forbids undefined reads and sampling", "[rhi][r8]")
{
    Session session;
    TextureDescription description;
    description.Width = description.Height = 2;
    description.MipLevels = 2;
    description.DeferredInitialization = true;
    TextureHandle texture;
    REQUIRE(CreateTexture(session.Device, description, {}, texture) == RasterStatus::Ready);
    TextureViewHandle view;
    CHECK(CreateTextureView(session.Device, texture, view) == RasterStatus::InvalidState);
    TextureResidency residency;
    REQUIRE(GetTextureResidency(session.Device, texture, residency) == RasterStatus::Ready);
    CHECK(residency.InitializedMips == 0);
    ReadbackTicket readback;
    CHECK(RequestTextureReadback(session.Device, texture, {0, 0, 0, 2, 2}, 8, readback) == RasterStatus::InvalidState);
    uint8 pixels[24]{1, 2, 3, 4, 5, 6, 7, 8, 99, 99, 99, 99, 9, 10, 11, 12, 13, 14, 15, 16, 99, 99, 99, 99};
    TransferTicket update;
    CHECK(RequestTextureUpdate(session.Device, texture, {0, 0, 0, 1, 1}, pixels, 4, 4, update) ==
          RasterStatus::InvalidState);
    REQUIRE(RequestTextureUpdate(session.Device, texture, {0, 0, 0, 2, 2}, pixels, 24, 12, update) ==
            RasterStatus::Pending);
    REQUIRE(GetTextureResidency(session.Device, texture, residency) == RasterStatus::Ready);
    CHECK(residency.InitializedMips == 0);
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(GetStatus(session.Device, update) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, update) == RasterStatus::Ready);
    REQUIRE(GetTextureResidency(session.Device, texture, residency) == RasterStatus::Ready);
    CHECK(residency.InitializedMips == 1);
    CHECK(residency.InitializedBytes == 16);
    CHECK(CreateTextureView(session.Device, texture, view) == RasterStatus::InvalidState);
    REQUIRE(RequestTextureUpdate(session.Device, texture, {1, 0, 0, 1, 1}, pixels, 4, 4, update) ==
            RasterStatus::Pending);
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(GetStatus(session.Device, update) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, update) == RasterStatus::Ready);
    REQUIRE(GetTextureResidency(session.Device, texture, residency) == RasterStatus::Ready);
    CHECK(residency.InitializedMips == 3);
    CHECK(residency.InitializedBytes == 20);
    REQUIRE(CreateTextureView(session.Device, texture, view) == RasterStatus::Ready);
    CHECK(RequestTextureUpdate(session.Device, texture, {0, 0, 0, 2, 2}, pixels, 24, 12, update) ==
          RasterStatus::InvalidState);
    REQUIRE(Destroy(session.Device, view) == RasterStatus::Ready);
    REQUIRE(RequestTextureReadback(session.Device, texture, {0, 0, 0, 2, 2}, 12, readback) == RasterStatus::Pending);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    uint8 actual[24];
    std::memset(actual, 88, sizeof(actual));
    CHECK(CopyReadback(session.Device, readback, actual, 24) == RasterStatus::Pending);
    CHECK(actual[0] == 88);
    reference::Transfers[1][0] = RasterStatus::Ready;
    REQUIRE(CopyReadback(session.Device, readback, actual, 24) == RasterStatus::Ready);
    for (usize i = 0; i < 24; ++i)
    {
        CHECK(actual[i] == (i % 12 < 8 ? pixels[i] : 0));
    }
    REQUIRE(Release(session.Device, readback) == RasterStatus::Ready);
}

TEST_CASE("R8 destination leases and GPU ordinals cover retained and in-flight reads", "[rhi][r8]")
{
    Session session;
    Scene scene(session.Device);
    uint8 bytes[4]{};
    TransferTicket ticket;
    CHECK(RequestBufferUpdate(session.Device, scene.Uniform, 0, bytes, 4, ticket) == RasterStatus::InvalidState);
    CommandBatch batch;
    REQUIRE(BeginCommands(session.Device, batch) == RasterStatus::Ready);
    REQUIRE(RecordDraw(session.Device, batch, scene.Draw()) == RasterStatus::Ready);
    CHECK(RequestBufferUpdate(session.Device, scene.Vertices, 0, bytes, 4, ticket) == RasterStatus::InvalidState);
    REQUIRE(DiscardCommands(session.Device, batch) == RasterStatus::Ready);
    OrderedGraph graph;
    GraphVersion imported;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    REQUIRE(ImportGraphBuffer(session.Device, graph, scene.Vertices, imported) == RasterStatus::Ready);
    CHECK(RequestBufferUpdate(session.Device, scene.Vertices, 0, bytes, 4, ticket) == RasterStatus::InvalidState);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(DrawIndexed(session.Device, scene.Draw()) == RasterStatus::Ready);
    SubmissionToken completion;
    REQUIRE(EndFrame(session.Device, completion) == RasterStatus::Ready);
    CHECK(RequestBufferUpdate(session.Device, scene.Vertices, 0, bytes, 4, ticket) == RasterStatus::InvalidState);
    reference::Completed = reference::Submitted;
    REQUIRE(RequestBufferUpdate(session.Device, scene.Vertices, 0, bytes, 4, ticket) == RasterStatus::Pending);
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(GetStatus(session.Device, ticket) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, ticket) == RasterStatus::Ready);
}

TEST_CASE("R8 staging pressure includes cancellation and validates arithmetic", "[rhi][r8]")
{
    StorageLimits limits;
    limits.TransferSlices = 1;
    limits.TransferBytes = 256;
    Session session(limits);
    LifetimeCapabilities capabilities;
    REQUIRE(GetLifetimeCapabilities(session.Device, capabilities) == RasterStatus::Ready);
    CHECK(capabilities.TransfersPerDirection == 1);
    CHECK(capabilities.TransferBytes == 256);
    uint8 pixels[8]{};
    TextureHandle textures[2];
    REQUIRE(CreateTexture(session.Device, {2, 1}, {pixels, 8}, textures[0]) == RasterStatus::Ready);
    REQUIRE(CreateTexture(session.Device, {2, 1}, {pixels, 8}, textures[1]) == RasterStatus::Ready);
    TransferTicket tickets[2];
    CHECK(RequestTextureUpdate(session.Device, textures[0], {0, 0, 0, 2, 1}, pixels, 8, ~usize{0} - 3, tickets[0]) ==
          RasterStatus::CapacityExceeded);
    CHECK(RequestTextureUpdate(session.Device, textures[0], {0, ~uint32{0}, 0, 2, 1}, pixels, 8, 8, tickets[0]) ==
          RasterStatus::InvalidDescription);
    REQUIRE(RequestTextureUpdate(session.Device, textures[0], {0, 0, 0, 2, 1}, pixels, 8, 8, tickets[0]) ==
            RasterStatus::Pending);
    REQUIRE(Release(session.Device, tickets[0]) == RasterStatus::Ready);
    CHECK(RequestTextureUpdate(session.Device, textures[1], {0, 0, 0, 2, 1}, pixels, 8, 8, tickets[1]) ==
          RasterStatus::CapacityExceeded);
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    REQUIRE(RequestTextureUpdate(session.Device, textures[1], {0, 0, 0, 2, 1}, pixels, 8, 8, tickets[1]) ==
            RasterStatus::Pending);
    reference::Transfers[0][0] = RasterStatus::Failed;
    CHECK(GetStatus(session.Device, tickets[1]) == RasterStatus::Failed);
    CHECK(GetStatus(session.Device, textures[1]) == RasterStatus::Failed);
    REQUIRE(Release(session.Device, tickets[1]) == RasterStatus::Ready);
}

TEST_CASE("R8 texture copies retain source and validate undefined and overlapping data", "[rhi][r8]")
{
    Session session;
    uint8 pixels[16]{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    TextureHandle source, destination;
    REQUIRE(CreateTexture(session.Device, {2, 2}, {pixels, 8}, source) == RasterStatus::Ready);
    TextureDescription description;
    description.Width = description.Height = 2;
    description.DeferredInitialization = true;
    REQUIRE(CreateTexture(session.Device, description, {}, destination) == RasterStatus::Ready);
    TransferTicket copy;
    CHECK(RequestTextureCopy(session.Device, source, {0, 0, 0, 2, 2}, source, {0, 0, 0, 2, 2}, copy) ==
          RasterStatus::InvalidDescription);
    CHECK(RequestTextureCopy(session.Device, source, {0, 0, 0, 1, 1}, destination, {0, 0, 0, 1, 1}, copy) ==
          RasterStatus::InvalidState);
    REQUIRE(RequestTextureCopy(session.Device, source, {0, 0, 0, 2, 2}, destination, {0, 0, 0, 2, 2}, copy) ==
            RasterStatus::Pending);
    TransferTicket other;
    CHECK(RequestTextureUpdate(session.Device, source, {0, 0, 0, 2, 2}, pixels, 16, 8, other) ==
          RasterStatus::InvalidState);
    REQUIRE(Destroy(session.Device, source) == RasterStatus::Ready);
    CHECK(reference::Destroys[1] == 0);
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(GetStatus(session.Device, copy) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, copy) == RasterStatus::Ready);
    CHECK(reference::Destroys[1] == 1);
    ReadbackTicket read;
    REQUIRE(RequestTextureReadback(session.Device, destination, {0, 0, 0, 2, 2}, 8, read) == RasterStatus::Pending);
    reference::Transfers[1][0] = RasterStatus::Ready;
    uint8 actual[16]{};
    REQUIRE(CopyReadback(session.Device, read, actual, 16) == RasterStatus::Ready);
    CHECK(std::memcmp(actual, pixels, 16) == 0);
    REQUIRE(Release(session.Device, read) == RasterStatus::Ready);
}

TEST_CASE("R8 loss and restart invalidate mutation ownership", "[rhi][r8]")
{
    Session session;
    uint8 bytes[16]{};
    BufferHandle buffer;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Uniform, 16}, bytes, buffer) == RasterStatus::Ready);
    TransferTicket ticket;
    REQUIRE(RequestBufferUpdate(session.Device, buffer, 0, bytes, 16, ticket) == RasterStatus::Pending);
    internal::Fail(backend::PendingToken, StartupError::DeviceLost);
    CHECK(GetStatus(session.Device, ticket) == RasterStatus::DeviceLost);
    Session replacement;
    CHECK(GetStatus(replacement.Device, ticket) == RasterStatus::InvalidHandle);
    CHECK(RequestBufferUpdate(replacement.Device, buffer, 0, bytes, 16, ticket) == RasterStatus::InvalidState);
    TransferTicket fresh;
    CHECK(RequestBufferUpdate(replacement.Device, buffer, 0, bytes, 16, fresh) == RasterStatus::InvalidHandle);
}

TEST_CASE("R8 index updates refresh CPU draw bounds", "[rhi][r8]")
{
    Session session;
    Scene scene(session.Device);
    const uint16 changed[2]{99, 1};
    TransferTicket update;
    REQUIRE(RequestBufferUpdate(session.Device, scene.Indices, 0, reinterpret_cast<const uint8*>(changed), 4, update) ==
            RasterStatus::Pending);
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(GetStatus(session.Device, update) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, update) == RasterStatus::Ready);
    CommandBatch batch;
    REQUIRE(BeginCommands(session.Device, batch) == RasterStatus::Ready);
    CHECK(RecordDraw(session.Device, batch, scene.Draw()) == RasterStatus::InvalidDescription);
    REQUIRE(DiscardCommands(session.Device, batch) == RasterStatus::Ready);
}

TEST_CASE("R8 staging startup limits reject malformed requests", "[rhi][r8]")
{
    DeviceDescription description;
    SECTION("zero slices")
    {
        description.Storage.TransferSlices = 0;
    }
    SECTION("too many slices")
    {
        description.Storage.TransferSlices = 5;
    }
    SECTION("too few bytes")
    {
        description.Storage.TransferBytes = 252;
    }
    SECTION("too many bytes")
    {
        description.Storage.TransferBytes = 262148;
    }
    SECTION("unaligned bytes")
    {
        description.Storage.TransferBytes = 257;
    }
    CHECK(ValidateDeviceDescription(description) == DeviceStatus::InvalidDescription);
}

TEST_CASE("R8 format copy uses are negotiated from implemented profiles", "[rhi][r8]")
{
    Session session;
    FormatCapabilities format;
    REQUIRE(GetFormatCapabilities(session.Device, RasterFormat::Rgba8Unorm, format) == RasterStatus::Ready);
    CHECK(format.Uses.CopySource);
    CHECK(format.Uses.CopyDestination);
    REQUIRE(GetFormatCapabilities(session.Device, RasterFormat::Rgba8Srgb, format) == RasterStatus::Ready);
    CHECK_FALSE(format.Uses.CopySource);
    CHECK_FALSE(format.Uses.CopyDestination);
}

TEST_CASE("R9 dimensional uploads validate slices blocks and retained typed views", "[rhi][r9]")
{
    Session session;
    uint8 bytes[768]{};
    TextureHandle texture;
    TextureDescription description
    {
        .Width = 4,
        .Height = 4,
        .Dimension = TextureDimension::D2Array,
        .DepthOrLayers = 3,
    };
    CHECK(CreateTexture(session.Device, description, {{bytes, 191}, 16}, texture) == RasterStatus::InvalidDescription);
    CHECK(IsNull(texture));
    REQUIRE(CreateTexture(session.Device, description, {{bytes, 192}, 16}, texture) == RasterStatus::Ready);
    TextureResidency residency;
    REQUIRE(GetTextureResidency(session.Device, texture, residency) == RasterStatus::Ready);
    CHECK(residency.InitializedBytes == 192);
    TextureViewHandle layer;
    REQUIRE(CreateTextureView(session.Device, texture, { .BaseLayer = 1, .LayerCount = 1 }, layer) ==
            RasterStatus::Ready);
    TextureViewHandle invalid;
    CHECK(CreateTextureView(session.Device, texture, { .BaseLayer = 2, .LayerCount = 2 }, invalid) ==
          RasterStatus::InvalidDescription);
    CHECK(GetStatus(session.Device, invalid) == RasterStatus::InvalidHandle);
    const RasterBinding entry{0, RasterBindingKind::Texture2DArray, RasterVisibility::Fragment, 0};
    BindingLayoutHandle layout;
    REQUIRE(CreateBindingLayout(session.Device, {&entry, 1}, layout) == RasterStatus::Ready);
    BindingSetHandle set;
    const RasterBindingResource resource{ .Texture = layer };
    CHECK(CreateBindingSet(session.Device, layout, {&resource, 1}, set) == RasterStatus::InvalidDescription);
    TextureViewHandle array;
    REQUIRE(CreateTextureView(session.Device, texture, array) == RasterStatus::Ready);
    const RasterBindingResource matching{ .Texture = array };
    REQUIRE(CreateBindingSet(session.Device, layout, {&matching, 1}, set) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, array) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, set) == RasterStatus::Ready);

    TextureHandle volume;
    description = { .Width = 4, .Height = 4, .MipLevels = 3, .Dimension = TextureDimension::D3, .DepthOrLayers = 4 };
    const TextureMipUpload mips[3]{{0, 16}, {256, 8}, {288, 4}};
    REQUIRE(CreateTexture(session.Device, description, {{bytes, 292}, 0, mips}, volume) == RasterStatus::Ready);
    REQUIRE(GetTextureResidency(session.Device, volume, residency) == RasterStatus::Ready);
    CHECK(residency.InitializedBytes == 292);
    CHECK(CreateTextureView(session.Device, volume, { .Dimension = TextureDimension::D3, .BaseLayer = 1 }, invalid) ==
          RasterStatus::InvalidDescription);
    REQUIRE(CreateTextureView(session.Device, volume, { .Dimension = TextureDimension::D3, .BaseMip = 2 }, invalid) ==
            RasterStatus::Ready);

    TextureHandle compressed;
    description = { .Width = 8, .Height = 4, .Format = RasterFormat::Bc1RgbaUnorm, .MipLevels = 4 };
    const TextureMipUpload blocks[4]{{0, 16}, {16, 8}, {24, 8}, {32, 8}};
    REQUIRE(CreateTexture(session.Device, description, {{bytes, 40}, 0, blocks}, compressed) == RasterStatus::Ready);
    REQUIRE(GetTextureResidency(session.Device, compressed, residency) == RasterStatus::Ready);
    CHECK(residency.InitializedBytes == 40);
    TextureHandle malformed;
    CHECK(CreateTexture(session.Device, description, {{bytes, 39}, 0, blocks}, malformed) ==
          RasterStatus::InvalidDescription);
    description.Width = 7;
    CHECK(CreateTexture(session.Device, description, {{bytes, 40}, 0, blocks}, malformed) == RasterStatus::Unsupported);
    description = { .Width = 4, .Height = 2, .Dimension = TextureDimension::Cube, .DepthOrLayers = 6 };
    CHECK(CreateTexture(session.Device, description, {{bytes, 192}, 16}, malformed) ==
          RasterStatus::InvalidDescription);
    description = { .Width = 4, .Height = 4, .Format = RasterFormat::Rgba16Float };
    REQUIRE(CreateTexture(session.Device, description, {{bytes, 128}, 32}, malformed) == RasterStatus::Ready);
}

TEST_CASE("R9 partial mip views retain CPU subrange and undefined range exclusion", "[rhi][r9]")
{
    Session session;
    const uint8 bytes[84]{};
    const TextureMipUpload mips[3]{{0, 16}, {64, 8}, {80, 4}};
    TextureHandle texture;
    REQUIRE(CreateTexture(session.Device,
                          { .Width = 4, .Height = 4, .MipLevels = 3, .RetainCpuContent = true },
                          {bytes, 0, mips},
                          texture) == RasterStatus::Ready);
    TextureViewHandle view;
    REQUIRE(CreateTextureView(session.Device, texture, { .BaseMip = 1, .MipCount = 1 }, view) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    TextureCpuContent content;
    REQUIRE(GetTextureCpuContent(session.Device, view, 0, content) == RasterStatus::Ready);
    CHECK(content.Width == 2);
    CHECK(content.Size == 16);
    CHECK(GetTextureCpuContent(session.Device, view, 1, content) == RasterStatus::InvalidDescription);
    CHECK(content.Width == 2);
    TextureHandle deferred;
    REQUIRE(CreateTexture(session.Device,
                          { .Width = 4, .Height = 4, .MipLevels = 3, .DeferredInitialization = true },
                          {},
                          deferred) == RasterStatus::Ready);
    TextureViewHandle forbidden;
    CHECK(CreateTextureView(session.Device, deferred, { .BaseMip = 2, .MipCount = 1 }, forbidden) ==
          RasterStatus::InvalidState);
    TransferTicket transfer;
    REQUIRE(RequestTextureUpdate(session.Device, deferred, {2, 0, 0, 1, 1}, bytes, 4, 4, transfer) ==
            RasterStatus::Pending);
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    REQUIRE(GetStatus(session.Device, transfer) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, transfer) == RasterStatus::Ready);
    REQUIRE(CreateTextureView(session.Device, deferred, { .BaseMip = 2, .MipCount = 1 }, forbidden) ==
            RasterStatus::Ready);
    TextureViewHandle full;
    CHECK(CreateTextureView(session.Device, deferred, full) == RasterStatus::InvalidState);
    CHECK(RequestTextureUpdate(session.Device, deferred, {0, 0, 0, 4, 4}, bytes, 64, 16, transfer) ==
          RasterStatus::InvalidState);
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, deferred, state) == RasterStatus::Ready);
    CHECK_FALSE(state.ColorDefined);
    GraphVersion version;
    REQUIRE(ImportGraphTexture(session.Device, graph, deferred, state, RasterTextureUse::SampledBoth, version) ==
            RasterStatus::Ready);
    const GraphUse use{version, GraphAccessMode::Sampled};
    GraphPassDescription pass;
    pass.Name = "R9 partial root";
    pass.Uses = &use;
    pass.UseCount = 1;
    pass.SideEffect = true;
    REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    REQUIRE(AddGraphRoot(session.Device, graph, version, GraphRoot::Readback) == RasterStatus::Ready);
    GraphReport report;
    CHECK(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::InvalidDescription);
}
namespace
{
TextureViewHandle AttachmentView(DeviceHandle device, TextureHandle texture, uint32 mip = 0, uint32 layer = 0)
{
    TextureViewHandle view;
    REQUIRE(CreateTextureView(device,
                              texture,
                              {
                                  .BaseMip = mip,
                                  .MipCount = 1,
                                  .BaseLayer = layer,
                                  .LayerCount = 1,
                                  .Attachment = true,
                              },
                              view) == RasterStatus::Ready);
    return view;
}
} // namespace
TEST_CASE("R10 pooled attachment incarnations reject prior color depth and stencil contents", "[rhi][graph][r10]")
{
    Session session;
    const auto partial = GENERATE(false, true);
    const auto aspect = GENERATE(0, 1, 2);
    const TextureDescription description
    {
        .Width = 8,
        .Height = 8,
        .Attachment = true,
        .MipLevels = partial ? 2U : 1U,
        .Dimension = partial ? TextureDimension::D2Array : TextureDimension::D2,
        .DepthOrLayers = partial ? 2U : 1U,
        .Stencil = true,
    };
    const uint32 selected = partial ? 1U : 0U;
    OrderedGraph first;
    GraphVersion initial, written;
    TextureHandle texture;
    REQUIRE(CreateOrderedGraph(session.Device, first) == RasterStatus::Ready);
    REQUIRE(CreateGraphTexture(session.Device, first, description, initial, texture) == RasterStatus::Ready);
    REQUIRE(NextGraphVersion(session.Device, first, initial, written) == RasterStatus::Ready);
    auto view = AttachmentView(session.Device, texture, selected, selected);
    RasterColorAttachment color{ .View = view };
    GraphUse use{written, GraphAccessMode::ColorWrite};
    GraphPassDescription pass;
    pass.Name = "pooled";
    pass.Uses = &use;
    pass.UseCount = 1;
    pass.SideEffect = true;
    pass.Attachment.Colors = &color;
    pass.Attachment.ColorCount = 1;
    pass.Attachment.DepthStore = RasterStore::Store;
    pass.Attachment.StencilStore = RasterStore::Store;
    REQUIRE(AddGraphPass(session.Device, first, pass) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, first, report) == RasterStatus::Ready);
    SubmissionToken token;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, first, token) == RasterStatus::Ready);
    reference::Completed = reference::Submitted;
    REQUIRE(GetStatus(session.Device, token) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, view) == RasterStatus::Ready);

    OrderedGraph second;
    GraphVersion fresh, output;
    TextureHandle reused;
    REQUIRE(CreateOrderedGraph(session.Device, second) == RasterStatus::Ready);
    REQUIRE(CreateGraphTexture(session.Device, second, description, fresh, reused) == RasterStatus::Ready);
    CHECK(reference::Creates[static_cast<usize>(internal::RasterKind::Texture)] == 1);
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, reused, state) == RasterStatus::Ready);
    CHECK_FALSE(state.ColorDefined);
    CHECK_FALSE(state.DepthDefined);
    CHECK_FALSE(state.StencilDefined);
    REQUIRE(NextGraphVersion(session.Device, second, fresh, output) == RasterStatus::Ready);
    view = AttachmentView(session.Device, reused, selected, selected);
    color.View = view;
    color.Load = aspect == 0 ? RasterLoad::Load : RasterLoad::Clear;
    pass.Attachment.DepthLoad = aspect == 1 ? RasterLoad::Load : RasterLoad::Clear;
    pass.Attachment.StencilLoad = aspect == 2 ? RasterLoad::Load : RasterLoad::Clear;
    use = {output, aspect == 0 ? GraphAccessMode::ColorReadWrite : GraphAccessMode::ColorWrite};
    REQUIRE(AddGraphPass(session.Device, second, pass) == RasterStatus::Ready);
    const auto passes = reference::Passes;
    CHECK(CompileOrderedGraph(session.Device, second, report) == RasterStatus::InvalidDescription);
    CHECK(reference::Passes == passes);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    CHECK(BeginRasterPass(session.Device, pass.Attachment) == RasterStatus::InvalidDescription);
    CHECK(reference::Passes == passes);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    REQUIRE(DiscardOrderedGraph(session.Device, second) == RasterStatus::Ready);
}
TEST_CASE("R10 selected attachment initialization preserves undefined siblings and aspects", "[rhi][raster][r10]")
{
    Session session;
    TextureHandle texture;
    REQUIRE(CreateTexture(session.Device,
                          {
                              .Width = 8,
                              .Height = 8,
                              .Attachment = true,
                              .MipLevels = 3,
                              .Dimension = TextureDimension::D2Array,
                              .DepthOrLayers = 2,
                              .Stencil = true,
                          },
                          {},
                          texture) == RasterStatus::Ready);
    auto selected = AttachmentView(session.Device, texture, 1, 1);
    auto sibling = AttachmentView(session.Device, texture, 1, 0);
    RasterColorAttachment color{ .View = selected };
    RasterPassDescription pass;
    pass.Colors = &color;
    pass.ColorCount = 1;
    pass.DepthStore = RasterStore::Store;
    pass.StencilStore = RasterStore::Store;
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    color.Load = RasterLoad::Load;
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidDescription);
    CHECK(reference::Passes == 0);
    color.Load = RasterLoad::Clear;
    REQUIRE(BeginRasterPass(session.Device, pass) == RasterStatus::Ready);
    color.Load = RasterLoad::Load;
    pass.DepthLoad = RasterLoad::Load;
    pass.StencilLoad = RasterLoad::Load;
    REQUIRE(BeginRasterPass(session.Device, pass) == RasterStatus::Ready);
    color.View = sibling;
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidDescription);
    color.View = selected;
    color.Store = RasterStore::Discard;
    REQUIRE(BeginRasterPass(session.Device, pass) == RasterStatus::Ready);
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidDescription);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, texture, state) == RasterStatus::Ready);
    CHECK_FALSE(state.ColorDefined);
    CHECK_FALSE(state.DepthDefined);
    CHECK_FALSE(state.StencilDefined);
}
TEST_CASE("R10 attachment sets reject mismatched extents samples and whole-allocation feedback", "[rhi][raster][r10]")
{
    Session session;
    TextureHandle a, b, small, msaa;
    REQUIRE(CreateTexture(session.Device,
                          {
                              .Width = 8,
                              .Height = 8,
                              .Attachment = true,
                          },
                          {},
                          a) == RasterStatus::Ready);
    REQUIRE(CreateTexture(session.Device,
                          {
                              .Width = 8,
                              .Height = 8,
                              .Attachment = true,
                          },
                          {},
                          b) == RasterStatus::Ready);
    REQUIRE(CreateTexture(session.Device, { .Width = 4, .Height = 4, .Attachment = true }, {}, small) ==
            RasterStatus::Ready);
    REQUIRE(CreateTexture(session.Device,
                          {
                              .Width = 8,
                              .Height = 8,
                              .Attachment = true,
                              .SampleCount = 4,
                          },
                          {},
                          msaa) == RasterStatus::Ready);
    auto av = AttachmentView(session.Device, a), bv = AttachmentView(session.Device, b),
         sv = AttachmentView(session.Device, small), mv = AttachmentView(session.Device, msaa);
    TextureViewHandle sampled;
    CHECK(CreateTextureView(session.Device, msaa, sampled) == RasterStatus::InvalidDescription);
    RasterColorAttachment colors[2]{{ .View = av }, { .View = bv }};
    RasterPassDescription pass;
    pass.Colors = colors;
    pass.ColorCount = 2;
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(BeginRasterPass(session.Device, pass) == RasterStatus::Ready);
    CHECK(reference::Pass.ColorCount == 2);
    colors[1].View = av;
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidDescription);
    colors[1].View = sv;
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidDescription);
    colors[1].View = mv;
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidDescription);
    colors[0].Resolve = bv;
    colors[1].View = bv;
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidDescription);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}
TEST_CASE("R10 color resolve defines its destination independently of source store", "[rhi][raster][r10]")
{
    Session session;
    TextureHandle source, destination;
    REQUIRE(
        CreateTexture(session.Device, { .Width = 8, .Height = 8, .Attachment = true, .SampleCount = 4 }, {}, source) ==
        RasterStatus::Ready);
    REQUIRE(CreateTexture(session.Device, { .Width = 8, .Height = 8, .Attachment = true }, {}, destination) ==
            RasterStatus::Ready);
    auto src = AttachmentView(session.Device, source), dst = AttachmentView(session.Device, destination);
    RasterColorAttachment color{ .View = src, .Resolve = dst, .Store = RasterStore::Discard };
    RasterPassDescription pass;
    pass.Colors = &color;
    pass.ColorCount = 1;
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(BeginRasterPass(session.Device, pass) == RasterStatus::Ready);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, source, state) == RasterStatus::Ready);
    CHECK_FALSE(state.ColorDefined);
    REQUIRE(GetTextureState(session.Device, destination, state) == RasterStatus::Ready);
    CHECK(state.ColorDefined);
    CHECK_FALSE(state.DepthDefined);
    color.View = dst;
    color.Resolve = {};
    color.Load = RasterLoad::Load;
    color.Store = RasterStore::Store;
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(BeginRasterPass(session.Device, pass) == RasterStatus::Ready);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}
TEST_CASE("R10 typed aspect bindings and writable views cannot be interchanged", "[rhi][raster][r10]")
{
    Session session;
    TextureHandle texture;
    REQUIRE(
        CreateTexture(session.Device, { .Width = 8, .Height = 8, .Attachment = true, .Stencil = true }, {}, texture) ==
        RasterStatus::Ready);
    auto attachment = AttachmentView(session.Device, texture);
    TextureViewHandle depth, stencil, color;
    REQUIRE(CreateTextureView(session.Device,
                              texture,
                              {
                                  .Aspect = TextureAspect::Depth,
                              },
                              depth) == RasterStatus::Ready);
    REQUIRE(CreateTextureView(session.Device, texture, { .Aspect = TextureAspect::Stencil }, stencil) ==
            RasterStatus::Ready);
    REQUIRE(CreateTextureView(session.Device, texture, color) == RasterStatus::Ready);
    const RasterBinding binding{0, RasterBindingKind::TextureDepth2D, RasterVisibility::Fragment, 0};
    BindingLayoutHandle layout;
    BindingSetHandle set;
    REQUIRE(CreateBindingLayout(session.Device, {&binding, 1}, layout) == RasterStatus::Ready);
    RasterBindingResource resource{ .Texture = attachment };
    CHECK(CreateBindingSet(session.Device, layout, {&resource, 1}, set) == RasterStatus::InvalidDescription);
    resource.Texture = color;
    CHECK(CreateBindingSet(session.Device, layout, {&resource, 1}, set) == RasterStatus::InvalidDescription);
    resource.Texture = stencil;
    CHECK(CreateBindingSet(session.Device, layout, {&resource, 1}, set) == RasterStatus::InvalidDescription);
    resource.Texture = depth;
    REQUIRE(CreateBindingSet(session.Device, layout, {&resource, 1}, set) == RasterStatus::Ready);
}
TEST_CASE("R10 GDI requires every MRT and resolve output and retains attachment views", "[rhi][raster][r10]")
{
    Session session;
    TextureHandle textures[3];
    TextureViewHandle views[3];
    for (usize i = 0; i < 3; ++i)
    {
        REQUIRE(CreateTexture(session.Device,
                              { .Width = 8, .Height = 8, .Attachment = true, .SampleCount = i == 0 ? 4U : 1U },
                              {},
                              textures[i]) == RasterStatus::Ready);
        views[i] = AttachmentView(session.Device, textures[i]);
    }
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphVersion imports[3], outputs[3];
    for (usize i = 0; i < 3; ++i)
    {
        RasterTextureState state;
        REQUIRE(GetTextureState(session.Device, textures[i], state) == RasterStatus::Ready);
        REQUIRE(
            ImportGraphTexture(session.Device, graph, textures[i], state, RasterTextureUse::SampledBoth, imports[i]) ==
            RasterStatus::Ready);
        REQUIRE(NextGraphVersion(session.Device, graph, imports[i], outputs[i]) == RasterStatus::Ready);
    }
    RasterColorAttachment color{ .View = views[0], .Resolve = views[1], .Store = RasterStore::Discard };
    GraphUse uses[2]{{outputs[0], GraphAccessMode::ColorWrite}, {outputs[1], GraphAccessMode::ColorWrite}};
    GraphPassDescription pass;
    pass.Name = "resolve";
    pass.Attachment.Colors = &color;
    pass.Attachment.ColorCount = 1;
    pass.Uses = uses;
    pass.UseCount = 2;
    REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    REQUIRE(AddGraphRoot(session.Device, graph, outputs[1], GraphRoot::Readback) == RasterStatus::Ready);
    // Caller metadata and public attachment-view ownership can end after AddGraphPass.
    color = {};
    REQUIRE(Destroy(session.Device, views[0]) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, views[1]) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    SubmissionToken token;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, token) == RasterStatus::Ready);
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, textures[1], state) == RasterStatus::Ready);
    CHECK(state.ColorDefined);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::View)] == 0);
    reference::Completed = reference::Submitted;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    CHECK(reference::Destroys[static_cast<usize>(internal::RasterKind::View)] == 2);
}
TEST_CASE("R10 graph compilation rejects undeclared resolve outputs before encoding", "[rhi][raster][r10]")
{
    Session session;
    TextureHandle source, destination;
    REQUIRE(
        CreateTexture(session.Device, { .Width = 8, .Height = 8, .Attachment = true, .SampleCount = 4 }, {}, source) ==
        RasterStatus::Ready);
    REQUIRE(CreateTexture(session.Device, { .Width = 8, .Height = 8, .Attachment = true }, {}, destination) ==
            RasterStatus::Ready);
    auto sourceView = AttachmentView(session.Device, source),
         destinationView = AttachmentView(session.Device, destination);
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphVersion imported[2], written[2];
    TextureHandle textures[2]{source, destination};
    for (usize i = 0; i < 2; ++i)
    {
        RasterTextureState state;
        REQUIRE(GetTextureState(session.Device, textures[i], state) == RasterStatus::Ready);
        REQUIRE(
            ImportGraphTexture(session.Device, graph, textures[i], state, RasterTextureUse::SampledBoth, imported[i]) ==
            RasterStatus::Ready);
        REQUIRE(NextGraphVersion(session.Device, graph, imported[i], written[i]) == RasterStatus::Ready);
    }
    const RasterColorAttachment color{ .View = sourceView, .Resolve = destinationView };
    const GraphUse use{written[0], GraphAccessMode::ColorWrite};
    GraphPassDescription pass;
    pass.Name = "missing resolve declaration";
    pass.Attachment.Colors = &color;
    pass.Attachment.ColorCount = 1;
    pass.Uses = &use;
    pass.UseCount = 1;
    pass.SideEffect = true;
    REQUIRE(AddGraphPass(session.Device, graph, pass) == RasterStatus::Ready);
    GraphReport report;
    CHECK(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::InvalidDescription);
    CHECK(reference::Passes == 0);
    CHECK(reference::Draws == 0);
}
TEST_CASE("R10 sampling checks the retained view's depth aspect independently of color", "[rhi][raster][r10]")
{
    Session session;
    Scene scene(session.Device);
    TextureHandle texture;
    TextureViewHandle depth;
    REQUIRE(CreateTexture(session.Device, { .Width = 8, .Height = 8, .Attachment = true }, {}, texture) ==
            RasterStatus::Ready);
    REQUIRE(CreateTextureView(session.Device,
                              texture,
                              {
                                  .Aspect = TextureAspect::Depth,
                              },
                              depth) == RasterStatus::Ready);
    const RasterBinding bindings[2]{{0, RasterBindingKind::UniformBuffer, RasterVisibility::Both, 16},
                                    {1, RasterBindingKind::TextureDepth2D, RasterVisibility::Fragment, 0}};
    BindingLayoutHandle layout;
    BindingSetHandle set;
    RasterPipelineHandle pipeline;
    REQUIRE(CreateBindingLayout(session.Device, bindings, layout) == RasterStatus::Ready);
    const RasterBindingResource resources[2]{
    {
        .Buffer = scene.Uniform,
        .Size = 16,
    },
                                             {
                                                 .Binding = 1,
                                                 .Texture = depth,
                                             }};
    REQUIRE(CreateBindingSet(session.Device, layout, resources, set) == RasterStatus::Ready);
    const RasterVertexStream stream{12, false};
    const RasterVertexAttribute attribute{0, 0, 0, RasterVertexFormat::Float3};
    REQUIRE(CreateRasterPipeline(session.Device,
                                 {scene.Vertex, scene.Fragment, layout, {&stream, 1}, {&attribute, 1}},
                                 pipeline) == RasterStatus::Ready);
    auto draw = scene.Draw();
    draw.Pipeline = pipeline;
    draw.Bindings = set;
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(BeginRasterPass(session.Device, { .Color = texture }) == RasterStatus::Ready);
    REQUIRE(BeginRasterPass(session.Device, {}) == RasterStatus::Ready);
    CHECK(DrawIndexed(session.Device, draw) == RasterStatus::InvalidDescription);
    REQUIRE(BeginRasterPass(session.Device,
                            { .Color = texture, .ColorLoad = RasterLoad::Load, .DepthStore = RasterStore::Store }) ==
            RasterStatus::Ready);
    REQUIRE(BeginRasterPass(session.Device, {}) == RasterStatus::Ready);
    REQUIRE(DrawIndexed(session.Device, draw) == RasterStatus::Ready);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}
TEST_CASE("R8 transfers share the R10 selected attachment mip initialization ledger", "[rhi][raster][r10]")
{
    Session session;
    TextureHandle texture;
    REQUIRE(CreateTexture(session.Device,
                          {
                              .Width = 8,
                              .Height = 8,
                              .Attachment = true,
                              .MipLevels = 2,
                          },
                          {},
                          texture) == RasterStatus::Ready);
    uint8 pixels[64]{};
    pixels[0] = 203;
    TransferTicket upload;
    REQUIRE(RequestTextureUpdate(session.Device, texture, {1, 0, 0, 4, 4}, pixels, sizeof(pixels), 16, upload) ==
            RasterStatus::Pending);
    reference::Transfers[0][0] = RasterStatus::Ready;
    REQUIRE(GetStatus(session.Device, upload) == RasterStatus::Ready);
    REQUIRE(Release(session.Device, upload) == RasterStatus::Ready);
    RasterTextureState state;
    REQUIRE(GetTextureState(session.Device, texture, state) == RasterStatus::Ready);
    CHECK_FALSE(state.ColorDefined);
    ReadbackTicket read;
    CHECK(RequestTextureReadback(session.Device, texture, {0, 0, 0, 8, 8}, 32, read) == RasterStatus::InvalidState);
    REQUIRE(RequestTextureReadback(session.Device, texture, {1, 0, 0, 4, 4}, 16, read) == RasterStatus::Pending);
    reference::Transfers[1][0] = RasterStatus::Ready;
    REQUIRE(GetStatus(session.Device, read) == RasterStatus::Ready);
    uint8 copied[64]{};
    REQUIRE(CopyReadback(session.Device, read, copied, sizeof(copied)) == RasterStatus::Ready);
    CHECK(copied[0] == 203);
}

TEST_CASE("R10 rejects NaN color and depth clears before backend calls", "[rhi][raster][r10]")
{
    Session session;
    const uint32 nanBits = 0x7fc00000U;
    float32 nan = 0;
    std::memcpy(&nan, &nanBits, sizeof(nan));
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    RasterPassDescription pass;
    pass.ClearDepth = nan;
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidDescription);
    pass.ClearDepth = 1;
    pass.Clear[2] = nan;
    CHECK(BeginRasterPass(session.Device, pass) == RasterStatus::InvalidDescription);
    CHECK(reference::Passes == 0);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
}

TEST_CASE("BC5 validates encoded mip and slice footprints before publication", "[rhi][r18][texture]")
{
    Session session;
    constexpr usize ENCODED_BYTES = 672;
    constexpr usize MIP_COUNT = 4;
    StaticArray<uint8, ENCODED_BYTES> bytes{};
    StaticArray<TextureMipUpload, MIP_COUNT> mips{{{0, 32}, {32, 16}, {48, 16}, {64, 16}}};
    TextureDescription description{ .Width = 8, .Height = 4, .Format = RasterFormat::Bc5RgUnorm, .MipLevels = 4 };
    usize byteCount = 80;
    SECTION("two-dimensional") {}
    SECTION("array")
    {
        description.Dimension = TextureDimension::D2Array;
        description.DepthOrLayers = 3;
        mips = {{{0, 32}, {96, 16}, {144, 16}, {192, 16}}};
        byteCount = 240;
    }
    SECTION("cube")
    {
        description.Height = 8;
        description.Dimension = TextureDimension::Cube;
        description.DepthOrLayers = 6;
        mips = {{{0, 32}, {384, 16}, {480, 16}, {576, 16}}};
        byteCount = 672;
    }
    TextureHandle texture;
    REQUIRE(CreateTexture(session.Device, description, {{bytes.GetData(), byteCount}, 0, mips}, texture) ==
            RasterStatus::Ready);
    TextureResidency residency;
    REQUIRE(GetTextureResidency(session.Device, texture, residency) == RasterStatus::Ready);
    CHECK(residency.InitializedBytes == byteCount);
    TextureViewHandle view;
    TextureViewDescription selected;
    selected.Dimension = description.Dimension;
    selected.BaseMip = 3;
    selected.MipCount = 1;
    REQUIRE(CreateTextureView(session.Device, texture, selected, view) == RasterStatus::Ready);

    StorageUsage before;
    REQUIRE(GetStorageUsage(session.Device, before) == RasterStatus::Ready);
    const usize publications = reference::Creates[static_cast<usize>(internal::RasterKind::Texture)];
    TextureHandle preserved = texture;
    CHECK(CreateTexture(session.Device, description, {{bytes.GetData(), byteCount - 1}, 0, mips}, preserved) ==
          RasterStatus::InvalidDescription);
    CHECK(GetStatus(session.Device, preserved) == RasterStatus::Ready);
    mips[1].RowPitch = 8;
    CHECK(CreateTexture(session.Device, description, {{bytes.GetData(), byteCount}, 0, mips}, preserved) ==
          RasterStatus::InvalidDescription);
    CHECK(reference::Creates[static_cast<usize>(internal::RasterKind::Texture)] == publications);
    StorageUsage after;
    REQUIRE(GetStorageUsage(session.Device, after) == RasterStatus::Ready);
    CHECK(after.ResourceBytes == before.ResourceBytes);
    CHECK(after.HostBytes == before.HostBytes);
    REQUIRE(Destroy(session.Device, preserved) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, texture) == RasterStatus::InvalidHandle);
    CHECK(GetStatus(session.Device, view) == RasterStatus::Ready);
}

TEST_CASE("BC5 rejects unsupported allocation roles and rolls back native failure", "[rhi][r18][texture]")
{
    Session session;
    constexpr usize BLOCK_BYTES = 16;
    StaticArray<uint8, BLOCK_BYTES> bytes{};
    TextureHandle texture;
    TextureDescription description{ .Width = 4, .Height = 4, .Format = RasterFormat::Bc5RgUnorm };
    SECTION("attachment")
    {
        description.Attachment = true;
        CHECK(CreateTexture(session.Device, description, {}, texture) == RasterStatus::InvalidDescription);
    }
    SECTION("CPU content")
    {
        description.RetainCpuContent = true;
        CHECK(CreateTexture(session.Device, description, {bytes, 16}, texture) == RasterStatus::InvalidDescription);
    }
    SECTION("volume")
    {
        description.Dimension = TextureDimension::D3;
        CHECK(CreateTexture(session.Device, description, {bytes, 16}, texture) == RasterStatus::Unsupported);
    }
    SECTION("unaligned base")
    {
        description.Width = 3;
        CHECK(CreateTexture(session.Device, description, {bytes, 16}, texture) == RasterStatus::Unsupported);
    }
    SECTION("backend failure")
    {
        StorageUsage before;
        REQUIRE(GetStorageUsage(session.Device, before) == RasterStatus::Ready);
        reference::Next = RasterStatus::OutOfMemory;
        CHECK(CreateTexture(session.Device, description, {bytes, 16}, texture) == RasterStatus::OutOfMemory);
        StorageUsage after;
        REQUIRE(GetStorageUsage(session.Device, after) == RasterStatus::Ready);
        CHECK(after.ResourceBytes == before.ResourceBytes);
        reference::Next = RasterStatus::Ready;
        REQUIRE(CreateTexture(session.Device, description, {bytes, 16}, texture) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    }
    CHECK(IsNull(texture) == true);
}

TEST_CASE("Negotiated large texture dimensions preserve byte budgets and outputs", "[rhi][r18][texture]")
{
    Session session({}, 16384);
    constexpr usize WIDTH = 8240;
    constexpr usize BYTE_COUNT = WIDTH * 4;
    StaticArray<uint8, BYTE_COUNT> bytes{};
    TextureHandle texture;
    REQUIRE(CreateTexture(session.Device, { .Width = WIDTH, .Height = 1 }, {bytes, BYTE_COUNT}, texture) ==
            RasterStatus::Ready);
    TextureDescription actual;
    REQUIRE(GetTextureDescription(session.Device, texture, actual) == RasterStatus::Ready);
    CHECK(actual.Width == WIDTH);
    constexpr usize LIMIT_BYTE_COUNT = usize{16384} * 4;
    StaticArray<uint8, LIMIT_BYTE_COUNT> boundary{};
    TextureHandle exact;
    REQUIRE(CreateTexture(session.Device, { .Width = 16384, .Height = 1 }, {boundary, LIMIT_BYTE_COUNT}, exact) ==
            RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, exact) == RasterStatus::Ready);
    TextureHandle preserved = texture;
    CHECK(CreateTexture(session.Device, { .Width = 16385, .Height = 1 }, {bytes, BYTE_COUNT}, preserved) ==
          RasterStatus::Unsupported);
    REQUIRE(GetTextureDescription(session.Device, preserved, actual) == RasterStatus::Ready);
    CHECK(actual.Width == WIDTH);
    CHECK(CreateTexture(session.Device, { .Width = WIDTH, .Height = 8200 }, {bytes, BYTE_COUNT}, preserved) ==
          RasterStatus::InvalidDescription);
    REQUIRE(GetTextureDescription(session.Device, preserved, actual) == RasterStatus::Ready);
    CHECK(actual.Width == WIDTH);
    REQUIRE(Destroy(session.Device, preserved) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, texture) == RasterStatus::InvalidHandle);
}

TEST_CASE("BC5 dimensions do not override the resource byte budget", "[rhi][r18][texture]")
{
    StorageLimits storage;
    storage.ResourceBytes = 15;
    Session session(storage);
    constexpr usize BLOCK_BYTES = 16;
    StaticArray<uint8, BLOCK_BYTES> bytes{};
    TextureHandle texture;
    CHECK(CreateTexture(session.Device,
                        { .Width = 4, .Height = 4, .Format = RasterFormat::Bc5RgUnorm },
                        {bytes, BLOCK_BYTES},
                        texture) == RasterStatus::CapacityExceeded);
    CHECK(IsNull(texture) == true);
    CHECK(reference::Creates[static_cast<usize>(internal::RasterKind::Texture)] == 0);
    StorageUsage usage;
    REQUIRE(GetStorageUsage(session.Device, usage) == RasterStatus::Ready);
    CHECK(usage.ResourceBytes == 0);
}

TEST_CASE("Native BC5 edge dimensions retain complete blocks and mip packing", "[rhi][r18][texture]")
{
    Session session({}, 4096, Backend::Metal);
    constexpr usize BYTE_COUNT = 64;
    constexpr usize MIP_COUNT = 3;
    StaticArray<uint8, BYTE_COUNT> bytes{};
    const StaticArray<TextureMipUpload, MIP_COUNT> mips{{{0, 32}, {32, 16}, {48, 16}}};
    FormatCapabilities format;
    REQUIRE(GetFormatCapabilities(session.Device, RasterFormat::Bc5RgUnorm, format) == RasterStatus::Ready);
    CHECK(format.HasUnalignedBlockExtents == true);
    TextureHandle texture;
    REQUIRE(CreateTexture(session.Device,
                          { .Width = 7, .Height = 3, .Format = RasterFormat::Bc5RgUnorm, .MipLevels = 3 },
                          {bytes, 0, mips},
                          texture) == RasterStatus::Ready);
    TextureResidency residency;
    REQUIRE(GetTextureResidency(session.Device, texture, residency) == RasterStatus::Ready);
    CHECK(residency.InitializedBytes == BYTE_COUNT);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
}

TEST_CASE("R18c array and volume extents admit their exact enabled boundary", "[rhi][raster][r18c]")
{
    Session session;
    constexpr uint32 EXTENT = 2048;
    constexpr usize UPLOAD_BYTES = usize{EXTENT} * 4;
    ludus::foundation::StaticArray<uint8, UPLOAD_BYTES> bytes{};
    TextureHandle texture;
    TextureDescription description
    {
        .Width = 1,
        .Height = 1,
        .Dimension = TextureDimension::D2Array,
        .DepthOrLayers = EXTENT,
    };
    SECTION("array") {}
    SECTION("volume")
    {
        description.Dimension = TextureDimension::D3;
    }
    REQUIRE(CreateTexture(session.Device, description, {bytes, 4}, texture) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    description.DepthOrLayers = EXTENT + 1;
    const usize creates = reference::Creates[static_cast<usize>(internal::RasterKind::Texture)];
    CHECK(CreateTexture(session.Device, description, {bytes, 4}, texture) == RasterStatus::Unsupported);
    CHECK(IsNull(texture) == true);
    CHECK(reference::Creates[static_cast<usize>(internal::RasterKind::Texture)] == creates);
}

TEST_CASE("R18c complete upload limit is independent of buffer and logical budgets", "[rhi][raster][r18c]")
{
    Session session;
    reference::RasterLimitOverride = backend::RasterLimits();
    reference::OverrideRasterLimits = true;
    // Configure a smaller complete-upload limit without changing the enabled buffer size.
    reference::RasterLimitOverride.MaxBufferSize = 64;
    reference::RasterLimitOverride.MaxTextureDimension2D = 4096;
    reference::RasterLimitOverride.MaxUniformRange = 16384;
    reference::RasterLimitOverride.MaxTextureUploadBytes = 16;
    constexpr usize BYTE_COUNT = 32;
    ludus::foundation::StaticArray<uint8, BYTE_COUNT> bytes{};
    TextureHandle texture;
    REQUIRE(CreateTexture(session.Device, { .Width = 1, .Height = 1 }, {{bytes.GetData(), 16}, 16}, texture) ==
            RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, texture) == RasterStatus::Ready);
    StorageUsage before;
    REQUIRE(GetStorageUsage(session.Device, before) == RasterStatus::Ready);
    CHECK(CreateTexture(session.Device, { .Width = 1, .Height = 1 }, {bytes, 32}, texture) ==
          RasterStatus::InvalidDescription);
    CHECK(IsNull(texture) == true);
    StorageUsage after;
    REQUIRE(GetStorageUsage(session.Device, after) == RasterStatus::Ready);
    CHECK(after.ResourceBytes == before.ResourceBytes);
}

TEST_CASE("Ordered graph encoding preserves the caller frame and retains submitted resources", "[rhi][graph][encode]")
{
    Session session;
    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphTexture texture(session.Device, graph);
    texture.Add(session.Device, graph);
    REQUIRE(AddGraphRoot(session.Device, graph, texture.Output, GraphRoot::Export) == RasterStatus::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == RasterStatus::Ready);
    CHECK(EncodeOrderedGraph(session.Device, session.Surface, graph) == RasterStatus::InvalidState);
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(EncodeOrderedGraph(session.Device, session.Surface, graph) == RasterStatus::Ready);
    CHECK(reference::Submitted == 0);
    FrameInfo information;
    CHECK(GetFrameInfo(session.Device, session.Surface, information) == DeviceStatus::Ready);
    SubmissionToken completion;
    REQUIRE(EndFrame(session.Device, completion) == RasterStatus::Ready);
    CHECK(GetStatus(session.Device, completion) == RasterStatus::Pending);
    CHECK(DiscardOrderedGraph(session.Device, graph) == RasterStatus::InvalidHandle);
    REQUIRE(Destroy(session.Device, texture.Texture) == RasterStatus::Ready);
    CHECK(reference::Destroys[1] == 0);
    reference::Completed = reference::Submitted;
    REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
    CHECK(reference::Destroys[1] == 1);
}
