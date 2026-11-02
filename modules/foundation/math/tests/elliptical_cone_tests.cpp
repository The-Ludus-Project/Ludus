#include <catch2/catch_test_macros.hpp>
#include <ludus/foundation/math/elliptical_cone.hpp>
#include <ludus/foundation/math/scalar.hpp>

using namespace ludus::foundation;
namespace math = ludus::foundation::math;

TEST_CASE("Elliptical cone finds a region where the centre ray misses", "[math][elliptical-cone]")
{
    math::EllipticalCone cone;
    cone.RadiusWorld = 0.2;
    cone.MinorToMajor = 0.5;
    cone.FarWorld = 10;
    const math::ConeTriangle triangle{{0.1, -1, 2}, {0.1, 1, 2}, {1, 0, 2}};
    math::ConeTriangleIntersection intersection;
    REQUIRE(math::TryIntersectEllipticalConeTriangle(cone, triangle, intersection) == true);
    REQUIRE(intersection.IsHit == true);
    CHECK(math::Abs(intersection.NearWorld - 2) < 1e-12);
    CHECK(math::Abs(intersection.FarWorld - 2) < 1e-12);
    cone.RadiusWorld = 0;
    REQUIRE(math::TryIntersectEllipticalConeTriangle(cone, triangle, intersection) == true);
    CHECK(intersection.IsHit == false);
}

TEST_CASE("Cone intersections preserve axial clip extrema and barycentric positions", "[math][elliptical-cone]")
{
    math::EllipticalCone cone;
    cone.RadiusWorld = 0.5;
    cone.NearWorld = 1;
    cone.FarWorld = 3;
    const math::ConeTriangle triangle{{-1, -1, 0}, {1, -1, 4}, {0, 1, 2}};
    math::ConeTriangleIntersection intersection;
    REQUIRE(math::TryIntersectEllipticalConeTriangle(cone, triangle, intersection) == true);
    REQUIRE(intersection.IsHit == true);
    CHECK(math::Abs(intersection.NearWorld - 1) < 4e-12);
    CHECK(math::Abs(intersection.FarWorld - 3) < 1e-12);
    const math::Vector3d nearPoint = triangle.FirstWorld * intersection.NearBarycentric.X +
                                     triangle.SecondWorld * intersection.NearBarycentric.Y +
                                     triangle.ThirdWorld * intersection.NearBarycentric.Z;
    CHECK(math::Abs(nearPoint.Z - intersection.NearWorld) < 1e-12);
    CHECK(nearPoint.X * nearPoint.X + nearPoint.Y * nearPoint.Y <= 0.25 + 1e-12);
}

TEST_CASE("Cone bounds retain triangle hits and reject remote boxes", "[math][elliptical-cone]")
{
    math::EllipticalCone cone;
    cone.Slope = 0.1;
    cone.MinorToMajor = 0.25;
    cone.FarWorld = 10;
    bool canIntersect = false;
    REQUIRE(math::TryTestEllipticalConeBounds(cone, {-1, -1, 2}, {1, 1, 3}, canIntersect) == true);
    CHECK(canIntersect == true);
    REQUIRE(math::TryTestEllipticalConeBounds(cone, {10, 10, 2}, {11, 11, 3}, canIntersect) == true);
    CHECK(canIntersect == false);
    cone.MinorToMajor = 0;
    canIntersect = true;
    CHECK(math::TryTestEllipticalConeBounds(cone, {-1, -1, 2}, {1, 1, 3}, canIntersect) == false);
    CHECK(canIntersect == true);
}

TEST_CASE("A cone can intersect the interior of a large triangle with all vertices outside", "[math][elliptical-cone]")
{
    math::EllipticalCone cone;
    cone.Slope = 0.0000001;
    cone.FarWorld = 10;
    math::ConeTriangle triangle{{-1000, -1000, 1}, {1000, -1000, 1}, {0, 1000, 1}};
    math::ConeTriangleIntersection intersection;
    REQUIRE(math::TryIntersectEllipticalConeTriangle(cone, triangle, intersection) == true);
    REQUIRE(intersection.IsHit == true);
    CHECK(math::Abs(intersection.NearWorld - 1) < 4e-12);
    cone.Slope = 0;
    REQUIRE(math::TryIntersectEllipticalConeTriangle(cone, triangle, intersection) == true);
    CHECK(intersection.IsHit == true);
    triangle.ThirdWorld = triangle.SecondWorld;
    REQUIRE(math::TryIntersectEllipticalConeTriangle(cone, triangle, intersection) == true);
    CHECK(intersection.IsHit == false);
}

TEST_CASE("Unrepresentable cone edge coefficients preserve the previous result", "[math][elliptical-cone]")
{
    math::EllipticalCone cone;
    cone.Slope = 1e200;
    cone.FarWorld = 1e-200;
    const math::ConeTriangle triangle{{-1, -1, -1}, {1, -1, 1}, {0, 1, 0}};
    math::ConeTriangleIntersection intersection;
    intersection.IsHit = true;
    intersection.NearWorld = 2;
    CHECK(math::TryIntersectEllipticalConeTriangle(cone, triangle, intersection) == false);
    CHECK(intersection.IsHit == true);
    CHECK(intersection.NearWorld == 2);
}
