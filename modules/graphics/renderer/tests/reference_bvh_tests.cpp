#include "../src/internal/reference_data.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace internal = ludus::graphics::renderer::internal;
namespace rhi = ludus::graphics::rhi;
namespace
{
constexpr uint32 LCG_MULTIPLIER = 1664525;
constexpr uint32 LCG_INCREMENT = 1013904223;
constexpr float32 LCG_SCALE = 1.F / 4294967296.F;
// Deterministic uniform values in [0,1) so failures reproduce exactly.
struct Sequence final
{
    uint32 State = 1;
    float32 Next() noexcept
    {
        State = State * LCG_MULTIPLIER + LCG_INCREMENT;
        return float32(State) * LCG_SCALE;
    }
};
void AddTriangle(internal::ReferenceSceneData& inoutScene, math::Vector3 a, math::Vector3 b, math::Vector3 c)
{
    internal::ReferenceTriangle triangle;
    triangle.Positions[0] = a;
    triangle.Positions[1] = b;
    triangle.Positions[2] = c;
    const math::Vector3 normal = math::Cross(b - a, c - a);
    triangle.Area = math::Length(normal) * .5F;
    triangle.GeometricNormal = triangle.Area > 0 ? normal / (2 * triangle.Area) : math::Vector3{};
    triangle.Primitive = static_cast<uint32>(inoutScene.Triangles.GetSize());
    REQUIRE(inoutScene.Triangles.TryAdd(triangle) == true);
}
// Small scattered triangles plus a few large walls that overlap many of them: the
// layout that makes a median split put large boxes deep in the tree.
void BuildRoom(internal::ReferenceSceneData& outScene, const internal::ReferenceSurfaceData& surface)
{
    constexpr usize SMALL_TRIANGLES = 3000;
    constexpr float32 ROOM_EXTENT = 10;
    constexpr float32 SMALL_EXTENT = .05F;
    Sequence sequence;
    for (usize i = 0; i < SMALL_TRIANGLES; ++i)
    {
        const math::Vector3 center{sequence.Next() * ROOM_EXTENT,
                                   sequence.Next() * ROOM_EXTENT,
                                   sequence.Next() * ROOM_EXTENT};
        AddTriangle(outScene,
                    center,
                    center + math::Vector3{SMALL_EXTENT, sequence.Next() * SMALL_EXTENT, 0},
                    center + math::Vector3{0, SMALL_EXTENT, sequence.Next() * SMALL_EXTENT});
    }
    AddTriangle(outScene, {0, 0, 0}, {ROOM_EXTENT, 0, 0}, {0, ROOM_EXTENT, 0});
    AddTriangle(outScene, {0, 0, 0}, {0, 0, ROOM_EXTENT}, {ROOM_EXTENT, 0, 0});
    AddTriangle(outScene, {0, 0, 0}, {0, ROOM_EXTENT, 0}, {0, 0, ROOM_EXTENT});
    AddTriangle(outScene, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}); // Collapsed: left out of the hierarchy.
    REQUIRE(outScene.Instances.TryResize(1) == true);
    outScene.Instances[0].Surface = &surface;
    outScene.Instances[0].TriangleCount = outScene.Triangles.GetSize();
    outScene.InstanceCount = 1;
}
bool Contains(const math::Aabb3& outer, const math::Aabb3& inner) noexcept
{
    return outer.Min.X <= inner.Min.X && outer.Min.Y <= inner.Min.Y && outer.Min.Z <= inner.Min.Z &&
           outer.Max.X >= inner.Max.X && outer.Max.Y >= inner.Max.Y && outer.Max.Z >= inner.Max.Z;
}
// Walks the hierarchy, checking bounds nesting, leaf sizes and depth; returns the deepest leaf.
uint32 CheckNode(const internal::ReferenceSceneData& scene, uint32 node, Array<uint32>& inoutSeen, uint32 depth)
{
    const internal::ReferenceBvhNode& value = scene.Nodes[node];
    if (value.Count != 0)
    {
        CHECK(value.Count <= internal::REFERENCE_BVH_MAX_LEAF_TRIANGLES);
        for (uint32 i = value.First; i < value.First + value.Count; ++i)
        {
            const internal::ReferenceTriangle& triangle = scene.Triangles[scene.Order[i]];
            for (const math::Vector3& position : triangle.Positions)
            {
                CHECK(Contains(value.Bounds, {position, position}) == true);
            }
            ++inoutSeen[scene.Order[i]];
        }
        return depth;
    }
    CHECK(value.SplitAxis < 3);
    CHECK(Contains(value.Bounds, scene.Nodes[value.Left].Bounds) == true);
    CHECK(Contains(value.Bounds, scene.Nodes[value.Right].Bounds) == true);
    return math::Max(CheckNode(scene, value.Left, inoutSeen, depth + 1),
                     CheckNode(scene, value.Right, inoutSeen, depth + 1));
}
} // namespace

TEST_CASE("Reference SAH BVH covers every surface once within leaf and depth limits", "[renderer][reference]")
{
    internal::ReferenceSurfaceData surface;
    internal::ReferenceSceneData scene;
    BuildRoom(scene, surface);
    REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
    Array<uint32> seen;
    REQUIRE(seen.TryResize(scene.Triangles.GetSize()) == true);
    const uint32 depth = CheckNode(scene, 0, seen, 0);
    CHECK(depth <= internal::REFERENCE_BVH_MAX_DEPTH);
    for (usize i = 0; i < scene.Triangles.GetSize(); ++i)
    {
        CHECK(seen[i] == (scene.Triangles[i].Area > 0 ? 1U : 0U));
    }

    internal::ReferenceSceneData again;
    BuildRoom(again, surface);
    REQUIRE(internal::BuildReferenceBvh(again) == rhi::RasterStatus::Ready);
    REQUIRE(again.Nodes.GetSize() == scene.Nodes.GetSize());
    for (usize i = 0; i < scene.Order.GetSize(); ++i)
    {
        CHECK(again.Order[i] == scene.Order[i]);
    }
    for (usize i = 0; i < scene.Nodes.GetSize(); ++i)
    {
        CHECK(again.Nodes[i].Left == scene.Nodes[i].Left);
        CHECK(again.Nodes[i].Count == scene.Nodes[i].Count);
        CHECK(again.Nodes[i].SplitAxis == scene.Nodes[i].SplitAxis);
    }
}

TEST_CASE("Reference SAH BVH bounds depth on geometrically growing layouts", "[renderer][reference]")
{
    // Each offset grows 10% (the last near 9e4), so cheapest-plane splitting peels a
    // few triangles per level; the median fallback must keep every leaf within the limit.
    constexpr usize TRIANGLE_COUNT = 120;
    constexpr float32 GROWTH = 1.1F;
    internal::ReferenceSurfaceData surface;
    internal::ReferenceSceneData scene;
    float32 offset = 1;
    for (usize i = 0; i < TRIANGLE_COUNT; ++i)
    {
        AddTriangle(scene, {offset, 0, 0}, {offset + .1F, 0, 0}, {offset, .1F, 0});
        offset *= GROWTH;
    }
    REQUIRE(scene.Instances.TryResize(1) == true);
    scene.Instances[0].Surface = &surface;
    scene.InstanceCount = 1;
    REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
    Array<uint32> seen;
    REQUIRE(seen.TryResize(scene.Triangles.GetSize()) == true);
    CHECK(CheckNode(scene, 0, seen, 0) <= internal::REFERENCE_BVH_MAX_DEPTH);
    for (const uint32 count : seen)
    {
        CHECK(count == 1);
    }
}

TEST_CASE("Reference oracle near-first traversal returns the brute-force closest hit", "[renderer][reference]")
{
    constexpr usize RAY_COUNT = 4000;
    constexpr float32 ROOM_EXTENT = 10;
    internal::ReferenceSurfaceData surface;
    internal::ReferenceSceneData scene;
    BuildRoom(scene, surface);
    REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
    // One leaf over every surface triangle tests them all in index order.
    internal::ReferenceSceneData flat;
    BuildRoom(flat, surface);
    REQUIRE(flat.Order.TryResize(scene.Order.GetSize()) == true);
    for (usize i = 0; i < flat.Order.GetSize(); ++i)
    {
        flat.Order[i] = static_cast<uint32>(i);
    }
    internal::ReferenceBvhNode leaf;
    leaf.Bounds = scene.Nodes[0].Bounds;
    leaf.Count = static_cast<uint32>(flat.Order.GetSize());
    REQUIRE(flat.Nodes.TryAdd(leaf) == true);

    Sequence sequence;
    usize hits = 0;
    for (usize i = 0; i < RAY_COUNT; ++i)
    {
        const math::Vector3 origin{sequence.Next() * ROOM_EXTENT,
                                   sequence.Next() * ROOM_EXTENT,
                                   sequence.Next() * ROOM_EXTENT};
        // Directions cover every sign octant, so both child orders are exercised.
        const math::Vector3 direction =
            internal::ReferenceNormalize({sequence.Next() * 2 - 1, sequence.Next() * 2 - 1, sequence.Next() * 2 - 1});
        const rhi::RayInput ray = internal::ReferenceRay(origin, direction, ROOM_EXTENT * 2);
        rhi::RayResult expected;
        rhi::RayResult actual;
        REQUIRE(internal::TraceReferenceOracle(flat, ray, expected) == rhi::RasterStatus::Ready);
        REQUIRE(internal::TraceReferenceOracle(scene, ray, actual) == rhi::RasterStatus::Ready);
        CHECK(actual.Hit == expected.Hit);
        CHECK(actual.Primitive == expected.Primitive);
        CHECK(actual.Distance == expected.Distance);
        hits += expected.Hit != 0 ? 1U : 0U;
    }
    // The room has three walls, so roughly a third of the rays hit one or a small triangle.
    CHECK(hits > RAY_COUNT / 4);
}

TEST_CASE("Reference oracle barycentrics on the edge opposite the first corner sum to at most one",
          "[renderer][reference]")
{
    // The ComputeBvh regression's quad (render_graph_ray_tests.cpp): rays straight down onto the diagonal from
    // (1, 0, 0) to (0, 1, 0), which both triangles list as their second and third corners. The float32 traversal
    // clamps its barycentrics to this bound; the oracle's float64 quotients must meet it without a clamp.
    constexpr usize RAY_COLUMNS = 96;
    constexpr usize RAY_ROWS = 64;
    constexpr usize RAY_COUNT = RAY_COLUMNS * RAY_ROWS;
    constexpr float32 ORIGIN_HEIGHT = 1;
    constexpr float32 RAY_LENGTH = 2;
    internal::ReferenceSurfaceData surface;
    internal::ReferenceSceneData scene;
    const math::Vector3 first{-.15F, -.4F, 0};
    const math::Vector3 second{1, 0, 0};
    const math::Vector3 opposite{1.3F, .9F, 0};
    const math::Vector3 third{0, 1, 0};
    AddTriangle(scene, first, second, third);
    AddTriangle(scene, opposite, third, second);
    REQUIRE(scene.Instances.TryResize(1) == true);
    scene.Instances[0].Surface = &surface;
    scene.Instances[0].TriangleCount = scene.Triangles.GetSize();
    scene.InstanceCount = 1;
    REQUIRE(internal::BuildReferenceBvh(scene) == rhi::RasterStatus::Ready);
    for (usize i = 0; i < RAY_COUNT; ++i)
    {
        // x in [0.5, 1) makes 1 - x exact, so the origin lies on the diagonal x + y = 1.
        const float32 x = .5F + (float32(i) + .5F) / float32(2 * RAY_COUNT);
        const rhi::RayInput ray = internal::ReferenceRay({x, 1 - x, ORIGIN_HEIGHT}, {0, 0, -1}, RAY_LENGTH);
        rhi::RayResult hit;
        REQUIRE(internal::TraceReferenceOracle(scene, ray, hit) == rhi::RasterStatus::Ready);
        CAPTURE(i, x, hit.U, hit.V);
        REQUIRE(hit.Hit == 1);
        CHECK(hit.U >= 0);
        CHECK(hit.V >= 0);
        CHECK(hit.U + hit.V <= 1);
    }
}
