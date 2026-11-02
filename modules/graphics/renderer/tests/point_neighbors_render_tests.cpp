#include "renderer_point_neighbors.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/point_neighbors.hpp>

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
} // namespace
TEST_CASE("Portable GPU point queries agree with the independent exhaustive-qualified CPU tree",
          "[renderer][gpu][point-neighbors]")
{
    rhi::Shutdown();
    struct Guard final
    {
        ~Guard() noexcept
        {
            rhi::Shutdown();
        }
    } guard;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::DeviceDescription description;
    description.Required.Compute = true;
    const rhi::DeviceStatus started = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, device, surface);
#if defined(LUDUS_TEST_METAL)
    if (started != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
        SKIP("No Metal adapter");
#endif
    REQUIRE(started == rhi::DeviceStatus::Ready);
    StaticArray<math::Vector3, POINT_COUNT> positionsWorld{};
    StaticArray<math::Vector3, QUERY_COUNT> queriesWorld{};
    math::RandomStream random;
    for (math::Vector3& positionWorld : positionsWorld)
        positionWorld = {random.NextFloat01() * POINT_SPREAD,
                         random.NextFloat01() * POINT_SPREAD,
                         random.NextFloat01() * POINT_SPREAD};
    for (math::Vector3& positionWorld : queriesWorld)
        positionWorld = {random.NextFloat01() * POINT_SPREAD,
                         random.NextFloat01() * POINT_SPREAD,
                         random.NextFloat01() * POINT_SPREAD};
    positionsWorld[1] = positionsWorld[0];
    queriesWorld[0] = positionsWorld[0];
    geometry::PointSearchTree tree;
    REQUIRE(tree.Build({positionsWorld.GetData(), positionsWorld.GetSize()}) == geometry::GeometryStatus::Success);
    rhi::ComputeWorkload workload;
    Status status = renderer::PreparePointNeighborWorkload(device,
                                                           tree,
                                                           {queriesWorld.GetData(), queriesWorld.GetSize()},
                                                           NEIGHBOR_COUNT,
                                                           ludus::shaders::renderer_point_neighbors::Compute(),
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
        status = rhi::ExecuteOrderedGraph(device, surface, graph, completion);
    REQUIRE(status == Status::Ready);
    rhi::BufferHandle resultBuffer;
    REQUIRE(workload.GetBuffer(renderer::POINT_NEIGHBOR_RESULT_BUFFER, resultBuffer) == Status::Ready);
    StaticArray<geometry::PointNeighbor, QUERY_COUNT * NEIGHBOR_COUNT> actual{};
    rhi::ReadbackTicket ticket;
    status = rhi::RequestBufferReadback(device, resultBuffer, 0, sizeof(actual), ticket);
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        REQUIRE(rhi::PollLifetime(device) == Status::Ready);
        status = rhi::GetStatus(device, ticket);
    }
    REQUIRE(status == Status::Ready);
    REQUIRE(rhi::CopyReadback(device, ticket, reinterpret_cast<uint8*>(actual.GetData()), sizeof(actual)) ==
            Status::Ready);
    REQUIRE(rhi::Release(device, ticket) == Status::Ready);
    for (usize query = 0; query < QUERY_COUNT; ++query)
    {
        StaticArray<geometry::PointNeighbor, NEIGHBOR_COUNT> expected{};
        REQUIRE(tree.FindNearest(queriesWorld[query], {expected.GetData(), expected.GetSize()}) ==
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
