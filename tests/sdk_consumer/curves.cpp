#include <ludus/foundation/math/cubic.hpp>
#include <ludus/foundation/math/patch.hpp>

bool ExerciseInstalledCurves() noexcept
{
    using namespace ludus::foundation::math;
    CubicBezier3 curve;
    if (IsSuccess(TryFromHermite(CubicHermite3{{}, {3, 6, 9}, {1, 2, 3}, {1, 2, 3}}, 3, curve)) == false)
    {
        return false;
    }
    CubicSample3 sample;
    if (IsSuccess(TryEvaluateCubic(curve, 0.5, sample)) == false || sample.Position != Vector3{1.5f, 3, 4.5f})
    {
        return false;
    }
    CubicBezier3 left, right;
    if (IsSuccess(TrySplitCubic(curve, 0.5, left, right)) == false || left.Control[3] != right.Control[0])
    {
        return false;
    }
    BicubicBezierPatch patch;
    for (ludus::foundation::usize i = 0; i < 4; ++i)
    {
        for (ludus::foundation::usize j = 0; j < 4; ++j)
        {
            patch.Control[i][j] = {static_cast<float32>(i), static_cast<float32>(j), 0};
        }
    }
    PatchSample patchSample;
    Vector3 normal;
    PatchSubdivision children;
    return IsSuccess(TryEvaluatePatch(patch, {0.25, 0.75}, patchSample)) &&
           IsSuccess(TryPatchNormal(patchSample, {}, normal)) && normal == Vector3{0, 0, 1} &&
           IsSuccess(TrySplitPatch(patch, {0.25, 0.75}, children)) &&
           children.U0V0.Control[3][3] == patchSample.Position;
}
