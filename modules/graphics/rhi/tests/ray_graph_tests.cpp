#include "internal/lifecycle.h"
#include "internal/raster.h"
#include "reference_raster.h"

#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/ray.h>

#include <catch2/catch_test_macros.hpp>

// R16a/R17a: GDI admission of native acceleration structures on the reference backend. These
// cases check explicit build-to-read ordering, build/import rollback, completion-gated retirement
// of detached geometry and scenes, binding declarations and unsupported-target admission without
// native ray hardware. Native hit/miss through an ordered graph is checked in ray_tests.cpp.
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
constexpr usize RESULT_BYTES = 16;
constexpr uint32 RESULT_ELEMENT_STRIDE = 4;
constexpr usize RAY_BYTE_BUDGET = usize{1} << 16;
constexpr usize RESIDENT_BYTES = 256;
constexpr usize SCRATCH_BYTES = 128;
constexpr uint32 SCENE_BINDING = 0;
constexpr uint32 RESULT_BINDING = 1;
constexpr usize QUERY_BINDING_COUNT = 2;

usize KindIndex(internal::RasterKind kind) noexcept
{
    return static_cast<usize>(kind);
}
struct RaySession final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    explicit RaySession(bool isRayAvailable = true)
    {
        Shutdown();
        reference::Reset();
        reference::IsRayAvailable = isRayAvailable;
        reference::RayResidentBytes = RESIDENT_BYTES;
        reference::RayScratchBytes = SCRATCH_BYTES;
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
    RaySession(const RaySession&) = delete;
    RaySession& operator=(const RaySession&) = delete;
    ~RaySession() noexcept
    {
        Shutdown();
    }
};
// One nondegenerate local triangle.
RayGeometryDescription MakeTriangle() noexcept
{
    static constexpr StaticArray<float32, 9> POSITIONS{-1, -1, 0, 1, -1, 0, 0, 1, 0};
    static constexpr StaticArray<uint32, 3> INDICES{0, 1, 2};
    RayGeometryDescription description;
    description.Positions = POSITIONS.GetData();
    description.VertexCount = 3;
    description.Indices = INDICES.GetData();
    description.IndexCount = INDICES.GetSize();
    description.ByteBudget = RAY_BYTE_BUDGET;
    return description;
}
// A Ready BLAS and a Ready one-instance TLAS, built through the reference backend.
struct ReadyScene final
{
    RayGeometryHandle Geometry;
    RaySceneHandle Scene;
    explicit ReadyScene(DeviceHandle device)
    {
        reference::RayBuildResult = RasterStatus::Ready;
        REQUIRE(CreateRayGeometry(device, MakeTriangle(), Geometry) == RasterStatus::Ready);
        RayInstanceDescription instance;
        instance.Geometry = Geometry;
        const RaySceneDescription description{&instance, 1, RAY_BYTE_BUDGET};
        REQUIRE(CreateRayScene(device, description, Scene) == RasterStatus::Ready);
        reference::RayBuildResult = RasterStatus::Pending;
    }
};
// A compute kernel whose reflected layout reads one ray scene and writes a small result range.
struct QueryKernel final
{
    BufferHandle Results;
    RasterShaderHandle Shader;
    BindingLayoutHandle Layout;
    BindingSetHandle Set;
    ComputePipelineHandle Pipeline;
    QueryKernel(DeviceHandle device, RaySceneHandle scene)
    {
        const StaticArray<uint8, RESULT_BYTES> bytes{};
        REQUIRE(
            CreateBuffer(device, {BufferRole::Storage, RESULT_BYTES}, {bytes.GetData(), bytes.GetSize()}, Results) ==
            RasterStatus::Ready);
        const StaticArray<RasterBinding, QUERY_BINDING_COUNT> bindings{{
            {SCENE_BINDING, RasterBindingKind::RayScene, RasterVisibility::Compute, 0},
            {RESULT_BINDING, RasterBindingKind::StorageReadWrite, RasterVisibility::Compute, RESULT_ELEMENT_STRIDE},
        }};
        RasterShaderDescription description;
        description.Artifact.Stage = ShaderStage::Compute;
        description.Artifact.Wgsl = "reference-only ray query metadata";
        description.Artifact.WgslEntry = "queryMain";
        description.Bindings = {bindings.GetData(), bindings.GetSize()};
        REQUIRE(CreateRasterShader(device, description, Shader) == RasterStatus::Ready);
        REQUIRE(CreateBindingLayout(device, {bindings.GetData(), bindings.GetSize()}, Layout) == RasterStatus::Ready);
        const StaticArray<RasterBindingResource, QUERY_BINDING_COUNT> resources{{
            { .Binding = SCENE_BINDING, .Scene = scene },
            { .Binding = RESULT_BINDING, .Buffer = Results, .Size = RESULT_BYTES },
        }};
        REQUIRE(CreateBindingSet(device, Layout, {resources.GetData(), resources.GetSize()}, Set) ==
                RasterStatus::Ready);
        REQUIRE(CreateComputePipeline(device, {Shader, Layout}, Pipeline) == RasterStatus::Ready);
    }
    [[nodiscard]] ComputeDispatch MakeDispatch() const noexcept
    {
        return {Pipeline, Set};
    }
};
// Graph state shared by the declaration cases: imported results and scene, plus the next result version.
struct QueryGraph final
{
    OrderedGraph Graph;
    GraphVersion Results;
    GraphVersion Written;
    GraphVersion Scene;
    QueryGraph(DeviceHandle device, const QueryKernel& kernel, RaySceneHandle scene)
    {
        REQUIRE(CreateOrderedGraph(device, Graph) == RasterStatus::Ready);
        REQUIRE(ImportGraphBuffer(device, Graph, kernel.Results, Results) == RasterStatus::Ready);
        REQUIRE(NextGraphVersion(device, Graph, Results, Written) == RasterStatus::Ready);
        REQUIRE(ImportGraphRayScene(device, Graph, scene, Scene) == RasterStatus::Ready);
    }
};
RasterStatus AddQueryPass(DeviceHandle device,
                          OrderedGraph graph,
                          const ComputeDispatch& dispatch,
                          const GraphUse* uses,
                          usize useCount,
                          bool isSideEffect = true) noexcept
{
    GraphPassDescription pass;
    pass.Name = "ray query";
    pass.Uses = uses;
    pass.UseCount = useCount;
    pass.Dispatch = &dispatch;
    pass.SideEffect = isSideEffect;
    return AddGraphPass(device, graph, pass);
}
GraphUse MakeSceneRead(GraphVersion scene) noexcept
{
    return {scene, GraphAccessMode::AccelerationStructureRead, 0, 0, RasterVisibility::Compute};
}
GraphUse MakeResultWrite(GraphVersion written) noexcept
{
    return {written, GraphAccessMode::StorageReadWrite, 0, RESULT_BYTES, RasterVisibility::Compute};
}
GraphUse MakeResultRead(GraphVersion version) noexcept
{
    return {version, GraphAccessMode::StorageRead, 0, RESULT_BYTES, RasterVisibility::Compute};
}
// Compile an empty-graph report to observe the logical resource count without adding passes.
uint32 CountResources(DeviceHandle device, OrderedGraph graph)
{
    GraphReport report;
    REQUIRE(CompileOrderedGraph(device, graph, report) == RasterStatus::Ready);
    return report.ResourceCount;
}
} // namespace

TEST_CASE("R16a/R17a graph scene imports require the negotiated static-triangle query profile", "[rhi][ray][graph]")
{
    RaySession session(false);
    RayCapabilities capabilities;
    CHECK(GetRayCapabilities(session.Device, capabilities) == RasterStatus::Unsupported);
    CHECK(capabilities.StaticTriangles == false);
    CHECK(capabilities.InlineQueries == false);
    RayGeometryHandle geometry;
    CHECK(CreateRayGeometry(session.Device, MakeTriangle(), geometry) == RasterStatus::Unsupported);
    const RasterBinding sceneBinding{SCENE_BINDING, RasterBindingKind::RayScene, RasterVisibility::Compute, 0};
    BindingLayoutHandle layout;
    CHECK(CreateBindingLayout(session.Device, {&sceneBinding, 1}, layout) == RasterStatus::Unsupported);

    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphVersion scene;
    // Capability admission precedes handle resolution, so a target without the profile never
    // reports a ray resource as merely stale.
    CHECK(ImportGraphRayScene(session.Device, graph, {}, scene) == RasterStatus::Unsupported);
    CHECK(CountResources(session.Device, graph) == 0);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
}

TEST_CASE("R16a graph scene imports wait for native builds and roll back rejected setup", "[rhi][ray][graph]")
{
    RaySession session;
    RayGeometryHandle geometry;
    REQUIRE(CreateRayGeometry(session.Device, MakeTriangle(), geometry) == RasterStatus::Pending);
    RayInstanceDescription instance;
    instance.Geometry = geometry;
    const RaySceneDescription description{&instance, 1, RAY_BYTE_BUDGET};
    RaySceneHandle scene;
    // A scene cannot be built over a Pending BLAS; nothing is published.
    CHECK(CreateRayScene(session.Device, description, scene) == RasterStatus::NotReady);
    CHECK(GetStatus(session.Device, scene) == RasterStatus::InvalidHandle);
    reference::RayBuildPoll = RasterStatus::Ready;
    REQUIRE(GetStatus(session.Device, geometry) == RasterStatus::Ready);
    reference::RayBuildPoll = RasterStatus::Pending;
    REQUIRE(CreateRayScene(session.Device, description, scene) == RasterStatus::Pending);
    RayResourceStats stats;
    REQUIRE(GetRayResourceStats(session.Device, stats) == RasterStatus::Ready);
    CHECK(stats.BuildScratchBytes == SCRATCH_BYTES);

    OrderedGraph graph;
    REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    GraphVersion sceneVersion;
    // Reads never race a Pending build: the import is rejected and the graph keeps no resource.
    CHECK(ImportGraphRayScene(session.Device, graph, scene, sceneVersion) == RasterStatus::NotReady);
    CHECK(ImportGraphRayScene(session.Device, graph, {}, sceneVersion) == RasterStatus::InvalidHandle);

    reference::RayBuildPoll = RasterStatus::Ready;
    REQUIRE(GetStatus(session.Device, scene) == RasterStatus::Ready);
    REQUIRE(GetRayResourceStats(session.Device, stats) == RasterStatus::Ready);
    // A completed build has released its scratch; only resident bytes remain.
    CHECK(stats.BuildScratchBytes == 0);
    CHECK(stats.ResidentBytes == 2 * RESIDENT_BYTES);
    REQUIRE(ImportGraphRayScene(session.Device, graph, scene, sceneVersion) == RasterStatus::Ready);
    GraphVersion duplicate;
    CHECK(ImportGraphRayScene(session.Device, graph, scene, duplicate) == RasterStatus::InvalidDescription);
    // Scenes are immutable inputs: no later version and no observable root.
    GraphVersion next;
    CHECK(NextGraphVersion(session.Device, graph, sceneVersion, next) == RasterStatus::InvalidDescription);
    CHECK(AddGraphRoot(session.Device, graph, sceneVersion, GraphRoot::Export) == RasterStatus::InvalidDescription);

    // A failed native build reports its failure rather than an empty acceleration structure.
    reference::RayBuildPoll = RasterStatus::Failed;
    RaySceneHandle failed;
    REQUIRE(CreateRayScene(session.Device, description, failed) == RasterStatus::Pending);
    REQUIRE(GetStatus(session.Device, failed) == RasterStatus::Failed);
    GraphVersion failedVersion;
    CHECK(ImportGraphRayScene(session.Device, graph, failed, failedVersion) == RasterStatus::Failed);
    CHECK(CountResources(session.Device, graph) == 1);
    // A compiled graph is frozen.
    CHECK(ImportGraphRayScene(session.Device, graph, scene, failedVersion) == RasterStatus::InvalidState);
    REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
    REQUIRE(Destroy(session.Device, failed) == RasterStatus::Ready);
}

TEST_CASE("R17a compute scene bindings require declared acceleration-structure reads", "[rhi][ray][graph]")
{
    RaySession session;
    const ReadyScene built(session.Device);
    const QueryKernel kernel(session.Device, built.Scene);
    const ComputeDispatch dispatch = kernel.MakeDispatch();

    SECTION("An undeclared bound scene rejects compilation at its pass")
    {
        QueryGraph setup(session.Device, kernel, built.Scene);
        const GraphUse write = MakeResultWrite(setup.Written);
        REQUIRE(AddQueryPass(session.Device, setup.Graph, dispatch, &write, 1) == RasterStatus::Ready);
        GraphReport report;
        CHECK(CompileOrderedGraph(session.Device, setup.Graph, report) == RasterStatus::InvalidDescription);
        CHECK(report.ErrorPass == 0);
        REQUIRE(DiscardOrderedGraph(session.Device, setup.Graph) == RasterStatus::Ready);
    }
    SECTION("Builds are reported, never authored")
    {
        QueryGraph setup(session.Device, kernel, built.Scene);
        const StaticArray<GraphUse, 2> uses{{
            MakeResultWrite(setup.Written),
            {setup.Scene, GraphAccessMode::AccelerationStructureBuild, 0, 0, RasterVisibility::Compute},
        }};
        CHECK(AddQueryPass(session.Device, setup.Graph, dispatch, uses.GetData(), uses.GetSize()) ==
              RasterStatus::InvalidDescription);
        CHECK(CountResources(session.Device, setup.Graph) == 2);
        REQUIRE(DiscardOrderedGraph(session.Device, setup.Graph) == RasterStatus::Ready);
    }
    SECTION("Malformed scene reads reject compilation at the scene")
    {
        const StaticArray<uint8, RESULT_BYTES> bytes{};
        BufferHandle spare;
        REQUIRE(CreateBuffer(session.Device,
                             {BufferRole::Storage, RESULT_BYTES},
                             {bytes.GetData(), bytes.GetSize()},
                             spare) == RasterStatus::Ready);
        // Each malformed use names either the imported scene (resource 1) or a spare buffer
        // (resource 2) alongside the correctly declared result write; the failure must name it.
        const auto compileWith = [&](GraphUse malformed, bool isBufferResource, bool isRasterPass) {
            QueryGraph setup(session.Device, kernel, built.Scene);
            GraphVersion spareVersion;
            REQUIRE(ImportGraphBuffer(session.Device, setup.Graph, spare, spareVersion) == RasterStatus::Ready);
            malformed.Resource = isBufferResource == true ? spareVersion : setup.Scene;
            // The malformed use leads. A compute pass keeps its result write, and a buffer-targeted
            // case also keeps the correct scene read; the raster pass declares only the malformed use.
            const StaticArray<GraphUse, 3> uses{{
                malformed,
                MakeResultWrite(setup.Written),
                MakeSceneRead(setup.Scene),
            }};
            GraphPassDescription pass;
            pass.Name = "malformed scene read";
            pass.SideEffect = true;
            pass.Uses = uses.GetData();
            pass.UseCount = isRasterPass == true ? 1 : isBufferResource == true ? uses.GetSize() : 2;
            pass.Dispatch = isRasterPass == true ? nullptr : &dispatch;
            REQUIRE(AddGraphPass(session.Device, setup.Graph, pass) == RasterStatus::Ready);
            GraphReport report;
            const RasterStatus compiled = CompileOrderedGraph(session.Device, setup.Graph, report);
            REQUIRE(DiscardOrderedGraph(session.Device, setup.Graph) == RasterStatus::Ready);
            CHECK(compiled == RasterStatus::InvalidDescription);
            return report.ErrorResource;
        };
        constexpr uint32 SCENE_RESOURCE = 1;
        constexpr uint32 SPARE_RESOURCE = 2;
        GraphUse sized = MakeSceneRead({});
        sized.Size = RESULT_ELEMENT_STRIDE;
        CHECK(compileWith(sized, false, false) == SCENE_RESOURCE);
        GraphUse offset = MakeSceneRead({});
        offset.Offset = RESULT_ELEMENT_STRIDE;
        CHECK(compileWith(offset, false, false) == SCENE_RESOURCE);
        GraphUse fragment = MakeSceneRead({});
        fragment.Visibility = RasterVisibility::Fragment;
        CHECK(compileWith(fragment, false, false) == SCENE_RESOURCE);
        GraphUse storage = MakeSceneRead({});
        storage.Access = GraphAccessMode::StorageRead;
        storage.Size = RESULT_BYTES;
        CHECK(compileWith(storage, false, false) == SCENE_RESOURCE);
        GraphUse buffer = MakeSceneRead({});
        CHECK(compileWith(buffer, true, false) == SPARE_RESOURCE);
        CHECK(compileWith(MakeSceneRead({}), false, true) == SCENE_RESOURCE);
    }
    SECTION("Declared reads report the external build once and order nothing between readers")
    {
        QueryGraph setup(session.Device, kernel, built.Scene);
        const StaticArray<GraphUse, 2> first{{MakeResultWrite(setup.Written), MakeSceneRead(setup.Scene)}};
        REQUIRE(AddQueryPass(session.Device, setup.Graph, dispatch, first.GetData(), first.GetSize(), false) ==
                RasterStatus::Ready);
        // A second reader of the same immutable scene, reading the first pass's results.
        const QueryKernel reader(session.Device, built.Scene);
        const ComputeDispatch readerDispatch = reader.MakeDispatch();
        GraphVersion readerResults;
        GraphVersion readerWritten;
        REQUIRE(ImportGraphBuffer(session.Device, setup.Graph, reader.Results, readerResults) == RasterStatus::Ready);
        REQUIRE(NextGraphVersion(session.Device, setup.Graph, readerResults, readerWritten) == RasterStatus::Ready);
        const StaticArray<GraphUse, 3> secondUses{{
            MakeResultRead(setup.Written),
            MakeSceneRead(setup.Scene),
            MakeResultWrite(readerWritten),
        }};
        REQUIRE(AddQueryPass(session.Device,
                             setup.Graph,
                             readerDispatch,
                             secondUses.GetData(),
                             secondUses.GetSize(),
                             true) == RasterStatus::Ready);
        GraphReport report;
        REQUIRE(CompileOrderedGraph(session.Device, setup.Graph, report) == RasterStatus::Ready);
        CHECK(report.LivePasses == 2);
        CHECK(report.ComputePasses == 2);
        CHECK(report.BufferBytes == 2 * RESULT_BYTES);
        CHECK(report.TextureBytes == 0);
        const uint32 sceneResource = 1;
        uint32 builds = 0;
        uint32 sceneEdges = 0;
        for (uint32 i = 0; i < report.ReportedDependencies; ++i)
        {
            const GraphDependency& edge = report.Dependencies[i];
            if (edge.Resource != sceneResource)
            {
                continue;
            }
            ++sceneEdges;
            if (edge.Before == GRAPH_EXTERNAL_INDEX && edge.After == 0 && edge.Hazard == GraphHazard::ReadAfterWrite &&
                edge.Source == GraphAccessMode::AccelerationStructureBuild &&
                edge.Destination == GraphAccessMode::AccelerationStructureRead)
            {
                ++builds;
            }
        }
        CHECK(builds == 1);
        CHECK(sceneEdges == 1);
        CHECK(report.FirstUse[sceneResource] == 0);
        CHECK(report.LastUse[sceneResource] == 1);
        REQUIRE(DiscardOrderedGraph(session.Device, setup.Graph) == RasterStatus::Ready);
    }
}

TEST_CASE("R16a graph imports retain detached scenes and geometry until GPU completion", "[rhi][ray][graph]")
{
    RaySession session;
    ReadyScene built(session.Device);
    SECTION("Discarding setup releases the imported incarnations")
    {
        OrderedGraph graph;
        REQUIRE(CreateOrderedGraph(session.Device, graph) == RasterStatus::Ready);
        GraphVersion scene;
        REQUIRE(ImportGraphRayScene(session.Device, graph, built.Scene, scene) == RasterStatus::Ready);
        const RaySceneHandle stale = built.Scene;
        REQUIRE(Destroy(session.Device, built.Scene) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, built.Geometry) == RasterStatus::Ready);
        CHECK(GetStatus(session.Device, stale) == RasterStatus::InvalidHandle);
        RayResourceStats stats;
        REQUIRE(GetRayResourceStats(session.Device, stats) == RasterStatus::Ready);
        CHECK(stats.Scenes == 1);
        CHECK(stats.Geometries == 1);
        CHECK(stats.DetachedBytes == 2 * RESIDENT_BYTES);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayScene)] == 0);
        REQUIRE(DiscardOrderedGraph(session.Device, graph) == RasterStatus::Ready);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayScene)] == 1);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayGeometry)] == 1);
        REQUIRE(GetRayResourceStats(session.Device, stats) == RasterStatus::Ready);
        CHECK(stats.Scenes == 0);
        CHECK(stats.ResidentBytes == 0);
    }
    SECTION("Execution keeps the scene until its frame completes")
    {
        QueryKernel kernel(session.Device, built.Scene);
        const ComputeDispatch dispatch = kernel.MakeDispatch();
        QueryGraph setup(session.Device, kernel, built.Scene);
        const StaticArray<GraphUse, 2> uses{{MakeResultWrite(setup.Written), MakeSceneRead(setup.Scene)}};
        REQUIRE(AddQueryPass(session.Device, setup.Graph, dispatch, uses.GetData(), uses.GetSize()) ==
                RasterStatus::Ready);
        GraphReport report;
        REQUIRE(CompileOrderedGraph(session.Device, setup.Graph, report) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, kernel.Pipeline) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, kernel.Set) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, built.Scene) == RasterStatus::Ready);
        REQUIRE(Destroy(session.Device, built.Geometry) == RasterStatus::Ready);
        SubmissionToken token;
        REQUIRE(ExecuteOrderedGraph(session.Device, session.Surface, setup.Graph, token) == RasterStatus::Ready);
        CHECK(reference::Dispatches == 1);
        REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayScene)] == 0);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayGeometry)] == 0);
        reference::Completed = reference::Submitted;
        REQUIRE(PollLifetime(session.Device) == RasterStatus::Ready);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayScene)] == 1);
        CHECK(reference::Destroys[KindIndex(internal::RasterKind::RayGeometry)] == 1);
    }
}
