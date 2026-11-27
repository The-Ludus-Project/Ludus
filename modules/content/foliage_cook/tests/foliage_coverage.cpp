#include "foliage_coverage.h"

// Offline coverage controls for the placement gap benchmark; see foliage_coverage.h for the references.

namespace ludus::content::foliage_cook::coverage
{
namespace math = foundation::math;

namespace
{
/// Marks an empty cell of the dart throwing sample grid.
constexpr uint32 NO_SAMPLE = ~uint32{0};
/// Scope of every dart throwing draw; the event is the dart ordinal.
constexpr uint64 DART_SCOPE = 1;
/// Dimension that selects a cell by area.
constexpr uint16 CELL_DIMENSION = 0;
/// Dimension of a dart's X offset in its cell.
constexpr uint16 OFFSET_X_DIMENSION = 1;
/// Dimension of a dart's Z offset in its cell.
constexpr uint16 OFFSET_Z_DIMENSION = 2;
/// Children of a split quadtree cell.
constexpr usize CHILD_COUNT = 4;
/// Squared-distance bin thresholds in sixteenths of the squared minimum distance: 1/4, 1, 25/16, 9/4, 4 and 9,
/// that is distances of 1/2, 1, 5/4, 3/2, 2 and 3 minimum distances.
constexpr foundation::StaticArray<int64, DISTANCE_BIN_COUNT - 1> BIN_SIXTEENTHS = {4, 16, 25, 36, 64, 144};
/// Sixteenths in one squared minimum distance.
constexpr int64 SIXTEENTHS = 16;
/// Search reach of nearest-neighbour and empty-distance histograms, in minimum distances.
constexpr int64 HISTOGRAM_REACH = 3;

int64 FloorModulo(int64 value, int64 divisor) noexcept
{
    return value - FloorDivide(value, divisor) * divisor;
}

int64 GetMinimum(int64 left, int64 right) noexcept
{
    return left < right ? left : right;
}

int64 GetMaximum(int64 left, int64 right) noexcept
{
    return left > right ? left : right;
}

bool IsInside(const FoliageLatticeRectangle& rectangle, int64 x, int64 z) noexcept
{
    return x >= rectangle.MinimumX && x < rectangle.MaximumX && z >= rectangle.MinimumZ && z < rectangle.MaximumZ;
}

/// One active quadtree cell: its minimum root.
struct DartCell final
{
    int64 X = 0;
    int64 Z = 0;
};

/// State of one hierarchical dart throwing fill.
class DartThrower final
{
public:
    DartThrower(const EligibleRegion& region,
                int64 minimumDistance,
                math::RandomKey key,
                foundation::Array<CoveragePoint>& inoutPoints,
                DartThrowingStatistics& inoutStatistics) noexcept
        : mRegion(region), mMinimumDistance(minimumDistance), mConflict(GetConflictDistanceSquared(minimumDistance)),
          mKey(key), mPoints(inoutPoints), mStatistics(inoutStatistics)
    {
    }

    [[nodiscard]] bool TryInitialize() noexcept
    {
        // The largest power-of-two cell whose roots all lie within one minimum distance of any root in it, so
        // an accepted dart covers its cell and a grid cell holds at most one sample.
        int64 size = 1;
        while (2 * (2 * size - 1) * (2 * size - 1) < mConflict && mRegion.CellSize % (2 * size) == 0)
        {
            size *= 2;
        }
        const int64 width = mRegion.Extent.MaximumX - mRegion.Extent.MinimumX;
        const int64 height = mRegion.Extent.MaximumZ - mRegion.Extent.MinimumZ;
        if (width <= 0 || height <= 0 || width % size != 0 || height % size != 0)
        {
            return false;
        }
        usize levels = 1;
        for (int64 edge = size; edge > 1; edge /= 2)
        {
            ++levels;
        }
        if (levels > MAX_DART_LEVELS)
        {
            return false;
        }
        mStatistics.BaseCellSize = size;
        mStatistics.SmallestCellSize = size;
        mStatistics.Levels = levels;
        mGridColumns = width / size;
        mGridRows = height / size;
        if (mGrid.TryResize(static_cast<usize>(mGridColumns * mGridRows), NO_SAMPLE) == false)
        {
            return false;
        }
        for (int64 row = 0; row < mGridRows; ++row)
        {
            for (int64 column = 0; column < mGridColumns; ++column)
            {
                const DartCell cell =
                {
                    .X = mRegion.Extent.MinimumX + column * size,
                    .Z = mRegion.Extent.MinimumZ + row * size,
                };
                // A base cell lies in one terrain cell, so its minimum root decides the keep-out for all of it.
                if (mRegion.IsEligible(cell.X, cell.Z) == true && tryActivate(0, cell) == false)
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool TryFill(uint64 maximumDarts) noexcept
    {
        while (true)
        {
            updatePeak();
            uint64 area = 0;
            for (usize level = 0; level < mStatistics.Levels; ++level)
            {
                area += static_cast<uint64>(mCells[level].GetSize()) * getCellArea(level);
            }
            if (area == 0)
            {
                return true;
            }
            if (mStatistics.Darts >= maximumDarts || area > ~uint32{0})
            {
                return false;
            }
            const uint32 event = static_cast<uint32>(mStatistics.Darts);
            uint32 choice = 0;
            if (math::TrySampleBounded(mKey, makeAddress(event, CELL_DIMENSION), static_cast<uint32>(area), choice) !=
                math::MathStatus::Success)
            {
                return false;
            }
            usize level = 0;
            uint64 remaining = choice;
            while (remaining >= static_cast<uint64>(mCells[level].GetSize()) * getCellArea(level))
            {
                remaining -= static_cast<uint64>(mCells[level].GetSize()) * getCellArea(level);
                ++level;
            }
            const usize index = static_cast<usize>(remaining / getCellArea(level));
            foundation::Array<DartCell>& cells = mCells[level];
            const DartCell cell = cells[index];
            cells.RemoveAtSwap(index);
            if (tryThrow(level, cell, event) == false)
            {
                return false;
            }
        }
    }

private:
    [[nodiscard]] int64 getCellSize(usize level) const noexcept
    {
        return mStatistics.BaseCellSize >> level;
    }

    [[nodiscard]] uint64 getCellArea(usize level) const noexcept
    {
        const uint64 size = static_cast<uint64>(getCellSize(level));
        return size * size;
    }

    [[nodiscard]] static math::RandomAddress makeAddress(uint32 event, uint16 dimension) noexcept
    {
        return { .Scope = DART_SCOPE, .Event = event, .Dimension = dimension };
    }

    [[nodiscard]] bool tryActivate(usize level, const DartCell& cell) noexcept
    {
        if (mCells[level].TryAdd(cell) == false)
        {
            return false;
        }
        mStatistics.ActiveCells[level] += 1;
        const int64 size = getCellSize(level);
        mStatistics.SmallestCellSize = GetMinimum(mStatistics.SmallestCellSize, size);
        return true;
    }

    /// Visit samples whose grid cells overlap [minimum, maximum] on each axis; the visitor returns true to stop.
    template <typename Visitor>
    [[nodiscard]] bool visitSamples(const FoliageLatticeRectangle& box, const Visitor& visitor) const noexcept
    {
        const int64 size = mStatistics.BaseCellSize;
        const int64 firstColumn = GetMaximum(FloorDivide(box.MinimumX - mRegion.Extent.MinimumX, size), 0);
        const int64 lastColumn =
            GetMinimum(FloorDivide(box.MaximumX - mRegion.Extent.MinimumX, size), mGridColumns - 1);
        const int64 firstRow = GetMaximum(FloorDivide(box.MinimumZ - mRegion.Extent.MinimumZ, size), 0);
        const int64 lastRow = GetMinimum(FloorDivide(box.MaximumZ - mRegion.Extent.MinimumZ, size), mGridRows - 1);
        for (int64 row = firstRow; row <= lastRow; ++row)
        {
            for (int64 column = firstColumn; column <= lastColumn; ++column)
            {
                const uint32 sample = mGrid[static_cast<usize>(row * mGridColumns + column)];
                if (sample != NO_SAMPLE && visitor(mPoints[sample]) == true)
                {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] bool isConflicting(int64 x, int64 z) const noexcept
    {
        const int64 reach = mMinimumDistance;
        const FoliageLatticeRectangle box =
        {
            .MinimumX = x - reach,
            .MinimumZ = z - reach,
            .MaximumX = x + reach,
            .MaximumZ = z + reach,
        };
        return visitSamples(box, [&](const CoveragePoint& sample) noexcept {
            const int64 differenceX = sample.X - x;
            const int64 differenceZ = sample.Z - z;
            return differenceX * differenceX + differenceZ * differenceZ < mConflict;
        });
    }

    /// Whether one sample's conflict disk holds every root of a cell: the disk is convex, so its corners suffice.
    [[nodiscard]] bool isCoveredByOneDisk(const DartCell& cell, int64 size) const noexcept
    {
        const int64 last = size - 1;
        const FoliageLatticeRectangle box =
        {
            .MinimumX = cell.X - mMinimumDistance,
            .MinimumZ = cell.Z - mMinimumDistance,
            .MaximumX = cell.X + last + mMinimumDistance,
            .MaximumZ = cell.Z + last + mMinimumDistance,
        };
        return visitSamples(box, [&](const CoveragePoint& sample) noexcept {
            for (int64 cornerZ = cell.Z; cornerZ <= cell.Z + last; cornerZ += GetMaximum(last, 1))
            {
                for (int64 cornerX = cell.X; cornerX <= cell.X + last; cornerX += GetMaximum(last, 1))
                {
                    const int64 differenceX = sample.X - cornerX;
                    const int64 differenceZ = sample.Z - cornerZ;
                    if (differenceX * differenceX + differenceZ * differenceZ >= mConflict)
                    {
                        return false;
                    }
                }
            }
            return true;
        });
    }

    [[nodiscard]] bool tryThrow(usize level, const DartCell& cell, uint32 event) noexcept
    {
        mStatistics.Darts += 1;
        const int64 size = getCellSize(level);
        uint32 offsetX = 0;
        uint32 offsetZ = 0;
        if (size > 1 &&
            (math::TrySampleBounded(mKey, makeAddress(event, OFFSET_X_DIMENSION), static_cast<uint32>(size), offsetX) !=
                 math::MathStatus::Success ||
             math::TrySampleBounded(mKey, makeAddress(event, OFFSET_Z_DIMENSION), static_cast<uint32>(size), offsetZ) !=
                 math::MathStatus::Success))
        {
            return false;
        }
        const int64 x = cell.X + offsetX;
        const int64 z = cell.Z + offsetZ;
        if (isConflicting(x, z) == false)
        {
            // The accepted sample covers its whole cell.
            const usize gridIndex =
                static_cast<usize>(FloorDivide(z - mRegion.Extent.MinimumZ, mStatistics.BaseCellSize) * mGridColumns +
                                   FloorDivide(x - mRegion.Extent.MinimumX, mStatistics.BaseCellSize));
            if (mGrid[gridIndex] != NO_SAMPLE || mPoints.GetSize() >= NO_SAMPLE ||
                mPoints.TryAdd({ .X = x, .Z = z }) == false)
            {
                return false;
            }
            mGrid[gridIndex] = static_cast<uint32>(mPoints.GetSize() - 1);
            mStatistics.Accepted += 1;
            return true;
        }
        if (size == 1)
        {
            // A single-root cell is decided exactly: its root conflicts.
            mStatistics.CoveredCells += 1;
            return true;
        }
        const int64 half = size / 2;
        const foundation::StaticArray<DartCell, CHILD_COUNT> children = {
            DartCell{ .X = cell.X, .Z = cell.Z },
            DartCell{ .X = cell.X + half, .Z = cell.Z },
            DartCell{ .X = cell.X, .Z = cell.Z + half },
            DartCell{ .X = cell.X + half, .Z = cell.Z + half },
        };
        for (const DartCell& child : children)
        {
            if (isCoveredByOneDisk(child, half) == true)
            {
                mStatistics.CoveredCells += 1;
            }
            else if (tryActivate(level + 1, child) == false)
            {
                return false;
            }
        }
        return true;
    }

    void updatePeak() noexcept
    {
        usize active = 0;
        usize bytes = mGrid.GetCapacity() * sizeof(uint32) + mPoints.GetCapacity() * sizeof(CoveragePoint);
        for (usize level = 0; level < mStatistics.Levels; ++level)
        {
            active += mCells[level].GetSize();
            bytes += mCells[level].GetCapacity() * sizeof(DartCell);
        }
        mStatistics.PeakActiveCells = active > mStatistics.PeakActiveCells ? active : mStatistics.PeakActiveCells;
        mStatistics.PeakBytes = bytes > mStatistics.PeakBytes ? bytes : mStatistics.PeakBytes;
    }

    const EligibleRegion& mRegion;
    int64 mMinimumDistance = 0;
    int64 mConflict = 0;
    math::RandomKey mKey;
    foundation::Array<CoveragePoint>& mPoints;
    DartThrowingStatistics& mStatistics;
    foundation::StaticArray<foundation::Array<DartCell>, MAX_DART_LEVELS> mCells{};
    foundation::Array<uint32> mGrid;
    int64 mGridColumns = 0;
    int64 mGridRows = 0;
};

void RecordDistance(foundation::StaticArray<uint64, DISTANCE_BIN_COUNT>& inoutHistogram,
                    const DistanceBins& bins,
                    bool isFound,
                    int64 distanceSquared) noexcept
{
    const usize bin = isFound == true ? bins.GetBin(distanceSquared) : DISTANCE_BIN_COUNT - 1;
    inoutHistogram[bin] += 1;
}

/// Axis classification bits: within the seam reach of a page boundary, and inside the strip's range on the axis.
constexpr uint8 AXIS_SEAM = 1;
constexpr uint8 AXIS_STRIP = 2;

/// Classify one coordinate on the X axis (isX) or the Z axis.
uint8 ClassifyAxis(const ZoneLayout& layout, int64 value, bool isX) noexcept
{
    const int64 modulo = FloorModulo(value, layout.PageSize);
    // A root at offset m from the page minimum is m from one boundary and PageSize - m from the next.
    const int64 distance = GetMinimum(modulo, layout.PageSize - modulo);
    const int64 minimum = isX == true ? layout.NarrowStrip.MinimumX : layout.NarrowStrip.MinimumZ;
    const int64 maximum = isX == true ? layout.NarrowStrip.MaximumX : layout.NarrowStrip.MaximumZ;
    uint8 kind = 0;
    if (distance < layout.SeamReach)
    {
        kind |= AXIS_SEAM;
    }
    if (value >= minimum && value < maximum)
    {
        kind |= AXIS_STRIP;
    }
    return kind;
}

/// The zone of a root from its row and column classifications; the narrow strip takes precedence.
CoverageZone CombineAxes(uint8 rowKind, uint8 columnKind) noexcept
{
    if ((rowKind & AXIS_STRIP) != 0 && (columnKind & AXIS_STRIP) != 0)
    {
        return CoverageZone::NarrowStrip;
    }
    if ((rowKind & AXIS_SEAM) != 0 && (columnKind & AXIS_SEAM) != 0)
    {
        return CoverageZone::Junction;
    }
    return ((rowKind | columnKind) & AXIS_SEAM) != 0 ? CoverageZone::Seam : CoverageZone::Interior;
}

int64 GetZoneStride(const ProbeSpacing& spacing, CoverageZone zone) noexcept
{
    switch (zone)
    {
        case CoverageZone::Interior:
            return spacing.Interior;
        case CoverageZone::Seam:
            return spacing.Seam;
        case CoverageZone::Junction:
            return spacing.Junction;
        case CoverageZone::NarrowStrip:
            return spacing.NarrowStrip;
    }
    return spacing.Interior;
}
} // namespace

bool EligibleRegion::IsEligible(int64 x, int64 z) const noexcept
{
    if (IsInside(Extent, x, z) == false)
    {
        return false;
    }
    if (Holes.IsEmpty() == true)
    {
        return true;
    }
    const int64 column = (x - Extent.MinimumX) / CellSize;
    const int64 row = (z - Extent.MinimumZ) / CellSize;
    return Holes[static_cast<usize>(row * CellColumns + column)] == 0;
}

bool PointGrid::TryBuild(foundation::BorrowedView<const CoveragePoint> points,
                         const FoliageLatticeRectangle& extent,
                         int64 cellSize) noexcept
{
    mPoints = points;
    mExtent = extent;
    mCellSize = cellSize;
    mColumns = (extent.MaximumX - extent.MinimumX + cellSize - 1) / cellSize;
    mRows = (extent.MaximumZ - extent.MinimumZ + cellSize - 1) / cellSize;
    const usize cells = static_cast<usize>(mColumns * mRows);
    foundation::Array<uint32> cursor;
    if (points.GetSize() >= ~uint32{0} || mFirst.TryResize(cells + 1, 0) == false ||
        mIndices.TryResize(points.GetSize(), 0) == false || cursor.TryResize(cells, 0) == false)
    {
        return false;
    }
    for (usize i = 0; i < points.GetSize(); ++i)
    {
        if (IsInside(extent, points[i].X, points[i].Z) == false)
        {
            return false;
        }
        const usize cell = static_cast<usize>(((points[i].Z - extent.MinimumZ) / cellSize) * mColumns +
                                              (points[i].X - extent.MinimumX) / cellSize);
        mFirst[cell + 1] += 1;
    }
    for (usize cell = 0; cell < cells; ++cell)
    {
        mFirst[cell + 1] += mFirst[cell];
        cursor[cell] = mFirst[cell];
    }
    for (usize i = 0; i < points.GetSize(); ++i)
    {
        const usize cell = static_cast<usize>(((points[i].Z - extent.MinimumZ) / cellSize) * mColumns +
                                              (points[i].X - extent.MinimumX) / cellSize);
        mIndices[cursor[cell]] = static_cast<uint32>(i);
        cursor[cell] += 1;
    }
    return true;
}

bool PointGrid::TryFindNearest(const NearestQuery& query, int64& outDistanceSquared) const noexcept
{
    bool isFound = false;
    int64 nearest = 0;
    static_cast<void>(VisitNear(query, [&](usize index) noexcept {
        const int64 differenceX = mPoints[index].X - query.Position.X;
        const int64 differenceZ = mPoints[index].Z - query.Position.Z;
        const int64 distanceSquared = differenceX * differenceX + differenceZ * differenceZ;
        if (isFound == false || distanceSquared < nearest)
        {
            nearest = distanceSquared;
            isFound = true;
        }
        return false;
    }));
    if (isFound == true)
    {
        outDistanceSquared = nearest;
    }
    return isFound;
}

bool TryThrowHierarchicalDarts(const EligibleRegion& region,
                               int64 minimumDistance,
                               math::RandomKey key,
                               uint64 maximumDarts,
                               foundation::Array<CoveragePoint>& outPoints,
                               DartThrowingStatistics& outStatistics) noexcept
{
    if (minimumDistance < 2 || region.CellSize <= 0 || maximumDarts > ~uint32{0})
    {
        return false;
    }
    foundation::Array<CoveragePoint> points;
    DartThrowingStatistics statistics;
    DartThrower thrower(region, minimumDistance, key, points, statistics);
    if (thrower.TryInitialize() == false || thrower.TryFill(maximumDarts) == false)
    {
        return false;
    }
    outPoints = foundation::Move(points);
    outStatistics = statistics;
    return true;
}

CoverageZone ZoneLayout::Classify(int64 x, int64 z) const noexcept
{
    return CombineAxes(ClassifyAxis(*this, z, false), ClassifyAxis(*this, x, true));
}

usize DistanceBins::GetBin(int64 distanceSquared) const noexcept
{
    const int64 scaled = distanceSquared * SIXTEENTHS;
    const int64 conflict = GetConflictDistanceSquared(MinimumDistance);
    for (usize bin = 0; bin < BIN_SIXTEENTHS.GetSize(); ++bin)
    {
        if (scaled < BIN_SIXTEENTHS[bin] * conflict)
        {
            return bin;
        }
    }
    return DISTANCE_BIN_COUNT - 1;
}

bool TryMeasureCoverage(const EligibleRegion& region,
                        const ZoneLayout& layout,
                        foundation::BorrowedView<const CoveragePoint> points,
                        int64 minimumDistance,
                        const ProbeSpacing& spacing,
                        CoverageMeasurement& outMeasurement) noexcept
{
    if (minimumDistance <= 0 || spacing.Interior <= 0 || spacing.Seam <= 0 || spacing.Junction <= 0 ||
        spacing.NarrowStrip <= 0)
    {
        return false;
    }
    PointGrid grid;
    if (grid.TryBuild(points, region.Extent, minimumDistance) == false)
    {
        return false;
    }
    const int64 histogramReach = HISTOGRAM_REACH * minimumDistance;
    const int64 conflict = GetConflictDistanceSquared(minimumDistance);
    const DistanceBins bins{ .MinimumDistance = minimumDistance };
    CoverageMeasurement measurement;
    for (usize i = 0; i < points.GetSize(); ++i)
    {
        ZoneMeasurement& zone = measurement.Zones[static_cast<usize>(layout.Classify(points[i].X, points[i].Z))];
        zone.Points += 1;
        int64 nearest = 0;
        const NearestQuery query{ .Position = points[i], .Reach = histogramReach, .SkipIndex = i };
        const bool isFound = grid.TryFindNearest(query, nearest);
        RecordDistance(zone.NearestNeighbour, bins, isFound, nearest);
        if (isFound == true && (zone.MinimumSpacingSquared < 0 || nearest < zone.MinimumSpacingSquared))
        {
            zone.MinimumSpacingSquared = nearest;
        }
    }
    // Zones are products of a row and a column test, so classify each row and column once.
    const usize columns = static_cast<usize>(region.Extent.MaximumX - region.Extent.MinimumX);
    foundation::Array<uint8> columnKinds;
    if (columnKinds.TryResize(columns, 0) == false)
    {
        return false;
    }
    for (usize column = 0; column < columns; ++column)
    {
        columnKinds[column] = ClassifyAxis(layout, region.Extent.MinimumX + static_cast<int64>(column), true);
    }
    for (int64 z = region.Extent.MinimumZ; z < region.Extent.MaximumZ; ++z)
    {
        const int64 row = z - region.Extent.MinimumZ;
        const uint8 rowKind = ClassifyAxis(layout, z, false);
        for (usize column = 0; column < columns; ++column)
        {
            const CoverageZone zoneKind = CombineAxes(rowKind, columnKinds[column]);
            const int64 stride = GetZoneStride(spacing, zoneKind);
            if (row % stride != 0 || static_cast<int64>(column) % stride != 0)
            {
                continue;
            }
            const int64 x = region.Extent.MinimumX + static_cast<int64>(column);
            if (region.IsEligible(x, z) == false)
            {
                measurement.ExcludedProbeRoots += 1;
                continue;
            }
            ZoneMeasurement& zone = measurement.Zones[static_cast<usize>(zoneKind)];
            zone.ProbeRoots += 1;
            // Most roots are covered, so a search of one minimum distance usually decides them.
            const CoveragePoint position{ .X = x, .Z = z };
            int64 nearest = 0;
            if (grid.TryFindNearest(NearestQuery{ .Position = position, .Reach = minimumDistance }, nearest) == true &&
                nearest < conflict)
            {
                RecordDistance(zone.EmptyDistance, bins, true, nearest);
                continue;
            }
            zone.AdmissibleRoots += 1;
            const bool isFound =
                grid.TryFindNearest(NearestQuery{ .Position = position, .Reach = histogramReach }, nearest);
            RecordDistance(zone.EmptyDistance, bins, isFound, nearest);
            const int64 empty = isFound == true ? nearest : histogramReach * histogramReach;
            zone.LargestEmptyDistanceSquared = GetMaximum(zone.LargestEmptyDistanceSquared, empty);
        }
    }
    outMeasurement = measurement;
    return true;
}
} // namespace ludus::content::foliage_cook::coverage
