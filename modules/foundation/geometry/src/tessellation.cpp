// Dyadic surface mesh preparation with Certified cell bounds (see tessellation.hpp).
//
// Thanks to Michael Bunnell, "Adaptive Tessellation of Subdivision Surfaces with
// Displacement Mapping", GPU Gems 2, chapter 7, sections 7.1.4-7.1.6 and 7.2.1
// (reviewed in docs/architecture/curves-surfaces-gems-review.md): one patch's
// data supplies each shared edge and corner so neighbours cannot disagree after
// rounding, and boundary subdivision must match on both sides. Departure: Ludus
// uses synchronized rectangular dyadic grids on the CPU instead of GPU
// render-to-texture tessellation, and never accepts an edge-only test as an
// interior bound.
//
// Thanks also to Luiz Henrique de Figueiredo, "Adaptive Sampling of Parametric
// Curves", Graphics Gems V, chapter IV.4, printed pages 173-178: refinement work
// is spent where the geometry requires it. Departure: instead of flatness probes,
// every cell is accepted only by a complete Certified subpatch bound.
//
// The cell bound itself (control net against the degree-elevated bilinear
// interpolant, plus Length(P00 - P10 - P01 + P11)/4 for the chosen diagonal) is
// derived in docs/architecture/curves-surfaces.md, "Tessellation and error
// ownership". On either parameter triangle the bilinear surface minus its
// triangle is -v(1 - u) or -u(1 - v) times the twist vector, whose factor is at
// most 1/4. Subpatch controls are enclosed with the blossoms of
// internal/control_box.hpp.

#include <ludus/foundation/geometry/tessellation.hpp>

#include "internal/control_box.hpp"
#include "internal/prepare_support.hpp"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/enclosure.hpp>
#include <ludus/foundation/math/patch.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::foundation::geometry
{
using core::Array;
using core::StaticArray;
using core::uint32;
using math::Enclosure;
using math::MakePointEnclosure;
using math::MathStatus;
using math::Vector3;
using math::Vector3d;

namespace
{
using internal::CubicControlBoxes;
using internal::EnclosureBox;
using internal::Fail;
using internal::MakePointBox;
using internal::ToGeometryStatus;
using internal::WriteDiagnostic;

// Index of the last control along a patch direction.
constexpr usize LAST_CONTROL = PATCH_CONTROL_ORDER - 1;
// Degree of the bicubic patch; the bilinear net weights are multiples of 1/DEGREE^2.
constexpr usize DEGREE = PATCH_CONTROL_ORDER - 1;
// Spatial axes of a position.
constexpr usize AXIS_COUNT = 3;
// Triangles emitted per grid cell.
constexpr usize TRIANGLES_PER_CELL = 2;
// Largest uint32 vertex index plus one: the number of addressable render vertices.
constexpr uint64 UINT32_VERTEX_LIMIT = uint64{1} << 32U;
// Normal samples per cell for the Estimated angle: four corners and the centre.
constexpr usize NORMAL_SAMPLE_COUNT = 5;
// The scalar factor of the twist vector on either parameter triangle is at most 1/4.
constexpr float64 TWIST_FACTOR = 0.25;
// Relative difference below which two refinement candidates' scores are equal, so ulp-level differences between
// the enclosures of mirrored grids do not choose the direction; the coarser direction is refined instead.
constexpr float64 SCORE_TIE_RELATIVE = 1e-9;

struct GridLevels final
{
    usize U = 0;
    usize V = 0;
};

// Bounds of one patch at one pair of levels.
struct PatchEvaluation final
{
    float64 PositionBound = 0.0;
    float64 NormalAngle = 0.0;
};

// One emitted position with its Certified distance to the exact surface point.
struct EmittedSample final
{
    Vector3 Position{};
    float64 ErrorBound = 0.0;
};

// Shared state of one TryBuildSurfaceMesh call.
struct BuildContext final
{
    const SurfaceView& Surface;
    const SurfaceMeshProfile& Profile;
    math::PatchNormalPolicy NormalPolicy{};
    Enclosure OneNinth{};
    usize CellTestCount = 0;
    GeometryDiagnostic* DiagnosticOrNull = nullptr;
};

[[nodiscard]] usize GetCellCount(usize level) noexcept
{
    return usize{1} << level;
}

// index / 2^level, exact because the divisor is a power of two.
[[nodiscard]] float64 ComputeDyadic(usize index, usize level) noexcept
{
    return static_cast<float64>(index) / static_cast<float64>(GetCellCount(level));
}

[[nodiscard]] bool IsUSide(PatchSide side) noexcept
{
    return side == PatchSide::U0 || side == PatchSide::U1;
}

// Level of the grid direction that runs along a side.
[[nodiscard]] usize GetAlongLevel(GridLevels levels, PatchSide side) noexcept
{
    return IsUSide(side) == true ? levels.V : levels.U;
}

void SetAlongLevel(GridLevels& inoutLevels, PatchSide side, usize level) noexcept
{
    if (IsUSide(side) == true)
    {
        inoutLevels.V = level;
    }
    else
    {
        inoutLevels.U = level;
    }
}

[[nodiscard]] usize FindIncidence(const SurfaceEdge& edge, usize patchIndex, PatchSide side) noexcept
{
    return edge.Incidence[0].PatchIndex == patchIndex && edge.Incidence[0].Side == side ? 0 : 1;
}

// Round an enclosed point to float32 and bound the distance from the rounded point to every point in the box.
[[nodiscard]] GeometryStatus TryRoundEnclosedPoint(const EnclosureBox& box, EmittedSample& outSample) noexcept
{
    const StaticArray<Enclosure, AXIS_COUNT> axes{{box.X, box.Y, box.Z}};
    StaticArray<float64, AXIS_COUNT> rounded{};
    EnclosureBox distance;
    const StaticArray<Enclosure*, AXIS_COUNT> distanceAxes{{&distance.X, &distance.Y, &distance.Z}};
    for (usize axis = 0; axis < axes.GetSize(); ++axis)
    {
        const Enclosure value = axes[axis];
        const float64 midpoint = value.Lower + 0.5 * (value.Upper - value.Lower);
        float32 narrowed = 0.0F;
        if (internal::TryNarrowComponent(midpoint, narrowed) == false)
        {
            return GeometryStatus::OutOfRange;
        }
        rounded[axis] = static_cast<float64>(narrowed);
        Enclosure belowLower;
        Enclosure aboveUpper;
        MathStatus status =
            math::TrySubtractOutward(MakePointEnclosure(rounded[axis]), MakePointEnclosure(value.Lower), belowLower);
        if (status == MathStatus::Success)
        {
            status = math::TrySubtractOutward(MakePointEnclosure(value.Upper),
                                              MakePointEnclosure(rounded[axis]),
                                              aboveUpper);
        }
        if (status != MathStatus::Success)
        {
            return ToGeometryStatus(status);
        }
        // Every exact coordinate in [Lower, Upper] is within this distance of the rounded one.
        const float64 farthest = math::Max(math::Max(math::Abs(belowLower.Lower), math::Abs(belowLower.Upper)),
                                           math::Max(math::Abs(aboveUpper.Lower), math::Abs(aboveUpper.Upper)));
        *distanceAxes[axis] = MakePointEnclosure(farthest);
    }
    Enclosure length;
    const MathStatus status = internal::TryEncloseBoxLength(distance, length);
    if (status != MathStatus::Success)
    {
        return ToGeometryStatus(status);
    }
    outSample = EmittedSample
    {
        .Position = Vector3{static_cast<float32>(rounded[0]),
                            static_cast<float32>(rounded[1]),
                            static_cast<float32>(rounded[2])},
        .ErrorBound = length.Upper,
    };
    return GeometryStatus::Success;
}

[[nodiscard]] CubicControlBoxes MakeCurveBoxes(const StaticArray<Vector3, PATCH_CONTROL_ORDER>& control) noexcept
{
    CubicControlBoxes boxes;
    for (usize k = 0; k < PATCH_CONTROL_ORDER; ++k)
    {
        boxes[k] = MakePointBox(control[k]);
    }
    return boxes;
}

[[nodiscard]] CubicControlBoxes MakeRowBoxes(const math::BicubicBezierPatch& patch, usize uIndex) noexcept
{
    CubicControlBoxes boxes;
    for (usize j = 0; j < PATCH_CONTROL_ORDER; ++j)
    {
        boxes[j] = MakePointBox(patch.Control[uIndex][j]);
    }
    return boxes;
}

// Interior sample of an edge on its canonical curve at canonical parameter t.
[[nodiscard]] GeometryStatus TryComputeEdgeSample(const SurfaceEdge& edge, float64 t, EmittedSample& outSample) noexcept
{
    EnclosureBox point;
    const MathStatus status = internal::TryEvaluateBlossom(MakeCurveBoxes(edge.Control), t, t, t, point);
    if (status != MathStatus::Success)
    {
        return ToGeometryStatus(status);
    }
    return TryRoundEnclosedPoint(point, outSample);
}

// Interior sample of a patch: de Casteljau in v for every row, then in u.
[[nodiscard]] GeometryStatus
TryComputeInteriorSample(const PreparedPatch& patch, float64 u, float64 v, EmittedSample& outSample) noexcept
{
    CubicControlBoxes column;
    for (usize i = 0; i < PATCH_CONTROL_ORDER; ++i)
    {
        const MathStatus status = internal::TryEvaluateBlossom(MakeRowBoxes(patch.Control, i), v, v, v, column[i]);
        if (status != MathStatus::Success)
        {
            return ToGeometryStatus(status);
        }
    }
    EnclosureBox point;
    const MathStatus status = internal::TryEvaluateBlossom(column, u, u, u, point);
    if (status != MathStatus::Success)
    {
        return ToGeometryStatus(status);
    }
    return TryRoundEnclosedPoint(point, outSample);
}

// The emitted sample at grid point (i, j) of a patch: a corner record, a canonical edge sample or an interior one.
// The same function produces the positions that are emitted, so cell bounds see the exact emitted values.
[[nodiscard]] GeometryStatus TryComputeGridSample(const SurfaceView& surface,
                                                  usize patchIndex,
                                                  GridLevels levels,
                                                  usize i,
                                                  usize j,
                                                  EmittedSample& outSample) noexcept
{
    const PreparedPatch& patch = surface.Patches[patchIndex];
    const usize uCells = GetCellCount(levels.U);
    const usize vCells = GetCellCount(levels.V);
    const bool isUBoundary = i == 0 || i == uCells;
    const bool isVBoundary = j == 0 || j == vCells;
    if (isUBoundary == true && isVBoundary == true)
    {
        const PatchCorner corner = i == 0 ? (j == 0 ? PatchCorner::U0V0 : PatchCorner::U0V1)
                                          : (j == 0 ? PatchCorner::U1V0 : PatchCorner::U1V1);
        outSample = EmittedSample{ .Position = surface.Corners[patch.Corner[static_cast<usize>(corner)]].Position };
        return GeometryStatus::Success;
    }
    if (isUBoundary == true || isVBoundary == true)
    {
        PatchSide side = PatchSide::U0;
        float64 s = 0.0;
        if (isUBoundary == true)
        {
            side = i == 0 ? PatchSide::U0 : PatchSide::U1;
            s = ComputeDyadic(j, levels.V);
        }
        else
        {
            side = j == 0 ? PatchSide::V0 : PatchSide::V1;
            s = ComputeDyadic(i, levels.U);
        }
        const SurfaceEdge& edge = surface.Edges[patch.Edge[static_cast<usize>(side)]];
        const usize incidence = FindIncidence(edge, patchIndex, side);
        // 1 - s is exact for a dyadic s, so both incidences name the same canonical parameter.
        const float64 t = incidence == 1 && edge.IsReversed == true ? 1.0 - s : s;
        return TryComputeEdgeSample(edge, t, outSample);
    }
    return TryComputeInteriorSample(patch, ComputeDyadic(i, levels.U), ComputeDyadic(j, levels.V), outSample);
}

// Certified control-net plus twist bound of the subpatch [u0, u1] x [v0, v1].
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the four cell bounds share one type.
[[nodiscard]] GeometryStatus TryBoundCellInterior(const PreparedPatch& patch,
                                                  Enclosure oneNinth,
                                                  float64 u0,
                                                  float64 u1,
                                                  float64 v0,
                                                  float64 v1,
                                                  float64& outBound) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    // Restrict every row to [v0, v1], then every resulting column to [u0, u1].
    StaticArray<CubicControlBoxes, PATCH_CONTROL_ORDER> rows;
    MathStatus status = MathStatus::Success;
    for (usize i = 0; i < PATCH_CONTROL_ORDER && status == MathStatus::Success; ++i)
    {
        status = internal::TryEncloseSubcurveControls(MakeRowBoxes(patch.Control, i), v0, v1, rows[i]);
    }
    StaticArray<CubicControlBoxes, PATCH_CONTROL_ORDER> sub;
    for (usize j = 0; j < PATCH_CONTROL_ORDER && status == MathStatus::Success; ++j)
    {
        const CubicControlBoxes column{{rows[0][j], rows[1][j], rows[2][j], rows[LAST_CONTROL][j]}};
        CubicControlBoxes restricted;
        status = internal::TryEncloseSubcurveControls(column, u0, u1, restricted);
        for (usize i = 0; i < PATCH_CONTROL_ORDER; ++i)
        {
            sub[i][j] = restricted[i];
        }
    }
    const EnclosureBox& c00 = sub[0][0];
    const EnclosureBox& c10 = sub[LAST_CONTROL][0];
    const EnclosureBox& c01 = sub[0][LAST_CONTROL];
    const EnclosureBox& c11 = sub[LAST_CONTROL][LAST_CONTROL];

    // Control i, j of the degree-elevated bilinear corner interpolant is
    // ((3-i)(3-j) C00 + i(3-j) C10 + (3-i) j C01 + i j C11) / 9. Corners coincide exactly and are skipped.
    float64 netBound = 0.0;
    for (usize i = 0; i < PATCH_CONTROL_ORDER && status == MathStatus::Success; ++i)
    {
        for (usize j = 0; j < PATCH_CONTROL_ORDER && status == MathStatus::Success; ++j)
        {
            const bool isCorner = (i == 0 || i == LAST_CONTROL) && (j == 0 || j == LAST_CONTROL);
            if (isCorner == true)
            {
                continue;
            }
            const StaticArray<float64, PATCH_CORNER_COUNT> weights{{
                static_cast<float64>((DEGREE - i) * (DEGREE - j)),
                static_cast<float64>(i * (DEGREE - j)),
                static_cast<float64>((DEGREE - i) * j),
                static_cast<float64>(i * j),
            }};
            const StaticArray<const EnclosureBox*, PATCH_CORNER_COUNT> corners{{&c00, &c10, &c01, &c11}};
            EnclosureBox weighted = MakePointBox(Vector3{});
            for (usize corner = 0; corner < PATCH_CORNER_COUNT && status == MathStatus::Success; ++corner)
            {
                EnclosureBox term;
                status = internal::TryScaleBox(*corners[corner], MakePointEnclosure(weights[corner]), term);
                if (status == MathStatus::Success)
                {
                    status = internal::TryAddBoxes(weighted, term, weighted);
                }
            }
            EnclosureBox bilinear;
            EnclosureBox difference;
            Enclosure length;
            if (status == MathStatus::Success)
            {
                status = internal::TryScaleBox(weighted, oneNinth, bilinear);
            }
            if (status == MathStatus::Success)
            {
                status = internal::TrySubtractBoxes(sub[i][j], bilinear, difference);
            }
            if (status == MathStatus::Success)
            {
                status = internal::TryEncloseBoxLength(difference, length);
            }
            if (status == MathStatus::Success)
            {
                netBound = math::Max(netBound, length.Upper);
            }
        }
    }

    // Twist vector P00 - P10 - P01 + P11 of the cell's exact corners.
    EnclosureBox twist;
    Enclosure twistLength;
    Enclosure twistBound;
    Enclosure total;
    if (status == MathStatus::Success)
    {
        status = internal::TrySubtractBoxes(c00, c10, twist);
    }
    if (status == MathStatus::Success)
    {
        status = internal::TrySubtractBoxes(twist, c01, twist);
    }
    if (status == MathStatus::Success)
    {
        status = internal::TryAddBoxes(twist, c11, twist);
    }
    if (status == MathStatus::Success)
    {
        status = internal::TryEncloseBoxLength(twist, twistLength);
    }
    if (status == MathStatus::Success)
    {
        status = math::TryMultiplyOutward(twistLength, MakePointEnclosure(TWIST_FACTOR), twistBound);
    }
    if (status == MathStatus::Success)
    {
        status = math::TryAddOutward(MakePointEnclosure(netBound), MakePointEnclosure(twistBound.Upper), total);
    }
    if (status != MathStatus::Success)
    {
        return ToGeometryStatus(status);
    }
    outBound = total.Upper;
    return GeometryStatus::Success;
}

// Largest angle between nonsingular analytic normals at the cell corners and centre (Estimated).
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the four cell bounds share one type.
[[nodiscard]] GeometryStatus TryEstimateCellNormalAngle(const PreparedPatch& patch,
                                                        const math::PatchNormalPolicy& policy,
                                                        float64 u0,
                                                        float64 u1,
                                                        float64 v0,
                                                        float64 v1,
                                                        float64& outAngle) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const float64 uMid = u0 + 0.5 * (u1 - u0);
    const float64 vMid = v0 + 0.5 * (v1 - v0);
    const StaticArray<math::PatchParameter, NORMAL_SAMPLE_COUNT> parameters{{
        math::PatchParameter{ .U = u0, .V = v0 },
        math::PatchParameter{ .U = u1, .V = v0 },
        math::PatchParameter{ .U = u0, .V = v1 },
        math::PatchParameter{ .U = u1, .V = v1 },
        math::PatchParameter{ .U = uMid, .V = vMid },
    }};
    StaticArray<Vector3d, NORMAL_SAMPLE_COUNT> normals{};
    usize normalCount = 0;
    for (const math::PatchParameter& parameter : parameters)
    {
        math::PatchSample sample;
        MathStatus status = math::TryEvaluatePatch(patch.Control, parameter, sample);
        Vector3 normal;
        if (status == MathStatus::Success)
        {
            status = math::TryPatchNormal(sample, policy, normal);
        }
        if (status == MathStatus::Degenerate || status == MathStatus::IllConditioned)
        {
            // Singular samples carry no direction; the degeneracy policy decides about them at emission.
            continue;
        }
        if (status != MathStatus::Success)
        {
            return ToGeometryStatus(status);
        }
        normals[normalCount] = internal::ToDouble(normal);
        ++normalCount;
    }
    float64 angle = 0.0;
    for (usize a = 0; a < normalCount; ++a)
    {
        for (usize b = a + 1; b < normalCount; ++b)
        {
            angle = math::Max(
                angle,
                math::Atan2(math::Length(math::Cross(normals[a], normals[b])), math::Dot(normals[a], normals[b])));
        }
    }
    outAngle = angle;
    return GeometryStatus::Success;
}

[[nodiscard]] bool IsNormalRequested(const SurfaceMeshProfile& profile) noexcept
{
    return profile.NormalAngleError > 0.0;
}

[[nodiscard]] bool IsWithinProfile(const SurfaceMeshProfile& profile, const PatchEvaluation& evaluation) noexcept
{
    return evaluation.PositionBound <= profile.PositionError &&
           (IsNormalRequested(profile) == false || evaluation.NormalAngle <= profile.NormalAngleError);
}

// Largest bound relative to its tolerance; refinement prefers the direction that lowers it more.
[[nodiscard]] float64 ComputeScore(const SurfaceMeshProfile& profile, const PatchEvaluation& evaluation) noexcept
{
    const float64 positionScore = evaluation.PositionBound / profile.PositionError;
    return IsNormalRequested(profile) == true
               ? math::Max(positionScore, evaluation.NormalAngle / profile.NormalAngleError)
               : positionScore;
}

[[nodiscard]] GeometryStatus FailBudget(const BuildContext& context,
                                        usize patchIndex,
                                        const char* property,
                                        const PatchEvaluation& evaluation) noexcept
{
    const bool isPositionMet = evaluation.PositionBound <= context.Profile.PositionError;
    WriteDiagnostic(
        context.DiagnosticOrNull,
        GeometryDiagnostic
        {
            .Code = GeometryStatus::BudgetExceeded,
            .SourceElement = patchIndex,
            .Property = property,
            .RequestedTolerance =
                isPositionMet == true ? context.Profile.NormalAngleError : context.Profile.PositionError,
            .AchievedBound = isPositionMet == true ? evaluation.NormalAngle : evaluation.PositionBound,
            .AchievedGuarantee = isPositionMet == true ? GuaranteeClass::Estimated : GuaranteeClass::Certified,
            .Domain = DomainKind::SurfaceParameter,
            .RangeStart = 0.0,
            .RangeEnd = 1.0,
        });
    return GeometryStatus::BudgetExceeded;
}

// Bounds of every cell of one patch grid; every term includes the emitted vertices' rounding error.
[[nodiscard]] GeometryStatus TryEvaluatePatchGrid(BuildContext& inoutContext,
                                                  usize patchIndex,
                                                  GridLevels levels,
                                                  PatchEvaluation& outEvaluation) noexcept
{
    const usize uCells = GetCellCount(levels.U);
    const usize vCells = GetCellCount(levels.V);
    const usize cellCount = uCells * vCells;
    usize testCount = 0;
    if (TryAdd(inoutContext.CellTestCount, cellCount, testCount) == false ||
        testCount > inoutContext.Profile.Limits.MaxCellTests)
    {
        return Fail(inoutContext.DiagnosticOrNull, GeometryStatus::BudgetExceeded, patchIndex, "Limits.MaxCellTests");
    }
    inoutContext.CellTestCount = testCount;
    const PreparedPatch& patch = inoutContext.Surface.Patches[patchIndex];
    PatchEvaluation evaluation;
    for (usize i = 0; i < uCells; ++i)
    {
        for (usize j = 0; j < vCells; ++j)
        {
            const float64 u0 = ComputeDyadic(i, levels.U);
            const float64 u1 = ComputeDyadic(i + 1, levels.U);
            const float64 v0 = ComputeDyadic(j, levels.V);
            const float64 v1 = ComputeDyadic(j + 1, levels.V);
            float64 interior = 0.0;
            GeometryStatus status = TryBoundCellInterior(patch, inoutContext.OneNinth, u0, u1, v0, v1, interior);
            // Rounded vertices move the triangle pair by at most the largest vertex error (convexity).
            float64 vertexError = 0.0;
            for (usize corner = 0; corner < PATCH_CORNER_COUNT && status == GeometryStatus::Success; ++corner)
            {
                EmittedSample sample;
                status = TryComputeGridSample(inoutContext.Surface,
                                              patchIndex,
                                              levels,
                                              i + (corner & 1U),
                                              j + (corner >> 1U),
                                              sample);
                vertexError = math::Max(vertexError, sample.ErrorBound);
            }
            Enclosure total;
            if (status == GeometryStatus::Success)
            {
                status = ToGeometryStatus(
                    math::TryAddOutward(MakePointEnclosure(interior), MakePointEnclosure(vertexError), total));
            }
            float64 normalAngle = 0.0;
            if (status == GeometryStatus::Success && IsNormalRequested(inoutContext.Profile) == true)
            {
                status = TryEstimateCellNormalAngle(patch, inoutContext.NormalPolicy, u0, u1, v0, v1, normalAngle);
            }
            if (status != GeometryStatus::Success)
            {
                return Fail(inoutContext.DiagnosticOrNull, status, patchIndex, "Patches.Control");
            }
            evaluation.PositionBound = math::Max(evaluation.PositionBound, total.Upper);
            evaluation.NormalAngle = math::Max(evaluation.NormalAngle, normalAngle);
        }
    }
    outEvaluation = evaluation;
    return GeometryStatus::Success;
}

// Refine one patch from its current levels until its grid meets the profile or MaxLevel stops it.
[[nodiscard]] GeometryStatus RefinePatch(BuildContext& inoutContext,
                                         usize patchIndex,
                                         GridLevels& inoutLevels,
                                         PatchEvaluation& inoutEvaluation) noexcept
{
    const SurfaceMeshProfile& profile = inoutContext.Profile;
    while (IsWithinProfile(profile, inoutEvaluation) == false)
    {
        const bool canRefineU = inoutLevels.U < profile.Limits.MaxLevel;
        const bool canRefineV = inoutLevels.V < profile.Limits.MaxLevel;
        if (canRefineU == false && canRefineV == false)
        {
            if (profile.IsCoarserMeshAccepted == true)
            {
                return GeometryStatus::Success;
            }
            return FailBudget(inoutContext, patchIndex, "Limits.MaxLevel", inoutEvaluation);
        }
        PatchEvaluation refinedU;
        PatchEvaluation refinedV;
        if (canRefineU == true)
        {
            const GeometryStatus status = TryEvaluatePatchGrid(inoutContext,
                                                               patchIndex,
                                                               GridLevels{ .U = inoutLevels.U + 1, .V = inoutLevels.V },
                                                               refinedU);
            if (status != GeometryStatus::Success)
            {
                return status;
            }
        }
        if (canRefineV == true)
        {
            const GeometryStatus status = TryEvaluatePatchGrid(inoutContext,
                                                               patchIndex,
                                                               GridLevels{ .U = inoutLevels.U, .V = inoutLevels.V + 1 },
                                                               refinedV);
            if (status != GeometryStatus::Success)
            {
                return status;
            }
        }
        // Deterministic choice: the clearly smaller normalized bound; otherwise the coarser direction, then u.
        bool shouldRefineU = canRefineU == true;
        if (canRefineU == true && canRefineV == true)
        {
            const float64 scoreU = ComputeScore(profile, refinedU);
            const float64 scoreV = ComputeScore(profile, refinedV);
            const float64 margin = SCORE_TIE_RELATIVE * math::Max(scoreU, scoreV);
            if (math::Abs(scoreU - scoreV) > margin)
            {
                shouldRefineU = scoreU < scoreV;
            }
            else
            {
                shouldRefineU = inoutLevels.U <= inoutLevels.V;
            }
        }
        if (shouldRefineU == true)
        {
            ++inoutLevels.U;
            inoutEvaluation = refinedU;
        }
        else
        {
            ++inoutLevels.V;
            inoutEvaluation = refinedV;
        }
    }
    return GeometryStatus::Success;
}

// Give both incidences of every shared edge the larger along-edge level; returns whether a level changed.
[[nodiscard]] bool
PropagateEdgeLevels(const SurfaceView& surface, Array<GridLevels>& inoutLevels, Array<bool>& inoutIsPending) noexcept
{
    bool hasChanged = false;
    bool isSettled = false;
    while (isSettled == false)
    {
        isSettled = true;
        for (usize edgeIndex = 0; edgeIndex < surface.Edges.GetSize(); ++edgeIndex)
        {
            const SurfaceEdge& edge = surface.Edges[edgeIndex];
            if (edge.IncidenceCount < MAX_EDGE_INCIDENCES)
            {
                continue;
            }
            const SurfaceEdgeIncidence& first = edge.Incidence[0];
            const SurfaceEdgeIncidence& second = edge.Incidence[1];
            const usize firstLevel = GetAlongLevel(inoutLevels[first.PatchIndex], first.Side);
            const usize secondLevel = GetAlongLevel(inoutLevels[second.PatchIndex], second.Side);
            if (firstLevel == secondLevel)
            {
                continue;
            }
            const usize level = math::Max(firstLevel, secondLevel);
            SetAlongLevel(inoutLevels[first.PatchIndex], first.Side, level);
            SetAlongLevel(inoutLevels[second.PatchIndex], second.Side, level);
            inoutIsPending[firstLevel < level ? first.PatchIndex : second.PatchIndex] = true;
            isSettled = false;
            hasChanged = true;
        }
    }
    return hasChanged;
}

[[nodiscard]] GeometryStatus ValidateProfile(const SurfaceView& surface,
                                             const SurfaceMeshProfile& profile,
                                             GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    if (surface.Patches.IsValid() == false || surface.Patches.IsEmpty() == true)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Surface");
    }
    if (math::IsFinite(profile.PositionError) == false || math::IsFinite(profile.NormalAngleError) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::NonFiniteInput, 0, "Profile");
    }
    const bool isKnownPrecision =
        profile.Precision == PrecisionProfile::Strict || profile.Precision == PrecisionProfile::Visual;
    const bool isKnownDegeneracy =
        profile.Degeneracy == SurfaceDegeneracyPolicy::Reject || profile.Degeneracy == SurfaceDegeneracyPolicy::Report;
    const bool isValid = isKnownPrecision == true && isKnownDegeneracy == true && profile.PositionError > 0.0 &&
                         profile.NormalAngleError >= 0.0 && profile.NormalAngleError < math::kHalfPi &&
                         profile.Limits.MaxLevel <= MAX_SURFACE_MESH_LEVEL &&
                         (profile.Precision == PrecisionProfile::Visual || profile.IsCoarserMeshAccepted == false);
    if (isValid == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Profile");
    }
    if (profile.Precision == PrecisionProfile::Strict && IsNormalRequested(profile) == true)
    {
        // No Certified derivative enclosure exists yet, so a normal bound would only be Estimated.
        WriteDiagnostic(outDiagnosticOrNull,
                        GeometryDiagnostic
                        {
                            .Code = GeometryStatus::UnsupportedPrecision,
                            .Property = "Profile.NormalAngleError",
                            .RequestedTolerance = profile.NormalAngleError,
                            .AchievedGuarantee = GuaranteeClass::Estimated,
                            .Domain = DomainKind::SurfaceParameter,
                        });
        return GeometryStatus::UnsupportedPrecision;
    }
    if (profile.PositionError < surface.PositionFloor)
    {
        WriteDiagnostic(outDiagnosticOrNull,
                        GeometryDiagnostic
                        {
                            .Code = GeometryStatus::UnsupportedPrecision,
                            .Property = "Profile.PositionError",
                            .RequestedTolerance = profile.PositionError,
                            .AchievedBound = surface.PositionFloor,
                            .AchievedGuarantee = GuaranteeClass::Exact,
                            .Domain = DomainKind::SurfaceParameter,
                        });
        return GeometryStatus::UnsupportedPrecision;
    }
    return GeometryStatus::Success;
}

// Level selection: refine each pending patch, propagate shared edge levels, repeat until consistent.
[[nodiscard]] GeometryStatus
SelectLevels(BuildContext& inoutContext, Array<GridLevels>& outLevels, Array<PatchEvaluation>& outEvaluations) noexcept
{
    const usize patchCount = inoutContext.Surface.Patches.GetSize();
    Array<bool> isPending;
    if (outLevels.TryResize(patchCount) == false || outEvaluations.TryResize(patchCount) == false ||
        isPending.TryResize(patchCount, true) == false)
    {
        return Fail(inoutContext.DiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Patches");
    }
    bool hasPending = true;
    while (hasPending == true)
    {
        for (usize patchIndex = 0; patchIndex < patchCount; ++patchIndex)
        {
            if (isPending[patchIndex] == false)
            {
                continue;
            }
            GeometryStatus status =
                TryEvaluatePatchGrid(inoutContext, patchIndex, outLevels[patchIndex], outEvaluations[patchIndex]);
            if (status == GeometryStatus::Success)
            {
                status = RefinePatch(inoutContext, patchIndex, outLevels[patchIndex], outEvaluations[patchIndex]);
            }
            if (status != GeometryStatus::Success)
            {
                return status;
            }
            isPending[patchIndex] = false;
        }
        hasPending = PropagateEdgeLevels(inoutContext.Surface, outLevels, isPending);
    }
    return GeometryStatus::Success;
}

// Output sizes of one level selection.
struct MeshCounts final
{
    usize Positions = 0;
    usize Vertices = 0;
    usize Triangles = 0;
    usize Bytes = 0;
};

// Count the outputs with overflow checks; false when a count is not representable.
[[nodiscard]] bool TryCountOutputs(const SurfaceView& surface,
                                   const Array<GridLevels>& levels,
                                   const Array<usize>& edgeLevels,
                                   MeshCounts& outCounts) noexcept
{
    MeshCounts counts{ .Positions = surface.Corners.GetSize() };
    bool isRepresentable = true;
    for (const usize level : edgeLevels)
    {
        isRepresentable =
            isRepresentable == true && TryAdd(counts.Positions, GetCellCount(level) - 1, counts.Positions);
    }
    for (const GridLevels& patchLevels : levels)
    {
        const usize uCells = GetCellCount(patchLevels.U);
        const usize vCells = GetCellCount(patchLevels.V);
        usize interior = 0;
        usize vertices = 0;
        usize cells = 0;
        usize triangles = 0;
        isRepresentable = isRepresentable == true && TryMultiply(uCells - 1, vCells - 1, interior) == true &&
                          TryMultiply(uCells + 1, vCells + 1, vertices) == true &&
                          TryMultiply(uCells, vCells, cells) == true &&
                          TryMultiply(cells, TRIANGLES_PER_CELL, triangles) == true &&
                          TryAdd(counts.Positions, interior, counts.Positions) == true &&
                          TryAdd(counts.Vertices, vertices, counts.Vertices) == true &&
                          TryAdd(counts.Triangles, triangles, counts.Triangles) == true;
    }
    usize positionBytes = 0;
    usize vertexBytes = 0;
    usize indexBytes = 0;
    usize indexCount = 0;
    const usize patchBytes = levels.GetSize() * sizeof(SurfaceMeshPatch);
    const usize edgeBytes = edgeLevels.GetSize() * sizeof(usize);
    isRepresentable =
        isRepresentable == true && TryMultiply(counts.Positions, sizeof(SurfaceMeshPosition), positionBytes) == true &&
        TryMultiply(counts.Vertices, sizeof(SurfaceMeshVertex), vertexBytes) == true &&
        TryMultiply(counts.Triangles, SURFACE_MESH_TRIANGLE_INDEX_COUNT, indexCount) == true &&
        TryMultiply(indexCount, sizeof(uint32), indexBytes) == true &&
        TryAdd(positionBytes, vertexBytes, counts.Bytes) && TryAdd(counts.Bytes, indexBytes, counts.Bytes) == true &&
        TryAdd(counts.Bytes, patchBytes, counts.Bytes) == true && TryAdd(counts.Bytes, edgeBytes, counts.Bytes) == true;
    if (isRepresentable == true)
    {
        outCounts = counts;
    }
    return isRepresentable;
}

// One grid point of one patch.
struct GridPoint final
{
    usize PatchIndex = 0;
    GridLevels Levels{};
    usize I = 0;
    usize J = 0;
};

// Arrays of a candidate mesh and the write cursors into them.
struct MeshOutput final
{
    Array<SurfaceMeshPosition>& Positions;
    Array<SurfaceMeshVertex>& Vertices;
    Array<uint32>& Indices;
    const Array<usize>& EdgeLevels;
    Array<usize>& EdgeFirstPosition;
    usize NextPosition = 0;
    usize NextVertex = 0;
    usize NextIndex = 0;
};

// Shared position of a grid point, given where its patch's interior samples start.
[[nodiscard]] usize GetPositionIndex(const SurfaceView& surface,
                                     const MeshOutput& output,
                                     GridPoint point,
                                     usize patchFirstPosition) noexcept
{
    const PreparedPatch& patch = surface.Patches[point.PatchIndex];
    const usize uCells = GetCellCount(point.Levels.U);
    const usize vCells = GetCellCount(point.Levels.V);
    const bool isUBoundary = point.I == 0 || point.I == uCells;
    const bool isVBoundary = point.J == 0 || point.J == vCells;
    if (isUBoundary == true && isVBoundary == true)
    {
        const PatchCorner corner = point.I == 0 ? (point.J == 0 ? PatchCorner::U0V0 : PatchCorner::U0V1)
                                                : (point.J == 0 ? PatchCorner::U1V0 : PatchCorner::U1V1);
        return patch.Corner[static_cast<usize>(corner)];
    }
    if (isUBoundary == true || isVBoundary == true)
    {
        const PatchSide side = isUBoundary == true ? (point.I == 0 ? PatchSide::U0 : PatchSide::U1)
                                                   : (point.J == 0 ? PatchSide::V0 : PatchSide::V1);
        const usize along = isUBoundary == true ? point.J : point.I;
        const usize edgeIndex = patch.Edge[static_cast<usize>(side)];
        const SurfaceEdge& edge = surface.Edges[edgeIndex];
        const usize edgeCells = GetCellCount(output.EdgeLevels[edgeIndex]);
        LUDUS_ASSERT(edgeCells == (isUBoundary == true ? vCells : uCells));
        const bool isMirrored = FindIncidence(edge, point.PatchIndex, side) == 1 && edge.IsReversed == true;
        const usize canonical = isMirrored == true ? edgeCells - along : along;
        return output.EdgeFirstPosition[edgeIndex] + canonical - 1;
    }
    return patchFirstPosition + (point.I - 1) * (vCells - 1) + (point.J - 1);
}

[[nodiscard]] GeometryStatus FailDegenerate(GeometryDiagnostic* outDiagnosticOrNull,
                                            GeometryStatus status,
                                            const char* property,
                                            SurfaceParameter parameter) noexcept
{
    WriteDiagnostic(outDiagnosticOrNull,
                    GeometryDiagnostic
                    {
                        .Code = status,
                        .SourceElement = parameter.PatchIndex,
                        .Property = property,
                        .Domain = DomainKind::SurfaceParameter,
                        .RangeStart = parameter.U,
                        .RangeEnd = parameter.V,
                    });
    return status;
}

// Corner records, then every edge's interior samples on its canonical curve, each emitted once.
[[nodiscard]] GeometryStatus TryEmitSharedPositions(const SurfaceView& surface,
                                                    MeshOutput& inoutOutput,
                                                    GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    for (usize cornerIndex = 0; cornerIndex < surface.Corners.GetSize(); ++cornerIndex)
    {
        inoutOutput.Positions[inoutOutput.NextPosition] = SurfaceMeshPosition
        {
            .Position = surface.Corners[cornerIndex].Position,
            .Kind = SurfaceSampleKind::Corner,
            .OwnerIndex = cornerIndex,
        };
        ++inoutOutput.NextPosition;
    }
    for (usize edgeIndex = 0; edgeIndex < surface.Edges.GetSize(); ++edgeIndex)
    {
        inoutOutput.EdgeFirstPosition[edgeIndex] = inoutOutput.NextPosition;
        const usize level = inoutOutput.EdgeLevels[edgeIndex];
        for (usize k = 1; k < GetCellCount(level); ++k)
        {
            const float64 t = ComputeDyadic(k, level);
            EmittedSample sample;
            const GeometryStatus status = TryComputeEdgeSample(surface.Edges[edgeIndex], t, sample);
            if (status != GeometryStatus::Success)
            {
                return Fail(outDiagnosticOrNull, status, edgeIndex, "Edges.Control");
            }
            inoutOutput.Positions[inoutOutput.NextPosition] = SurfaceMeshPosition
            {
                .Position = sample.Position,
                .Kind = SurfaceSampleKind::Edge,
                .OwnerIndex = edgeIndex,
                .EdgeParameter = t,
                .ErrorBound = sample.ErrorBound,
            };
            ++inoutOutput.NextPosition;
        }
    }
    return GeometryStatus::Success;
}

// One render vertex: the shared position plus this patch's normal and parameter.
[[nodiscard]] GeometryStatus TryEmitVertex(const BuildContext& context,
                                           GridPoint point,
                                           usize patchFirstPosition,
                                           MeshOutput& inoutOutput,
                                           SurfaceMeshReport& inoutReport) noexcept
{
    const SurfaceParameter parameter
    {
        .PatchIndex = point.PatchIndex,
        .U = ComputeDyadic(point.I, point.Levels.U),
        .V = ComputeDyadic(point.J, point.Levels.V),
    };
    SurfaceMeshVertex& vertex = inoutOutput.Vertices[inoutOutput.NextVertex];
    vertex.PositionIndex = GetPositionIndex(context.Surface, inoutOutput, point, patchFirstPosition);
    vertex.Position = inoutOutput.Positions[vertex.PositionIndex].Position;
    vertex.Parameter = parameter;
    math::PatchSample sample;
    MathStatus normalStatus = math::TryEvaluatePatch(context.Surface.Patches[point.PatchIndex].Control,
                                                     math::PatchParameter{ .U = parameter.U, .V = parameter.V },
                                                     sample);
    if (normalStatus == MathStatus::Success)
    {
        normalStatus = math::TryPatchNormal(sample, context.NormalPolicy, vertex.Normal);
    }
    vertex.NormalStatus = ToGeometryStatus(normalStatus);
    if (vertex.NormalStatus == GeometryStatus::Degenerate)
    {
        ++inoutReport.DegenerateNormalCount;
    }
    else if (vertex.NormalStatus == GeometryStatus::IllConditioned)
    {
        ++inoutReport.IllConditionedNormalCount;
    }
    const bool isSingular =
        vertex.NormalStatus == GeometryStatus::Degenerate || vertex.NormalStatus == GeometryStatus::IllConditioned;
    const bool shouldFail = vertex.NormalStatus != GeometryStatus::Success &&
                            (isSingular == false || context.Profile.Degeneracy == SurfaceDegeneracyPolicy::Reject);
    if (shouldFail == true)
    {
        return FailDegenerate(context.DiagnosticOrNull, vertex.NormalStatus, "Vertices.Normal", parameter);
    }
    ++inoutOutput.NextVertex;
    return GeometryStatus::Success;
}

// The two triangles of cell (i, j), split along (u0, v0)-(u1, v1) as in the twist bound, counterclockwise in (u, v).
[[nodiscard]] GeometryStatus TryEmitCell(const BuildContext& context,
                                         usize firstVertex,
                                         GridPoint cell,
                                         MeshOutput& inoutOutput,
                                         SurfaceMeshReport& inoutReport) noexcept
{
    const usize rowLength = GetCellCount(cell.Levels.V) + 1;
    const usize v00 = firstVertex + cell.I * rowLength + cell.J;
    const usize v10 = v00 + rowLength;
    const usize v01 = v00 + 1;
    const usize v11 = v10 + 1;
    const StaticArray<usize, TRIANGLES_PER_CELL * SURFACE_MESH_TRIANGLE_INDEX_COUNT> corners{
        {v00, v10, v11, v00, v11, v01}};
    for (usize triangle = 0; triangle < TRIANGLES_PER_CELL; ++triangle)
    {
        const usize base = triangle * SURFACE_MESH_TRIANGLE_INDEX_COUNT;
        const Vector3& a = inoutOutput.Vertices[corners[base]].Position;
        const Vector3& b = inoutOutput.Vertices[corners[base + 1]].Position;
        const Vector3& c = inoutOutput.Vertices[corners[base + 2]].Position;
        if (a == b || b == c || a == c)
        {
            if (context.Profile.Degeneracy == SurfaceDegeneracyPolicy::Reject)
            {
                return FailDegenerate(context.DiagnosticOrNull,
                                      GeometryStatus::Degenerate,
                                      "Indices",
                                      inoutOutput.Vertices[corners[base]].Parameter);
            }
            ++inoutReport.CollapsedTriangleCount;
        }
    }
    for (const usize corner : corners)
    {
        inoutOutput.Indices[inoutOutput.NextIndex] = static_cast<uint32>(corner);
        ++inoutOutput.NextIndex;
    }
    return GeometryStatus::Success;
}

// Interior positions, render vertices and triangles of one patch grid.
[[nodiscard]] GeometryStatus TryEmitPatch(const BuildContext& context,
                                          usize patchIndex,
                                          GridLevels levels,
                                          MeshOutput& inoutOutput,
                                          SurfaceMeshReport& inoutReport,
                                          SurfaceMeshPatch& outRecord) noexcept
{
    const PreparedPatch& patch = context.Surface.Patches[patchIndex];
    const usize uCells = GetCellCount(levels.U);
    const usize vCells = GetCellCount(levels.V);
    const usize patchFirstPosition = inoutOutput.NextPosition;
    for (usize i = 1; i < uCells; ++i)
    {
        for (usize j = 1; j < vCells; ++j)
        {
            EmittedSample sample;
            const GeometryStatus status =
                TryComputeInteriorSample(patch, ComputeDyadic(i, levels.U), ComputeDyadic(j, levels.V), sample);
            if (status != GeometryStatus::Success)
            {
                return Fail(context.DiagnosticOrNull, status, patchIndex, "Patches.Control");
            }
            inoutOutput.Positions[inoutOutput.NextPosition] = SurfaceMeshPosition
            {
                .Position = sample.Position,
                .Kind = SurfaceSampleKind::Interior,
                .OwnerIndex = patchIndex,
                .ErrorBound = sample.ErrorBound,
            };
            ++inoutOutput.NextPosition;
        }
    }
    const usize firstVertex = inoutOutput.NextVertex;
    for (usize i = 0; i <= uCells; ++i)
    {
        for (usize j = 0; j <= vCells; ++j)
        {
            const GeometryStatus status =
                TryEmitVertex(context,
                              GridPoint{ .PatchIndex = patchIndex, .Levels = levels, .I = i, .J = j },
                              patchFirstPosition,
                              inoutOutput,
                              inoutReport);
            if (status != GeometryStatus::Success)
            {
                return status;
            }
        }
    }
    const usize firstIndex = inoutOutput.NextIndex;
    for (usize i = 0; i < uCells; ++i)
    {
        for (usize j = 0; j < vCells; ++j)
        {
            const GeometryStatus status =
                TryEmitCell(context,
                            firstVertex,
                            GridPoint{ .PatchIndex = patchIndex, .Levels = levels, .I = i, .J = j },
                            inoutOutput,
                            inoutReport);
            if (status != GeometryStatus::Success)
            {
                return status;
            }
        }
    }
    outRecord = SurfaceMeshPatch
    {
        .LevelU = levels.U,
        .LevelV = levels.V,
        .FirstVertex = firstVertex,
        .VertexCount = inoutOutput.NextVertex - firstVertex,
        .FirstIndex = firstIndex,
        .IndexCount = inoutOutput.NextIndex - firstIndex,
    };
    return GeometryStatus::Success;
}

// Check the output counts against the profile's budgets before anything is allocated.
[[nodiscard]] GeometryStatus ValidateCounts(const SurfaceView& surface,
                                            const SurfaceMeshProfile& profile,
                                            const Array<GridLevels>& levels,
                                            const Array<usize>& edgeLevels,
                                            MeshCounts& outCounts,
                                            GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    MeshCounts counts;
    if (TryCountOutputs(surface, levels, edgeLevels, counts) == false || counts.Vertices > profile.Limits.MaxVertices)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::BudgetExceeded, 0, "Limits.MaxVertices");
    }
    if (counts.Triangles > profile.Limits.MaxTriangles)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::BudgetExceeded, 0, "Limits.MaxTriangles");
    }
    if (static_cast<uint64>(counts.Vertices) > UINT32_VERTEX_LIMIT)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::OutOfRange, 0, "Indices");
    }
    if (counts.Bytes > profile.Limits.MaxOutputBytes)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::BudgetExceeded, 0, "Limits.MaxOutputBytes");
    }
    outCounts = counts;
    return GeometryStatus::Success;
}
} // namespace

SurfaceMeshView SurfaceMesh::GetView() const noexcept
{
    return SurfaceMeshView
    {
        .Positions = BorrowedView<const SurfaceMeshPosition>(mPositions.GetData(), mPositions.GetSize()),
        .Vertices = BorrowedView<const SurfaceMeshVertex>(mVertices.GetData(), mVertices.GetSize()),
        .Indices = BorrowedView<const uint32>(mIndices.GetData(), mIndices.GetSize()),
        .Patches = BorrowedView<const SurfaceMeshPatch>(mPatches.GetData(), mPatches.GetSize()),
        .EdgeLevels = BorrowedView<const usize>(mEdgeLevels.GetData(), mEdgeLevels.GetSize()),
        .SurfaceRevision = mSurfaceRevision,
        .Report = mReport,
    };
}

bool SurfaceMesh::IsEmpty() const noexcept
{
    return mPatches.IsEmpty();
}

GeometryStatus TryBuildSurfaceMesh(const SurfaceView& surface,
                                   const SurfaceMeshProfile& profile,
                                   SurfaceMesh& outMesh,
                                   GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    GeometryStatus status = ValidateProfile(surface, profile, outDiagnosticOrNull);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    BuildContext context
    {
        .Surface = surface,
        .Profile = profile,
        .NormalPolicy =
            math::PatchNormalPolicy
            {
                .MinDerivativeLength = surface.Tolerance.NormalMinDerivativeLength,
                .MinSinAngle = surface.Tolerance.NormalMinSinAngle,
            },
        // 1/9 is not a float64; its neighbours enclose it.
        .OneNinth = Enclosure{ .Lower = math::NextFloat64Down(1.0 / 9.0), .Upper = math::NextFloat64Up(1.0 / 9.0) },
        .DiagnosticOrNull = outDiagnosticOrNull,
    };
    Array<GridLevels> levels;
    Array<PatchEvaluation> evaluations;
    status = SelectLevels(context, levels, evaluations);
    if (status != GeometryStatus::Success)
    {
        return status;
    }

    const usize patchCount = surface.Patches.GetSize();
    const usize edgeCount = surface.Edges.GetSize();
    SurfaceMesh candidate;
    if (candidate.mEdgeLevels.TryResize(edgeCount) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "EdgeLevels");
    }
    for (usize edgeIndex = 0; edgeIndex < edgeCount; ++edgeIndex)
    {
        const SurfaceEdgeIncidence& owner = surface.Edges[edgeIndex].Incidence[0];
        candidate.mEdgeLevels[edgeIndex] = GetAlongLevel(levels[owner.PatchIndex], owner.Side);
    }
    MeshCounts counts;
    status = ValidateCounts(surface, profile, levels, candidate.mEdgeLevels, counts, outDiagnosticOrNull);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    Array<usize> edgeFirstPosition;
    if (candidate.mPositions.TryResize(counts.Positions) == false ||
        candidate.mVertices.TryResize(counts.Vertices) == false ||
        candidate.mIndices.TryResize(counts.Triangles * SURFACE_MESH_TRIANGLE_INDEX_COUNT) == false ||
        candidate.mPatches.TryResize(patchCount) == false || edgeFirstPosition.TryResize(edgeCount) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Positions");
    }

    // Unique positions: corner records, then each edge's interior samples, then each patch's interior samples.
    MeshOutput output
    {
        .Positions = candidate.mPositions,
        .Vertices = candidate.mVertices,
        .Indices = candidate.mIndices,
        .EdgeLevels = candidate.mEdgeLevels,
        .EdgeFirstPosition = edgeFirstPosition,
    };
    status = TryEmitSharedPositions(surface, output, outDiagnosticOrNull);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    SurfaceMeshReport report
    {
        .RequestedPositionError = profile.PositionError,
        .RequestedNormalAngleError = profile.NormalAngleError,
        .HasNormalAngleEstimate = IsNormalRequested(profile),
        .CellTestCount = context.CellTestCount,
    };
    for (usize patchIndex = 0; patchIndex < patchCount; ++patchIndex)
    {
        SurfaceMeshPatch& record = candidate.mPatches[patchIndex];
        status = TryEmitPatch(context, patchIndex, levels[patchIndex], output, report, record);
        if (status != GeometryStatus::Success)
        {
            return status;
        }
        const PatchEvaluation& evaluation = evaluations[patchIndex];
        record.PositionBound = evaluation.PositionBound;
        record.NormalAngleEstimate = evaluation.NormalAngle;
        record.IsWithinTolerance = IsWithinProfile(profile, evaluation);
        report.PositionBound = math::Max(report.PositionBound, evaluation.PositionBound);
        report.NormalAngleEstimate = math::Max(report.NormalAngleEstimate, evaluation.NormalAngle);
        report.DeepestLevel = math::Max(report.DeepestLevel, math::Max(record.LevelU, record.LevelV));
    }
    LUDUS_ASSERT(output.NextPosition == counts.Positions && output.NextVertex == counts.Vertices &&
                 output.NextIndex == candidate.mIndices.GetSize());
    report.IsWithinPositionError = report.PositionBound <= profile.PositionError;

    candidate.mSurfaceRevision = surface.Revision;
    candidate.mReport = report;
    outMesh = Move(candidate);
    return GeometryStatus::Success;
}
} // namespace ludus::foundation::geometry
