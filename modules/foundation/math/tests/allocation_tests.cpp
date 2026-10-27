// Zero-allocation probe for FoundationMath. Representative checked operations are
// driven through the real production functions under a counting operator
// new/delete; none of them may allocate. Built as its own executable and only
// when sanitizers are OFF (the sanitizer runtime provides its own new/delete).
// Mirrors modules/foundation/containers/tests/allocation_tests.cpp.

#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/batch.hpp>
#include <ludus/foundation/math/cubic.hpp>
#include <ludus/foundation/math/dynamics.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/patch.hpp>
#include <ludus/foundation/math/projected_solid_angle.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/transform.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>

namespace
{
long gNews = 0;
long gDeletes = 0;
bool gTracking = false;

void* AlignedAlloc(std::size_t n, std::size_t al) noexcept
{
    if (al < alignof(std::max_align_t))
    {
        al = alignof(std::max_align_t);
    }
    std::size_t rounded = (n + al - 1) & ~(al - 1);
    if (rounded == 0)
    {
        rounded = al;
    }
    return std::aligned_alloc(al, rounded);
}
void CountNew() noexcept
{
    if (gTracking)
    {
        ++gNews;
    }
}
void CountDelete(void* p) noexcept
{
    if (p != nullptr && gTracking)
    {
        ++gDeletes;
    }
}
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t n)
{
    CountNew();
    void* p = AlignedAlloc(n, alignof(std::max_align_t));
    if (p == nullptr)
    {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new(std::size_t n, std::align_val_t al)
{
    CountNew();
    void* p = AlignedAlloc(n, static_cast<std::size_t>(al));
    if (p == nullptr)
    {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    CountNew();
    return AlignedAlloc(n, alignof(std::max_align_t));
}
void operator delete(void* p) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    CountDelete(p);
    std::free(p);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

using namespace ludus::foundation::math;

namespace
{
struct TrackGuard
{
    TrackGuard()
    {
        gNews = 0;
        gDeletes = 0;
        gTracking = true;
    }
    ~TrackGuard()
    {
        gTracking = false;
    }
};
} // namespace

TEST_CASE("Checked math operations never allocate", "[math][alloc]")
{
    TrackGuard guard;

    // Normalisation.
    Vector3 unit{};
    REQUIRE(TryNormalize(Vector3{3, 0, 4}, unit) == MathStatus::Success);

    // Quaternion construction + rotation + conversion + extraction.
    Quaternion q{};
    REQUIRE(TryFromAxisAngle(Vector3{0, 0, 1}, 0.7f, q) == MathStatus::Success);
    const Vector3 rotated = Rotate(q, Vector3{1, 2, 3});
    const Matrix3 r = ToMatrix3(q);
    Quaternion back{};
    REQUIRE(TryToRotation(r, back) == MathStatus::Success);
    (void)rotated;

    // Matrix / affine inverse.
    Matrix4 m = ToMatrix4([] {
        Affine3 a = Affine3::Identity();
        a.Translation = Vector3{1, 2, 3};
        return a;
    }());
    Matrix4 inv{};
    REQUIRE(TryInverse(m, inv) == MathStatus::Success);

    // Projection + classification.
    Matrix4 p{};
    REQUIRE(TryPerspectiveReverseZ(1.0f, 1.0f, 0.1f, 100.0f, p) == MathStatus::Success);
    Frustum fr{};
    REQUIRE(TryExtractFrustum(p, FrustumMode::FinitePerspectiveOrOrthographic, fr) == MathStatus::Success);
    FrustumRelation rel = FrustumRelation::Outside;
    REQUIRE(TryClassifySphere(fr, Sphere{Vector3{0, 0, -10}, 1.0f}, 0.0f, rel) == MathStatus::Success);

    // Queries.
    RayHit hit{};
    REQUIRE(TryRaySphere(Ray3{Vector3{-5, 0, 0}, Vector3{1, 0, 0}},
                         Sphere{Vector3{0, 0, 0}, 1.0f},
                         RayInterval{0.0, 1e30},
                         hit) == MathStatus::Success);

    // Drag integration and swept radial contact.
    LinearDragStep drag;
    REQUIRE(TryComputeLinearDragStep(0.7, 1.0 / 60.0, drag) == MathStatus::Success);
    RadialSweepContact radial;
    REQUIRE(TrySweepRadialBand({10, 0, 0}, {10, 0, 0}, {}, 0, 20, 2, radial) == MathStatus::Success);
    REQUIRE(radial.Hit);

    // Cubic and patch kernels.
    CubicBezier3 curve;
    REQUIRE(TryFromHermite(CubicHermite3{{}, {3, 6, 9}, {1, 2, 3}, {1, 2, 3}}, 3, curve) == MathStatus::Success);
    CubicSample3 cubic;
    REQUIRE(TryEvaluateCubic(curve, 0.5, cubic) == MathStatus::Success);
    CubicBezier3 left, right;
    REQUIRE(TrySplitCubic(curve, 0.5, left, right) == MathStatus::Success);
    BicubicBezierPatch patch;
    for (ludus::foundation::usize i = 0; i < 4; ++i)
    {
        for (ludus::foundation::usize j = 0; j < 4; ++j)
        {
            patch.Control[i][j] = {static_cast<float32>(i), static_cast<float32>(j), 0};
        }
    }
    PatchSample patchSample;
    REQUIRE(TryEvaluatePatch(patch, {0.2, 0.8}, patchSample) == MathStatus::Success);
    Vector3 normal;
    REQUIRE(TryPatchNormal(patchSample, {}, normal) == MathStatus::Success);
    PatchSubdivision children;
    REQUIRE(TrySplitPatch(patch, {0.2, 0.8}, children) == MathStatus::Success);

    // Random stream.
    RandomStream rng;
    (void)rng.NextUInt32();
    (void)rng.NextFloat01();
    RandomKey key;
    REQUIRE(TryMakeRandomKey(42, 7, key) == MathStatus::Success);
    const RandomAddress address{0x0123456789abcdefULL, 99, 1234};
    REQUIRE(SampleUInt32(key, address) == 0x942d2d40u);
    REQUIRE(SampleFloat01(key, address) < 1.0f);
    RandomBlock block;
    REQUIRE(TrySampleBlock(key, {}, block) == MathStatus::Success);
    uint32 ticket = 0;
    REQUIRE(TrySampleBounded(key, address, 1000, ticket) == MathStatus::Success);
    PreparedBound32 bound;
    REQUIRE(TryPrepareBound32(1000, bound) == MathStatus::Success);
    REQUIRE(TrySampleBounded(key, address, bound, ticket) == MathStatus::Success);

    // Projected solid angle sampling of a polygonal light.
    ProjectedPolygonInput polygon;
    polygon.VertexCount = 4;
    polygon.PositionsTangent[0] = Vector3{1, -1, 0.5f};
    polygon.PositionsTangent[1] = Vector3{1, -1, 2};
    polygon.PositionsTangent[2] = Vector3{1, 1, 2};
    polygon.PositionsTangent[3] = Vector3{1, 1, 0.5f};
    ProjectedSolidAnglePolygon prepared;
    REQUIRE(TryPrepareProjectedSolidAnglePolygon(polygon, prepared) == MathStatus::Success);
    Vector3 lightDirection;
    REQUIRE(TrySampleProjectedSolidAnglePolygon(prepared, Vector2{0.3f, 0.6f}, lightDirection) == MathStatus::Success);
    REQUIRE(EvaluateProjectedSolidAnglePolygonDensity(prepared, lightDirection) > 0.0f);

    // Batch (caller-owned stack buffers; no heap scratch inside).
    std::array<Vector3, 8> input{};
    std::array<Vector3, 8> output{};
    REQUIRE(TryTransformPoints(
                [] {
                    Affine3 a = Affine3::Identity();
                    return a;
                }(),
                std::span<const Vector3>(input),
                std::span<Vector3>(output)) == MathStatus::Success);

    REQUIRE(gNews == 0);
    REQUIRE(gDeletes == 0);
}
