#include "baseline_renderer_point_neighbors.h"
#include "renderer_point_neighbors.h"
#include "renderer_point_neighbors_slots.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/point_neighbors.hpp>

#include <bit>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace renderer = ludus::graphics::renderer;
using Status = rhi::RasterStatus;
constexpr usize POINT_COUNT = 257;
constexpr usize QUERY_COUNT = 67;
constexpr usize NEIGHBOR_COUNT = 32;
constexpr usize BUDGET = usize{4} * 1024 * 1024;
constexpr float32 POINT_SPREAD = 20;
constexpr float32 DISTANCE_RELATIVE_TOLERANCE = 0.000002F;
constexpr uint64 TIMEOUT = 30 * time::NANOSECONDS_PER_SECOND;
bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
using Results = StaticArray<geometry::PointNeighbor, QUERY_COUNT * NEIGHBOR_COUNT>;
struct PointScene final
{
    StaticArray<math::Vector3, POINT_COUNT> PositionsWorld{};
    StaticArray<math::Vector3, QUERY_COUNT> QueriesWorld{};
    geometry::PointSearchTree Tree;
};
// Random points and queries, with a duplicated point that one query sits on.
void MakePointScene(PointScene& outScene)
{
    math::RandomStream random;
    for (math::Vector3& positionWorld : outScene.PositionsWorld)
    {
        positionWorld = {random.NextFloat01() * POINT_SPREAD,
                         random.NextFloat01() * POINT_SPREAD,
                         random.NextFloat01() * POINT_SPREAD};
    }
    for (math::Vector3& positionWorld : outScene.QueriesWorld)
    {
        positionWorld = {random.NextFloat01() * POINT_SPREAD,
                         random.NextFloat01() * POINT_SPREAD,
                         random.NextFloat01() * POINT_SPREAD};
    }
    outScene.PositionsWorld[1] = outScene.PositionsWorld[0];
    outScene.QueriesWorld[0] = outScene.PositionsWorld[0];
    REQUIRE(outScene.Tree.Build({outScene.PositionsWorld.GetData(), outScene.PositionsWorld.GetSize()}) ==
            geometry::GeometryStatus::Success);
}
// Dispatch `shader` over the scene's queries on `device` and read its query-major results back.
void FindNeighbors(rhi::DeviceHandle device,
                   rhi::SurfaceHandle surface,
                   const PointScene& scene,
                   const rhi::RasterShaderDescription& shader,
                   Results& outResults)
{
    rhi::ComputeWorkload workload;
    Status status = renderer::PreparePointNeighborWorkload(device,
                                                           scene.Tree,
                                                           {scene.QueriesWorld.GetData(), scene.QueriesWorld.GetSize()},
                                                           NEIGHBOR_COUNT,
                                                           shader,
                                                           BUDGET,
                                                           workload);
    REQUIRE(IsAccepted(status) == true);
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = workload.Poll();
    }
    REQUIRE(status == Status::Ready);
    rhi::OrderedGraph graph;
    REQUIRE(workload.BuildGraph(graph) == Status::Ready);
    rhi::GraphReport report;
    REQUIRE(rhi::CompileOrderedGraph(device, graph, report) == Status::Ready);
    CHECK(report.ComputePasses == 1);
    REQUIRE(rhi::SetFrameTarget(device, surface, {8, 8, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
    rhi::SubmissionToken completion;
    status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    while (status == Status::NotReady && time::NowTicks() - start < TIMEOUT)
    {
        status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    }
    REQUIRE(status == Status::Ready);
    rhi::BufferHandle resultBuffer;
    REQUIRE(workload.GetBuffer(renderer::POINT_NEIGHBOR_RESULT_BUFFER, resultBuffer) == Status::Ready);
    rhi::ReadbackTicket ticket;
    status = rhi::RequestBufferReadback(device, resultBuffer, 0, sizeof(outResults), ticket);
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = rhi::GetStatus(device, ticket);
    }
    REQUIRE(status == Status::Ready);
    REQUIRE(rhi::CopyReadback(device, ticket, reinterpret_cast<uint8*>(outResults.GetData()), sizeof(outResults)) ==
            Status::Ready);
    REQUIRE(rhi::Release(device, ticket) == Status::Ready);
}
// A compute-capable 8x8 device, or SKIP without a Metal adapter.
void StartComputeDevice(rhi::DeviceHandle& outDevice, rhi::SurfaceHandle& outSurface)
{
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, outDevice, outSurface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
    {
        SKIP("No Metal adapter");
    }
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
}
struct RhiSession final
{
    RhiSession() noexcept
    {
        rhi::Shutdown();
    }
    ~RhiSession() noexcept
    {
        rhi::Shutdown();
    }
    RhiSession(const RhiSession&) = delete;
    RhiSession& operator=(const RhiSession&) = delete;
};
} // namespace
TEST_CASE("Portable GPU point queries agree with the independent exhaustive-qualified CPU tree",
          "[renderer][gpu][point-neighbors]")
{
    const RhiSession session;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    StartComputeDevice(device, surface);
    PointScene scene;
    MakePointScene(scene);
    Results actual{};
    FindNeighbors(device, surface, scene, ludus::shaders::renderer_point_neighbors::Compute(), actual);
    for (usize query = 0; query < QUERY_COUNT; ++query)
    {
        StaticArray<geometry::PointNeighbor, NEIGHBOR_COUNT> expected{};
        REQUIRE(scene.Tree.FindNearest(scene.QueriesWorld[query], {expected.GetData(), expected.GetSize()}) ==
                geometry::GeometryStatus::Success);
        for (usize i = 0; i < NEIGHBOR_COUNT; ++i)
        {
            const geometry::PointNeighbor neighbor = actual[query * NEIGHBOR_COUNT + i];
            CHECK(neighbor.PointIndex == expected[i].PointIndex);
            CHECK(math::Abs(neighbor.DistanceSquared - expected[i].DistanceSquared) <=
                  DISTANCE_RELATIVE_TOLERANCE * math::Max(1.F, expected[i].DistanceSquared));
        }
    }
}

TEST_CASE("The LUSL point-neighbor recipe returns its Slang original's results",
          "[renderer][gpu][point-neighbors][lusl]")
{
    // LUSL L5d wave 2: the same tree and queries searched by renderer_point_neighbors and by its Slang original
    // (shaders/renderer_point_neighbors.slang) on one device; every result byte must match. Both bind the workload's
    // buffers through the LUSL recipe's slots, the Slang original's manual layout.
    static_assert(ludus::shaders::renderer_point_neighbors::slots::PARAMETERS == 0 &&
                  ludus::shaders::renderer_point_neighbors::slots::NODES == 1 &&
                  ludus::shaders::renderer_point_neighbors::slots::QUERIES == 2 &&
                  ludus::shaders::renderer_point_neighbors::slots::RESULTS == renderer::POINT_NEIGHBOR_RESULT_BUFFER);
    const RhiSession session;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    StartComputeDevice(device, surface);
    PointScene scene;
    MakePointScene(scene);
    Results slang{};
    Results lusl{};
    FindNeighbors(device, surface, scene, ludus::shaders::baseline_renderer_point_neighbors::Compute(), slang);
    FindNeighbors(device, surface, scene, ludus::shaders::renderer_point_neighbors::Compute(), lusl);
    usize differing = 0;
    usize found = 0;
    for (usize i = 0; i < lusl.GetSize(); ++i)
    {
        differing += lusl[i].PointIndex != slang[i].PointIndex ? 1 : 0;
        differing +=
            std::bit_cast<uint32>(lusl[i].DistanceSquared) != std::bit_cast<uint32>(slang[i].DistanceSquared) ? 1 : 0;
        found += lusl[i].PointIndex < POINT_COUNT ? 1 : 0;
    }
    CHECK(differing == 0);
    // Equality is not vacuous: every query found its neighbours, none the absent sentinel.
    CHECK(found == lusl.GetSize());
}
