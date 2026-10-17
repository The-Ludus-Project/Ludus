// Prepared surface validation, narrowing, topology and evaluation (see surface.hpp).

#include <ludus/foundation/geometry/surface.hpp>

#include "internal/control_box.hpp"
#include "internal/prepare_support.hpp"

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::foundation::geometry
{
using core::Array;
using core::StaticArray;
using math::Vector3;
using math::Vector3d;

namespace
{
using internal::ComputeRelativeDifference;
using internal::Fail;
using internal::GetMaxAbsComponent;
using internal::ToDouble;
using internal::TryNarrowVector;
using internal::WriteDiagnostic;

// Index of the last control along a patch direction or boundary curve.
constexpr usize LAST_CONTROL = PATCH_CONTROL_ORDER - 1;
// Marks an unused patch side or an unassigned corner record.
constexpr usize UNASSIGNED = ~usize{0};
// The start (s = 0) and end (s = 1) of a side.
constexpr StaticArray<bool, EDGE_CORNER_COUNT> EDGE_ENDS{{false, true}};

[[nodiscard]] bool IsKnownSide(PatchSide side) noexcept
{
    return side == PatchSide::U0 || side == PatchSide::U1 || side == PatchSide::V0 || side == PatchSide::V1;
}

[[nodiscard]] usize GetSlot(usize patchIndex, PatchSide side) noexcept
{
    return patchIndex * PATCH_SIDE_COUNT + static_cast<usize>(side);
}

[[nodiscard]] usize GetSlot(usize patchIndex, PatchCorner corner) noexcept
{
    return patchIndex * PATCH_CORNER_COUNT + static_cast<usize>(corner);
}

// Control k of a side, in increasing along-edge parameter s.
[[nodiscard]] const Vector3& GetSideControl(const math::BicubicBezierPatch& patch, PatchSide side, usize k) noexcept
{
    switch (side)
    {
        case PatchSide::U0:
            return patch.Control[0][k];
        case PatchSide::U1:
            return patch.Control[LAST_CONTROL][k];
        case PatchSide::V0:
            return patch.Control[k][0];
        case PatchSide::V1:
            break;
    }
    return patch.Control[k][LAST_CONTROL];
}

// The control one row inside the patch from side control k.
[[nodiscard]] const Vector3& GetInnerControl(const math::BicubicBezierPatch& patch, PatchSide side, usize k) noexcept
{
    switch (side)
    {
        case PatchSide::U0:
            return patch.Control[1][k];
        case PatchSide::U1:
            return patch.Control[LAST_CONTROL - 1][k];
        case PatchSide::V0:
            return patch.Control[k][1];
        case PatchSide::V1:
            break;
    }
    return patch.Control[k][LAST_CONTROL - 1];
}

// Corner at s = 0 or s = 1 of a side.
[[nodiscard]] PatchCorner GetSideCorner(PatchSide side, bool isEnd) noexcept
{
    switch (side)
    {
        case PatchSide::U0:
            return isEnd == true ? PatchCorner::U0V1 : PatchCorner::U0V0;
        case PatchSide::U1:
            return isEnd == true ? PatchCorner::U1V1 : PatchCorner::U1V0;
        case PatchSide::V0:
            return isEnd == true ? PatchCorner::U1V0 : PatchCorner::U0V0;
        case PatchSide::V1:
            break;
    }
    return isEnd == true ? PatchCorner::U1V1 : PatchCorner::U0V1;
}

[[nodiscard]] const Vector3& GetCornerControl(const math::BicubicBezierPatch& patch, PatchCorner corner) noexcept
{
    switch (corner)
    {
        case PatchCorner::U0V0:
            return patch.Control[0][0];
        case PatchCorner::U1V0:
            return patch.Control[LAST_CONTROL][0];
        case PatchCorner::U0V1:
            return patch.Control[0][LAST_CONTROL];
        case PatchCorner::U1V1:
            break;
    }
    return patch.Control[LAST_CONTROL][LAST_CONTROL];
}

// Whether the counterclockwise (u, v) boundary of a patch runs along the side in increasing s.
[[nodiscard]] bool IsCounterclockwiseIncreasing(PatchSide side) noexcept
{
    return side == PatchSide::V0 || side == PatchSide::U1;
}

// Outward cross-boundary derivative control k of a side, per normalized parameter.
[[nodiscard]] Vector3d ComputeOutwardDerivative(const math::BicubicBezierPatch& patch, PatchSide side, usize k) noexcept
{
    return 3.0 * (ToDouble(GetSideControl(patch, side, k)) - ToDouble(GetInnerControl(patch, side, k)));
}

// Whether (leftId, left) orders before (rightId, right); enum order breaks ties of equal IDs.
template <typename EnumType>
[[nodiscard]] bool IsIdentityBefore(uint64 leftId, EnumType left, uint64 rightId, EnumType right) noexcept
{
    return leftId != rightId ? leftId < rightId : static_cast<usize>(left) < static_cast<usize>(right);
}

// Root of a corner slot with path halving; roots are the smallest slot of their class.
[[nodiscard]] usize FindRoot(Array<usize>& inoutParent, usize slot) noexcept
{
    while (inoutParent[slot] != slot)
    {
        inoutParent[slot] = inoutParent[inoutParent[slot]];
        slot = inoutParent[slot];
    }
    return slot;
}

void JoinCorners(Array<usize>& inoutParent, usize left, usize right) noexcept
{
    const usize leftRoot = FindRoot(inoutParent, left);
    const usize rightRoot = FindRoot(inoutParent, right);
    if (leftRoot < rightRoot)
    {
        inoutParent[rightRoot] = leftRoot;
    }
    else
    {
        inoutParent[leftRoot] = rightRoot;
    }
}

[[nodiscard]] GeometryStatus ValidateSurfaceParameter(const SurfaceView& surface, SurfaceParameter parameter) noexcept
{
    if (math::IsFinite(parameter.U) == false || math::IsFinite(parameter.V) == false)
    {
        return GeometryStatus::NonFiniteInput;
    }
    const bool isInside = parameter.PatchIndex < surface.Patches.GetSize() && parameter.U >= 0.0 &&
                          parameter.U <= 1.0 && parameter.V >= 0.0 && parameter.V <= 1.0;
    return isInside == true ? GeometryStatus::Success : GeometryStatus::InvalidDomain;
}

// Largest distance between corresponding narrowed controls of two sides (Rounded), for diagnostics.
[[nodiscard]] float64 MeasureSideGap(const math::BicubicBezierPatch& first,
                                     PatchSide firstSide,
                                     const math::BicubicBezierPatch& second,
                                     PatchSide secondSide,
                                     bool isReversed) noexcept
{
    float64 gap = 0.0;
    for (usize k = 0; k < PATCH_CONTROL_ORDER; ++k)
    {
        const usize mapped = isReversed == true ? LAST_CONTROL - k : k;
        gap = math::Max(gap,
                        math::Length(ToDouble(GetSideControl(first, firstSide, k)) -
                                     ToDouble(GetSideControl(second, secondSide, mapped))));
    }
    return gap;
}

[[nodiscard]] bool AreSidesEqual(const math::BicubicBezierPatch& first,
                                 PatchSide firstSide,
                                 const math::BicubicBezierPatch& second,
                                 PatchSide secondSide,
                                 bool isReversed) noexcept
{
    for (usize k = 0; k < PATCH_CONTROL_ORDER; ++k)
    {
        const usize mapped = isReversed == true ? LAST_CONTROL - k : k;
        if ((GetSideControl(first, firstSide, k) == GetSideControl(second, secondSide, mapped)) == false)
        {
            return false;
        }
    }
    return true;
}

// Largest stored component and narrowing error of all patches.
struct NarrowingResult final
{
    float64 MaxAbsComponent = 0.0;
    float64 MaxNarrowingError = 0.0;
};

// Narrow every patch, recording the floor and narrowing error; returns the first failure.
[[nodiscard]] GeometryStatus NarrowPatches(const SurfaceSource& source,
                                           Array<PreparedPatch>& inoutPatches,
                                           NarrowingResult& outResult,
                                           GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    float64 maxAbsComponent = 0.0;
    float64 maxNarrowingError = 0.0;
    for (usize patchIndex = 0; patchIndex < source.Patches.GetSize(); ++patchIndex)
    {
        const SurfacePatchSource& patch = source.Patches[patchIndex];
        PreparedPatch& prepared = inoutPatches[patchIndex];
        prepared.Id = patch.Id;
        for (usize i = 0; i < PATCH_CONTROL_ORDER; ++i)
        {
            for (usize j = 0; j < PATCH_CONTROL_ORDER; ++j)
            {
                const Vector3d& control = patch.Control[i][j];
                if (math::IsFinite(control) == false)
                {
                    return Fail(outDiagnosticOrNull, GeometryStatus::NonFiniteInput, patchIndex, "Patches.Control");
                }
                // Subtract the origin in float64 before narrowing.
                const Vector3d relative = control - source.Origin;
                Vector3& narrowed = prepared.Control.Control[i][j];
                if (math::IsFinite(relative) == false || TryNarrowVector(relative, narrowed) == false)
                {
                    return Fail(outDiagnosticOrNull, GeometryStatus::OutOfRange, patchIndex, "Patches.Control");
                }
                maxAbsComponent = math::Max(maxAbsComponent, GetMaxAbsComponent(narrowed));
                maxNarrowingError = math::Max(maxNarrowingError, math::Length(relative - ToDouble(narrowed)));
            }
        }
    }
    outResult = NarrowingResult{ .MaxAbsComponent = maxAbsComponent, .MaxNarrowingError = maxNarrowingError };
    return GeometryStatus::Success;
}

[[nodiscard]] GeometryStatus ValidateUniqueIds(const SurfaceSource& source,
                                               Array<usize>& inoutOrder,
                                               GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    const usize patchCount = source.Patches.GetSize();
    if (inoutOrder.TryResize(patchCount) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Patches");
    }
    for (usize i = 0; i < patchCount; ++i)
    {
        inoutOrder[i] = i;
    }
    const BorrowedView<const SurfacePatchSource> patches = source.Patches;
    Sort(inoutOrder.GetData(), patchCount, [&patches](usize left, usize right) noexcept {
        return patches[left].Id != patches[right].Id ? patches[left].Id < patches[right].Id : left < right;
    });
    for (usize i = 1; i < patchCount; ++i)
    {
        if (patches[inoutOrder[i]].Id == patches[inoutOrder[i - 1]].Id)
        {
            return Fail(outDiagnosticOrNull, GeometryStatus::InvalidTopology, inoutOrder[i], "Patches.Id");
        }
    }
    return GeometryStatus::Success;
}

// Validate every adjacency record and record which one uses each patch side.
[[nodiscard]] GeometryStatus ValidateAdjacency(const SurfaceSource& source,
                                               const Array<PreparedPatch>& patches,
                                               Array<usize>& inoutSideAdjacency,
                                               GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    const usize patchCount = patches.GetSize();
    if (inoutSideAdjacency.TryResize(patchCount * PATCH_SIDE_COUNT, UNASSIGNED) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Adjacency");
    }
    for (usize index = 0; index < source.Adjacency.GetSize(); ++index)
    {
        const SurfaceAdjacencySource& adjacency = source.Adjacency[index];
        if (IsKnownSide(adjacency.FirstSide) == false || IsKnownSide(adjacency.SecondSide) == false)
        {
            return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, index, "Adjacency.Side");
        }
        if (adjacency.FirstPatch >= patchCount || adjacency.SecondPatch >= patchCount)
        {
            return Fail(outDiagnosticOrNull, GeometryStatus::InvalidTopology, index, "Adjacency.Patch");
        }
        const usize firstSlot = GetSlot(adjacency.FirstPatch, adjacency.FirstSide);
        const usize secondSlot = GetSlot(adjacency.SecondPatch, adjacency.SecondSide);
        if (firstSlot == secondSlot)
        {
            return Fail(outDiagnosticOrNull, GeometryStatus::InvalidTopology, index, "Adjacency.Side");
        }
        // A third incidence on a patch side would make the edge nonmanifold.
        if (inoutSideAdjacency[firstSlot] != UNASSIGNED || inoutSideAdjacency[secondSlot] != UNASSIGNED)
        {
            return Fail(outDiagnosticOrNull, GeometryStatus::InvalidTopology, index, "Adjacency.Incidence");
        }
        const math::BicubicBezierPatch& first = patches[adjacency.FirstPatch].Control;
        const math::BicubicBezierPatch& second = patches[adjacency.SecondPatch].Control;
        if (AreSidesEqual(first, adjacency.FirstSide, second, adjacency.SecondSide, adjacency.IsReversed) == false)
        {
            // Partial joins and curves that differ after the orientation mapping are never welded.
            WriteDiagnostic(
                outDiagnosticOrNull,
                GeometryDiagnostic
                {
                    .Code = GeometryStatus::InvalidTopology,
                    .SourceElement = index,
                    .Property = "Adjacency.Control",
                    .RequestedTolerance = 0.0,
                    .AchievedBound =
                        MeasureSideGap(first, adjacency.FirstSide, second, adjacency.SecondSide, adjacency.IsReversed),
                    .AchievedGuarantee = GuaranteeClass::Rounded,
                });
            return GeometryStatus::InvalidTopology;
        }
        inoutSideAdjacency[firstSlot] = index;
        inoutSideAdjacency[secondSlot] = index;
    }
    return GeometryStatus::Success;
}

[[nodiscard]] SurfaceEdge MakeEdge(const Array<PreparedPatch>& patches,
                                   SurfaceEdgeIncidence owner,
                                   const SurfaceEdgeIncidence* otherOrNull,
                                   bool isReversed,
                                   const ToleranceProfile& tolerance) noexcept
{
    const PreparedPatch& ownerPatch = patches[owner.PatchIndex];
    SurfaceEdge edge;
    edge.Id = SurfaceEdgeId{ .PatchId = ownerPatch.Id, .Side = owner.Side };
    edge.Incidence[0] = owner;
    for (usize k = 0; k < PATCH_CONTROL_ORDER; ++k)
    {
        edge.Control[k] = GetSideControl(ownerPatch.Control, owner.Side, k);
    }
    edge.Corner[0] = ownerPatch.Corner[static_cast<usize>(GetSideCorner(owner.Side, false))];
    edge.Corner[1] = ownerPatch.Corner[static_cast<usize>(GetSideCorner(owner.Side, true))];
    if (otherOrNull == nullptr)
    {
        return edge;
    }
    const SurfaceEdgeIncidence other = *otherOrNull;
    const PreparedPatch& otherPatch = patches[other.PatchIndex];
    edge.Incidence[1] = other;
    edge.IncidenceCount = MAX_EDGE_INCIDENCES;
    edge.IsReversed = isReversed;
    // Orientable joins traverse the shared curve in opposite directions along their counterclockwise boundaries.
    const bool isOtherIncreasing = IsCounterclockwiseIncreasing(other.Side) != isReversed;
    edge.IsOrientationConsistent = IsCounterclockwiseIncreasing(owner.Side) != isOtherIncreasing;
    float64 difference = 0.0;
    for (usize k = 0; k < PATCH_CONTROL_ORDER; ++k)
    {
        const usize mapped = isReversed == true ? LAST_CONTROL - k : k;
        // C1 across the edge: the outward derivative of one patch is the inward derivative of the other.
        difference =
            math::Max(difference,
                      ComputeRelativeDifference(ComputeOutwardDerivative(ownerPatch.Control, owner.Side, k),
                                                -ComputeOutwardDerivative(otherPatch.Control, other.Side, mapped)));
    }
    edge.CrossDerivativeDifference = difference;
    edge.IsC1 = difference <= tolerance.ContinuityDerivative;
    return edge;
}
} // namespace

SurfaceView PreparedSurface::GetView() const noexcept
{
    return SurfaceView
    {
        .Patches = BorrowedView<const PreparedPatch>(mPatches.GetData(), mPatches.GetSize()),
        .Edges = BorrowedView<const SurfaceEdge>(mEdges.GetData(), mEdges.GetSize()),
        .Corners = BorrowedView<const SurfaceCorner>(mCorners.GetData(), mCorners.GetSize()),
        .CornerIncidences =
            BorrowedView<const SurfaceCornerIncidence>(mCornerIncidences.GetData(), mCornerIncidences.GetSize()),
        .Origin = mOrigin,
        .Revision = mRevision,
        .PositionFloor = mPositionFloor,
        .EffectivePositionError = mEffectivePositionError,
        .Tolerance = mTolerance,
    };
}

bool PreparedSurface::IsEmpty() const noexcept
{
    return mPatches.IsEmpty();
}

GeometryStatus TryPrepareSurface(const SurfaceSource& source,
                                 const SurfacePreparation& preparation,
                                 PreparedSurface& outSurface,
                                 GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    const ToleranceProfile& tolerance = preparation.Tolerance;
    const GeometryStatus toleranceStatus = TryValidateToleranceProfile(tolerance);
    if (toleranceStatus != GeometryStatus::Success)
    {
        return Fail(outDiagnosticOrNull, toleranceStatus, 0, "Tolerance");
    }
    const usize patchCount = source.Patches.GetSize();
    if (source.Patches.IsValid() == false || patchCount == 0 || source.Adjacency.IsValid() == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Patches");
    }
    if (patchCount > preparation.Limits.MaxPatches)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::BudgetExceeded, 0, "Patches");
    }
    if (math::IsFinite(source.Origin) == false || math::IsFinite(source.AssetScale) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::NonFiniteInput, 0, "Origin");
    }
    if (source.AssetScale < 0.0)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "AssetScale");
    }
    usize sideCount = 0;
    usize patchBytes = 0;
    const bool areCountsRepresentable = TryMultiply(patchCount, PATCH_SIDE_COUNT, sideCount) == true &&
                                        TryMultiply(patchCount, sizeof(PreparedPatch), patchBytes) == true;
    if (areCountsRepresentable == false || patchBytes > preparation.Limits.MaxPreparedBytes)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::BudgetExceeded, 0, "Limits.MaxPreparedBytes");
    }
    // Two incidences per shared edge: more records than half the sides cannot all be distinct.
    if (source.Adjacency.GetSize() > sideCount / MAX_EDGE_INCIDENCES)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidTopology, 0, "Adjacency");
    }

    PreparedSurface candidate;
    if (candidate.mPatches.TryResize(patchCount) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Patches");
    }
    NarrowingResult narrowing;
    GeometryStatus status = NarrowPatches(source, candidate.mPatches, narrowing, outDiagnosticOrNull);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    const float64 positionFloor = ComputePositionFloor(narrowing.MaxAbsComponent);
    const float64 maxNarrowingError = narrowing.MaxNarrowingError;
    const float64 effectiveError =
        math::Max(tolerance.PositionError, tolerance.RelativePositionError * source.AssetScale);
    if (effectiveError < positionFloor || maxNarrowingError > effectiveError)
    {
        WriteDiagnostic(outDiagnosticOrNull,
                        GeometryDiagnostic
                        {
                            .Code = GeometryStatus::UnsupportedPrecision,
                            .Property = "Tolerance.PositionError",
                            .RequestedTolerance = effectiveError,
                            .AchievedBound = math::Max(positionFloor, maxNarrowingError),
                            .AchievedGuarantee = GuaranteeClass::Rounded,
                        });
        return GeometryStatus::UnsupportedPrecision;
    }

    Array<usize> idOrder;
    status = ValidateUniqueIds(source, idOrder, outDiagnosticOrNull);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    Array<usize> sideAdjacency;
    status = ValidateAdjacency(source, candidate.mPatches, sideAdjacency, outDiagnosticOrNull);
    if (status != GeometryStatus::Success)
    {
        return status;
    }

    // Join corner slots through every shared edge's endpoints.
    const usize cornerSlotCount = patchCount * PATCH_CORNER_COUNT;
    Array<usize> parent;
    Array<usize> slotCorner;
    if (parent.TryResize(cornerSlotCount) == false || slotCorner.TryResize(cornerSlotCount, UNASSIGNED) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Corners");
    }
    for (usize slot = 0; slot < cornerSlotCount; ++slot)
    {
        parent[slot] = slot;
    }
    for (usize index = 0; index < source.Adjacency.GetSize(); ++index)
    {
        const SurfaceAdjacencySource& adjacency = source.Adjacency[index];
        for (const bool isEnd : EDGE_ENDS)
        {
            const PatchCorner firstCorner = GetSideCorner(adjacency.FirstSide, isEnd);
            const PatchCorner secondCorner = GetSideCorner(adjacency.SecondSide, isEnd != adjacency.IsReversed);
            JoinCorners(parent,
                        GetSlot(adjacency.FirstPatch, firstCorner),
                        GetSlot(adjacency.SecondPatch, secondCorner));
        }
    }
    // Number corner records by their first incidence; roots are the smallest slot of each class.
    usize cornerCount = 0;
    for (usize slot = 0; slot < cornerSlotCount; ++slot)
    {
        const usize root = FindRoot(parent, slot);
        if (slotCorner[root] == UNASSIGNED)
        {
            slotCorner[root] = cornerCount;
            ++cornerCount;
        }
        slotCorner[slot] = slotCorner[root];
    }

    const usize edgeCount = sideCount - source.Adjacency.GetSize();
    usize preparedBytes = patchBytes;
    const usize edgeBytes = edgeCount * sizeof(SurfaceEdge);
    const usize cornerBytes = cornerCount * sizeof(SurfaceCorner);
    const usize incidenceBytes = cornerSlotCount * sizeof(SurfaceCornerIncidence);
    const bool isSizeRepresentable = TryAdd(preparedBytes, edgeBytes, preparedBytes) == true &&
                                     TryAdd(preparedBytes, cornerBytes, preparedBytes) == true &&
                                     TryAdd(preparedBytes, incidenceBytes, preparedBytes) == true;
    if (isSizeRepresentable == false || preparedBytes > preparation.Limits.MaxPreparedBytes)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::BudgetExceeded, 0, "Limits.MaxPreparedBytes");
    }
    if (candidate.mEdges.TryResize(edgeCount) == false || candidate.mCorners.TryResize(cornerCount) == false ||
        candidate.mCornerIncidences.TryResize(cornerSlotCount) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Edges");
    }

    // Corner records: incidence counts, then grouped incidences and the smallest stable identity.
    for (usize slot = 0; slot < cornerSlotCount; ++slot)
    {
        ++candidate.mCorners[slotCorner[slot]].IncidenceCount;
    }
    usize nextIncidence = 0;
    for (SurfaceCorner& corner : candidate.mCorners)
    {
        corner.FirstIncidence = nextIncidence;
        nextIncidence += corner.IncidenceCount;
        corner.IncidenceCount = 0;
    }
    for (usize slot = 0; slot < cornerSlotCount; ++slot)
    {
        const usize patchIndex = slot / PATCH_CORNER_COUNT;
        const PatchCorner patchCorner = static_cast<PatchCorner>(slot % PATCH_CORNER_COUNT);
        PreparedPatch& patch = candidate.mPatches[patchIndex];
        SurfaceCorner& corner = candidate.mCorners[slotCorner[slot]];
        if (corner.IncidenceCount == 0)
        {
            corner.Position = GetCornerControl(patch.Control, patchCorner);
            corner.Id = SurfaceCornerId{ .PatchId = patch.Id, .Corner = patchCorner };
        }
        else if (IsIdentityBefore(patch.Id, patchCorner, corner.Id.PatchId, corner.Id.Corner) == true)
        {
            corner.Id = SurfaceCornerId{ .PatchId = patch.Id, .Corner = patchCorner };
        }
        candidate.mCornerIncidences[corner.FirstIncidence + corner.IncidenceCount] =
            SurfaceCornerIncidence{ .PatchIndex = patchIndex, .Corner = patchCorner };
        ++corner.IncidenceCount;
        patch.Corner[static_cast<usize>(patchCorner)] = slotCorner[slot];
    }

    // Edges in the order of their owner's (patch index, side); the owner has the smallest stable identity.
    usize edgeIndex = 0;
    for (usize slot = 0; slot < sideCount; ++slot)
    {
        const SurfaceEdgeIncidence incidence
        {
            .PatchIndex = slot / PATCH_SIDE_COUNT,
            .Side = static_cast<PatchSide>(slot % PATCH_SIDE_COUNT),
        };
        const usize adjacencyIndex = sideAdjacency[slot];
        if (adjacencyIndex == UNASSIGNED)
        {
            candidate.mEdges[edgeIndex] = MakeEdge(candidate.mPatches, incidence, nullptr, false, tolerance);
            candidate.mPatches[incidence.PatchIndex].Edge[static_cast<usize>(incidence.Side)] = edgeIndex;
            ++edgeIndex;
            continue;
        }
        const SurfaceAdjacencySource& adjacency = source.Adjacency[adjacencyIndex];
        const bool isFirst = adjacency.FirstPatch == incidence.PatchIndex && adjacency.FirstSide == incidence.Side;
        SurfaceEdgeIncidence other{ .PatchIndex = adjacency.FirstPatch, .Side = adjacency.FirstSide };
        if (isFirst == true)
        {
            other = SurfaceEdgeIncidence{ .PatchIndex = adjacency.SecondPatch, .Side = adjacency.SecondSide };
        }
        const uint64 ownId = candidate.mPatches[incidence.PatchIndex].Id;
        const uint64 otherId = candidate.mPatches[other.PatchIndex].Id;
        if (IsIdentityBefore(ownId, incidence.Side, otherId, other.Side) == false)
        {
            continue;
        }
        candidate.mEdges[edgeIndex] = MakeEdge(candidate.mPatches, incidence, &other, adjacency.IsReversed, tolerance);
        candidate.mPatches[incidence.PatchIndex].Edge[static_cast<usize>(incidence.Side)] = edgeIndex;
        candidate.mPatches[other.PatchIndex].Edge[static_cast<usize>(other.Side)] = edgeIndex;
        ++edgeIndex;
    }
    LUDUS_ASSERT(edgeIndex == edgeCount);

    candidate.mOrigin = source.Origin;
    candidate.mRevision = source.Revision;
    candidate.mPositionFloor = positionFloor;
    candidate.mEffectivePositionError = effectiveError;
    candidate.mTolerance = tolerance;
    outSurface = Move(candidate);
    return GeometryStatus::Success;
}

GeometryStatus TryEvaluateSurface(const SurfaceView& surface, SurfaceParameter parameter, Vector3& outPosition) noexcept
{
    const GeometryStatus status = ValidateSurfaceParameter(surface, parameter);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    return internal::ToGeometryStatus(math::TryEvaluatePatch(surface.Patches[parameter.PatchIndex].Control,
                                                             math::PatchParameter{ .U = parameter.U, .V = parameter.V },
                                                             outPosition));
}

GeometryStatus
TryEvaluateSurface(const SurfaceView& surface, SurfaceParameter parameter, math::PatchSample& outSample) noexcept
{
    const GeometryStatus status = ValidateSurfaceParameter(surface, parameter);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    return internal::ToGeometryStatus(math::TryEvaluatePatch(surface.Patches[parameter.PatchIndex].Control,
                                                             math::PatchParameter{ .U = parameter.U, .V = parameter.V },
                                                             outSample));
}

GeometryStatus
TryEvaluateSurfaceNormal(const SurfaceView& surface, SurfaceParameter parameter, Vector3& outNormal) noexcept
{
    math::PatchSample sample;
    const GeometryStatus status = TryEvaluateSurface(surface, parameter, sample);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    const math::PatchNormalPolicy policy
    {
        .MinDerivativeLength = surface.Tolerance.NormalMinDerivativeLength,
        .MinSinAngle = surface.Tolerance.NormalMinSinAngle,
    };
    return internal::ToGeometryStatus(math::TryPatchNormal(sample, policy, outNormal));
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): an edge index and an incidence index are both indices.
GeometryStatus TryMapEdgeParameter(const SurfaceView& surface,
                                   usize edgeIndex,
                                   usize incidence,
                                   float64 edgeParameter,
                                   SurfaceParameter& outParameter) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (math::IsFinite(edgeParameter) == false)
    {
        return GeometryStatus::NonFiniteInput;
    }
    if (edgeIndex >= surface.Edges.GetSize() || edgeParameter < 0.0 || edgeParameter > 1.0)
    {
        return GeometryStatus::InvalidDomain;
    }
    const SurfaceEdge& edge = surface.Edges[edgeIndex];
    if (incidence >= edge.IncidenceCount)
    {
        return GeometryStatus::InvalidDomain;
    }
    const SurfaceEdgeIncidence& side = edge.Incidence[incidence];
    const float64 s = incidence == 1 && edge.IsReversed == true ? 1.0 - edgeParameter : edgeParameter;
    SurfaceParameter parameter{ .PatchIndex = side.PatchIndex };
    switch (side.Side)
    {
        case PatchSide::U0:
            parameter.V = s;
            break;
        case PatchSide::U1:
            parameter.U = 1.0;
            parameter.V = s;
            break;
        case PatchSide::V0:
            parameter.U = s;
            break;
        case PatchSide::V1:
            parameter.U = s;
            parameter.V = 1.0;
            break;
    }
    outParameter = parameter;
    return GeometryStatus::Success;
}
} // namespace ludus::foundation::geometry
