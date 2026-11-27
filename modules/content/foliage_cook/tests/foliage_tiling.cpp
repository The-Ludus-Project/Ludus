#include "foliage_tiling.h"

#include <ludus/foundation/containers/sort.hpp>

// Randomized tiling with boundary repair; see foliage_tiling.h for the reference and the departures from it.

namespace ludus::content::foliage_cook::coverage
{
namespace math = foundation::math;

namespace
{
/// Event of a tile's clipping choices; seed point k uses event k + 1 for its priority.
constexpr uint32 TILE_CHOICE_EVENT = 0;
/// Dimension of a tile's clipping offset along X.
constexpr uint16 OFFSET_X_DIMENSION = 0;
/// Dimension of a tile's clipping offset along Z.
constexpr uint16 OFFSET_Z_DIMENSION = 1;
/// Dimension of a tile's symmetry.
constexpr uint16 SYMMETRY_DIMENSION = 2;
/// Dimension of a clipped point's priority.
constexpr uint16 CLIPPED_PRIORITY_DIMENSION = 3;
/// Dimension of a gap root's fill priority; its scope is the root, its event zero.
constexpr uint16 FILL_PRIORITY_DIMENSION = 4;
/// Symmetry bits: exchange the axes, then mirror X, then mirror Z.
constexpr uint32 SWAP_AXES = 1;
constexpr uint32 MIRROR_X = 2;
constexpr uint32 MIRROR_Z = 4;
/// Priority bits that select a bucket of the fill order.
constexpr uint32 BUCKET_SHIFT = 16;
/// Buckets of the fill order.
constexpr usize BUCKET_COUNT = usize{1} << BUCKET_SHIFT;
/// Marks an empty cell of the occupancy grid.
constexpr uint32 NO_POINT = ~uint32{0};
/// FNV-1a parameters for configuration signatures.
constexpr uint64 SIGNATURE_OFFSET = 0xcbf29ce484222325ULL;
constexpr uint64 SIGNATURE_PRIME = 0x100000001b3ULL;

bool IsInt32(int64 value) noexcept
{
    return static_cast<int64>(static_cast<int32>(value)) == value;
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

/// Scope of a pair of int32 coordinates, such as a tile address or a root.
uint64 PackPair(int64 x, int64 z) noexcept
{
    return (uint64{static_cast<uint32>(static_cast<int32>(x))} << 32) | static_cast<uint32>(static_cast<int32>(z));
}

/// Whether a lies before b in Z-then-X order.
bool IsBefore(const CoveragePoint& a, const CoveragePoint& b) noexcept
{
    return a.Z != b.Z ? a.Z < b.Z : a.X < b.X;
}

/// Apply a symmetry of the square [0, size)^2 to a point in it.
CoveragePoint TransformSeedPoint(int64 size, const CoveragePoint& point, uint32 symmetry) noexcept
{
    CoveragePoint result = point;
    if ((symmetry & SWAP_AXES) != 0)
    {
        result = { .X = point.Z, .Z = point.X };
    }
    if ((symmetry & MIRROR_X) != 0)
    {
        result.X = size - 1 - result.X;
    }
    if ((symmetry & MIRROR_Z) != 0)
    {
        result.Z = size - 1 - result.Z;
    }
    return result;
}

/// Apply a symmetry of the square to a displacement.
CoveragePoint TransformDisplacement(const CoveragePoint& vector, uint32 symmetry) noexcept
{
    CoveragePoint result = vector;
    if ((symmetry & SWAP_AXES) != 0)
    {
        result = { .X = vector.Z, .Z = vector.X };
    }
    if ((symmetry & MIRROR_X) != 0)
    {
        result.X = -result.X;
    }
    if ((symmetry & MIRROR_Z) != 0)
    {
        result.Z = -result.Z;
    }
    return result;
}

/// The largest power-of-two cell in which two roots are always closer than the minimum distance.
int64 GetSingleSampleCellSize(int64 minimumDistance) noexcept
{
    const int64 conflict = GetConflictDistanceSquared(minimumDistance);
    int64 size = 1;
    while (2 * (2 * size - 1) * (2 * size - 1) < conflict)
    {
        size *= 2;
    }
    return size;
}

/// The largest h >= 0 with h * h < value, for value >= 1; the conflict distance keeps h * h far from overflow.
int64 GetHalfWidthBelow(int64 value) noexcept
{
    int64 low = 0;
    int64 high = value;
    while (low < high)
    {
        const int64 middle = low + (high - low + 1) / 2;
        if (middle * middle < value)
        {
            low = middle;
        }
        else
        {
            high = middle - 1;
        }
    }
    return low;
}

/// A root no kept point covers, with its fill priority.
struct GapRoot final
{
    int32 X = 0;
    int32 Z = 0;
    uint32 Priority = 0;
};

/// Whether a gap root is filled before another: higher priority first, ties in Z-then-X order.
bool IsFilledBefore(const GapRoot& a, const GapRoot& b) noexcept
{
    if (a.Priority != b.Priority)
    {
        return a.Priority > b.Priority;
    }
    return a.Z != b.Z ? a.Z < b.Z : a.X < b.X;
}

/// Covered X range of one point on one row, inclusive.
struct CoveredInterval final
{
    int64 First = 0;
    int64 Last = 0;
};

/// Every state of one tiling call.
class TilingRepair final
{
public:
    TilingRepair(const EligibleRegion& region,
                 const FoliageLatticeRectangle& window,
                 const SeedPattern& seed,
                 int64 minimumDistance,
                 const TilingParameters& parameters) noexcept
        : mRegion(region), mWindow(window), mSeed(seed), mMinimumDistance(minimumDistance),
          mConflict(GetConflictDistanceSquared(minimumDistance)), mParameters(parameters)
    {
    }

    [[nodiscard]] bool TryRun(foundation::Array<CoveragePoint>& outPoints) noexcept
    {
        if (tryClipTiles() == false || tryRemoveConflicts() == false || tryBuildOccupancy() == false ||
            tryFindGaps() == false || tryFill() == false)
        {
            return false;
        }
        foundation::Sort(mPoints.GetData(), mPoints.GetSize(), IsBefore);
        outPoints = foundation::Move(mPoints);
        return true;
    }

    [[nodiscard]] const TilingStatistics& GetStatistics() const noexcept
    {
        return mStatistics;
    }

private:
    [[nodiscard]] bool tryClipTiles() noexcept
    {
        const int64 tileSize = mParameters.TileSize;
        const int64 firstX = FloorDivide(mWindow.MinimumX, tileSize);
        const int64 firstZ = FloorDivide(mWindow.MinimumZ, tileSize);
        const int64 columns = FloorDivide(mWindow.MaximumX - 1, tileSize) - firstX + 1;
        const int64 rows = FloorDivide(mWindow.MaximumZ - 1, tileSize) - firstZ + 1;
        const uint64 tiles = static_cast<uint64>(columns * rows);
        mStatistics.Tiles = tiles;
        for (uint64 ordinal = 0; ordinal < tiles; ++ordinal)
        {
            const uint64 tile = mParameters.IsReversed == true ? tiles - 1 - ordinal : ordinal;
            const int64 tileX = firstX + static_cast<int64>(tile % static_cast<uint64>(columns));
            const int64 tileZ = firstZ + static_cast<int64>(tile / static_cast<uint64>(columns));
            if (tryClipTile(tileX, tileZ) == false)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool tryClipTile(int64 tileX, int64 tileZ) noexcept
    {
        if (IsInt32(tileX) == false || IsInt32(tileZ) == false)
        {
            return false;
        }
        const int64 tileSize = mParameters.TileSize;
        const uint64 scope = PackPair(tileX, tileZ);
        const uint32 span = static_cast<uint32>(mSeed.Size - tileSize + 1);
        uint32 offsetX = 0;
        uint32 offsetZ = 0;
        uint32 symmetry = 0;
        if (math::TrySampleBounded(mParameters.Key,
                                   makeAddress(scope, TILE_CHOICE_EVENT, OFFSET_X_DIMENSION),
                                   span,
                                   offsetX) != math::MathStatus::Success ||
            math::TrySampleBounded(mParameters.Key,
                                   makeAddress(scope, TILE_CHOICE_EVENT, OFFSET_Z_DIMENSION),
                                   span,
                                   offsetZ) != math::MathStatus::Success ||
            math::TrySampleBounded(mParameters.Key,
                                   makeAddress(scope, TILE_CHOICE_EVENT, SYMMETRY_DIMENSION),
                                   mParameters.SymmetryCount,
                                   symmetry) != math::MathStatus::Success)
        {
            return false;
        }
        for (usize k = 0; k < mSeed.Points.GetSize(); ++k)
        {
            const CoveragePoint transformed = TransformSeedPoint(mSeed.Size, mSeed.Points[k], symmetry);
            const int64 localX = transformed.X - static_cast<int64>(offsetX);
            const int64 localZ = transformed.Z - static_cast<int64>(offsetZ);
            if (localX < 0 || localX >= tileSize || localZ < 0 || localZ >= tileSize)
            {
                continue;
            }
            const CoveragePoint point{ .X = tileX * tileSize + localX, .Z = tileZ * tileSize + localZ };
            if (IsInside(mWindow, point.X, point.Z) == false)
            {
                continue;
            }
            mStatistics.ClippedPoints += 1;
            if (mRegion.IsEligible(point.X, point.Z) == false)
            {
                mStatistics.IneligiblePoints += 1;
                continue;
            }
            const uint32 priority =
                math::SampleUInt32(mParameters.Key,
                                   makeAddress(scope, static_cast<uint32>(k + 1), CLIPPED_PRIORITY_DIMENSION));
            if (mClipped.TryAdd(point) == false || mClippedPriorities.TryAdd(priority) == false)
            {
                return false;
            }
        }
        return true;
    }

    /// Whether clipped point a wins a conflict with clipped point b.
    [[nodiscard]] bool isWinning(usize a, usize b) const noexcept
    {
        if (mClippedPriorities[a] != mClippedPriorities[b])
        {
            return mClippedPriorities[a] > mClippedPriorities[b];
        }
        return IsBefore(mClipped[a], mClipped[b]);
    }

    [[nodiscard]] bool tryRemoveConflicts() noexcept
    {
        PointGrid grid;
        if (grid.TryBuild({mClipped.GetData(), mClipped.GetSize()}, mWindow, mMinimumDistance) == false ||
            mPoints.TryEnsureCapacity(mClipped.GetSize()) == false)
        {
            return false;
        }
        for (usize i = 0; i < mClipped.GetSize(); ++i)
        {
            const NearestQuery query{ .Position = mClipped[i], .Reach = mMinimumDistance, .SkipIndex = i };
            const bool isRemoved = grid.VisitNear(query, [&](usize other) noexcept {
                const int64 differenceX = mClipped[other].X - mClipped[i].X;
                const int64 differenceZ = mClipped[other].Z - mClipped[i].Z;
                return differenceX * differenceX + differenceZ * differenceZ < mConflict && isWinning(other, i);
            });
            if (isRemoved == true)
            {
                mStatistics.RemovedPoints += 1;
            }
            else if (mPoints.TryAdd(mClipped[i]) == false)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool tryBuildOccupancy() noexcept
    {
        mCellSize = GetSingleSampleCellSize(mMinimumDistance);
        mGridColumns = (mWindow.MaximumX - mWindow.MinimumX + mCellSize - 1) / mCellSize;
        mGridRows = (mWindow.MaximumZ - mWindow.MinimumZ + mCellSize - 1) / mCellSize;
        if (mOccupancy.TryResize(static_cast<usize>(mGridColumns * mGridRows), NO_POINT) == false)
        {
            return false;
        }
        for (usize i = 0; i < mPoints.GetSize(); ++i)
        {
            if (tryOccupy(i) == false)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] usize getOccupancyCell(int64 x, int64 z) const noexcept
    {
        return static_cast<usize>(((z - mWindow.MinimumZ) / mCellSize) * mGridColumns +
                                  (x - mWindow.MinimumX) / mCellSize);
    }

    [[nodiscard]] bool tryOccupy(usize index) noexcept
    {
        const usize cell = getOccupancyCell(mPoints[index].X, mPoints[index].Z);
        // Two points in one cell would be closer than the minimum distance.
        if (mOccupancy[cell] != NO_POINT || index >= NO_POINT)
        {
            return false;
        }
        mOccupancy[cell] = static_cast<uint32>(index);
        return true;
    }

    [[nodiscard]] bool isConflicting(const CoveragePoint& root) const noexcept
    {
        const int64 x = root.X;
        const int64 z = root.Z;
        const int64 column = (x - mWindow.MinimumX) / mCellSize;
        const int64 row = (z - mWindow.MinimumZ) / mCellSize;
        const int64 reach = (mMinimumDistance + mCellSize - 1) / mCellSize;
        for (int64 r = GetMaximum(row - reach, 0); r <= GetMinimum(row + reach, mGridRows - 1); ++r)
        {
            for (int64 c = GetMaximum(column - reach, 0); c <= GetMinimum(column + reach, mGridColumns - 1); ++c)
            {
                const uint32 index = mOccupancy[static_cast<usize>(r * mGridColumns + c)];
                if (index == NO_POINT)
                {
                    continue;
                }
                const int64 differenceX = mPoints[index].X - x;
                const int64 differenceZ = mPoints[index].Z - z;
                if (differenceX * differenceX + differenceZ * differenceZ < mConflict)
                {
                    return true;
                }
            }
        }
        return false;
    }

    /// Record the eligible roots of [first, last) on a row as gaps, skipping keep-out cells whole.
    [[nodiscard]] bool tryAddGaps(int64 z, int64 first, int64 last) noexcept
    {
        int64 x = first;
        while (x < last)
        {
            const int64 cellEnd =
                mRegion.Extent.MinimumX + ((x - mRegion.Extent.MinimumX) / mRegion.CellSize + 1) * mRegion.CellSize;
            const int64 end = GetMinimum(cellEnd, last);
            if (mRegion.IsEligible(x, z) == true)
            {
                for (int64 root = x; root < end; ++root)
                {
                    const uint32 priority =
                        math::SampleUInt32(mParameters.Key, makeAddress(PackPair(root, z), 0, FILL_PRIORITY_DIMENSION));
                    if (mGaps.TryAdd(GapRoot
                    {
                        .X = static_cast<int32>(root),
                        .Z = static_cast<int32>(z),
                        .Priority = priority,
                    }) == false)
                    {
                        return false;
                    }
                }
            }
            x = end;
        }
        return true;
    }

    /// Find every eligible root no kept point covers, row by row with exact integer disk chords.
    [[nodiscard]] bool tryFindGaps() noexcept
    {
        const int64 height = mWindow.MaximumZ - mWindow.MinimumZ;
        foundation::Array<uint32> rowFirst;
        foundation::Array<uint32> rowPoints;
        foundation::Array<uint32> cursor;
        if (rowFirst.TryResize(static_cast<usize>(height + 1), 0) == false ||
            rowPoints.TryResize(mPoints.GetSize(), 0) == false ||
            cursor.TryResize(static_cast<usize>(height), 0) == false)
        {
            return false;
        }
        for (const CoveragePoint& point : mPoints)
        {
            rowFirst[static_cast<usize>(point.Z - mWindow.MinimumZ + 1)] += 1;
        }
        for (usize row = 0; row < static_cast<usize>(height); ++row)
        {
            rowFirst[row + 1] += rowFirst[row];
            cursor[row] = rowFirst[row];
        }
        for (usize i = 0; i < mPoints.GetSize(); ++i)
        {
            const usize row = static_cast<usize>(mPoints[i].Z - mWindow.MinimumZ);
            rowPoints[cursor[row]] = static_cast<uint32>(i);
            cursor[row] += 1;
        }
        foundation::Array<CoveredInterval> intervals;
        for (int64 ordinal = 0; ordinal < height; ++ordinal)
        {
            const int64 z = mWindow.MinimumZ + (mParameters.IsReversed == true ? height - 1 - ordinal : ordinal);
            intervals.Clear();
            const int64 firstRow = GetMaximum(z - mMinimumDistance + 1, mWindow.MinimumZ);
            const int64 lastRow = GetMinimum(z + mMinimumDistance - 1, mWindow.MaximumZ - 1);
            for (int64 pointRow = firstRow; pointRow <= lastRow; ++pointRow)
            {
                const usize row = static_cast<usize>(pointRow - mWindow.MinimumZ);
                const int64 differenceZ = pointRow - z;
                const int64 halfWidth = GetHalfWidthBelow(mConflict - differenceZ * differenceZ);
                for (uint32 entry = rowFirst[row]; entry < rowFirst[row + 1]; ++entry)
                {
                    const int64 x = mPoints[rowPoints[entry]].X;
                    if (intervals.TryAdd({ .First = x - halfWidth, .Last = x + halfWidth }) == false)
                    {
                        return false;
                    }
                }
            }
            foundation::Sort(
                intervals.GetData(),
                intervals.GetSize(),
                [](const CoveredInterval& a, const CoveredInterval& b) noexcept { return a.First < b.First; });
            int64 cursorX = mWindow.MinimumX;
            for (const CoveredInterval& interval : intervals)
            {
                if (interval.First > cursorX &&
                    tryAddGaps(z, cursorX, GetMinimum(interval.First, mWindow.MaximumX)) == false)
                {
                    return false;
                }
                cursorX = GetMaximum(cursorX, interval.Last + 1);
            }
            if (cursorX < mWindow.MaximumX && tryAddGaps(z, cursorX, mWindow.MaximumX) == false)
            {
                return false;
            }
        }
        mStatistics.GapRoots = mGaps.GetSize();
        return true;
    }

    /// Fill gap roots greedily in priority order: a bucket pass on the high priority bits, then a sort per bucket.
    [[nodiscard]] bool tryFill() noexcept
    {
        foundation::Array<uint32> bucketFirst;
        foundation::Array<GapRoot> ordered;
        if (mGaps.GetSize() >= NO_POINT || bucketFirst.TryResize(BUCKET_COUNT + 1, 0) == false ||
            ordered.TryResize(mGaps.GetSize()) == false)
        {
            return false;
        }
        const auto getBucket = [](const GapRoot& gap) noexcept {
            return BUCKET_COUNT - 1 - static_cast<usize>(gap.Priority >> BUCKET_SHIFT);
        };
        for (const GapRoot& gap : mGaps)
        {
            bucketFirst[getBucket(gap) + 1] += 1;
        }
        for (usize bucket = 0; bucket < BUCKET_COUNT; ++bucket)
        {
            bucketFirst[bucket + 1] += bucketFirst[bucket];
        }
        {
            foundation::Array<uint32> cursor;
            if (cursor.TryResize(BUCKET_COUNT, 0) == false)
            {
                return false;
            }
            for (usize bucket = 0; bucket < BUCKET_COUNT; ++bucket)
            {
                cursor[bucket] = bucketFirst[bucket];
            }
            for (const GapRoot& gap : mGaps)
            {
                const usize bucket = getBucket(gap);
                ordered[cursor[bucket]] = gap;
                cursor[bucket] += 1;
            }
            updatePeak(ordered.GetCapacity() * sizeof(GapRoot) +
                       (bucketFirst.GetCapacity() + cursor.GetCapacity()) * sizeof(uint32));
        }
        mGaps = foundation::Array<GapRoot>();
        for (usize bucket = 0; bucket < BUCKET_COUNT; ++bucket)
        {
            foundation::Sort(ordered.GetData() + bucketFirst[bucket],
                             bucketFirst[bucket + 1] - bucketFirst[bucket],
                             IsFilledBefore);
        }
        for (const GapRoot& gap : ordered)
        {
            if (isConflicting({ .X = gap.X, .Z = gap.Z }) == true)
            {
                continue;
            }
            if (mPoints.TryAdd({ .X = gap.X, .Z = gap.Z }) == false || tryOccupy(mPoints.GetSize() - 1) == false)
            {
                return false;
            }
            mStatistics.FilledPoints += 1;
        }
        return true;
    }

    void updatePeak(usize extraBytes) noexcept
    {
        const usize bytes = extraBytes + mClipped.GetCapacity() * sizeof(CoveragePoint) +
                            mClippedPriorities.GetCapacity() * sizeof(uint32) +
                            mPoints.GetCapacity() * sizeof(CoveragePoint) + mOccupancy.GetCapacity() * sizeof(uint32) +
                            mGaps.GetCapacity() * sizeof(GapRoot);
        mStatistics.PeakBytes = bytes > mStatistics.PeakBytes ? bytes : mStatistics.PeakBytes;
    }

    [[nodiscard]] static math::RandomAddress makeAddress(uint64 scope, uint32 event, uint16 dimension) noexcept
    {
        return { .Scope = scope, .Event = event, .Dimension = dimension };
    }

    const EligibleRegion& mRegion;
    FoliageLatticeRectangle mWindow{};
    const SeedPattern& mSeed;
    int64 mMinimumDistance = 0;
    int64 mConflict = 0;
    TilingParameters mParameters{};
    TilingStatistics mStatistics{};
    foundation::Array<CoveragePoint> mClipped;
    foundation::Array<uint32> mClippedPriorities;
    foundation::Array<CoveragePoint> mPoints;
    foundation::Array<uint32> mOccupancy;
    foundation::Array<GapRoot> mGaps;
    int64 mCellSize = 1;
    int64 mGridColumns = 0;
    int64 mGridRows = 0;
};

/// FNV-1a over the coordinates of displacements in order.
uint64 HashDisplacements(const foundation::Array<CoveragePoint>& displacements) noexcept
{
    uint64 hash = SIGNATURE_OFFSET;
    for (const CoveragePoint& displacement : displacements)
    {
        hash = (hash ^ static_cast<uint64>(displacement.X)) * SIGNATURE_PRIME;
        hash = (hash ^ static_cast<uint64>(displacement.Z)) * SIGNATURE_PRIME;
    }
    return hash;
}

/// Entries of a signature list whose value occurs more than once.
uint64 CountRepeats(foundation::Array<uint64>& inoutSignatures) noexcept
{
    foundation::Sort(inoutSignatures.GetData(), inoutSignatures.GetSize(), [](uint64 a, uint64 b) noexcept {
        return a < b;
    });
    uint64 repeats = 0;
    for (usize i = 0; i < inoutSignatures.GetSize(); ++i)
    {
        const bool isSameAsPrevious = i > 0 && inoutSignatures[i - 1] == inoutSignatures[i];
        const bool isSameAsNext = i + 1 < inoutSignatures.GetSize() && inoutSignatures[i + 1] == inoutSignatures[i];
        if (isSameAsPrevious == true || isSameAsNext == true)
        {
            repeats += 1;
        }
    }
    return repeats;
}
} // namespace

bool TryMakeSeedPattern(int64 size,
                        int64 minimumDistance,
                        math::RandomKey key,
                        uint64 maximumDarts,
                        SeedPattern& outPattern,
                        DartThrowingStatistics& outStatistics) noexcept
{
    const EligibleRegion square =
    {
        .Extent = { .MinimumX = 0, .MinimumZ = 0, .MaximumX = size, .MaximumZ = size },
        .CellSize = size,
        .CellColumns = 1,
        .CellRows = 1,
        .Holes = {},
    };
    SeedPattern pattern;
    pattern.Size = size;
    if (size <= 0 ||
        TryThrowHierarchicalDarts(square, minimumDistance, key, maximumDarts, pattern.Points, outStatistics) == false)
    {
        return false;
    }
    outPattern = foundation::Move(pattern);
    return true;
}

bool TryTileAndRepair(const EligibleRegion& region,
                      const FoliageLatticeRectangle& window,
                      const SeedPattern& seed,
                      int64 minimumDistance,
                      const TilingParameters& parameters,
                      foundation::Array<CoveragePoint>& outPoints,
                      TilingStatistics& outStatistics) noexcept
{
    const FoliageLatticeRectangle clipped =
    {
        .MinimumX = GetMaximum(window.MinimumX, region.Extent.MinimumX),
        .MinimumZ = GetMaximum(window.MinimumZ, region.Extent.MinimumZ),
        .MaximumX = GetMinimum(window.MaximumX, region.Extent.MaximumX),
        .MaximumZ = GetMinimum(window.MaximumZ, region.Extent.MaximumZ),
    };
    const bool isSymmetryValid =
        parameters.SymmetryCount == TRANSLATION_ONLY || parameters.SymmetryCount == DIHEDRAL_SYMMETRIES;
    if (minimumDistance < 2 || parameters.TileSize <= 0 || seed.Size < parameters.TileSize ||
        isSymmetryValid == false || region.CellSize <= 0 || clipped.MinimumX >= clipped.MaximumX ||
        clipped.MinimumZ >= clipped.MaximumZ || IsInt32(clipped.MinimumX) == false ||
        IsInt32(clipped.MaximumX) == false || IsInt32(clipped.MinimumZ) == false ||
        IsInt32(clipped.MaximumZ) == false || seed.Points.GetSize() >= NO_POINT)
    {
        return false;
    }
    TilingRepair repair(region, clipped, seed, minimumDistance, parameters);
    foundation::Array<CoveragePoint> points;
    if (repair.TryRun(points) == false)
    {
        return false;
    }
    outPoints = foundation::Move(points);
    outStatistics = repair.GetStatistics();
    return true;
}

bool TryMeasureRepetition(foundation::BorrowedView<const CoveragePoint> points,
                          const FoliageLatticeRectangle& extent,
                          int64 reach,
                          RepetitionMeasurement& outMeasurement) noexcept
{
    PointGrid grid;
    if (reach <= 0 || grid.TryBuild(points, extent, reach) == false)
    {
        return false;
    }
    const int64 reachSquared = reach * reach;
    RepetitionMeasurement measurement;
    measurement.Points = points.GetSize();
    foundation::Array<CoveragePoint> displacements;
    foundation::Array<CoveragePoint> transformed;
    foundation::Array<uint64> translated;
    foundation::Array<uint64> symmetric;
    for (usize i = 0; i < points.GetSize(); ++i)
    {
        displacements.Clear();
        bool hasFailed = false;
        static_cast<void>(
            grid.VisitNear({ .Position = points[i], .Reach = reach, .SkipIndex = i }, [&](usize other) noexcept {
                const CoveragePoint displacement
                {
                    .X = points[other].X - points[i].X,
                    .Z = points[other].Z - points[i].Z,
                };
                if (displacement.X * displacement.X + displacement.Z * displacement.Z < reachSquared &&
                    displacements.TryAdd(displacement) == false)
                {
                    hasFailed = true;
                    return true;
                }
                return false;
            }));
        if (hasFailed == true)
        {
            return false;
        }
        if (displacements.GetSize() < MINIMUM_CONFIGURATION_NEIGHBOURS)
        {
            continue;
        }
        measurement.Configurations += 1;
        foundation::Sort(displacements.GetData(), displacements.GetSize(), IsBefore);
        uint64 canonical = ~uint64{0};
        for (uint32 symmetry = 0; symmetry < DIHEDRAL_SYMMETRIES; ++symmetry)
        {
            transformed.Clear();
            for (const CoveragePoint& displacement : displacements)
            {
                if (transformed.TryAdd(TransformDisplacement(displacement, symmetry)) == false)
                {
                    return false;
                }
            }
            foundation::Sort(transformed.GetData(), transformed.GetSize(), IsBefore);
            const uint64 hash = HashDisplacements(transformed);
            canonical = hash < canonical ? hash : canonical;
        }
        if (translated.TryAdd(HashDisplacements(displacements)) == false || symmetric.TryAdd(canonical) == false)
        {
            return false;
        }
    }
    measurement.TranslatedRepeats = CountRepeats(translated);
    measurement.SymmetricRepeats = CountRepeats(symmetric);
    outMeasurement = measurement;
    return true;
}
} // namespace ludus::content::foliage_cook::coverage
