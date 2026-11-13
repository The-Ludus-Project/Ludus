// The general convex cast against independent oracles on seeded random scenes.
//
// Hierarchy traversal is compared exactly with the all-triangles traversal.
// The answers themselves are compared with a separate first-contact oracle:
// for a shape translating against a convex triangle, the separation (exact
// point and segment distances for spheres and capsules, the separating-axis
// gap for boxes) is a convex function of time, so a ternary search finds its
// minimum and a bisection finds the first time it reaches zero. Queries whose
// answer lies within the oracle's resolution of a tangency are skipped.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;
namespace math = ludus::foundation::math;

namespace
{
constexpr uint32 SCENE_SEED_COUNT = 3;
constexpr uint32 QUERIES_PER_KIND = 300;
constexpr uint32 SOUP_TRIANGLE_COUNT = 150;
constexpr float32 SOUP_EXTENT_METERS = 8.0f;
constexpr uint32 KIND_COUNT = 4;
constexpr uint32 SEARCH_ITERATIONS = 200;
constexpr float64 GOLDEN_SECTION = 0.6180339887498949;
/// Oracle separations within this many meters of zero are treated as tangent and skipped.
constexpr float64 TANGENCY_METERS = 1.0e-6;
/// Allowed difference between oracle and cast contact distances along the translation, in meters.
constexpr float64 CONTACT_DISTANCE_TOLERANCE_METERS = 1.0e-6;
constexpr usize BOX_CORNER_COUNT = 8;

/// The posed query shape as the oracle sees it, in mesh space.
struct OracleShape final
{
    ConvexShapeKind Kind = ConvexShapeKind::Sphere;
    Vector3d CenterMesh{};
    StaticArray<Vector3d, 3> AxesMesh{};
    Vector3d HalfExtents{};
    float64 HalfHeight = 0.0;
    float64 Radius = 0.0;
};

struct Triangle final
{
    Vector3d First;
    Vector3d Second;
    Vector3d Third;
};

Vector3d ClosestPointOnTriangle(Vector3d point, const Triangle& triangle)
{
    // Barycentric projection with edge and vertex fallbacks; independent of the module's simplex code.
    const Vector3d a = triangle.First;
    const Vector3d b = triangle.Second;
    const Vector3d c = triangle.Third;
    const Vector3d ab = b - a;
    const Vector3d ac = c - a;
    const Vector3d normal = math::Cross(ab, ac);
    const float64 normalSquared = math::Dot(normal, normal);
    const Vector3d projected = point - normal * (math::Dot(point - a, normal) / normalSquared);
    const float64 u = math::Dot(math::Cross(b - projected, c - projected), normal) / normalSquared;
    const float64 v = math::Dot(math::Cross(c - projected, a - projected), normal) / normalSquared;
    const float64 w = 1.0 - u - v;
    if (u >= 0.0 && v >= 0.0 && w >= 0.0)
    {
        return projected;
    }
    const auto onSegment = [point](Vector3d start, Vector3d end) {
        const Vector3d edge = end - start;
        const float64 t = math::Clamp(math::Dot(point - start, edge) / math::Dot(edge, edge), 0.0, 1.0);
        return start + edge * t;
    };
    const StaticArray<Vector3d, 3> candidates{onSegment(a, b), onSegment(b, c), onSegment(c, a)};
    Vector3d best = candidates[0];
    for (const Vector3d candidate : candidates)
    {
        if (math::LengthSquared(candidate - point) < math::LengthSquared(best - point))
        {
            best = candidate;
        }
    }
    return best;
}

/// Closest distance between segments [p1, q1] and [p2, q2], after Ericson, "Real-Time Collision Detection",
/// section 5.1.9.
struct Segment final
{
    Vector3d Start;
    Vector3d End;
};

float64 GetSegmentDistance(const Segment& first, const Segment& second)
{
    const Vector3d p1 = first.Start;
    const Vector3d q1 = first.End;
    const Vector3d p2 = second.Start;
    const Vector3d q2 = second.End;
    const Vector3d d1 = q1 - p1;
    const Vector3d d2 = q2 - p2;
    const Vector3d r = p1 - p2;
    const float64 a = math::Dot(d1, d1);
    const float64 e = math::Dot(d2, d2);
    const float64 f = math::Dot(d2, r);
    float64 s = 0.0;
    float64 t = 0.0;
    if (a <= 0.0)
    {
        t = math::Clamp(f / e, 0.0, 1.0);
    }
    else
    {
        const float64 c = math::Dot(d1, r);
        if (e <= 0.0)
        {
            s = math::Clamp(-c / a, 0.0, 1.0);
        }
        else
        {
            const float64 b = math::Dot(d1, d2);
            const float64 denominator = a * e - b * b;
            s = denominator > 0.0 ? math::Clamp((b * f - c * e) / denominator, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0)
            {
                t = 0.0;
                s = math::Clamp(-c / a, 0.0, 1.0);
            }
            else if (t > 1.0)
            {
                t = 1.0;
                s = math::Clamp((b - c) / a, 0.0, 1.0);
            }
        }
    }
    return math::Length((p1 + d1 * s) - (p2 + d2 * t));
}

float64 GetSegmentTriangleDistance(Vector3d start, Vector3d end, const Triangle& triangle)
{
    const Vector3d normal = math::Cross(triangle.Second - triangle.First, triangle.Third - triangle.First);
    const float64 startSide = math::Dot(start - triangle.First, normal);
    const float64 endSide = math::Dot(end - triangle.First, normal);
    if ((startSide <= 0.0 && endSide >= 0.0) || (startSide >= 0.0 && endSide <= 0.0))
    {
        const float64 span = startSide - endSide;
        const Vector3d crossing = span != 0.0 ? start + (end - start) * (startSide / span) : start;
        // Barycentric signs decide containment; an exact distance test fails on the crossing's rounding.
        const float64 normalSquared = math::Dot(normal, normal);
        const float64 u =
            math::Dot(math::Cross(triangle.Second - crossing, triangle.Third - crossing), normal) / normalSquared;
        const float64 v =
            math::Dot(math::Cross(triangle.Third - crossing, triangle.First - crossing), normal) / normalSquared;
        if (u >= 0.0 && v >= 0.0 && 1.0 - u - v >= 0.0)
        {
            return 0.0;
        }
    }
    float64 distance = math::Min(math::Length(ClosestPointOnTriangle(start, triangle) - start),
                                 math::Length(ClosestPointOnTriangle(end, triangle) - end));
    distance = math::Min(distance, GetSegmentDistance({start, end}, {triangle.First, triangle.Second}));
    distance = math::Min(distance, GetSegmentDistance({start, end}, {triangle.Second, triangle.Third}));
    return math::Min(distance, GetSegmentDistance({start, end}, {triangle.Third, triangle.First}));
}

/// Largest separating-axis gap between an oriented box and a triangle; positive exactly when they are disjoint.
float64 GetBoxTriangleGap(const OracleShape& shape, Vector3d centerMesh, const Triangle& triangle)
{
    const StaticArray<Vector3d, 3> corners{triangle.First, triangle.Second, triangle.Third};
    const StaticArray<Vector3d, 3> edges{triangle.Second - triangle.First,
                                         triangle.Third - triangle.Second,
                                         triangle.First - triangle.Third};
    Array<Vector3d> axes;
    axes.Add(math::Cross(edges[0], edges[1]));
    for (const Vector3d boxAxis : shape.AxesMesh)
    {
        axes.Add(boxAxis);
        for (const Vector3d edge : edges)
        {
            axes.Add(math::Cross(boxAxis, edge));
        }
    }
    const StaticArray<float64, 3> halves{shape.HalfExtents.X, shape.HalfExtents.Y, shape.HalfExtents.Z};
    float64 gap = -1.0e300;
    for (const Vector3d rawAxis : axes)
    {
        const float64 length = math::Length(rawAxis);
        if (length < 1.0e-12)
        {
            continue;
        }
        const Vector3d axis = rawAxis * (1.0 / length);
        float64 radius = 0.0;
        for (usize index = 0; index < 3; ++index)
        {
            radius += halves[index] * math::Abs(math::Dot(shape.AxesMesh[index], axis));
        }
        const float64 center = math::Dot(centerMesh, axis);
        float64 minimum = math::Dot(corners[0], axis);
        float64 maximum = minimum;
        for (const Vector3d corner : corners)
        {
            minimum = math::Min(minimum, math::Dot(corner, axis));
            maximum = math::Max(maximum, math::Dot(corner, axis));
        }
        gap = math::Max(gap, math::Max(minimum - (center + radius), (center - radius) - maximum));
    }
    return gap;
}

float64 GetSeparation(const OracleShape& shape, Vector3d centerMesh, const Triangle& triangle)
{
    switch (shape.Kind)
    {
        case ConvexShapeKind::Sphere:
            return math::Length(ClosestPointOnTriangle(centerMesh, triangle) - centerMesh) - shape.Radius;
        case ConvexShapeKind::Capsule: {
            const Vector3d offset = shape.AxesMesh[1] * shape.HalfHeight;
            return GetSegmentTriangleDistance(centerMesh - offset, centerMesh + offset, triangle) - shape.Radius;
        }
        case ConvexShapeKind::Box:
        case ConvexShapeKind::Hull:
            return GetBoxTriangleGap(shape, centerMesh, triangle);
    }
    return 0.0;
}

/// Oracle answer for one triangle.
struct OracleContact final
{
    bool IsTangent = false; ///< Too close to a tangency to compare.
    bool IsOverlapping = false;
    bool HasContact = false;
    float64 Fraction = 1.0;
};

/// Radius of a ball about the shape centre that holds the whole shape.
float64 GetBoundingRadius(const OracleShape& shape)
{
    switch (shape.Kind)
    {
        case ConvexShapeKind::Sphere:
            return shape.Radius;
        case ConvexShapeKind::Capsule:
            return shape.HalfHeight + shape.Radius;
        case ConvexShapeKind::Box:
        case ConvexShapeKind::Hull:
            return math::Length(shape.HalfExtents);
    }
    return 0.0;
}

/// Whether the swept bounding ball can come within the tangency band of the triangle's bounds; a false answer
/// proves the triangle is neither touched nor tangent, so the oracle can skip it.
bool CanReach(const OracleShape& shape, Vector3d translationMesh, const Triangle& triangle)
{
    const float64 reach = GetBoundingRadius(shape) + 2.0 * TANGENCY_METERS;
    const Vector3d start = shape.CenterMesh;
    const Vector3d end = shape.CenterMesh + translationMesh;
    const StaticArray<float64, 3> starts{start.X, start.Y, start.Z};
    const StaticArray<float64, 3> ends{end.X, end.Y, end.Z};
    const StaticArray<Vector3d, 3> corners{triangle.First, triangle.Second, triangle.Third};
    for (usize axis = 0; axis < 3; ++axis)
    {
        float64 minimum = 1.0e300;
        float64 maximum = -1.0e300;
        for (const Vector3d corner : corners)
        {
            const float64 value = axis == 0 ? corner.X : (axis == 1 ? corner.Y : corner.Z);
            minimum = math::Min(minimum, value);
            maximum = math::Max(maximum, value);
        }
        if (minimum > math::Max(starts[axis], ends[axis]) + reach ||
            maximum < math::Min(starts[axis], ends[axis]) - reach)
        {
            return false;
        }
    }
    return true;
}

OracleContact FindFirstContact(const OracleShape& shape, Vector3d translationMesh, const Triangle& triangle)
{
    OracleContact contact;
    if (CanReach(shape, translationMesh, triangle) == false)
    {
        return contact;
    }
    const auto separationAt = [&](float64 t) {
        return GetSeparation(shape, shape.CenterMesh + translationMesh * t, triangle);
    };
    const float64 start = separationAt(0.0);
    if (math::Abs(start) <= TANGENCY_METERS)
    {
        contact.IsTangent = true;
        return contact;
    }
    if (start < 0.0)
    {
        contact.IsOverlapping = true;
        return contact;
    }
    // Golden-section search for the minimum of the convex separation over [0, 1].
    float64 low = 0.0;
    float64 high = 1.0;
    for (uint32 iteration = 0; iteration < SEARCH_ITERATIONS; ++iteration)
    {
        const float64 left = high - (high - low) * GOLDEN_SECTION;
        const float64 right = low + (high - low) * GOLDEN_SECTION;
        if (separationAt(left) <= separationAt(right))
        {
            high = right;
        }
        else
        {
            low = left;
        }
    }
    const float64 minimumTime = 0.5 * (low + high);
    const float64 minimum = separationAt(minimumTime);
    if (math::Abs(minimum) <= TANGENCY_METERS)
    {
        contact.IsTangent = true;
        return contact;
    }
    if (minimum > 0.0)
    {
        return contact;
    }
    float64 before = 0.0;
    float64 after = minimumTime;
    for (uint32 iteration = 0; iteration < SEARCH_ITERATIONS; ++iteration)
    {
        const float64 middle = 0.5 * (before + after);
        (separationAt(middle) > 0.0 ? before : after) = middle;
    }
    contact.HasContact = true;
    contact.Fraction = after;
    if ((1.0 - after) * math::Length(translationMesh) <= TANGENCY_METERS)
    {
        contact.IsTangent = true;
    }
    return contact;
}

math::Quaternion NextOrientation(RandomStream& inoutRandom)
{
    const Vector3d axis = NextDirection(inoutRandom);
    math::Quaternion orientation;
    const float32 angle = NextInRange(inoutRandom, -math::kPiF, math::kPiF);
    REQUIRE(math::TryFromAxisAngle(
                {static_cast<float32>(axis.X), static_cast<float32>(axis.Y), static_cast<float32>(axis.Z)},
                angle,
                orientation) == math::MathStatus::Success);
    return orientation;
}

/// Shape axes in mesh space by the same float64 conversion the module documents.
StaticArray<Vector3d, 3> GetAxes(math::Quaternion orientation)
{
    const float64 x = static_cast<float64>(orientation.X);
    const float64 y = static_cast<float64>(orientation.Y);
    const float64 z = static_cast<float64>(orientation.Z);
    const float64 w = static_cast<float64>(orientation.W);
    const float64 scale = 2.0 / (x * x + y * y + z * z + w * w);
    return {Vector3d{1.0 - scale * (y * y + z * z), scale * (x * y + w * z), scale * (x * z - w * y)},
            Vector3d{scale * (x * y - w * z), 1.0 - scale * (x * x + z * z), scale * (y * z + w * x)},
            Vector3d{scale * (x * z + w * y), scale * (y * z - w * x), 1.0 - scale * (x * x + y * y)}};
}

bool AreEqual(const ConvexCastResult& first, const ConvexCastResult& second)
{
    return first.HasHit == second.HasHit && first.Fraction == second.Fraction &&
           first.DistanceMeters == second.DistanceMeters &&
           first.HasStartedOverlapping == second.HasStartedOverlapping && first.HasNormal == second.HasNormal &&
           first.NormalMesh == second.NormalMesh && first.HasWitness == second.HasWitness &&
           first.WitnessPositionMesh == second.WitnessPositionMesh && first.TriangleIndex == second.TriangleIndex;
}

/// Counts that show a scene exercised every outcome.
struct Coverage final
{
    uint32 Hits = 0;
    uint32 Misses = 0;
    uint32 Overlaps = 0;
    uint32 Skipped = 0;
};
} // namespace

TEST_CASE("Convex casts match the first-contact oracle and the all-triangles traversal", "[collision][convex][oracle]")
{
    for (uint32 seed = 1; seed <= SCENE_SEED_COUNT; ++seed)
    {
        const MeshData data = MakeTriangleSoup(
        {
            .Seed = seed,
            .TriangleCount = SOUP_TRIANGLE_COUNT,
            .ExtentMeters = SOUP_EXTENT_METERS,
        });
        const TriangleMesh mesh = CookOrFail(data);
        RandomStream random;
        REQUIRE(random.TryReseed(seed + 100, 0) == true);
        for (uint32 kindIndex = 0; kindIndex < KIND_COUNT; ++kindIndex)
        {
            const ConvexShapeKind kind = static_cast<ConvexShapeKind>(kindIndex);
            INFO("seed " << seed << " kind " << kindIndex);
            Coverage coverage;
            for (uint32 query = 0; query < QUERIES_PER_KIND; ++query)
            {
                const Vector3d halfExtents{static_cast<float64>(NextInRange(random, 0.1f, 1.2f)),
                                           static_cast<float64>(NextInRange(random, 0.1f, 1.2f)),
                                           static_cast<float64>(NextInRange(random, 0.1f, 1.2f))};
                // Hull points are float32; build the hull and the oracle box from the same rounded corners.
                const Vector3 hullHalf{static_cast<float32>(halfExtents.X),
                                       static_cast<float32>(halfExtents.Y),
                                       static_cast<float32>(halfExtents.Z)};
                Array<Vector3> hullPoints;
                for (uint32 corner = 0; corner < BOX_CORNER_COUNT; ++corner)
                {
                    hullPoints.Add({(corner & 1U) == 0 ? -hullHalf.X : hullHalf.X,
                                    (corner & 2U) == 0 ? -hullHalf.Y : hullHalf.Y,
                                    (corner & 4U) == 0 ? -hullHalf.Z : hullHalf.Z});
                }
                hullPoints.Add({0.0f, 0.0f, 0.0f});
                ConvexHull hull;
                REQUIRE(ConvexHull::TryCook(
                            { .PointsShape = BorrowedView<const Vector3>(hullPoints.GetData(), hullPoints.GetSize()) },
                            hull)
                            .Result == Status::Success);
                OracleShape oracle;
                oracle.Kind = kind;
                ConvexCastInput input;
                switch (kind)
                {
                    case ConvexShapeKind::Sphere:
                        input.Shape = MakeSphereShape(halfExtents.X);
                        break;
                    case ConvexShapeKind::Capsule:
                        input.Shape = MakeCapsuleShape(halfExtents.Y, halfExtents.X);
                        break;
                    case ConvexShapeKind::Box:
                        input.Shape = MakeBoxShape(halfExtents);
                        break;
                    case ConvexShapeKind::Hull:
                        input.Shape = MakeHullShape(hull);
                        break;
                }
                oracle.Radius = halfExtents.X;
                oracle.HalfHeight = halfExtents.Y;
                oracle.HalfExtents = kind == ConvexShapeKind::Hull ? Widen(hullHalf) : halfExtents;
                input.PositionMesh = Widen(NextPoint(random, SOUP_EXTENT_METERS + 2.0f));
                input.OrientationMesh = NextOrientation(random);
                input.TranslationMesh = NextDirection(random) * static_cast<float64>(NextInRange(random, 0.0f, 12.0f));
                oracle.CenterMesh = input.PositionMesh;
                oracle.AxesMesh = GetAxes(input.OrientationMesh);

                ConvexCastResult hierarchy;
                ConvexCastResult brute;
                input.Traversal = QueryTraversal::Hierarchy;
                REQUIRE(mesh.CastConvexClosest(input, hierarchy) == Status::Success);
                input.Traversal = QueryTraversal::AllTriangles;
                REQUIRE(mesh.CastConvexClosest(input, brute) == Status::Success);
                REQUIRE(AreEqual(hierarchy, brute) == true);

                bool isTangent = false;
                bool isOverlapping = false;
                bool hasContact = false;
                float64 fraction = 1.0;
                for (usize index = 0; index + 2 < data.Indices.GetSize(); index += 3)
                {
                    const Triangle triangle{Widen(data.Positions[data.Indices[index]]),
                                            Widen(data.Positions[data.Indices[index + 1]]),
                                            Widen(data.Positions[data.Indices[index + 2]])};
                    const OracleContact contact = FindFirstContact(oracle, input.TranslationMesh, triangle);
                    isTangent = isTangent == true || contact.IsTangent == true;
                    isOverlapping = isOverlapping == true || contact.IsOverlapping == true;
                    if (contact.HasContact == true && contact.Fraction < fraction)
                    {
                        hasContact = true;
                        fraction = contact.Fraction;
                    }
                }
                if (isTangent == true)
                {
                    ++coverage.Skipped;
                    continue;
                }
                INFO("query " << query);
                if (isOverlapping == true)
                {
                    CHECK(hierarchy.HasStartedOverlapping == true);
                    ++coverage.Overlaps;
                    continue;
                }
                REQUIRE(hierarchy.HasStartedOverlapping == false);
                REQUIRE(hierarchy.HasHit == hasContact);
                if (hasContact == true)
                {
                    const float64 length = math::Length(input.TranslationMesh);
                    CHECK(math::Abs(hierarchy.Fraction - fraction) * length <= CONTACT_DISTANCE_TOLERANCE_METERS);
                    CHECK(hierarchy.HasNormal == true);
                    CHECK(math::Dot(hierarchy.NormalMesh, input.TranslationMesh) < 0.0);
                    ++coverage.Hits;
                }
                else
                {
                    ++coverage.Misses;
                }
            }
            UNSCOPED_INFO("hits " << coverage.Hits << " misses " << coverage.Misses << " overlaps " << coverage.Overlaps
                                  << " skipped " << coverage.Skipped);
            CHECK(coverage.Hits > 0);
            CHECK(coverage.Misses > 0);
            CHECK(coverage.Overlaps > 0);
            CHECK(coverage.Skipped < QUERIES_PER_KIND / 10);
        }
    }
}

TEST_CASE("Convex casts with point-cloud hulls match the all-triangles traversal", "[collision][convex][oracle]")
{
    RandomStream heights;
    REQUIRE(heights.TryReseed(31, 0) == true);
    constexpr uint32 TERRAIN_CELLS = 16;
    const TriangleMesh terrain = CookOrFail(MakeGrid(TERRAIN_CELLS, &heights));
    RandomStream random;
    REQUIRE(random.TryReseed(32, 0) == true);
    constexpr uint32 HULL_POINT_COUNT = 40;
    constexpr uint32 QUERY_COUNT = 300;
    uint32 hits = 0;
    for (uint32 query = 0; query < QUERY_COUNT; ++query)
    {
        Array<Vector3> points;
        for (uint32 point = 0; point < HULL_POINT_COUNT; ++point)
        {
            points.Add(NextPoint(random, 0.6f));
        }
        ConvexHull hull;
        REQUIRE(ConvexHull::TryCook(
                    {
                        .PointsShape = BorrowedView<const Vector3>(points.GetData(), points.GetSize()),
                    },
                    hull)
                    .Result == Status::Success);
        ConvexCastInput input
        {
            .Shape = MakeHullShape(hull),
            .PositionMesh = {static_cast<float64>(NextInRange(random, 0.0f, 16.0f)),
                             static_cast<float64>(NextInRange(random, 0.5f, 3.0f)),
                             static_cast<float64>(NextInRange(random, 0.0f, 16.0f))},
            .OrientationMesh = NextOrientation(random),
            .TranslationMesh = NextDirection(random) * 6.0,
            .OverlapPolicy = static_cast<InitialOverlapPolicy>(query % 2),
        };
        ConvexCastResult hierarchy;
        ConvexCastResult brute;
        REQUIRE(terrain.CastConvexClosest(input, hierarchy) == Status::Success);
        input.Traversal = QueryTraversal::AllTriangles;
        REQUIRE(terrain.CastConvexClosest(input, brute) == Status::Success);
        REQUIRE(AreEqual(hierarchy, brute) == true);
        hits += hierarchy.HasHit == true ? 1 : 0;
    }
    CHECK(hits > QUERY_COUNT / 4);
}
