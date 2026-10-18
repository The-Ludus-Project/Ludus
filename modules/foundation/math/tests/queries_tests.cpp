// Geometric queries: plane distance/reflection, conservative AABB transform,
// ray/AABB, ray/sphere, closest segment points, frustum extraction and
// conservative classification against clip inequalities.

#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/queries.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace ludus::foundation::math;

namespace
{
[[nodiscard]] bool Close(double a, double b, double t = 1e-5)
{
    return std::fabs(a - b) <= t;
}
// Overload so a float32 result (e.g. a Plane coefficient) can be compared
// without an implicit float->double promotion at the call site
// (-Wdouble-promotion -Werror). The widening is explicit here instead.
[[nodiscard]] bool Close(float a, double b, double t = 1e-5)
{
    return std::fabs(static_cast<double>(a) - b) <= t;
}
constexpr double kInf = std::numeric_limits<double>::infinity();
} // namespace

TEST_CASE("Plane construction and signed distance", "[math][queries]")
{
    Plane pl{};
    REQUIRE(TryMakePlane(Vector3{0, 0, 2}, Vector3{0, 0, 5}, pl) == MathStatus::Success);
    REQUIRE(Close(pl.Normal.Z, 1.0));
    REQUIRE(Close(pl.D, -5.0));
    float d = 0.0f;
    REQUIRE(TryPlaneSignedDistance(pl, Vector3{0, 0, 8}, d) == MathStatus::Success);
    REQUIRE(Close(d, 3.0));
    REQUIRE(TryPlaneSignedDistance(pl, Vector3{0, 0, 2}, d) == MathStatus::Success);
    REQUIRE(Close(d, -3.0));
    // Non-unit plane fields rejected.
    Plane bad{Vector3{0, 0, 2}, 0.0f};
    REQUIRE(TryPlaneSignedDistance(bad, Vector3{0, 0, 0}, d) == MathStatus::Degenerate);
    // Zero normal to construct -> Degenerate.
    Plane out = pl;
    const Plane keep = out;
    REQUIRE(TryMakePlane(Vector3{0, 0, 0}, Vector3{0, 0, 0}, out) == MathStatus::Degenerate);
    REQUIRE((out.Normal.Z == keep.Normal.Z && out.D == keep.D));
}

TEST_CASE("Conservative AABB transform encloses all corners", "[math][queries]")
{
    Quaternion q{};
    REQUIRE(TryFromAxisAngle(Vector3{0, 0, 1}, kPiF / 4.0f, q) == MathStatus::Success);
    const Matrix3 r = ToMatrix3(q);
    Affine3 a = Affine3::Identity();
    a.Columns[0] = r.Columns[0];
    a.Columns[1] = r.Columns[1];
    a.Columns[2] = r.Columns[2];
    a.Translation = Vector3{10, -5, 0};
    const Aabb3 box{Vector3{0, 0, 0}, Vector3{1, 1, 1}};
    Aabb3 out{};
    REQUIRE(TryTransformAabb(a, box, out) == MathStatus::Success);
    for (int i = 0; i < 8; ++i)
    {
        const Vector3 corner{(i & 1) ? 1.0f : 0.0f, (i & 2) ? 1.0f : 0.0f, (i & 4) ? 1.0f : 0.0f};
        const Vector3 tc = TransformPoint(a, corner);
        REQUIRE(tc.X >= out.Min.X);
        REQUIRE(tc.X <= out.Max.X);
        REQUIRE(tc.Y >= out.Min.Y);
        REQUIRE(tc.Y <= out.Max.Y);
    }
    // Empty stays empty.
    Aabb3 emptyOut{};
    REQUIRE(TryTransformAabb(a, EmptyAabb3(), emptyOut) == MathStatus::Success);
    REQUIRE(IsEmpty(emptyOut));
}

TEST_CASE("Ray/AABB: hit, parallel miss, inside start, invalid direction", "[math][queries]")
{
    const Aabb3 box{Vector3{-1, -1, -1}, Vector3{1, 1, 1}};
    const RayInterval iv{0.0, kInf};
    RayHit h{};
    REQUIRE(TryRayAabb(Ray3{Vector3{-5, 0, 0}, Vector3{1, 0, 0}}, box, iv, h) == MathStatus::Success);
    REQUIRE(h.Result == HitResult::Hit);
    REQUIRE(Close(h.EntryT, 4.0));
    REQUIRE(Close(h.ExitT, 6.0));
    // Parallel outside slab misses.
    REQUIRE(TryRayAabb(Ray3{Vector3{0, 5, 0}, Vector3{1, 0, 0}}, box, iv, h) == MathStatus::Success);
    REQUIRE(h.Result == HitResult::Miss);
    // Inside start clips entry to minT.
    REQUIRE(TryRayAabb(Ray3{Vector3{0, 0, 0}, Vector3{1, 0, 0}}, box, iv, h) == MathStatus::Success);
    REQUIRE(h.Result == HitResult::Hit);
    REQUIRE(Close(h.EntryT, 0.0));
    // Zero direction invalid.
    REQUIRE(TryRayAabb(Ray3{Vector3{0, 0, 0}, Vector3{0, 0, 0}}, box, iv, h) == MathStatus::InvalidArgument);
}

TEST_CASE("Ray/sphere: hit, tangent, miss, inside start, negative radius", "[math][queries]")
{
    const Sphere sp{Vector3{0, 0, 0}, 1.0f};
    const RayInterval iv{0.0, kInf};
    RayHit h{};
    REQUIRE(TryRaySphere(Ray3{Vector3{-5, 0, 0}, Vector3{1, 0, 0}}, sp, iv, h) == MathStatus::Success);
    REQUIRE(h.Result == HitResult::Hit);
    REQUIRE(Close(h.EntryT, 4.0));
    REQUIRE(Close(h.ExitT, 6.0));
    // Tangent counts as hit.
    REQUIRE(TryRaySphere(Ray3{Vector3{-5, 1, 0}, Vector3{1, 0, 0}}, sp, iv, h) == MathStatus::Success);
    REQUIRE(h.Result == HitResult::Hit);
    // Miss.
    REQUIRE(TryRaySphere(Ray3{Vector3{-5, 2, 0}, Vector3{1, 0, 0}}, sp, iv, h) == MathStatus::Success);
    REQUIRE(h.Result == HitResult::Miss);
    // Inside start -> entry minT.
    REQUIRE(TryRaySphere(Ray3{Vector3{0, 0, 0}, Vector3{1, 0, 0}}, sp, iv, h) == MathStatus::Success);
    REQUIRE(h.Result == HitResult::Hit);
    REQUIRE(Close(h.EntryT, 0.0));
    // Negative radius invalid.
    REQUIRE(TryRaySphere(Ray3{Vector3{-5, 0, 0}, Vector3{1, 0, 0}}, Sphere{Vector3{0, 0, 0}, -1.0f}, iv, h) ==
            MathStatus::InvalidArgument);
}

TEST_CASE("Closest points between segments: skew, parallel, degenerate", "[math][queries]")
{
    SegmentClosest sc{};
    // Skew segments crossing at distance 1 along Z.
    REQUIRE(TryClosestPointsSegments(Vector3{-1, 0, 0}, Vector3{1, 0, 0}, Vector3{0, -1, 1}, Vector3{0, 1, 1}, sc) ==
            MathStatus::Success);
    REQUIRE(Close(sc.SquaredDistance, 1.0, 1e-6));
    REQUIRE(Close(sc.S, 0.5, 1e-6));
    REQUIRE(Close(sc.T, 0.5, 1e-6));
    // Parallel segments.
    REQUIRE(TryClosestPointsSegments(Vector3{0, 0, 0}, Vector3{1, 0, 0}, Vector3{0, 1, 0}, Vector3{1, 1, 0}, sc) ==
            MathStatus::Success);
    REQUIRE(Close(sc.SquaredDistance, 1.0, 1e-6));
    // Point vs segment.
    REQUIRE(
        TryClosestPointsSegments(Vector3{0.5f, 2, 0}, Vector3{0.5f, 2, 0}, Vector3{0, 0, 0}, Vector3{1, 0, 0}, sc) ==
        MathStatus::Success);
    REQUIRE(Close(sc.SquaredDistance, 4.0, 1e-6));
    REQUIRE(Close(sc.T, 0.5, 1e-6));
}

namespace
{
// Independent brute-force oracle: the minimum squared distance between the two
// segments over a dense (s,t) grid. Does NOT call the production helper, so it is
// a real cross-check (design §13: a test oracle must not call what it verifies).
[[nodiscard]] double BruteForceSegmentSq(Vector3 p0, Vector3 p1, Vector3 q0, Vector3 q1)
{
    constexpr int kSteps = 400;
    double best = kInf;
    for (int i = 0; i <= kSteps; ++i)
    {
        const double s = static_cast<double>(i) / kSteps;
        const double ax = static_cast<double>(p0.X) + s * (static_cast<double>(p1.X) - static_cast<double>(p0.X));
        const double ay = static_cast<double>(p0.Y) + s * (static_cast<double>(p1.Y) - static_cast<double>(p0.Y));
        const double az = static_cast<double>(p0.Z) + s * (static_cast<double>(p1.Z) - static_cast<double>(p0.Z));
        for (int j = 0; j <= kSteps; ++j)
        {
            const double t = static_cast<double>(j) / kSteps;
            const double bx = static_cast<double>(q0.X) + t * (static_cast<double>(q1.X) - static_cast<double>(q0.X));
            const double by = static_cast<double>(q0.Y) + t * (static_cast<double>(q1.Y) - static_cast<double>(q0.Y));
            const double bz = static_cast<double>(q0.Z) + t * (static_cast<double>(q1.Z) - static_cast<double>(q0.Z));
            const double dx = ax - bx, dy = ay - by, dz = az - bz;
            best = std::fmin(best, dx * dx + dy * dy + dz * dz);
        }
    }
    return best;
}
} // namespace

TEST_CASE("Closest segments: optimum on an s-boundary with interior t", "[math][queries]")
{
    // Regression for a sign error in the s=0 endpoint candidate: the true minimum
    // is at s=0 (segment 1's p0) and an INTERIOR t on segment 2, which no other
    // candidate pins. A long bar whose near end is closest to the middle of a
    // crossing segment.
    SegmentClosest sc{};
    REQUIRE(TryClosestPointsSegments(Vector3{0, 0, 0},
                                     Vector3{0, 0, 5},
                                     Vector3{-1, 0.5F, -10},
                                     Vector3{1, 0.5F, 10},
                                     sc) == MathStatus::Success);
    REQUIRE(Close(sc.SquaredDistance, 0.25, 1e-5));
    REQUIRE(Close(sc.S, 0.0, 1e-6));
    REQUIRE(Close(sc.T, 0.5, 1e-3));

    // Mirror: optimum at s=1 with interior t.
    REQUIRE(TryClosestPointsSegments(Vector3{0, 0, 5},
                                     Vector3{0, 0, 0},
                                     Vector3{-1, 0.5F, -10},
                                     Vector3{1, 0.5F, 10},
                                     sc) == MathStatus::Success);
    REQUIRE(Close(sc.SquaredDistance, 0.25, 1e-5));
    REQUIRE(Close(sc.S, 1.0, 1e-6));
}

TEST_CASE("Closest segments agree with an independent brute-force oracle", "[math][queries]")
{
    struct Pair
    {
        Vector3 p0, p1, q0, q1;
    };
    // Deterministic adversarial corpus covering boundary and interior optima.
    const Pair cases[] = {
        {{0, 0, 0}, {0, 0, 5}, {-1, 0.5F, -10}, {1, 0.5F, 10}},
        {{-2, 1, 0}, {2, 1, 0}, {0, -1, 3}, {0, -1, -3}},
        {{1, 2, 3}, {-1, -2, -3}, {3, -1, 2}, {-2, 2, -1}},
        {{0, 0, 0}, {1, 0, 0}, {5, 0, 1}, {5, 1, 1}}, // optimum at s=1, t boundary/interior
        {{0, 0, 0}, {0, 1, 0}, {2, 0.3F, 0}, {2, 0.7F, 0}},
        {{-5, 0, 0}, {5, 0, 0}, {0, 2, 0}, {0, 2, 0}}, // segment vs point
    };
    for (const Pair& k : cases)
    {
        SegmentClosest sc{};
        REQUIRE(TryClosestPointsSegments(k.p0, k.p1, k.q0, k.q1, sc) == MathStatus::Success);
        const double oracle = BruteForceSegmentSq(k.p0, k.p1, k.q0, k.q1);
        // The grid oracle slightly overestimates the true minimum; the production
        // result must be <= oracle + grid slack and must not exceed it.
        REQUIRE(sc.SquaredDistance <= oracle + 1e-3);
        REQUIRE(std::fabs(std::sqrt(sc.SquaredDistance) - std::sqrt(std::fmax(oracle, 0.0))) < 2e-2);
    }
}

TEST_CASE("Frustum extraction and conservative classification", "[math][queries]")
{
    Matrix4 p{};
    REQUIRE(TryPerspectiveReverseZ(DegreesToRadians(90.0f), 1.0f, 0.1f, 100.0f, p) == MathStatus::Success);
    Frustum fr{};
    REQUIRE(TryExtractFrustum(p, FrustumMode::FinitePerspectiveOrOrthographic, fr) == MathStatus::Success);
    FrustumRelation rel = FrustumRelation::Outside;
    REQUIRE(TryClassifySphere(fr, Sphere{Vector3{0, 0, -10}, 0.1f}, 0.0f, rel) == MathStatus::Success);
    REQUIRE(rel == FrustumRelation::Inside);
    REQUIRE(TryClassifySphere(fr, Sphere{Vector3{0, 0, 10}, 0.1f}, 0.0f, rel) == MathStatus::Success);
    REQUIRE(rel == FrustumRelation::Outside);
    REQUIRE(TryClassifySphere(fr, Sphere{Vector3{0, 0, -0.1f}, 0.5f}, 0.0f, rel) == MathStatus::Success);
    REQUIRE(rel == FrustumRelation::Intersecting);
    // AABB inside and empty-outside.
    REQUIRE(TryClassifyAabb(fr, Aabb3{Vector3{-0.1f, -0.1f, -10.1f}, Vector3{0.1f, 0.1f, -9.9f}}, 0.0f, rel) ==
            MathStatus::Success);
    REQUIRE(rel == FrustumRelation::Inside);
    REQUIRE(TryClassifyAabb(fr, EmptyAabb3(), 0.0f, rel) == MathStatus::Success);
    REQUIRE(rel == FrustumRelation::Outside);
}

TEST_CASE("Infinite frustum marks far plane inactive and does not cull distant objects", "[math][queries]")
{
    Matrix4 p{};
    REQUIRE(TryPerspectiveReverseZInfinite(DegreesToRadians(90.0f), 1.0f, 0.1f, p) == MathStatus::Success);
    Frustum fr{};
    REQUIRE(TryExtractFrustum(p, FrustumMode::InfiniteReverseZPerspective, fr) == MathStatus::Success);
    REQUIRE((fr.ActiveMask & (1u << 5)) == 0); // far inactive
    FrustumRelation rel = FrustumRelation::Outside;
    REQUIRE(TryClassifySphere(fr, Sphere{Vector3{0, 0, -1e6f}, 1.0f}, 0.0f, rel) == MathStatus::Success);
    REQUIRE(rel == FrustumRelation::Inside); // no false-negative far culling
}

TEST_CASE("Classification agrees with direct clip inequalities (no false-negative culling)", "[math][queries]")
{
    Matrix4 p{};
    REQUIRE(TryPerspectiveReverseZ(DegreesToRadians(70.0f), 1.3f, 0.2f, 50.0f, p) == MathStatus::Success);
    Frustum fr{};
    REQUIRE(TryExtractFrustum(p, FrustumMode::FinitePerspectiveOrOrthographic, fr) == MathStatus::Success);
    // Sample a grid of points; any point whose clip coords satisfy the canonical
    // inequalities (-w<=x,y<=w, 0<=z<=w) must NOT be classified Outside.
    int tested = 0;
    for (int ix = -6; ix <= 6; ++ix)
    {
        for (int iy = -6; iy <= 6; ++iy)
        {
            for (int iz = 1; iz <= 10; ++iz)
            {
                const Vector3 world{static_cast<float>(ix), static_cast<float>(iy), -static_cast<float>(iz)};
                const Vector4 clip = p * Vector4{world.X, world.Y, world.Z, 1.0f};
                const float w = clip.W;
                const bool insideClip = (w > 0.0f) && (clip.X >= -w) && (clip.X <= w) && (clip.Y >= -w) &&
                                        (clip.Y <= w) && (clip.Z >= 0.0f) && (clip.Z <= w);
                if (insideClip == false)
                {
                    continue;
                }
                FrustumRelation rel = FrustumRelation::Outside;
                REQUIRE(TryClassifySphere(fr, Sphere{world, 0.0f}, 0.0f, rel) == MathStatus::Success);
                REQUIRE(rel != FrustumRelation::Outside); // conservative: never cull a visible point
                ++tested;
            }
        }
    }
    REQUIRE(tested > 0);
}

TEST_CASE("Ray origin offsets project error, preserve tangents and fail transactionally", "[math][ray]")
{
    Vector3 output{7, 8, 9};
    RayOriginOffset offset{{100000, 2, 0}, {.001F, .002F, .003F}, {0, 0, 1}, {0, 0, 1}};
    REQUIRE(TryOffsetRayOrigin(offset, output) == MathStatus::Success);
    CHECK(output.X == offset.Position.X);
    CHECK(output.Y == offset.Position.Y);
    CHECK(output.Z > .003F);
    CHECK(output.Z < .00301F);
    offset.Direction = {0, 0, -1};
    REQUIRE(TryOffsetRayOrigin(offset, output) == MathStatus::Success);
    CHECK(output.Z < -.003F);
    offset.Position = {};
    offset.Error = {};
    REQUIRE(TryOffsetRayOrigin(offset, output) == MathStatus::Success);
    CHECK(output.Z == -std::numeric_limits<ludus::foundation::float32>::min());
    const auto keep = output;
    offset.Error.X = -1;
    CHECK(TryOffsetRayOrigin(offset, output) == MathStatus::InvalidArgument);
    CHECK(output == keep);
    offset.Error.X = 0;
    offset.Position.Z = std::numeric_limits<ludus::foundation::float32>::infinity();
    CHECK(TryOffsetRayOrigin(offset, output) == MathStatus::NonFiniteInput);
    CHECK(output == keep);
}
