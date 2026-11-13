// Convex hull cooking and the general convex cast: analytic answers, the
// initial-overlap and touching rules, and every validation failure.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;
namespace math = ludus::foundation::math;

namespace
{
constexpr usize BOX_CORNER_COUNT = 8;
constexpr float64 ANSWER_TOLERANCE = 1.0e-9;
constexpr float64 NAN_VALUE = std::numeric_limits<float64>::quiet_NaN();

Array<Vector3> MakeBoxPoints(Vector3 halfExtents)
{
    Array<Vector3> points;
    for (uint32 corner = 0; corner < BOX_CORNER_COUNT; ++corner)
    {
        points.Add({(corner & 1U) == 0 ? -halfExtents.X : halfExtents.X,
                    (corner & 2U) == 0 ? -halfExtents.Y : halfExtents.Y,
                    (corner & 4U) == 0 ? -halfExtents.Z : halfExtents.Z});
    }
    return points;
}

ConvexHullSource GetSource(const Array<Vector3>& points)
{
    return { .PointsShape = BorrowedView<const Vector3>(points.GetData(), points.GetSize()) };
}

ConvexHull CookHullOrFail(const Array<Vector3>& points)
{
    ConvexHull hull;
    const ConvexHullCookResult result = ConvexHull::TryCook(GetSource(points), hull);
    REQUIRE(result.Result == Status::Success);
    return hull;
}

TriangleMesh MakeFloor()
{
    MeshData data;
    AddFloorQuad(data);
    return CookOrFail(data);
}

ConvexCastResult CastOrFail(const TriangleMesh& mesh, const ConvexCastInput& input)
{
    ConvexCastResult result;
    REQUIRE(mesh.CastConvexClosest(input, result) == Status::Success);
    return result;
}

ConvexCastInput MakeDrop(ConvexShape shape, float64 height, float64 distance)
{
    return
    {
        .Shape = shape,
        .PositionMesh = {0.5, height, 0.5},
        .TranslationMesh = {0.0, -distance, 0.0},
    };
}

math::Quaternion MakeRotation(Vector3 axis, float32 radians)
{
    math::Quaternion rotation;
    REQUIRE(math::TryFromAxisAngle(axis, radians, rotation) == math::MathStatus::Success);
    return rotation;
}

/// A sentinel result that a failing query must leave untouched.
ConvexCastResult MakeSentinel()
{
    ConvexCastResult sentinel;
    sentinel.HasHit = true;
    sentinel.Fraction = 0.25;
    sentinel.TriangleIndex = 7;
    return sentinel;
}

bool IsSentinel(const ConvexCastResult& result)
{
    return result.HasHit == true && result.Fraction == 0.25 && result.TriangleIndex == 7 && result.HasNormal == false &&
           result.HasWitness == false;
}
} // namespace

TEST_CASE("A convex hull cooks bounds, an interior point and its source points", "[collision][hull]")
{
    Array<Vector3> points = MakeBoxPoints({0.5f, 1.0f, 0.25f});
    // Interior and duplicate points are admitted and never change a support result.
    points.Add({0.0f, 0.0f, 0.0f});
    points.Add(points[0]);
    ConvexHull hull = CookHullOrFail(points);
    CHECK(hull.IsCooked() == true);
    CHECK(hull.GetPointsShape().GetSize() == points.GetSize());
    CHECK(hull.GetBoundsShape().Min == Vector3{-0.5f, -1.0f, -0.25f});
    CHECK(hull.GetBoundsShape().Max == Vector3{0.5f, 1.0f, 0.25f});
    const Vector3d interior = hull.GetInteriorPointShape();
    CHECK(math::Abs(interior.X) < 0.5);
    CHECK(math::Abs(interior.Y) < 1.0);
    CHECK(math::Abs(interior.Z) < 0.25);

    ConvexHull moved = ludus::foundation::Move(hull);
    CHECK(moved.IsCooked() == true);
    CHECK(hull.IsCooked() == false);
    CHECK(hull.GetPointsShape().GetSize() == 0);
}

TEST_CASE("Convex hull cooking reports each failure and preserves the output", "[collision][hull]")
{
    const Array<Vector3> box = MakeBoxPoints({1.0f, 1.0f, 1.0f});
    ConvexHull hull = CookHullOrFail(MakeBoxPoints({2.0f, 2.0f, 2.0f}));
    const auto expectFailure = [&hull](const ConvexHullSource& source, Status status, uint32 pointIndex) {
        const ConvexHullCookResult result = ConvexHull::TryCook(source, hull);
        CHECK(result.Result == status);
        CHECK(result.PointIndex == pointIndex);
        CHECK(hull.GetBoundsShape().Max == Vector3{2.0f, 2.0f, 2.0f});
        CHECK(hull.GetPointsShape().GetSize() == BOX_CORNER_COUNT);
    };
    expectFailure({ .PointsShape = BorrowedView<const Vector3>(nullptr, 4) },
                  Status::InvalidArgument,
                  INVALID_POINT_INDEX);
    expectFailure({}, Status::NoGeometry, INVALID_POINT_INDEX);
    expectFailure({ .PointsShape = BorrowedView<const Vector3>(box.GetData(), 3) },
                  Status::DegenerateShape,
                  INVALID_POINT_INDEX);

    Array<Vector3> many;
    for (uint32 index = 0; index <= MAX_CONVEX_HULL_POINTS; ++index)
    {
        many.Add(box[index % BOX_CORNER_COUNT]);
    }
    expectFailure(GetSource(many), Status::CapacityExceeded, INVALID_POINT_INDEX);

    Array<Vector3> invalid = MakeBoxPoints({1.0f, 1.0f, 1.0f});
    invalid[2].Y = std::numeric_limits<float32>::infinity();
    expectFailure(GetSource(invalid), Status::NonFiniteInput, 2);
    invalid[2].Y = 1.0f;
    invalid[5].X = 2.0f * MAX_COORDINATE_METERS;
    expectFailure(GetSource(invalid), Status::OutOfRange, 5);

    Array<Vector3> coincident;
    Array<Vector3> collinear;
    Array<Vector3> coplanar;
    for (uint32 index = 0; index < BOX_CORNER_COUNT; ++index)
    {
        const float32 offset = static_cast<float32>(index);
        coincident.Add({1.0f, 2.0f, 3.0f});
        collinear.Add({offset, 2.0f * offset, -offset});
        coplanar.Add({offset, static_cast<float32>(index % 3), 0.0f});
    }
    expectFailure(GetSource(coincident), Status::DegenerateShape, INVALID_POINT_INDEX);
    expectFailure(GetSource(collinear), Status::DegenerateShape, INVALID_POINT_INDEX);
    expectFailure(GetSource(coplanar), Status::DegenerateShape, INVALID_POINT_INDEX);
}

TEST_CASE("Convex hull flatness is scale free", "[collision][hull]")
{
    for (const float32 scale : {1.0e-3f, 1.0f, 1.0e3f})
    {
        INFO("scale " << scale);
        ConvexHull hull;
        // Thickness ratios well below and well above MIN_CONVEX_HULL_THICKNESS_RATIO.
        const Array<Vector3> flat = MakeBoxPoints({scale, scale, scale * 1.0e-8f});
        CHECK(ConvexHull::TryCook(GetSource(flat), hull).Result == Status::DegenerateShape);
        const Array<Vector3> thin = MakeBoxPoints({scale, scale, scale * 1.0e-4f});
        CHECK(ConvexHull::TryCook(GetSource(thin), hull).Result == Status::Success);
    }
}

TEST_CASE("Convex casts drop each shape onto a floor at the analytic fraction", "[collision][convex]")
{
    const TriangleMesh floor = MakeFloor();
    const ConvexHull boxHull = CookHullOrFail(MakeBoxPoints({0.2f, 0.3f, 0.2f}));
    struct Drop final
    {
        const char* Name;
        ConvexShape Shape;
        math::Quaternion Orientation;
        float64 BottomBelowCenter;
    };
    const math::Quaternion identity{};
    const math::Quaternion lying = MakeRotation({0.0f, 0.0f, 1.0f}, math::kPiF * 0.5f);
    const math::Quaternion spun = MakeRotation({0.0f, 1.0f, 0.0f}, 0.7f);
    const StaticArray<Drop, 6> drops{Drop{"sphere", MakeSphereShape(0.25), identity, 0.25},
                                     Drop{"upright capsule", MakeCapsuleShape(0.3, 0.1), identity, 0.4},
                                     Drop{"lying capsule", MakeCapsuleShape(0.3, 0.1), lying, 0.1},
                                     Drop{"box", MakeBoxShape({0.2, 0.3, 0.2}), identity, 0.3},
                                     Drop{"spun box", MakeBoxShape({0.2, 0.3, 0.2}), spun, 0.3},
                                     Drop{"hull", MakeHullShape(boxHull), spun, 0.3}};
    for (const Drop& drop : drops)
    {
        INFO(drop.Name);
        ConvexCastInput input = MakeDrop(drop.Shape, 1.0, 2.0);
        input.OrientationMesh = drop.Orientation;
        const float64 expected = (1.0 - drop.BottomBelowCenter) / 2.0;
        for (const QueryTraversal traversal : {QueryTraversal::Hierarchy, QueryTraversal::AllTriangles})
        {
            input.Traversal = traversal;
            const ConvexCastResult result = CastOrFail(floor, input);
            REQUIRE(result.HasHit == true);
            CHECK(result.HasStartedOverlapping == false);
            CHECK(result.Fraction == Catch::Approx(expected).margin(ANSWER_TOLERANCE));
            CHECK(result.DistanceMeters == Catch::Approx(expected * 2.0).margin(ANSWER_TOLERANCE));
            REQUIRE(result.HasNormal == true);
            CHECK(result.NormalMesh.Y == Catch::Approx(1.0).margin(ANSWER_TOLERANCE));
            REQUIRE(result.HasWitness == true);
            CHECK(result.WitnessPositionMesh.Y == Catch::Approx(0.0).margin(ANSWER_TOLERANCE));
            CHECK(result.WitnessPositionMesh.X >= 0.0);
            CHECK(result.WitnessPositionMesh.X <= 1.0);
        }
    }
    // The sphere touches the floor at the point below its center.
    const ConvexCastResult sphere = CastOrFail(floor, MakeDrop(MakeSphereShape(0.25), 1.0, 2.0));
    CHECK(sphere.WitnessPositionMesh.X == Catch::Approx(0.5).margin(ANSWER_TOLERANCE));
    CHECK(sphere.WitnessPositionMesh.Z == Catch::Approx(0.5).margin(ANSWER_TOLERANCE));
}

TEST_CASE("The general cast agrees with the separating-axis box cast on a wall", "[collision][convex]")
{
    MeshData data;
    // A wall in the plane x = 2, facing -x, spanning y and z in [-1, 1].
    data.AddTriangle({2.0f, -1.0f, -1.0f}, {2.0f, 1.0f, 1.0f}, {2.0f, -1.0f, 1.0f});
    data.AddTriangle({2.0f, -1.0f, -1.0f}, {2.0f, 1.0f, -1.0f}, {2.0f, 1.0f, 1.0f});
    const TriangleMesh wall = CookOrFail(data);
    const BoxCastInput box
    {
        .CenterMesh = {0.0, 0.25, 0.1},
        .HalfExtentsMesh = {0.5, 0.3, 0.2},
        .TranslationMesh = {3.0, 0.0, 0.0},
    };
    BoxCastResult exact;
    REQUIRE(wall.CastBoxClosest(box, exact) == Status::Success);
    REQUIRE(exact.HasHit == true);
    ConvexCastInput general
    {
        .Shape = MakeBoxShape(box.HalfExtentsMesh),
        .PositionMesh = box.CenterMesh,
        .TranslationMesh = box.TranslationMesh,
    };
    const ConvexCastResult result = CastOrFail(wall, general);
    REQUIRE(result.HasHit == true);
    CHECK(result.Fraction == Catch::Approx(exact.Fraction).margin(ANSWER_TOLERANCE));
    CHECK(result.Fraction == Catch::Approx(0.5).margin(ANSWER_TOLERANCE));
    CHECK(result.NormalMesh.X == Catch::Approx(-1.0).margin(ANSWER_TOLERANCE));
    CHECK(result.WitnessPositionMesh.X == Catch::Approx(2.0).margin(ANSWER_TOLERANCE));
}

TEST_CASE("Convex casts classify touching and overlapping starts", "[collision][convex]")
{
    const TriangleMesh floor = MakeFloor();
    const ConvexShape sphere = MakeSphereShape(0.25);

    // Resting exactly on the floor: moving into it is a hit at zero; away, along it or not at all is a miss.
    ConvexCastInput resting = MakeDrop(sphere, 0.25, 1.0);
    ConvexCastResult into = CastOrFail(floor, resting);
    REQUIRE(into.HasHit == true);
    CHECK(into.Fraction == 0.0);
    CHECK(into.HasStartedOverlapping == false);
    REQUIRE(into.HasNormal == true);
    CHECK(into.NormalMesh.Y == Catch::Approx(1.0).margin(ANSWER_TOLERANCE));
    resting.TranslationMesh = {0.0, 1.0, 0.0};
    CHECK(CastOrFail(floor, resting).HasHit == false);
    resting.TranslationMesh = {0.3, 0.0, 0.0};
    CHECK(CastOrFail(floor, resting).HasHit == false);
    resting.TranslationMesh = {};
    CHECK(CastOrFail(floor, resting).HasHit == false);

    // Sunk into the floor: the policy decides.
    ConvexCastInput sunk = MakeDrop(sphere, 0.1, 1.0);
    const ConvexCastResult reported = CastOrFail(floor, sunk);
    REQUIRE(reported.HasHit == true);
    CHECK(reported.HasStartedOverlapping == true);
    CHECK(reported.Fraction == 0.0);
    CHECK(reported.HasNormal == false);
    CHECK(reported.HasWitness == false);
    sunk.TranslationMesh = {};
    CHECK(CastOrFail(floor, sunk).HasStartedOverlapping == true);
    sunk.OverlapPolicy = InitialOverlapPolicy::Ignore;
    CHECK(CastOrFail(floor, sunk).HasHit == false);
    sunk.OverlapPolicy = InitialOverlapPolicy::Reject;
    ConvexCastResult rejected = MakeSentinel();
    CHECK(floor.CastConvexClosest(sunk, rejected) == Status::InitialOverlap);
    CHECK(IsSentinel(rejected) == true);

    // Ending short of the floor, or moving away from it, is a miss.
    CHECK(CastOrFail(floor, MakeDrop(sphere, 1.0, 0.7)).HasHit == false);
    CHECK(CastOrFail(floor, MakeDrop(sphere, 1.0, -2.0)).HasHit == false);
    // Passing beside the floor is a miss.
    ConvexCastInput beside = MakeDrop(sphere, 1.0, 2.0);
    beside.PositionMesh.X = 1.3;
    CHECK(CastOrFail(floor, beside).HasHit == false);
    beside.PositionMesh.X = 1.2;
    CHECK(CastOrFail(floor, beside).HasHit == true);
}

TEST_CASE("Convex cast validation fails without changing the result", "[collision][convex]")
{
    const TriangleMesh floor = MakeFloor();
    const ConvexHull uncooked;
    const ConvexCastInput valid = MakeDrop(MakeSphereShape(0.25), 1.0, 2.0);
    const auto expectFailure = [&floor](const ConvexCastInput& input, Status status) {
        ConvexCastResult result = MakeSentinel();
        CHECK(floor.CastConvexClosest(input, result) == status);
        CHECK(IsSentinel(result) == true);
    };
    ConvexCastResult result = MakeSentinel();
    CHECK(TriangleMesh{}.CastConvexClosest(valid, result) == Status::NoGeometry);
    CHECK(IsSentinel(result) == true);

    ConvexCastInput input = valid;
    input.Shape.Kind = static_cast<ConvexShapeKind>(9);
    expectFailure(input, Status::InvalidArgument);
    input = valid;
    input.PositionMesh.Y = NAN_VALUE;
    expectFailure(input, Status::NonFiniteInput);
    input = valid;
    input.TranslationMesh.Z = NAN_VALUE;
    expectFailure(input, Status::NonFiniteInput);
    input = valid;
    input.Shape.RadiusMeters = NAN_VALUE;
    expectFailure(input, Status::NonFiniteInput);
    input = valid;
    input.OrientationMesh.W = std::numeric_limits<float32>::quiet_NaN();
    expectFailure(input, Status::NonFiniteInput);
    input = valid;
    input.OrientationMesh = {0.0f, 0.0f, 0.0f, 2.0f};
    expectFailure(input, Status::InvalidArgument);
    input = valid;
    input.Shape.RadiusMeters = 0.0;
    expectFailure(input, Status::InvalidArgument);
    input = valid;
    input.Shape = MakeCapsuleShape(-0.1, 0.2);
    expectFailure(input, Status::InvalidArgument);
    input = valid;
    input.Shape = MakeBoxShape({0.2, 0.0, 0.2});
    expectFailure(input, Status::InvalidArgument);
    input = valid;
    input.Shape.Kind = ConvexShapeKind::Hull;
    expectFailure(input, Status::InvalidArgument);
    input.Shape = MakeHullShape(uncooked);
    expectFailure(input, Status::InvalidArgument);
    input = valid;
    input.OverlapPolicy = static_cast<InitialOverlapPolicy>(9);
    expectFailure(input, Status::InvalidArgument);
    input = valid;
    input.Traversal = static_cast<QueryTraversal>(9);
    expectFailure(input, Status::InvalidArgument);
    input = valid;
    input.PositionMesh.X = 2.0 * static_cast<float64>(MAX_COORDINATE_METERS);
    expectFailure(input, Status::OutOfRange);
    input = valid;
    input.TranslationMesh.Y = -2.0 * MAX_QUERY_DISTANCE_METERS;
    expectFailure(input, Status::OutOfRange);
    input = valid;
    input.Shape.RadiusMeters = 2.0 * MAX_QUERY_DISTANCE_METERS;
    expectFailure(input, Status::OutOfRange);
    input = valid;
    input.Shape = MakeCapsuleShape(2.0 * MAX_QUERY_DISTANCE_METERS, 0.1);
    expectFailure(input, Status::OutOfRange);
}

TEST_CASE("Convex casts are deterministic across repeated queries", "[collision][convex]")
{
    // The floor's two triangles share the diagonal x = z; a sphere centred on it lands on both.
    const TriangleMesh floor = MakeFloor();
    const ConvexCastInput input = MakeDrop(MakeSphereShape(0.25), 1.0, 2.0);
    const ConvexCastResult first = CastOrFail(floor, input);
    const ConvexCastResult second = CastOrFail(floor, input);
    CHECK(first.Fraction == second.Fraction);
    CHECK(first.TriangleIndex == second.TriangleIndex);
    CHECK(first.NormalMesh == second.NormalMesh);
    CHECK(first.WitnessPositionMesh == second.WitnessPositionMesh);
    CHECK(first.TriangleIndex <= 1);
}
