#include "internal/lifecycle.h"
#include "internal/raster.h"
#include "reference_raster.h"

#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/ray.h>

#include <catch2/catch_test_macros.hpp>

// R16b: refit and compaction admission on the reference backend. These cases check the negotiated
// profile and build options, topology-change rejection, stale and pending sources, device loss and
// completion-gated retirement of refit and compaction sources without native ray hardware. Native
// parity of refits and compacted copies against rebuilt structures is checked in ray_tests.cpp.
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
constexpr uint32 TEST_SURFACE_WIDTH = 4096;
constexpr uint32 TEST_SURFACE_HEIGHT = 16384;
constexpr usize RAY_BYTE_BUDGET = usize{1} << 16;
constexpr usize RESIDENT_BYTES = 256;
constexpr usize SCRATCH_BYTES = 128;
constexpr usize COMPACTED_BYTES = 96;
constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize QUAD_INDEX_COUNT = 6;
constexpr usize POSITION_COMPONENTS = 3;

usize KindIndex(internal::RasterKind kind) noexcept
{
    return static_cast<usize>(kind);
}
struct UpdateSession final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    explicit UpdateSession(bool isRayAvailable = true)
    {
        Shutdown();
        reference::Reset();
        reference::IsRayAvailable = isRayAvailable;
        reference::RayResidentBytes = RESIDENT_BYTES;
        reference::RayScratchBytes = SCRATCH_BYTES;
        reference::RayCompactedBytes = COMPACTED_BYTES;
        backend::ImmediateError = StartupError::None;
        backend::NextFrame = FrameStatus::Ready;
        backend::ActiveKind = Backend::WebGPU;
        DeviceDescription description;
        description.Required.Compute = true;
        REQUIRE(CreateDevice({}, {}, description, Device, Surface) == DeviceStatus::Pending);
        internal::Complete(backend::PendingToken, StartupError::None, {TEST_SURFACE_WIDTH, TEST_SURFACE_HEIGHT});
        DeviceInfo info;
        REQUIRE(GetDeviceInfo(Device, info) == DeviceStatus::Ready);
    }
    UpdateSession(const UpdateSession&) = delete;
    UpdateSession& operator=(const UpdateSession&) = delete;
    ~UpdateSession() noexcept
    {
        Shutdown();
    }
};
// A two-triangle quad whose positions and indices the cases can change independently.
struct Quad final
{
    StaticArray<float32, QUAD_VERTEX_COUNT * POSITION_COMPONENTS> Positions{{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0}};
    StaticArray<uint32, QUAD_INDEX_COUNT> Indices{{0, 1, 2, 0, 2, 3}};
    [[nodiscard]] RayGeometryDescription MakeDescription(bool shouldAllowUpdate, bool shouldAllowCompaction) noexcept
    {
        RayGeometryDescription description;
        description.Positions = Positions.GetData();
        description.VertexCount = QUAD_VERTEX_COUNT;
        description.Indices = Indices.GetData();
        description.IndexCount = QUAD_INDEX_COUNT;
        description.ByteBudget = RAY_BYTE_BUDGET;
        description.ShouldAllowUpdate = shouldAllowUpdate;
        description.ShouldAllowCompaction = shouldAllowCompaction;
        return description;
    }
};
// Create a geometry whose reference build is already Ready.
RayGeometryHandle MakeReadyGeometry(DeviceHandle device, const RayGeometryDescription& description)
{
    reference::RayBuildResult = RasterStatus::Ready;
    RayGeometryHandle geometry;
    REQUIRE(CreateRayGeometry(device, description, geometry) == RasterStatus::Ready);
    reference::RayBuildResult = RasterStatus::Pending;
    return geometry;
}
RayResourceStats GetStats(DeviceHandle device)
{
    RayResourceStats stats;
    REQUIRE(GetRayResourceStats(device, stats) == RasterStatus::Ready);
    return stats;
}
} // namespace

TEST_CASE("R16b refit and compaction require the negotiated profile and build options", "[rhi][ray][update]")
{
    SECTION("A backend without the ray profile rejects every operation")
    {
        UpdateSession session(false);
        RayCapabilities capabilities;
        CHECK(GetRayCapabilities(session.Device, capabilities) == RasterStatus::Unsupported);
        CHECK(capabilities.Updates == false);
        CHECK(capabilities.CanCompact == false);
        Quad quad;
        RayGeometryHandle output;
        CHECK(UpdateRayGeometry(session.Device, {}, quad.MakeDescription(true, false), output) ==
              RasterStatus::Unsupported);
        CHECK(CompactRayGeometry(session.Device, {{}, RAY_BYTE_BUDGET}, output) == RasterStatus::Unsupported);
        RaySceneHandle scene;
        CHECK(UpdateRayScene(session.Device, {}, {nullptr, 0, RAY_BYTE_BUDGET, true}, scene) ==
              RasterStatus::Unsupported);
    }
    UpdateSession session;
    RayCapabilities capabilities;
    REQUIRE(GetRayCapabilities(session.Device, capabilities) == RasterStatus::Ready);
    CHECK(capabilities.Updates == true);
    CHECK(capabilities.CanCompact == true);
    Quad quad;
    SECTION("Sources built without the options are rejected before native work")
    {
        const RayGeometryHandle source = MakeReadyGeometry(session.Device, quad.MakeDescription(false, false));
        RayGeometryHandle output;
        CHECK(UpdateRayGeometry(session.Device, source, quad.MakeDescription(false, false), output) ==
              RasterStatus::InvalidState);
        CHECK(CompactRayGeometry(session.Device, {source, RAY_BYTE_BUDGET}, output) == RasterStatus::InvalidState);
        CHECK(GetStatus(session.Device, output) == RasterStatus::InvalidHandle);
        RayInstanceDescription instance;
        instance.Geometry = source;
        reference::RayBuildResult = RasterStatus::Ready;
        RaySceneHandle scene;
        REQUIRE(CreateRayScene(session.Device, {&instance, 1, RAY_BYTE_BUDGET}, scene) == RasterStatus::Ready);
        RaySceneHandle updated;
        CHECK(UpdateRayScene(session.Device, scene, {&instance, 1, RAY_BYTE_BUDGET}, updated) ==
              RasterStatus::InvalidState);
        CHECK(reference::RayGeometryUpdates == 0);
        CHECK(reference::RaySceneUpdates == 0);
        CHECK(reference::RayCompactions == 0);
    }
    SECTION("Pending, failed and stale sources are rejected")
    {
        RayGeometryHandle pending;
        REQUIRE(CreateRayGeometry(session.Device, quad.MakeDescription(true, true), pending) == RasterStatus::Pending);
        RayGeometryHandle output;
        CHECK(UpdateRayGeometry(session.Device, pending, quad.MakeDescription(true, true), output) ==
              RasterStatus::NotReady);
        CHECK(CompactRayGeometry(session.Device, {pending, RAY_BYTE_BUDGET}, output) == RasterStatus::NotReady);
        reference::RayBuildPoll = RasterStatus::Failed;
        REQUIRE(GetStatus(session.Device, pending) == RasterStatus::Failed);
        CHECK(UpdateRayGeometry(session.Device, pending, quad.MakeDescription(true, true), output) ==
              RasterStatus::Failed);
        const RayGeometryHandle stale = pending;
        REQUIRE(Destroy(session.Device, pending) == RasterStatus::Ready);
        CHECK(UpdateRayGeometry(session.Device, stale, quad.MakeDescription(true, true), output) ==
              RasterStatus::InvalidHandle);
        CHECK(CompactRayGeometry(session.Device, {stale, RAY_BYTE_BUDGET}, output) == RasterStatus::InvalidHandle);
        CHECK(GetStatus(session.Device, output) == RasterStatus::InvalidHandle);
    }
    SECTION("Compaction needs an explicit budget")
    {
        const RayGeometryHandle source = MakeReadyGeometry(session.Device, quad.MakeDescription(false, true));
        RayGeometryHandle output;
        CHECK(CompactRayGeometry(session.Device, {source, 0}, output) == RasterStatus::InvalidDescription);
        CHECK(CompactRayGeometry(session.Device, {source, COMPACTED_BYTES - 1}, output) ==
              RasterStatus::CapacityExceeded);
        CHECK(GetStatus(session.Device, output) == RasterStatus::InvalidHandle);
        CHECK(GetStats(session.Device).Geometries == 1);
    }
}

TEST_CASE("R16b geometry refits reject topology changes and keep the source until completion", "[rhi][ray][update]")
{
    UpdateSession session;
    Quad quad;
    RayGeometryHandle source = MakeReadyGeometry(session.Device, quad.MakeDescription(true, false));
    RayGeometryHandle output;
    SECTION("Every topology change asks for a rebuild")
    {
        RayGeometryDescription changed = quad.MakeDescription(true, false);
        changed.VertexCount = QUAD_VERTEX_COUNT - 1;
        changed.IndexCount = QUAD_INDEX_COUNT / 2;
        CHECK(UpdateRayGeometry(session.Device, source, changed, output) == RasterStatus::InvalidDescription);
        changed = quad.MakeDescription(true, false);
        changed.IndexCount = QUAD_INDEX_COUNT / 2;
        CHECK(UpdateRayGeometry(session.Device, source, changed, output) == RasterStatus::InvalidDescription);
        changed = quad.MakeDescription(true, false);
        changed.OpaqueTriangleCount = 1;
        CHECK(UpdateRayGeometry(session.Device, source, changed, output) == RasterStatus::InvalidDescription);
        CHECK(UpdateRayGeometry(session.Device, source, quad.MakeDescription(true, true), output) ==
              RasterStatus::InvalidDescription);
        CHECK(UpdateRayGeometry(session.Device, source, quad.MakeDescription(false, false), output) ==
              RasterStatus::InvalidDescription);
        Quad rewound;
        rewound.Indices = {{0, 2, 1, 0, 3, 2}};
        CHECK(UpdateRayGeometry(session.Device, source, rewound.MakeDescription(true, false), output) ==
              RasterStatus::InvalidDescription);
        Quad degenerate;
        degenerate.Positions[3] = -1;
        CHECK(UpdateRayGeometry(session.Device, source, degenerate.MakeDescription(true, false), output) ==
              RasterStatus::InvalidDescription);
        CHECK(GetStatus(session.Device, output) == RasterStatus::InvalidHandle);
        CHECK(reference::RayGeometryUpdates == 0);
        CHECK(GetStats(session.Device).Geometries == 1);
    }
    SECTION("A refit is a new incarnation that releases its source on completion")
    {
        Quad moved;
        moved.Positions[2] = 0.5F;
        moved.Positions[5] = -0.25F;
        REQUIRE(UpdateRayGeometry(session.Device, source, moved.MakeDescription(true, false), output) ==
                RasterStatus::Pending);
        CHECK(reference::RayGeometryUpdates == 1);
        CHECK(GetStatus(session.Device, source) == RasterStatus::Ready);
        RayAllocation allocation;
        REQUIRE(GetRayAllocation(session.Device, output, allocation) == RasterStatus::Ready);
        CHECK(allocation.ResidentBytes == RESIDENT_BYTES);
        CHECK(allocation.ScratchBytes == SCRATCH_BYTES);
        // Refitting the Pending result waits for it, like any other dependency.
        RayGeometryHandle chained;
        CHECK(UpdateRayGeometry(session.Device, output, quad.MakeDescription(true, false), chained) ==
              RasterStatus::NotReady);
        REQUIRE(Destroy(session.Device, source) == RasterStatus::Ready);
        RayResourceStats stats = GetStats(session.Device);
        CHECK(stats.Geometries == 2);
        CHECK(stats.DetachedBytes == RESIDENT_BYTES);
        CHECK(stats.BuildScratchBytes == SCRATCH_BYTES);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayGeometry)] == 0);
        reference::RayBuildPoll = RasterStatus::Ready;
        REQUIRE(GetStatus(session.Device, output) == RasterStatus::Ready);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayGeometry)] == 1);
        stats = GetStats(session.Device);
        CHECK(stats.Geometries == 1);
        CHECK(stats.DetachedBytes == 0);
        CHECK(stats.BuildScratchBytes == 0);
        // The refit keeps its build options, so refits chain and scenes use the newest version.
        REQUIRE(UpdateRayGeometry(session.Device, output, quad.MakeDescription(true, false), chained) ==
                RasterStatus::Pending);
        CHECK(reference::RayGeometryUpdates == 2);
        RayInstanceDescription instance;
        instance.Geometry = chained;
        RaySceneHandle scene;
        CHECK(CreateRayScene(session.Device, {&instance, 1, RAY_BYTE_BUDGET}, scene) == RasterStatus::Pending);
    }
    SECTION("A failed refit still releases its source")
    {
        REQUIRE(UpdateRayGeometry(session.Device, source, quad.MakeDescription(true, false), output) ==
                RasterStatus::Pending);
        REQUIRE(Destroy(session.Device, source) == RasterStatus::Ready);
        reference::RayBuildPoll = RasterStatus::Failed;
        CHECK(GetStatus(session.Device, output) == RasterStatus::Failed);
        CHECK(GetStats(session.Device).Geometries == 1);
        REQUIRE(Destroy(session.Device, output) == RasterStatus::Ready);
        CHECK(GetStats(session.Device).Geometries == 0);
    }
}

TEST_CASE("R16b scene refits keep instance topology and serve in-flight reads from the source", "[rhi][ray][update]")
{
    UpdateSession session;
    Quad quad;
    const RayGeometryHandle first = MakeReadyGeometry(session.Device, quad.MakeDescription(false, false));
    const RayGeometryHandle second = MakeReadyGeometry(session.Device, quad.MakeDescription(false, false));
    StaticArray<RayInstanceDescription, 2> instances{};
    instances[0].Geometry = first;
    instances[1].Geometry = first;
    instances[1].Transform[3] = 4;
    reference::RayBuildResult = RasterStatus::Ready;
    RaySceneHandle source;
    REQUIRE(CreateRayScene(session.Device, {instances.GetData(), instances.GetSize(), RAY_BYTE_BUDGET, true}, source) ==
            RasterStatus::Ready);
    reference::RayBuildResult = RasterStatus::Pending;
    RaySceneHandle output;
    SECTION("Instance count, geometry identity, opacity flags and options are topology")
    {
        CHECK(UpdateRayScene(session.Device, source, {instances.GetData(), 1, RAY_BYTE_BUDGET, true}, output) ==
              RasterStatus::InvalidDescription);
        StaticArray<RayInstanceDescription, 2> changed = instances;
        changed[1].Geometry = second;
        CHECK(UpdateRayScene(session.Device,
                             source,
                             {changed.GetData(), changed.GetSize(), RAY_BYTE_BUDGET, true},
                             output) == RasterStatus::InvalidDescription);
        changed = instances;
        changed[0].AlphaMasked = true;
        CHECK(UpdateRayScene(session.Device,
                             source,
                             {changed.GetData(), changed.GetSize(), RAY_BYTE_BUDGET, true},
                             output) == RasterStatus::InvalidDescription);
        CHECK(UpdateRayScene(session.Device,
                             source,
                             {instances.GetData(), instances.GetSize(), RAY_BYTE_BUDGET},
                             output) == RasterStatus::InvalidDescription);
        changed = instances;
        changed[1].Transform[0] = 0;
        changed[1].Transform[1] = 0;
        changed[1].Transform[2] = 0;
        CHECK(UpdateRayScene(session.Device,
                             source,
                             {changed.GetData(), changed.GetSize(), RAY_BYTE_BUDGET, true},
                             output) == RasterStatus::InvalidDescription);
        CHECK(GetStatus(session.Device, output) == RasterStatus::InvalidHandle);
        CHECK(reference::RaySceneUpdates == 0);
    }
    SECTION("Transforms, masks and opacity tables refit into a new incarnation")
    {
        StaticArray<RayInstanceDescription, 2> moved = instances;
        moved[1].Transform[3] = 6;
        moved[1].Mask = 2;
        moved[0].Opacity = 0.5F;
        REQUIRE(
            UpdateRayScene(session.Device, source, {moved.GetData(), moved.GetSize(), RAY_BYTE_BUDGET, true}, output) ==
            RasterStatus::Pending);
        CHECK(reference::RaySceneUpdates == 1);
        RayGeometryHandle detached = first;
        REQUIRE(Destroy(session.Device, detached) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, source) == RasterStatus::Ready);
        RayResourceStats stats = GetStats(session.Device);
        CHECK(stats.Scenes == 2);
        CHECK(stats.Geometries == 2);
        reference::RayBuildPoll = RasterStatus::Ready;
        REQUIRE(GetStatus(session.Device, output) == RasterStatus::Ready);
        stats = GetStats(session.Device);
        CHECK(stats.Scenes == 1);
        // The refit retains the same geometry incarnation as its source did.
        CHECK(stats.Geometries == 2);
        REQUIRE(Destroy(session.Device, output) == RasterStatus::Ready);
        stats = GetStats(session.Device);
        CHECK(stats.Scenes == 0);
        CHECK(stats.Geometries == 1);
    }
    SECTION("A graph frame and a refit both retain the replaced scene")
    {
        OrderedGraph graph;
        REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
        GraphVersion imported;
        REQUIRE(ImportGraphRayScene(session.Device, graph, source, imported) == RasterStatus::Ready);
        REQUIRE(UpdateRayScene(session.Device,
                               source,
                               {instances.GetData(), instances.GetSize(), RAY_BYTE_BUDGET, true},
                               output) == RasterStatus::Pending);
        REQUIRE(Destroy(session.Device, source) == RasterStatus::Ready);
        reference::RayBuildPoll = RasterStatus::Ready;
        REQUIRE(GetStatus(session.Device, output) == RasterStatus::Ready);
        CHECK(GetStats(session.Device).Scenes == 2);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayScene)] == 0);
        REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayScene)] == 1);
        CHECK(GetStats(session.Device).Scenes == 1);
    }
}

TEST_CASE("R16b compaction retires the source after the compacted copy completes", "[rhi][ray][update]")
{
    UpdateSession session;
    Quad quad;
    RayGeometryHandle source = MakeReadyGeometry(session.Device, quad.MakeDescription(true, true));
    RayGeometryHandle compacted;
    REQUIRE(CompactRayGeometry(session.Device, {source, COMPACTED_BYTES}, compacted) == RasterStatus::Pending);
    CHECK(reference::RayCompactions == 1);
    RayAllocation allocation;
    REQUIRE(GetRayAllocation(session.Device, compacted, allocation) == RasterStatus::Ready);
    CHECK(allocation.ResidentBytes == COMPACTED_BYTES);
    CHECK(allocation.ScratchBytes == 0);
    REQUIRE(Destroy(session.Device, source) == RasterStatus::Ready);
    RayResourceStats stats = GetStats(session.Device);
    CHECK(stats.Geometries == 2);
    CHECK(stats.ResidentBytes == RESIDENT_BYTES + COMPACTED_BYTES);
    CHECK(stats.DetachedBytes == RESIDENT_BYTES);
    SECTION("Completion retires the detached source")
    {
        reference::RayBuildPoll = RasterStatus::Ready;
        REQUIRE(GetStatus(session.Device, compacted) == RasterStatus::Ready);
        stats = GetStats(session.Device);
        CHECK(stats.Geometries == 1);
        CHECK(stats.ResidentBytes == COMPACTED_BYTES);
        CHECK(stats.DetachedBytes == 0);
        // A compacted copy is immutable: no refit and no second compaction.
        RayGeometryHandle output;
        CHECK(UpdateRayGeometry(session.Device, compacted, quad.MakeDescription(true, true), output) ==
              RasterStatus::InvalidState);
        CHECK(CompactRayGeometry(session.Device, {compacted, COMPACTED_BYTES}, output) == RasterStatus::InvalidState);
        // It keeps the source's triangle table for scene opacity ranges.
        RayInstanceDescription instance;
        instance.Geometry = compacted;
        instance.AlphaMasked = true;
        RaySceneHandle scene;
        CHECK(CreateRayScene(session.Device, {&instance, 1, RAY_BYTE_BUDGET}, scene) == RasterStatus::Pending);
    }
    SECTION("A failed copy also releases the source")
    {
        reference::RayBuildPoll = RasterStatus::Failed;
        CHECK(GetStatus(session.Device, compacted) == RasterStatus::Failed);
        CHECK(GetStats(session.Device).Geometries == 1);
    }
}

TEST_CASE("R16b device loss closes refit and compaction admission", "[rhi][ray][update]")
{
    Quad quad;
    RayGeometryHandle source;
    RayGeometryHandle compacted;
    {
        UpdateSession session;
        source = MakeReadyGeometry(session.Device, quad.MakeDescription(true, true));
        REQUIRE(CompactRayGeometry(session.Device, {source, COMPACTED_BYTES}, compacted) == RasterStatus::Pending);
        internal::Fail(backend::PendingToken, StartupError::DeviceLost);
        CHECK(GetStatus(session.Device, compacted) == RasterStatus::DeviceLost);
        RayGeometryHandle output;
        CHECK(UpdateRayGeometry(session.Device, source, quad.MakeDescription(true, true), output) ==
              RasterStatus::DeviceLost);
        CHECK(CompactRayGeometry(session.Device, {source, COMPACTED_BYTES}, output) == RasterStatus::DeviceLost);
        CHECK(GetStatus(session.Device, output) == RasterStatus::DeviceLost);
        CHECK(reference::RayGeometryUpdates == 0);
        CHECK(reference::RayCompactions == 1);
    }
    // Identities from the lost session never resolve in its replacement.
    UpdateSession replacement;
    RayGeometryHandle output;
    CHECK(UpdateRayGeometry(replacement.Device, source, quad.MakeDescription(true, true), output) ==
          RasterStatus::InvalidHandle);
    CHECK(CompactRayGeometry(replacement.Device, {compacted, COMPACTED_BYTES}, output) == RasterStatus::InvalidHandle);
}
