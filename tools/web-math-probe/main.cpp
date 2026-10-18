// Browser/Node numerical corpus for FoundationMath. Native unit tests prove CPU
// correctness; this probe re-runs a representative corpus through the SAME public
// API compiled for the pinned Emscripten target, so the browser's wasm math is
// actually exercised rather than assumed. It prints a single machine-checkable
// summary line the verify script asserts on.
//
// This is NOT a GPU test and NOT a renderer: it validates CPU numerical meaning
// on the web toolchain. GPU transfer convention is verified separately by the
// WebGPU shader fixture (M5).

#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/batch.hpp>
#include <ludus/foundation/math/cubic.hpp>
#include <ludus/foundation/math/enclosure.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/patch.hpp>
#include <ludus/foundation/math/precision.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/transform.hpp>

#include "enclosure_oracle.hpp"

#include <cmath>
#include <cstdio>

using namespace ludus::foundation::math;

bool ExerciseInstalledGeometry() noexcept;

namespace
{
// Same deterministic enclosure corpus as modules/foundation/math/tests/enclosure_tests.cpp.
constexpr ludus::foundation::core::uint64 ENCLOSURE_SEED = 0x1D0C5EEDULL;
constexpr ludus::foundation::core::int32 ENCLOSURE_CASE_COUNT = 200000;
int gFail = 0;

void Check(bool condition, const char* label)
{
    if (condition == false)
    {
        std::printf("FAIL: %s\n", label);
        ++gFail;
    }
}

bool CloseF(float a, float b, float t)
{
    return std::fabs(a - b) <= t;
}
} // namespace

int main()
{
    // Cross and rotation.
    const Vector3 z = Cross(Vector3{1, 0, 0}, Vector3{0, 1, 0});
    Check(z.X == 0.0f && z.Y == 0.0f && z.Z == 1.0f, "cross X,Y=Z");

    Quaternion q{};
    Check(TryFromAxisAngle(Vector3{0, 0, 1}, kHalfPiF, q) == MathStatus::Success, "axis-angle");
    const Vector3 r = Rotate(q, Vector3{1, 0, 0});
    Check(CloseF(r.X, 0.0f, 1e-5f) && CloseF(r.Y, 1.0f, 1e-5f), "rotate X->Y");
    const Vector3 rm = ToMatrix3(q) * Vector3{1, 0, 0};
    Check(CloseF(rm.X, r.X, 1e-5f) && CloseF(rm.Y, r.Y, 1e-5f), "rotate==matrix action");

    // Normalisation of extremes.
    Vector3 u{};
    Check(TryNormalize(Vector3{1e30f, 2e30f, 2e30f}, u) == MathStatus::Success, "normalize huge");
    Check(CloseF(Length(u), 1.0f, 2e-6f), "huge unit length");

    // Inverse round-trip.
    Matrix4 m = Matrix4::Identity();
    m.Columns[3] = Vector4{3, 4, 5, 1};
    Matrix4 inv{};
    Check(TryInverse(m, inv) == MathStatus::Success, "inverse");
    const Vector4 back = inv * (m * Vector4{1, 2, 3, 1});
    Check(CloseF(back.X, 1.0f, 1e-4f) && CloseF(back.Z, 3.0f, 1e-4f), "inverse round-trip");

    // Reverse-Z depth endpoints.
    Matrix4 p{};
    Check(TryPerspectiveReverseZ(DegreesToRadians(60.0f), 1.0f, 0.1f, 100.0f, p) == MathStatus::Success, "proj");
    {
        Vector3 ndc{};
        (void)TryPerspectiveDivide(p * Vector4{0, 0, -0.1f, 1}, 0.0f, ndc);
        Check(CloseF(ndc.Z, 1.0f, 1e-4f), "near depth 1");
        (void)TryPerspectiveDivide(p * Vector4{0, 0, -100.0f, 1}, 0.0f, ndc);
        Check(CloseF(ndc.Z, 0.0f, 1e-4f), "far depth 0");
    }

    // Large coordinate precision.
    Vector3 rel{};
    Check(TryMakeRelative(Vector3d{1000000000.5, 0, 0}, Vector3d{1000000000.0, 0, 0}, 10.0, rel) == MathStatus::Success,
          "relative");
    Check(CloseF(rel.X, 0.5f, 1e-4f), "relative preserves 0.5");

    // Addressed and PCG known answers (bit-exact contract across targets).
    {
        RandomKey key;
        Check(TryMakeRandomKey(42, 7, key) == MathStatus::Success, "random key");
        Check(key.Value == 0xccf635ee9e9e2fa4ULL, "random key v1");
        const RandomAddress address{0x0123456789abcdefULL, 99, 1234};
        Check(SampleUInt32(key, address) == 0x942d2d40u, "Philox addressed v1");
        Check(SampleFloat01(key, address) == 0.5788143277168274f, "Philox float v1");
        uint32 ticket = 0;
        Check(TrySampleBounded(key, address, 1000, ticket) == MathStatus::Success && ticket == 578, "Philox bound v1");
        Check(TrySampleBounded(key, {}, 0x80000001u, ticket) == MathStatus::Success && ticket == 0x3c0cb600u,
              "Philox rejection v1");
        RandomBlock block;
        Check(TrySampleBlock({}, {}, block) == MathStatus::Success, "Philox block");
        const uint32 expected[] = {0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u};
        for (ludus::foundation::usize lane = 0; lane < 4; ++lane)
        {
            Check(block.Values[lane] == expected[lane], "Philox zero KAT");
        }
    }
    {
        RandomStream rng;
        (void)rng.TryReseed(42, 54);
        const uint32 expected[6] = {0xa15c02b7u, 0x7b47f409u, 0xba1d3330u, 0x83d2f293u, 0xbfa4784bu, 0xcbed606eu};
        bool ok = true;
        for (uint32 e : expected)
        {
            ok = ok && (rng.NextUInt32() == e);
        }
        Check(ok, "PCG known answers");
    }

    // Cubic/patch kernels through the same exported API as native and SDK users.
    CubicSample1 cubic;
    Check(TryEvaluateCubic(CubicBezier1{{0, 0, 0, 1}}, 0.5, cubic) == MathStatus::Success && cubic.Position == 0.125f &&
              cubic.FirstDerivative == 0.75f && cubic.SecondDerivative == 3,
          "cubic analytic derivatives");
    CubicBezier1 left, right;
    Check(TrySplitCubic(CubicBezier1{{0, 0, 0, 1}}, 0.5, left, right) == MathStatus::Success &&
              left.Control[3] == 0.125f && right.Control[0] == left.Control[3],
          "cubic subdivision");
    Check(TryFromHermite(CubicHermite1{2, 5, 3, -6}, 2, left) == MathStatus::Success && left.Control[1] == 4 &&
              left.Control[2] == 9,
          "Hermite domain scaling");
    BicubicBezierPatch patch;
    for (ludus::foundation::usize i = 0; i < 4; ++i)
    {
        for (ludus::foundation::usize j = 0; j < 4; ++j)
        {
            patch.Control[i][j] = {static_cast<float32>(i), static_cast<float32>(j), 0};
        }
    }
    PatchSample patchSample;
    Check(TryEvaluatePatch(patch, {0.25, 0.75}, patchSample) == MathStatus::Success &&
              patchSample.Position == Vector3{0.75f, 2.25f, 0} && patchSample.DU == Vector3{3, 0, 0} &&
              patchSample.DV == Vector3{0, 3, 0},
          "patch axis convention");
    Vector3 normal;
    Check(TryPatchNormal(patchSample, {}, normal) == MathStatus::Success && normal == Vector3{0, 0, 1}, "patch normal");
    PatchSubdivision children;
    Check(TrySplitPatch(patch, {0.25, 0.75}, children) == MathStatus::Success &&
              children.U0V0.Control[3][3] == patchSample.Position,
          "patch subdivision");
    cubic = {7, 8, 9};
    Check(TryEvaluateCubic(CubicBezier1{}, -1, cubic) == MathStatus::InvalidArgument && cubic.Position == 7,
          "cubic failure preserves output");

    // Outward-rounded enclosures (ADR 0024 Certified class): the same exact-oracle corpus as the native tests,
    // so the pinned Wasm toolchain validates the helper before Geometry advertises Certified bounds.
    const ludus::foundation::core::int32 enclosureFailures =
        enclosure_oracle::RunCorpus(ENCLOSURE_SEED, ENCLOSURE_CASE_COUNT);
    Check(enclosureFailures == 0, "outward enclosures contain exact results");
    Enclosure exactSum;
    Check(TryAddOutward(MakePointEnclosure(1.0), MakePointEnclosure(2.0), exactSum) == MathStatus::Success &&
              exactSum.Lower == 3.0 && exactSum.Upper == 3.0,
          "exact sum is not widened");
    Check(ExerciseInstalledGeometry() == true, "geometry path, distance map, track and Certified surface mesh");

    std::printf("LUDUS_WEB_MATH_RESULT failures=%d\n", gFail);
    return gFail == 0 ? 0 : 1;
}
