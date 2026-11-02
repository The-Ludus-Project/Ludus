#include "internal/lifecycle.h"
#include "reference_raster.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/compute_cache.hpp>
#include <ludus/graphics/rhi/compute_workload.hpp>

#include <catch2/catch_test_macros.hpp>

namespace ludus::graphics::rhi::backend
{
extern foundation::uint32 PendingToken;
extern StartupError ImmediateError;
extern FrameStatus NextFrame;
extern Backend ActiveKind;
} // namespace ludus::graphics::rhi::backend
namespace
{
using namespace ludus::foundation;
using namespace ludus::graphics::rhi;
using Status = RasterStatus;
constexpr usize BUFFER_BYTES = 16;
constexpr usize BUFFER_COUNT = 2;
constexpr usize BINDING_COUNT = 2;
constexpr usize KERNEL_COUNT = 6;
struct Session final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    Session()
    {
        Shutdown();
        reference::Reset();
        backend::ImmediateError = StartupError::None;
        backend::NextFrame = FrameStatus::Ready;
        backend::ActiveKind = Backend::WebGPU;
        DeviceDescription description;
        description.Required.Compute = true;
        REQUIRE(CreateDevice({}, {}, description, Device, Surface) == DeviceStatus::Pending);
        internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
        DeviceInfo info;
        REQUIRE(GetDeviceInfo(Device, info) == DeviceStatus::Ready);
    }
    ~Session() noexcept
    {
        Shutdown();
    }
};
struct Fixture final
{
    StaticArray<uint8, BUFFER_BYTES> Weights{};
    StaticArray<ComputeWorkloadBuffer, BUFFER_COUNT> Buffers{};
    StaticArray<RasterBinding, BINDING_COUNT> Reflection{};
    StaticArray<ComputeWorkloadBinding, BINDING_COUNT> Bindings{};
    StaticArray<ComputeWorkloadKernel, KERNEL_COUNT> Kernels{};
    Fixture()
    {
        Weights[0] = 1;
        Buffers[0] = {BUFFER_BYTES, {Weights.GetData(), Weights.GetSize()}, true};
        Buffers[1] = {BUFFER_BYTES, {}, false};
        Reflection[0] = {0, RasterBindingKind::StorageRead, RasterVisibility::Compute, sizeof(uint32)};
        Reflection[1] = {1, RasterBindingKind::StorageReadWrite, RasterVisibility::Compute, sizeof(uint32)};
        Bindings[0] = {0, 0, 0, BUFFER_BYTES, false};
        Bindings[1] = {1, 1, 0, BUFFER_BYTES, true};
        for (ComputeWorkloadKernel& kernel : Kernels)
        {
            kernel.NameOrNull = "buffer-only stage";
            kernel.Shader.Artifact.Stage = ShaderStage::Compute;
            kernel.Shader.Artifact.Wgsl = "reference compute metadata";
            kernel.Shader.Artifact.WgslEntry = "ComputeMain";
            kernel.Shader.Bindings = {Reflection.GetData(), Reflection.GetSize()};
            kernel.Bindings = {Bindings.GetData(), Bindings.GetSize()};
        }
    }
    ComputeWorkloadDescription Describe() const noexcept
    {
        return {{Buffers.GetData(), Buffers.GetSize()},
                {Kernels.GetData(), Kernels.GetSize()},
                BUFFER_BYTES * BUFFER_COUNT};
    }
};
} // namespace
TEST_CASE("Prepared compute admits buffer-only sequences beyond the scene ray pass bound", "[rhi][compute-workload]")
{
    Session session;
    Fixture fixture;
    ComputeWorkload workload;
    REQUIRE(workload.Initialize(session.Device, fixture.Describe()) == Status::Ready);
    CHECK(workload.GetBufferBytes() == BUFFER_BYTES * BUFFER_COUNT);
    OrderedGraph graph;
    StaticArray<GraphVersion, BUFFER_COUNT> versions{};
    REQUIRE(workload.BuildGraph(graph, {versions.GetData(), versions.GetSize()}) == Status::Ready);
    REQUIRE(AddGraphRoot(session.Device, graph, versions[1], GraphRoot::Readback) == Status::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == Status::Ready);
    CHECK(report.ComputePasses == KERNEL_COUNT);
    CHECK(report.ResourceCount == BUFFER_COUNT);
    CHECK(report.BufferBytes == BUFFER_BYTES * BUFFER_COUNT);
    CHECK(report.DependencyCount >= KERNEL_COUNT - 1);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == Status::Ready);
    REQUIRE(workload.BuildGraph(graph) == Status::Ready);
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == Status::Ready);
    workload.Reset();
    // The graph's retained pipeline/set/buffer leases survive owner retirement.
    CHECK(report.ComputePasses == KERNEL_COUNT);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == Status::Ready);
}
TEST_CASE("Prepared compute rejects immutable writes and mismatched ranges before publishing ownership",
          "[rhi][compute-workload]")
{
    Session session;
    Fixture fixture;
    ComputeWorkload workload;
    SECTION("immutable allocation")
    {
        fixture.Buffers[1].IsReadOnly = true;
    }
    SECTION("out-of-bounds range")
    {
        fixture.Bindings[1].Offset = BUFFER_BYTES;
    }
    SECTION("mismatched access")
    {
        fixture.Bindings[1].IsWritable = false;
    }
    SECTION("aliased allocation")
    {
        fixture.Bindings[1].Buffer = 0;
    }
    SECTION("partial upload")
    {
        fixture.Buffers[0].InitialBytes = {fixture.Weights.GetData(), 1};
    }
    REQUIRE(workload.Initialize(session.Device, fixture.Describe()) == Status::InvalidDescription);
    CHECK(workload.GetBufferBytes() == 0);
    CHECK(workload.Poll() == Status::NotReady);
}
TEST_CASE("Prepared compute preserves outputs and validates budgets, empty owners and moves", "[rhi][compute-workload]")
{
    Session session;
    Fixture fixture;
    ComputeWorkload workload;
    ComputeWorkloadDescription description = fixture.Describe();
    --description.MaximumBufferBytes;
    CHECK(workload.Initialize(session.Device, description) == Status::CapacityExceeded);
    REQUIRE(workload.Initialize(session.Device, fixture.Describe()) == Status::Ready);
    BufferHandle buffer;
    REQUIRE(workload.GetBuffer(0, buffer) == Status::Ready);
    const BufferHandle preserved = buffer;
    CHECK(workload.GetBuffer(BUFFER_COUNT, buffer) == Status::InvalidDescription);
    CHECK(buffer == preserved);
    ComputeWorkload moved{Move(workload)};
    CHECK(workload.Poll() == Status::NotReady);
    CHECK(moved.Poll() == Status::Ready);
    CHECK(moved.Initialize(session.Device, fixture.Describe()) == Status::InvalidDescription);
    OrderedGraph occupied;
    REQUIRE(CreateOrderedGraph(session.Device, occupied) == Status::Ready);
    CHECK(moved.BuildGraph(occupied) == Status::InvalidState);
    REQUIRE(DiscardOrderedGraph(session.Device, occupied) == Status::Ready);
}

TEST_CASE("Compute cache publishes only completed candidate work and preserves the active generation",
          "[rhi][compute-cache]")
{
    Session session;
    Fixture fixture;
    ComputeCache cache;
    const ComputeCacheKey first{1, 1};
    REQUIRE(cache.Stage(session.Device, first, fixture.Describe()) == Status::Pending);
    CHECK(cache.HasActive() == false);
    CHECK(cache.GetPhase() == ComputeCachePhase::Prepared);
    CHECK(cache.Poll() == Status::Pending);
    OrderedGraph graph;
    REQUIRE(cache.BuildCandidateGraph(graph) == Status::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == Status::Ready);
    const ComputeCacheTicket firstTicket = cache.GetCandidateTicket();
    CHECK(cache.SetCandidateCompletion(firstTicket, {}) == Status::InvalidHandle);
    SubmissionToken completion;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, completion) == Status::Ready);
    REQUIRE(cache.SetCandidateCompletion(firstTicket, completion) == Status::Pending);
    CHECK(cache.Poll() == Status::Pending);
    CHECK(cache.HasActive() == false);
    reference::Completed = reference::Submitted;
    REQUIRE(cache.Poll() == Status::Ready);
    REQUIRE(cache.HasActive() == true);
    CHECK(cache.GetActiveKey() == first);
    BufferHandle active;
    REQUIRE(cache.GetActiveBuffer(1, active) == Status::Ready);
    CHECK(cache.Stage(session.Device, first, {}) == Status::Ready);
    const ComputeCacheKey failed{2, 1};
    reference::Next = Status::Pending;
    REQUIRE(cache.Stage(session.Device, failed, fixture.Describe()) == Status::Pending);
    CHECK(cache.GetPhase() == ComputeCachePhase::Preparing);
    reference::Next = Status::Ready;
    for (usize i = 0; i < reference::PendingCount; ++i)
    {
        internal::RasterComplete(reference::PendingRequests[i], i == 0 ? Status::OutOfMemory : Status::Ready);
    }
    CHECK(cache.Poll() == Status::OutOfMemory);
    CHECK(cache.GetPhase() == ComputeCachePhase::Failed);
    CHECK(cache.GetActiveKey() == first);
    BufferHandle afterFailure;
    REQUIRE(cache.GetActiveBuffer(1, afterFailure) == Status::Ready);
    CHECK(afterFailure == active);
    cache.CancelCandidate();
    const ComputeCacheKey second{2, 1};
    fixture.Buffers[1].IsReadOnly = true;
    CHECK(cache.Stage(session.Device, second, fixture.Describe()) == Status::InvalidDescription);
    CHECK(cache.GetActiveKey() == first);
    fixture.Buffers[1].IsReadOnly = false;
    REQUIRE(cache.Stage(session.Device, second, fixture.Describe()) == Status::Pending);
    BufferHandle retained;
    REQUIRE(cache.GetActiveBuffer(1, retained) == Status::Ready);
    CHECK(retained == active);
    REQUIRE(cache.BuildCandidateGraph(graph) == Status::Ready);
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == Status::Ready);
    completion = {};
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, completion) == Status::Ready);
    CHECK(cache.SetCandidateCompletion(firstTicket, completion) == Status::InvalidDescription);
    REQUIRE(cache.SetCandidateCompletion(cache.GetCandidateTicket(), completion) == Status::Pending);
    CHECK(cache.GetActiveKey() == first);
    reference::Completed = reference::Submitted;
    REQUIRE(cache.Poll() == Status::Ready);
    CHECK(cache.GetActiveKey() == second);
    REQUIRE(cache.GetActiveBuffer(1, retained) == Status::Ready);
    CHECK(retained != active);
    cache.Reset();
    CHECK(cache.HasActive() == false);
}

TEST_CASE("Prepared compute borrows external buffers and tracks their producer revisions", "[rhi][compute-workload]")
{
    Session session;
    Fixture fixture;
    BufferHandle external;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Storage, BUFFER_BYTES}, fixture.Weights.AsSpan(), external) ==
            Status::Ready);
    fixture.Buffers[0].InitialBytes = {};
    fixture.Buffers[0].External = external;
    ComputeWorkload workload;
    REQUIRE(workload.Initialize(session.Device, fixture.Describe()) == Status::Ready);
    BufferHandle borrowed;
    REQUIRE(workload.GetBuffer(0, borrowed) == Status::Ready);
    CHECK(borrowed == external);
    OrderedGraph graph;
    REQUIRE(workload.BuildGraph(graph) == Status::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == Status::Ready);
    workload.Reset();
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == Status::Ready);
    CHECK(GetStatus(session.Device, external) == Status::Ready);
    REQUIRE(Destroy(session.Device, external) == Status::Ready);
}

TEST_CASE("Prepared compute polls pending uploads and pipeline dependencies without borrowing descriptions",
          "[rhi][compute-workload]")
{
    Session session;
    Fixture fixture;
    ComputeWorkload workload;
    reference::Next = Status::Pending;
    REQUIRE(workload.Initialize(session.Device, fixture.Describe()) == Status::Pending);
    CHECK(workload.Poll() == Status::Pending);
    reference::Next = Status::Ready;
    for (usize i = 0; i < reference::PendingCount; ++i)
    {
        internal::RasterComplete(reference::PendingRequests[i], Status::Ready);
    }
    // Destroy caller metadata after synchronous consumption; no descriptor view survives.
    for (ComputeWorkloadKernel& kernel : fixture.Kernels)
    {
        kernel.Bindings = {};
        kernel.NameOrNull = nullptr;
    }
    REQUIRE(workload.Poll() == Status::Ready);
    OrderedGraph graph;
    REQUIRE(workload.BuildGraph(graph) == Status::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == Status::Ready);
    CHECK(report.ComputePasses == KERNEL_COUNT);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == Status::Ready);
}

TEST_CASE("Compute cache rejects callbacks from cancelled attempts even when semantic keys are reused",
          "[rhi][compute-cache]")
{
    Session session;
    Fixture fixture;
    ComputeCache cache;
    const ComputeCacheKey key{1, 1};
    REQUIRE(cache.Stage(session.Device, key, fixture.Describe()) == Status::Pending);
    OrderedGraph graph;
    REQUIRE(cache.BuildCandidateGraph(graph) == Status::Ready);
    const ComputeCacheTicket cancelled = cache.GetCandidateTicket();
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == Status::Ready);
    cache.CancelCandidate();
    REQUIRE(cache.Stage(session.Device, key, fixture.Describe()) == Status::Pending);
    REQUIRE(cache.BuildCandidateGraph(graph) == Status::Ready);
    GraphReport report;
    REQUIRE(CompileOrderedGraph(session.Device, graph, report) == Status::Ready);
    SubmissionToken completion;
    REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, graph, completion) == Status::Ready);
    CHECK(cache.SetCandidateCompletion(cancelled, completion) == Status::InvalidDescription);
    REQUIRE(cache.SetCandidateCompletion(cache.GetCandidateTicket(), completion) == Status::Pending);
    reference::Completed = reference::Submitted;
    CHECK(cache.Poll() == Status::Ready);
}

TEST_CASE("Compute cache rejects external candidate writes while permitting shared immutable weights",
          "[rhi][compute-cache]")
{
    Session session;
    Fixture fixture;
    ComputeCache cache;
    BufferHandle external;
    REQUIRE(CreateBuffer(session.Device, {BufferRole::Storage, BUFFER_BYTES}, fixture.Weights.AsSpan(), external) ==
            Status::Ready);
    fixture.Buffers[1].External = external;
    CHECK(cache.Stage(session.Device, {1, 1}, fixture.Describe()) == Status::InvalidDescription);
    CHECK(cache.HasActive() == false);
    CHECK(cache.GetPhase() == ComputeCachePhase::Empty);
    fixture.Buffers[1].External = {};
    fixture.Buffers[0].External = external;
    fixture.Buffers[0].InitialBytes = {};
    REQUIRE(cache.Stage(session.Device, {1, 1}, fixture.Describe()) == Status::Pending);
    cache.CancelCandidate();
    CHECK(GetStatus(session.Device, external) == Status::Ready);
    REQUIRE(Destroy(session.Device, external) == Status::Ready);
}
