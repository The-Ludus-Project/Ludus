#include "internal/raster_mesh_light_grid.h"

#include "internal/raster_mesh_lights.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>

// Thanks to Ola Olsson, Markus Billeter and Ulf Assarsson, "Clustered Deferred and Forward Shading", Proc. High
// Performance Graphics 2012, section 3, https://doi.org/10.2312/EGGH/HPG12/087-096: lights are assigned to
// world/view cells by their bounded influence volumes, and each shaded sample loops over its cell's list only.
// This variant assigns emitter polygon records to a static world-space grid over the receivers, built once per
// scene. An area light has no finite range, so its influence radius is where the unshadowed Lambert term's bound
// emission * area / (pi * distance^2) falls below MESH_LIGHT_CULL_RADIANCE; the bound is the polygon's solid angle
// A / d^2 at its nearest distance d, which the projected solid angle never exceeds.
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace
{
using Status = rhi::RasterStatus;
/// Receiver bounds are padded so reconstructed positions on their faces stay inside, in metres plus a relative part.
constexpr float32 RECEIVER_MARGIN_METERS = 0.01F;
constexpr float32 RECEIVER_MARGIN_RELATIVE = 1.0e-4F;
/// Cubic cells are at least this large, so a degenerate (planar or point) receiver set still has positive extent.
constexpr float32 MINIMUM_CELL_METERS = 0.01F;
/// Integers up to 2^24 survive the float32 words the resolve reads.
constexpr usize MAX_EXACT_WORD = usize{1} << 24;
constexpr usize NORMAL_WORD = 0;
constexpr usize EMISSION_WORD = 1;
constexpr usize AXIS_COUNT = 3;
struct Influence final
{
    math::Vector3 CenterWorld;
    float32 Radius = 0;
    math::Vector3 NormalWorld;
    math::Vector3 OriginWorld;
    bool IsTwoSided = false;
    uint32 Word = 0;
};
struct Grid final
{
    math::Vector3 OriginWorld;
    float32 CellSize = 0;
    StaticArray<uint32, AXIS_COUNT> Cells{};
};
math::Vector3 GetPosition(math::Vector4 word) noexcept
{
    return {word.X, word.Y, word.Z};
}
float32 GetAxis(math::Vector3 value, usize axis) noexcept
{
    return axis == 0 ? value.X : (axis == 1 ? value.Y : value.Z);
}
float32 ComputeMagnitude(math::Vector3 value) noexcept
{
    return math::Max(math::Abs(value.X), math::Max(math::Abs(value.Y), math::Abs(value.Z)));
}
[[nodiscard]] bool
TryMakeInfluence(BorrowedView<const math::Vector4> words, usize offset, Influence& outInfluence) noexcept
{
    const usize recordWords = GetRasterMeshLightRecordWords(words, offset);
    if (recordWords == 0)
    {
        return false;
    }
    const usize cornerCount = recordWords - MESH_LIGHT_POLYGON_HEADER_WORDS;
    const usize firstCorner = offset + MESH_LIGHT_POLYGON_HEADER_WORDS;
    math::Vector3 sumWorld{};
    for (usize corner = 0; corner < cornerCount; ++corner)
    {
        sumWorld = sumWorld + GetPosition(words[firstCorner + corner]);
    }
    Influence influence;
    influence.CenterWorld = sumWorld * (1.0F / static_cast<float32>(cornerCount));
    float32 polygonRadius = 0;
    const math::Vector3 originWorld = GetPosition(words[firstCorner]);
    for (usize corner = 0; corner < cornerCount; ++corner)
    {
        polygonRadius =
            math::Max(polygonRadius, math::Length(GetPosition(words[firstCorner + corner]) - influence.CenterWorld));
    }
    const math::Vector4 emission = words[offset + EMISSION_WORD];
    const float32 radiance = math::Max(emission.X, math::Max(emission.Y, emission.Z));
    // The first corner's w holds the polygon's area (deferred_layout.h).
    const float32 area = words[firstCorner].W;
    const float32 reach =
        math::Sqrt(math::Max(radiance, 0.0F) * area / (static_cast<float32>(math::kPi) * MESH_LIGHT_CULL_RADIANCE));
    influence.Radius = polygonRadius + reach;
    influence.NormalWorld = GetPosition(words[offset + NORMAL_WORD]);
    influence.OriginWorld = originWorld;
    influence.IsTwoSided = words[offset + NORMAL_WORD].W != 0;
    influence.Word = static_cast<uint32>(offset);
    if (math::IsFinite(influence.CenterWorld) == false || math::IsFinite(influence.Radius) == false || area < 0)
    {
        return false;
    }
    outInfluence = influence;
    return true;
}
math::Aabb3 GetCellBounds(const Grid& grid, uint32 x, uint32 y, uint32 z) noexcept
{
    const math::Vector3 minimumWorld =
        grid.OriginWorld +
        math::Vector3{static_cast<float32>(x), static_cast<float32>(y), static_cast<float32>(z)} * grid.CellSize;
    return {minimumWorld, minimumWorld + math::Vector3{grid.CellSize, grid.CellSize, grid.CellSize}};
}
// Whether a receiver in the cell can lie within the record's influence sphere and in front of a one-sided plane.
bool CanReach(const Influence& influence, const math::Aabb3& cellWorld) noexcept
{
    float32 distanceSquared = 0;
    for (usize axis = 0; axis < AXIS_COUNT; ++axis)
    {
        const float32 center = GetAxis(influence.CenterWorld, axis);
        const float32 outside =
            math::Max(GetAxis(cellWorld.Min, axis) - center, math::Max(center - GetAxis(cellWorld.Max, axis), 0.0F));
        distanceSquared += outside * outside;
    }
    if (distanceSquared > influence.Radius * influence.Radius)
    {
        return false;
    }
    if (influence.IsTwoSided == true)
    {
        return true;
    }
    // The cell corner farthest along the normal; the resolve skips receivers behind or on the plane.
    const math::Vector3 frontWorld{influence.NormalWorld.X >= 0 ? cellWorld.Max.X : cellWorld.Min.X,
                                   influence.NormalWorld.Y >= 0 ? cellWorld.Max.Y : cellWorld.Min.Y,
                                   influence.NormalWorld.Z >= 0 ? cellWorld.Max.Z : cellWorld.Min.Z};
    return math::Dot(influence.NormalWorld, frontWorld - influence.OriginWorld) >= 0;
}
// Inclusive cell range [outFirst, outLast] of the sphere's bounding box on one axis; false when it misses the grid.
bool GetCellRange(const Grid& grid, const Influence& influence, usize axis, uint32& outFirst, uint32& outLast) noexcept
{
    const float32 center = GetAxis(influence.CenterWorld, axis) - GetAxis(grid.OriginWorld, axis);
    const float64 first = math::Floor(static_cast<float64>((center - influence.Radius) / grid.CellSize));
    const float64 last = math::Floor(static_cast<float64>((center + influence.Radius) / grid.CellSize));
    const float64 cells = static_cast<float64>(grid.Cells[axis]);
    if (last < 0 || first >= cells)
    {
        return false;
    }
    outFirst = static_cast<uint32>(math::Max(first, 0.0));
    outLast = static_cast<uint32>(math::Min(last, cells - 1));
    return true;
}
// Visit every cell the record reaches, x fastest; visit(cellIndex) returns false to stop.
template <typename Visitor>
bool VisitCells(const Grid& grid, const Influence& influence, Visitor&& visit) noexcept
{
    StaticArray<uint32, AXIS_COUNT> first{};
    StaticArray<uint32, AXIS_COUNT> last{};
    for (usize axis = 0; axis < AXIS_COUNT; ++axis)
    {
        if (GetCellRange(grid, influence, axis, first[axis], last[axis]) == false)
        {
            return true;
        }
    }
    for (uint32 z = first[2]; z <= last[2]; ++z)
    {
        for (uint32 y = first[1]; y <= last[1]; ++y)
        {
            for (uint32 x = first[0]; x <= last[0]; ++x)
            {
                if (CanReach(influence, GetCellBounds(grid, x, y, z)) == true &&
                    visit(x + grid.Cells[0] * (y + grid.Cells[1] * z)) == false)
                {
                    return false;
                }
            }
        }
    }
    return true;
}
Grid MakeGrid(const math::Aabb3& receiversWorld) noexcept
{
    const float32 margin =
        RECEIVER_MARGIN_METERS + RECEIVER_MARGIN_RELATIVE * math::Max(ComputeMagnitude(receiversWorld.Min),
                                                                      ComputeMagnitude(receiversWorld.Max));
    const math::Vector3 minimumWorld = receiversWorld.Min - math::Vector3{margin, margin, margin};
    const math::Vector3 extent = receiversWorld.Max + math::Vector3{margin, margin, margin} - minimumWorld;
    Grid grid;
    grid.OriginWorld = minimumWorld;
    grid.CellSize = math::Max(math::Max(extent.X, math::Max(extent.Y, extent.Z)) /
                                  static_cast<float32>(MESH_LIGHT_GRID_CELLS_PER_AXIS),
                              MINIMUM_CELL_METERS);
    for (usize axis = 0; axis < AXIS_COUNT; ++axis)
    {
        const float64 cells = math::Ceil(static_cast<float64>(GetAxis(extent, axis) / grid.CellSize));
        grid.Cells[axis] =
            static_cast<uint32>(math::Clamp(cells, 1.0, static_cast<float64>(MESH_LIGHT_GRID_CELLS_PER_AXIS)));
    }
    return grid;
}
} // namespace

Status AppendRasterMeshLightGrid(Array<math::Vector4>& inoutWords,
                                 uint32 recordCount,
                                 const math::Aabb3& receiversWorld,
                                 uint32& outGridWord) noexcept
{
    if (recordCount == 0 || math::IsEmpty(receiversWorld) == true || math::IsFinite(receiversWorld) == false)
    {
        outGridWord = 0;
        return Status::Ready;
    }
    const BorrowedView<const math::Vector4> words{inoutWords.GetData(), inoutWords.GetSize()};
    Array<Influence> influences;
    if (influences.TryEnsureCapacity(recordCount) == false)
    {
        return Status::OutOfMemory;
    }
    usize offset = 0;
    for (uint32 record = 0; record < recordCount; ++record)
    {
        Influence influence;
        if (TryMakeInfluence(words, offset, influence) == false)
        {
            return Status::InvalidDescription;
        }
        static_cast<void>(influences.TryAdd(influence));
        offset += GetRasterMeshLightRecordWords(words, offset);
    }
    const Grid grid = MakeGrid(receiversWorld);
    const usize cellCount = usize{grid.Cells[0]} * grid.Cells[1] * grid.Cells[2];
    Array<uint32> firstEntries;
    if (firstEntries.TryResize(cellCount + 1) == false)
    {
        return Status::OutOfMemory;
    }
    // Count each cell's entries, stopping once the lists cannot fit.
    usize entryCount = 0;
    for (const Influence& influence : influences)
    {
        const bool isWithinCapacity = VisitCells(grid, influence, [&](usize cell) noexcept {
            ++firstEntries[cell + 1];
            return ++entryCount <= MAX_MESH_LIGHT_GRID_ASSIGNMENTS;
        });
        if (isWithinCapacity == false)
        {
            outGridWord = 0;
            return Status::Ready;
        }
    }
    const usize gridWord = inoutWords.GetSize();
    const usize tableWords = (cellCount + MESH_LIGHT_GRID_CELLS_PER_WORD - 1) / MESH_LIGHT_GRID_CELLS_PER_WORD;
    const usize entryWord = gridWord + MESH_LIGHT_GRID_HEADER_WORDS + tableWords;
    const usize totalWords =
        entryWord + (entryCount + MESH_LIGHT_GRID_ENTRIES_PER_WORD - 1) / MESH_LIGHT_GRID_ENTRIES_PER_WORD;
    if (totalWords >= MAX_EXACT_WORD)
    {
        outGridWord = 0;
        return Status::Ready;
    }
    for (usize cell = 0; cell < cellCount; ++cell)
    {
        firstEntries[cell + 1] += firstEntries[cell];
    }
    Array<uint32> entries;
    Array<uint32> cursors;
    if (entries.TryResize(entryCount) == false || cursors.TryAddRange({firstEntries.GetData(), cellCount}) == false ||
        inoutWords.TryEnsureCapacity(totalWords) == false)
    {
        return Status::OutOfMemory;
    }
    for (const Influence& influence : influences)
    {
        static_cast<void>(VisitCells(grid, influence, [&](usize cell) noexcept {
            entries[cursors[cell]++] = influence.Word;
            return true;
        }));
    }
    // Reserved above, so the appends below cannot fail. The three header words come first.
    static_assert(MESH_LIGHT_GRID_CULL_WORD + 1 == MESH_LIGHT_GRID_HEADER_WORDS);
    static_cast<void>(inoutWords.TryAdd({grid.OriginWorld.X, grid.OriginWorld.Y, grid.OriginWorld.Z, grid.CellSize}));
    static_cast<void>(inoutWords.TryAdd({static_cast<float32>(grid.Cells[0]),
                                         static_cast<float32>(grid.Cells[1]),
                                         static_cast<float32>(grid.Cells[2]),
                                         static_cast<float32>(entryWord)}));
    static_cast<void>(inoutWords.TryAdd({MESH_LIGHT_CULL_RADIANCE, 0, 0, 0}));
    for (usize cell = 0; cell < cellCount; cell += MESH_LIGHT_GRID_CELLS_PER_WORD)
    {
        const bool hasPair = cell + 1 < cellCount;
        static_cast<void>(inoutWords.TryAdd(
            {static_cast<float32>(firstEntries[cell]),
             static_cast<float32>(firstEntries[cell + 1] - firstEntries[cell]),
             hasPair == true ? static_cast<float32>(firstEntries[cell + 1]) : 0.0F,
             hasPair == true ? static_cast<float32>(firstEntries[cell + 2] - firstEntries[cell + 1]) : 0.0F}));
    }
    for (usize entry = 0; entry < entryCount; entry += MESH_LIGHT_GRID_ENTRIES_PER_WORD)
    {
        math::Vector4 packed{};
        for (usize lane = 0; lane < MESH_LIGHT_GRID_ENTRIES_PER_WORD && entry + lane < entryCount; ++lane)
        {
            float32& value = lane == 0 ? packed.X : (lane == 1 ? packed.Y : (lane == 2 ? packed.Z : packed.W));
            value = static_cast<float32>(entries[entry + lane]);
        }
        static_cast<void>(inoutWords.TryAdd(packed));
    }
    outGridWord = static_cast<uint32>(gridWord);
    return Status::Ready;
}

bool GetRasterMeshLightCell(BorrowedView<const math::Vector4> words,
                            uint32 gridWord,
                            math::Vector3 positionWorld,
                            Array<uint32>& outRecordWords) noexcept
{
    if (gridWord == 0 || usize{gridWord} + MESH_LIGHT_GRID_HEADER_WORDS > words.GetSize())
    {
        return false;
    }
    const math::Vector4 origin = words[gridWord];
    const math::Vector4 cells = words[gridWord + 1];
    const math::Vector3 cellWorld = (positionWorld - GetPosition(origin)) * (1.0F / origin.W);
    const StaticArray<float64, AXIS_COUNT> coordinates{{math::Floor(static_cast<float64>(cellWorld.X)),
                                                        math::Floor(static_cast<float64>(cellWorld.Y)),
                                                        math::Floor(static_cast<float64>(cellWorld.Z))}};
    const StaticArray<float32, AXIS_COUNT> dimensions{{cells.X, cells.Y, cells.Z}};
    for (usize axis = 0; axis < AXIS_COUNT; ++axis)
    {
        if (coordinates[axis] < 0 || coordinates[axis] >= static_cast<float64>(dimensions[axis]))
        {
            return false;
        }
    }
    const usize cell =
        static_cast<usize>(coordinates[0]) +
        static_cast<usize>(dimensions[0]) * (static_cast<usize>(coordinates[1]) +
                                             static_cast<usize>(dimensions[1]) * static_cast<usize>(coordinates[2]));
    const usize tableWord = gridWord + MESH_LIGHT_GRID_HEADER_WORDS + cell / MESH_LIGHT_GRID_CELLS_PER_WORD;
    if (tableWord >= words.GetSize())
    {
        return false;
    }
    const math::Vector4 pair = words[tableWord];
    const bool isSecond = cell % MESH_LIGHT_GRID_CELLS_PER_WORD != 0;
    const usize first = static_cast<usize>(isSecond == true ? pair.Z : pair.X);
    const usize count = static_cast<usize>(isSecond == true ? pair.W : pair.Y);
    const usize entryWord = static_cast<usize>(cells.W);
    for (usize entry = first; entry < first + count; ++entry)
    {
        const usize word = entryWord + entry / MESH_LIGHT_GRID_ENTRIES_PER_WORD;
        if (word >= words.GetSize())
        {
            return false;
        }
        const math::Vector4 packed = words[word];
        const usize lane = entry % MESH_LIGHT_GRID_ENTRIES_PER_WORD;
        const float32 value = lane == 0 ? packed.X : (lane == 1 ? packed.Y : (lane == 2 ? packed.Z : packed.W));
        if (outRecordWords.TryAdd(static_cast<uint32>(value)) == false)
        {
            return false;
        }
    }
    return true;
}
} // namespace ludus::graphics::renderer::internal
