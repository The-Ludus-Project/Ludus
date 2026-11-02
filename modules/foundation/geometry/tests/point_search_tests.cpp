#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/point_search.hpp>
#include <ludus/foundation/math/random.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
using namespace ludus::foundation::geometry;
constexpr usize POINT_COUNT = 257;
constexpr usize QUERY_COUNT = 13;
constexpr float32 POINT_SPREAD = 20;
bool IsNearer(const PointNeighbor& left, const PointNeighbor& right) noexcept
{
    return left.DistanceSquared < right.DistanceSquared ||
           (left.DistanceSquared == right.DistanceSquared && left.PointIndex < right.PointIndex);
}
} // namespace
TEST_CASE("Point search matches exhaustive distance sorting for every admitted neighbor count", "[geometry][points]")
{
    StaticArray<math::Vector3, POINT_COUNT> points{};
    math::RandomStream random;
    for (math::Vector3& positionWorld : points)
    {
        positionWorld = {random.NextFloat01() * POINT_SPREAD,
                         random.NextFloat01() * POINT_SPREAD,
                         random.NextFloat01() * POINT_SPREAD};
    }
    PointSearchTree tree;
    REQUIRE(tree.Build({points.GetData(), points.GetSize()}) == GeometryStatus::Success);
    for (usize query = 0; query < QUERY_COUNT; ++query)
    {
        const math::Vector3 positionWorld{random.NextFloat01() * POINT_SPREAD,
                                          random.NextFloat01() * POINT_SPREAD,
                                          random.NextFloat01() * POINT_SPREAD};
        StaticArray<PointNeighbor, POINT_COUNT> exhaustive{};
        for (usize i = 0; i < points.GetSize(); ++i)
        {
            const math::Vector3 delta = positionWorld - points[i];
            exhaustive[i] = {static_cast<uint32>(i), math::Dot(delta, delta)};
        }
        Sort(exhaustive.GetData(), exhaustive.GetSize(), IsNearer);
        for (usize count = 1; count <= MAX_POINT_NEIGHBORS; ++count)
        {
            StaticArray<PointNeighbor, MAX_POINT_NEIGHBORS> result{};
            REQUIRE(tree.FindNearest(positionWorld, {result.GetData(), count}) == GeometryStatus::Success);
            for (usize i = 0; i < count; ++i)
            {
                REQUIRE(result[i].PointIndex == exhaustive[i].PointIndex);
                REQUIRE(result[i].DistanceSquared == exhaustive[i].DistanceSquared);
            }
        }
    }
}
TEST_CASE("Point search preserves source identities and deterministic ties", "[geometry][points]")
{
    constexpr usize COUNT = 7;
    StaticArray<math::Vector3, COUNT> duplicates{};
    PointSearchTree tree;
    REQUIRE(tree.Build({duplicates.GetData(), duplicates.GetSize()}) == GeometryStatus::Success);
    StaticArray<PointNeighbor, COUNT> neighbors{};
    REQUIRE(tree.FindNearest({}, {neighbors.GetData(), neighbors.GetSize()}) == GeometryStatus::Success);
    for (usize i = 0; i < COUNT; ++i)
    {
        CHECK(neighbors[i].PointIndex == i);
        CHECK(neighbors[i].DistanceSquared == 0);
    }
    const uint32 root = tree.GetRoot();
    const PointSearchNode saved = tree.GetNodes()[root];
    CHECK(tree.Build({nullptr, 1}) == GeometryStatus::InvalidArgument);
    CHECK(tree.GetNodes()[root].PointIndex == saved.PointIndex);
    neighbors[0].PointIndex = ABSENT_POINT_NODE;
    CHECK(tree.FindNearest({}, {neighbors.GetData(), COUNT + 1}) == GeometryStatus::InvalidArgument);
    CHECK(neighbors[0].PointIndex == ABSENT_POINT_NODE);
    duplicates[0].X = __builtin_inff();
    CHECK(tree.Build({duplicates.GetData(), duplicates.GetSize()}) == GeometryStatus::NonFiniteInput);
    CHECK(tree.GetNodes()[root].PointIndex == saved.PointIndex);
}
