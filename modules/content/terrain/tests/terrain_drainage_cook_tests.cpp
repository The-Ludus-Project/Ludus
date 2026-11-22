// Drainage over cooked terrain pages: a branching basin whose tributaries cross page boundaries and join before one
// outlet, cooked at two page partitions and in shuffled order, then recooked after a stamp that diverts one
// tributary across a low divide; and a shallow saddle on a page boundary whose two spill routes sit on either side
// of one codec quantum. Pages cook heights only, so drainage is rerun over the reassembled decoded field and
// compared with the drainage of the complete source field. See
// docs/modules/content/terrain.md#drainage-over-decoded-pages.
//
// Thanks to Kelley, Malin and Nielson, "Terrain Simulation Using a Model of Stream Erosion", SIGGRAPH '88,
// pp. 263-268 (https://doi.org/10.1145/378456.378519), for treating a branching drainage network as the structure
// of terrain; the basin fixture uses a confluence as that structure. Thanks to Yuxiao Li, Mingze Xia, Xin Liang,
// Bei Wang and Hanqi Guo, "Preserving Discrete Morse-Smale Complexes in Error-Bounded Lossy Compression", IEEE TVCG
// 32(7), pp. 6593-6609 (https://arxiv.org/abs/2409.17346), for showing that a bounded pointwise error can still
// change critical points and their connections; the saddle fixture applies that distinction to the terrain height
// codec. No source code is copied from these works.

#include <ludus/content/terrain/terrain_cook.h>
#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/content/terrain/terrain_rivers.h>
#include <ludus/content/terrain/terrain_tile.h>
#include <ludus/content/terrain_format/terrain_format.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace terrain = ludus::content::terrain;
namespace format = ludus::content::terrain_format;
namespace math = ludus::foundation::math;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::float32;
using ludus::foundation::float64;
using ludus::foundation::int32;
using ludus::foundation::int64;
using ludus::foundation::StaticArray;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using terrain::TerrainCookStatus;
using terrain::TerrainFlowDirection;

constexpr uint32 COARSE_PAGE_CELLS = 16;
constexpr uint32 FINE_PAGE_CELLS = 8;
constexpr float32 QUANTUM_METRES = 1.0F / 64.0F;
/// Every source elevation is a binary32 value and every decoded height an exact multiple of the quantum, so the
/// decode error is at most half a quantum with no further rounding.
constexpr float64 HALF_QUANTUM_METRES = 0.5 * static_cast<float64>(QUANTUM_METRES);
constexpr int32 HALO = static_cast<int32>(format::PAGE_HALO_SAMPLES);
/// Prime, so it visits every page once for any page count that is not a multiple of it.
constexpr usize SHUFFLE_STRIDE = 7;
constexpr uint64 BASE_REVISION = 1;
constexpr uint64 EDITED_REVISION = 2;

/// One complete elevation field in field coordinates: column along +X, row along +Z.
struct Field final
{
    terrain::TerrainFieldGrid Grid{};
    Array<float32> Elevations;
};

[[nodiscard]] usize GetSampleIndex(const terrain::TerrainFieldGrid& grid, uint32 column, uint32 row)
{
    return (static_cast<usize>(row) * grid.Columns) + column;
}

/// A domain of CoarsePagesX by CoarsePagesZ coarse pages, cooked at CellsPerPage cells per page.
struct LayoutShape final
{
    uint32 CoarsePagesX = 0;
    uint32 CoarsePagesZ = 0;
    uint32 CellsPerPage = COARSE_PAGE_CELLS;
};

/// A layout of the shape's domain. The first page sits at a negative tile address so the fixtures cross the signed
/// lattice origin.
[[nodiscard]] terrain::TerrainRevisionLayout MakeLayout(const LayoutShape& shape, uint64 revision)
{
    const uint32 cellsPerPage = shape.CellsPerPage;
    const uint32 scale = COARSE_PAGE_CELLS / cellsPerPage;
    terrain::TerrainRevisionLayout layout;
    layout.Revision = revision;
    layout.RecipeVersion = 1;
    layout.SourceDigest = {revision, 0};
    layout.FirstPage = {-1 * static_cast<int64>(scale), -1 * static_cast<int64>(scale)};
    layout.PagesX = shape.CoarsePagesX * scale;
    layout.PagesZ = shape.CoarsePagesZ * scale;
    layout.CellsPerPage = cellsPerPage;
    layout.SpacingMetres = 1;
    layout.Codec =
    {
        .Encoding = terrain::TerrainHeightEncoding::Unsigned16,
        .OriginMetres = 0,
        .QuantumMetres = QUANTUM_METRES,
    };
    return layout;
}

[[nodiscard]] usize GetPageCount(const terrain::TerrainRevisionLayout& layout)
{
    return static_cast<usize>(layout.PagesX) * layout.PagesZ;
}

[[nodiscard]] terrain::TerrainSampleAddress GetDomainOrigin(const terrain::TerrainRevisionLayout& layout)
{
    terrain::TerrainSampleAddress origin;
    REQUIRE(terrain::TryGetTerrainTileOrigin(layout.FirstPage, layout.CellsPerPage, origin) ==
            terrain::TerrainTileStatus::Success);
    return origin;
}

/// Cook every page independently in a shuffled order, as concurrent workers would finish them, and assemble.
void CookShuffled(const terrain::TerrainRevisionLayout& layout,
                  const Field& field,
                  terrain::TerrainCookedRevision& outRevision)
{
    const usize pageCount = GetPageCount(layout);
    REQUIRE(pageCount % SHUFFLE_STRIDE != 0);
    Array<terrain::TerrainCookedPage> pages;
    REQUIRE(pages.TryResize(pageCount));
    for (usize i = 0; i < pageCount; ++i)
    {
        const usize index = (i * SHUFFLE_STRIDE) % pageCount;
        const terrain::TerrainTileAddress address{layout.FirstPage.X + static_cast<int64>(index % layout.PagesX),
                                                  layout.FirstPage.Z + static_cast<int64>(index / layout.PagesX)};
        REQUIRE(terrain::TryCookTerrainPage(layout, field.Grid, field.Elevations.AsView(), address, pages[i]) ==
                TerrainCookStatus::Success);
    }
    REQUIRE(terrain::TryAssembleTerrainRevision(layout, pages.AsView(), outRevision) == TerrainCookStatus::Success);
}

[[nodiscard]] bool AreBytesEqual(BorrowedView<const uint8> first, BorrowedView<const uint8> second)
{
    if (first.GetSize() != second.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < first.GetSize(); ++i)
    {
        if (first[i] != second[i])
        {
            return false;
        }
    }
    return true;
}

/// The pages of one revision, each read from its own copied byte range, and the field they decode to.
struct DecodedRevision final
{
    terrain::TerrainManifest Manifest;
    Array<terrain::TerrainPage> Pages;
    /// Every field sample decoded from the page that owns it (the last page owns the far edge).
    Array<float32> Elevations;
};

/// A field sample by column and row.
struct FieldSample final
{
    uint32 Column = 0;
    uint32 Row = 0;
};

/// Page-local coordinates of a field sample relative to one page; may lie outside the page's extended window.
struct PageLocalSample final
{
    int64 Column = 0;
    int64 Row = 0;
};

[[nodiscard]] PageLocalSample
GetPageLocalSample(const terrain::TerrainRevisionLayout& layout, usize pageIndex, FieldSample sample)
{
    const int64 cells = layout.CellsPerPage;
    const int64 pageColumn = static_cast<int64>(pageIndex % layout.PagesX);
    const int64 pageRow = static_cast<int64>(pageIndex / layout.PagesX);
    return {static_cast<int64>(sample.Column) - (pageColumn * cells),
            static_cast<int64>(sample.Row) - (pageRow * cells)};
}

[[nodiscard]] bool
IsInsidePageWindow(const terrain::TerrainRevisionLayout& layout, PageLocalSample sample, int32 margin)
{
    const int64 last = static_cast<int64>(layout.CellsPerPage) + margin;
    return sample.Column >= -margin && sample.Column <= last && sample.Row >= -margin && sample.Row <= last;
}

/// Decode a field sample from one page's core or halo.
[[nodiscard]] float64 ReadPageSample(const DecodedRevision& decoded, usize pageIndex, FieldSample sample)
{
    const PageLocalSample local = GetPageLocalSample(decoded.Manifest.GetLayout(), pageIndex, sample);
    REQUIRE(IsInsidePageWindow(decoded.Manifest.GetLayout(), local, HALO) == true);
    float64 height = 0;
    REQUIRE(terrain::TryGetTerrainPageHeight(decoded.Pages[pageIndex],
                                             static_cast<int32>(local.Column),
                                             static_cast<int32>(local.Row),
                                             height) == TerrainCookStatus::Success);
    return height;
}

/// Canonical index of the page that owns a field sample in the half-open partition.
[[nodiscard]] usize GetOwningPage(const terrain::TerrainRevisionLayout& layout, FieldSample sample)
{
    const uint32 pageColumn = math::Min(sample.Column / layout.CellsPerPage, layout.PagesX - 1);
    const uint32 pageRow = math::Min(sample.Row / layout.CellsPerPage, layout.PagesZ - 1);
    return (static_cast<usize>(pageRow) * layout.PagesX) + pageColumn;
}

void DecodeRevision(const terrain::TerrainCookedRevision& revision,
                    const terrain::TerrainFieldGrid& grid,
                    DecodedRevision& outDecoded)
{
    REQUIRE(terrain::TryReadTerrainManifest(revision.GetManifestBytes(), outDecoded.Manifest) ==
            TerrainCookStatus::Success);
    const BorrowedView<const terrain::TerrainPageRecord> records = outDecoded.Manifest.GetPageRecords();
    REQUIRE(outDecoded.Pages.TryResize(records.GetSize()));
    for (usize i = 0; i < records.GetSize(); ++i)
    {
        Array<uint8> range;
        REQUIRE(range.TryAddRange(
            BorrowedView<const uint8>{revision.GetPagePackBytes().GetDataOrNull() + records[i].ByteOffset,
                                      records[i].ByteSize}));
        REQUIRE(terrain::TryReadTerrainPage(outDecoded.Manifest, i, range.AsView(), outDecoded.Pages[i]) ==
                TerrainCookStatus::Success);
    }
    const terrain::TerrainRevisionLayout& layout = outDecoded.Manifest.GetLayout();
    REQUIRE(grid.Columns == (layout.PagesX * layout.CellsPerPage) + 1);
    REQUIRE(grid.Rows == (layout.PagesZ * layout.CellsPerPage) + 1);
    REQUIRE(outDecoded.Elevations.TryResize(terrain::GetTerrainSampleCount(grid)));
    for (uint32 row = 0; row < grid.Rows; ++row)
    {
        for (uint32 column = 0; column < grid.Columns; ++column)
        {
            const float64 height = ReadPageSample(outDecoded, GetOwningPage(layout, {column, row}), {column, row});
            const float32 narrowed = static_cast<float32>(height);
            // Decoded heights are multiples of the quantum well inside binary32's exact range.
            REQUIRE(static_cast<float64>(narrowed) == height);
            outDecoded.Elevations[GetSampleIndex(grid, column, row)] = narrowed;
        }
    }
}

/// Largest |decoded - source| over the field, in metres.
[[nodiscard]] float64 GetMaximumDecodeError(const Field& source, const DecodedRevision& decoded)
{
    float64 largest = 0;
    for (usize i = 0; i < source.Elevations.GetSize(); ++i)
    {
        largest = math::Max(
            largest,
            math::Abs(static_cast<float64>(decoded.Elevations[i]) - static_cast<float64>(source.Elevations[i])));
    }
    return largest;
}

/// Outputs of one TryComputeTerrainDrainage solve.
struct Drainage final
{
    Array<float32> Filled;
    Array<TerrainFlowDirection> Direction;
    Array<uint32> Accumulation;
    terrain::TerrainDrainageSummary Summary;
};

void ComputeDrainage(const terrain::TerrainFieldGrid& grid, const Array<float32>& elevations, Drainage& outDrainage)
{
    const usize samples = terrain::GetTerrainSampleCount(grid);
    REQUIRE(outDrainage.Filled.TryResize(samples));
    REQUIRE(outDrainage.Direction.TryResize(samples, TerrainFlowDirection::Outlet));
    REQUIRE(outDrainage.Accumulation.TryResize(samples));
    REQUIRE(terrain::TryComputeTerrainDrainage(grid,
                                               elevations.AsView(),
                                               outDrainage.Filled.AsView(),
                                               outDrainage.Direction.AsView(),
                                               outDrainage.Accumulation.AsView(),
                                               outDrainage.Summary) == terrain::TerrainFieldStatus::Success);
}

/// Whether two solves route and accumulate every sample identically.
[[nodiscard]] bool AreRoutingsEqual(const Drainage& first, const Drainage& second)
{
    if (first.Direction.GetSize() != second.Direction.GetSize())
    {
        return false;
    }
    for (usize i = 0; i < first.Direction.GetSize(); ++i)
    {
        if (first.Direction[i] != second.Direction[i] || first.Accumulation[i] != second.Accumulation[i])
        {
            return false;
        }
    }
    return first.Summary.OutletSamples == second.Summary.OutletSamples &&
           first.Summary.OutletAccumulation == second.Summary.OutletAccumulation;
}

/// Drop per metre from a sample toward one of its neighbours on a filled surface.
[[nodiscard]] float64 GetDrop(const terrain::TerrainFieldGrid& grid,
                              const Array<float32>& filled,
                              usize sample,
                              TerrainFlowDirection direction)
{
    const terrain::TerrainFlowOffset offset = terrain::GetTerrainFlowOffset(direction);
    const int64 column = static_cast<int64>(sample % grid.Columns) + offset.X;
    const int64 row = static_cast<int64>(sample / grid.Columns) + offset.Z;
    const usize neighbour = GetSampleIndex(grid, static_cast<uint32>(column), static_cast<uint32>(row));
    const float64 runX = static_cast<float64>(offset.X) * static_cast<float64>(grid.SpacingXMetres);
    const float64 runZ = static_cast<float64>(offset.Z) * static_cast<float64>(grid.SpacingZMetres);
    return (static_cast<float64>(filled[sample]) - static_cast<float64>(filled[neighbour])) /
           math::Sqrt((runX * runX) + (runZ * runZ));
}

/// Samples whose decoded routing picks another neighbour than the source routing.
struct RoutingFlips final
{
    usize Count = 0;
    /// Contributing samples the flips carry, the larger of the two solves at each flip: the most any downstream
    /// accumulation can move.
    uint64 CarriedSamples = 0;
};

/// Find the samples whose decoded routing differs from the source routing, checking that each is a near-tie the
/// codec may flip: every decoded height lies within half a quantum of its source, so a drop moves by at most one
/// quantum per metre of spacing, and two competing drops by at most two.
[[nodiscard]] RoutingFlips
FindNearTieFlips(const terrain::TerrainFieldGrid& grid, const Drainage& source, const Drainage& decoded)
{
    const float64 spacing =
        math::Min(static_cast<float64>(grid.SpacingXMetres), static_cast<float64>(grid.SpacingZMetres));
    const float64 tolerance = 2 * static_cast<float64>(QUANTUM_METRES) / spacing;
    RoutingFlips flips;
    for (usize i = 0; i < source.Direction.GetSize(); ++i)
    {
        if (source.Direction[i] == decoded.Direction[i])
        {
            continue;
        }
        ++flips.Count;
        flips.CarriedSamples += math::Max(source.Accumulation[i], decoded.Accumulation[i]);
        REQUIRE(source.Direction[i] != TerrainFlowDirection::Outlet);
        REQUIRE(decoded.Direction[i] != TerrainFlowDirection::Outlet);
        const float64 sourceDrop = GetDrop(grid, source.Filled, i, source.Direction[i]);
        const float64 decodedDrop = GetDrop(grid, source.Filled, i, decoded.Direction[i]);
        CHECK(math::Abs(sourceDrop - decodedDrop) <= tolerance);
    }
    return flips;
}

/// The sample a drainage path from start leaves the domain at.
[[nodiscard]] usize
TraceToOutlet(const terrain::TerrainFieldGrid& grid, const Drainage& drainage, uint32 column, uint32 row)
{
    usize sample = GetSampleIndex(grid, column, row);
    for (usize step = 0; step < drainage.Direction.GetSize(); ++step)
    {
        if (drainage.Direction[sample] == TerrainFlowDirection::Outlet)
        {
            return sample;
        }
        const terrain::TerrainFlowOffset offset = terrain::GetTerrainFlowOffset(drainage.Direction[sample]);
        const int64 nextColumn = static_cast<int64>(sample % grid.Columns) + offset.X;
        const int64 nextRow = static_cast<int64>(sample / grid.Columns) + offset.Z;
        sample = GetSampleIndex(grid, static_cast<uint32>(nextColumn), static_cast<uint32>(nextRow));
    }
    FAIL("The routing has a cycle");
    return sample;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Branching basin across pages
// ---------------------------------------------------------------------------------------------------------------

namespace
{
/// The basin covers 3 by 3 coarse pages: 49 by 49 samples at one metre.
constexpr uint32 BASIN_COARSE_PAGES = 3;
/// The single outlet, on the domain's -Z border.
constexpr uint32 OUTLET_COLUMN = 24;
constexpr uint32 OUTLET_ROW = 0;
/// Where tributaries A and B join the main stem.
constexpr uint32 CONFLUENCE_COLUMN = 24;
constexpr uint32 CONFLUENCE_ROW = 18;
/// Channel heads; tributary A crosses the coarse page edges at x = 16 and z = 32, B at x = 32 and z = 32.
constexpr uint32 TRIBUTARY_A_HEAD_COLUMN = 6;
constexpr uint32 TRIBUTARY_B_HEAD_COLUMN = 42;
constexpr uint32 TRIBUTARY_HEAD_ROW = 42;
/// A raised trough between the two tributaries along z = 34, with its sill at x = 24.
constexpr float64 DIVIDE_ROW = 34;
constexpr float64 DIVIDE_WEST_X = 12;
constexpr float64 DIVIDE_EAST_X = 36;
constexpr float64 DIVIDE_SILL_X = 24;
/// The sill stands this far above the channel beds where the trough meets them.
constexpr float64 DIVIDE_SILL_RISE_METRES = 1.5;
/// The trough floor falls this many metres per metre from the sill toward each tributary.
constexpr float64 DIVIDE_FALL = 0.05;
constexpr float64 CHANNEL_BASE_METRES = 10;
/// Channel beds rise this many metres per metre of +Z.
constexpr float64 CHANNEL_GRADIENT = 0.1;
/// Valley walls rise this many metres per metre of distance from the nearest channel or trough.
constexpr float64 VALLEY_SIDE_SLOPE = 0.6;
/// Every border sample but the outlet, above the whole interior.
constexpr float32 RIM_METRES = 45;
/// A dam across tributary B, below the divide: field columns 20 through 47, rows 28 through 32.
constexpr uint32 DAM_FIRST_COLUMN = 20;
constexpr uint32 DAM_LAST_COLUMN = 47;
constexpr uint32 DAM_FIRST_ROW = 28;
constexpr uint32 DAM_LAST_ROW = 32;
/// The dam crest, above the divide's sill, so tributary B's upper reach spills across the divide into A.
constexpr float32 DAM_CREST_METRES = 24;
/// Contributing area that starts a channel: above the rim gullies and the divide trough, below either tributary.
constexpr float32 BASIN_CHANNEL_CATCHMENT_SQUARE_METRES = 100;
/// Hillslope samples whose decoded routing may pick the other of two near-tied neighbours.
constexpr usize MAXIMUM_NEAR_TIE_FLIPS = 8;

/// The main stem and the two tributaries.
constexpr usize BASIN_CHANNEL_COUNT = 3;

/// A straight channel from its upstream point to its downstream point.
struct ChannelLine final
{
    float64 UpstreamX = 0;
    float64 UpstreamZ = 0;
    float64 DownstreamX = 0;
    float64 DownstreamZ = 0;
};

/// Distance from (x, z) to the closest point of a line, and that point.
struct LineProximity final
{
    float64 Distance = 0;
    float64 ClosestX = 0;
    float64 ClosestZ = 0;
};

[[nodiscard]] LineProximity MeasureLineProximity(const ChannelLine& line, float64 x, float64 z)
{
    const float64 directionX = line.DownstreamX - line.UpstreamX;
    const float64 directionZ = line.DownstreamZ - line.UpstreamZ;
    const float64 lengthSquared = (directionX * directionX) + (directionZ * directionZ);
    const float64 projection =
        (((x - line.UpstreamX) * directionX) + ((z - line.UpstreamZ) * directionZ)) / lengthSquared;
    const float64 t = math::Clamp(projection, 0.0, 1.0);
    LineProximity proximity;
    proximity.ClosestX = line.UpstreamX + (t * directionX);
    proximity.ClosestZ = line.UpstreamZ + (t * directionZ);
    const float64 offsetX = x - proximity.ClosestX;
    const float64 offsetZ = z - proximity.ClosestZ;
    proximity.Distance = math::Sqrt((offsetX * offsetX) + (offsetZ * offsetZ));
    return proximity;
}

/// The basin: the lowest of a cone around each channel (bed rising with +Z) and around the divide trough (floor
/// falling from its sill to both tributaries), inside a rim broken only by the outlet.
[[nodiscard]] float32 ComputeBasinElevation(uint32 column, uint32 row, uint32 columns, uint32 rows)
{
    const bool isBorder = column == 0 || row == 0 || column + 1 == columns || row + 1 == rows;
    if (isBorder == true && (column != OUTLET_COLUMN || row != OUTLET_ROW))
    {
        return RIM_METRES;
    }
    const float64 x = static_cast<float64>(column);
    const float64 z = static_cast<float64>(row);
    const StaticArray<ChannelLine, BASIN_CHANNEL_COUNT> channels{{
        {CONFLUENCE_COLUMN, CONFLUENCE_ROW, OUTLET_COLUMN, OUTLET_ROW},
        {TRIBUTARY_A_HEAD_COLUMN, TRIBUTARY_HEAD_ROW, CONFLUENCE_COLUMN, CONFLUENCE_ROW},
        {TRIBUTARY_B_HEAD_COLUMN, TRIBUTARY_HEAD_ROW, CONFLUENCE_COLUMN, CONFLUENCE_ROW},
    }};
    float64 elevation = static_cast<float64>(RIM_METRES);
    for (const ChannelLine& channel : channels)
    {
        const LineProximity proximity = MeasureLineProximity(channel, x, z);
        const float64 bed = CHANNEL_BASE_METRES + (CHANNEL_GRADIENT * proximity.ClosestZ);
        elevation = math::Min(elevation, bed + (VALLEY_SIDE_SLOPE * proximity.Distance));
    }
    const ChannelLine divide{DIVIDE_WEST_X, DIVIDE_ROW, DIVIDE_EAST_X, DIVIDE_ROW};
    const LineProximity proximity = MeasureLineProximity(divide, x, z);
    const float64 sill = CHANNEL_BASE_METRES + (CHANNEL_GRADIENT * DIVIDE_ROW) + DIVIDE_SILL_RISE_METRES;
    const float64 floor = sill - (DIVIDE_FALL * math::Abs(proximity.ClosestX - DIVIDE_SILL_X));
    elevation = math::Min(elevation, floor + (VALLEY_SIDE_SLOPE * proximity.Distance));
    return static_cast<float32>(elevation);
}

void FillBasin(const terrain::TerrainRevisionLayout& layout, Field& outField)
{
    outField.Grid =
    {
        .Columns = (layout.PagesX * layout.CellsPerPage) + 1,
        .Rows = (layout.PagesZ * layout.CellsPerPage) + 1,
    };
    REQUIRE(outField.Elevations.TryResize(terrain::GetTerrainSampleCount(outField.Grid)));
    for (uint32 row = 0; row < outField.Grid.Rows; ++row)
    {
        for (uint32 column = 0; column < outField.Grid.Columns; ++column)
        {
            outField.Elevations[GetSampleIndex(outField.Grid, column, row)] =
                ComputeBasinElevation(column, row, outField.Grid.Columns, outField.Grid.Rows);
        }
    }
}

/// Raise the dam to its crest without lowering anything, as a flatten-up stamp would.
void StampDam(Field& inoutField)
{
    for (uint32 row = DAM_FIRST_ROW; row <= DAM_LAST_ROW; ++row)
    {
        for (uint32 column = DAM_FIRST_COLUMN; column <= DAM_LAST_COLUMN; ++column)
        {
            float32& elevation = inoutField.Elevations[GetSampleIndex(inoutField.Grid, column, row)];
            elevation = math::Max(elevation, DAM_CREST_METRES);
        }
    }
}

/// One river extraction, with storage for any network the basin can hold.
struct Network final
{
    Array<uint32> Points;
    Array<terrain::TerrainRiverSegment> Segments;
    terrain::TerrainRiverNetworkSummary Summary;
};

void ExtractNetwork(const terrain::TerrainFieldGrid& grid, const Drainage& drainage, Network& outNetwork)
{
    // Every point is a channel sample, plus one repeated confluence per segment.
    const usize samples = terrain::GetTerrainSampleCount(grid);
    REQUIRE(outNetwork.Points.TryResize(2 * samples));
    REQUIRE(outNetwork.Segments.TryResize(samples));
    terrain::TerrainRiverParameters parameters;
    parameters.MinimumCatchmentSquareMetres = BASIN_CHANNEL_CATCHMENT_SQUARE_METRES;
    REQUIRE(terrain::TryExtractTerrainRivers(grid,
                                             drainage.Direction.AsView(),
                                             drainage.Accumulation.AsView(),
                                             parameters,
                                             outNetwork.Points.AsView(),
                                             outNetwork.Segments.AsView(),
                                             outNetwork.Summary) == terrain::TerrainRiverStatus::Success);
}

[[nodiscard]] bool AreNetworksEqual(const Network& first, const Network& second)
{
    if (first.Summary.ChannelSamples != second.Summary.ChannelSamples ||
        first.Summary.PointCount != second.Summary.PointCount ||
        first.Summary.SegmentCount != second.Summary.SegmentCount ||
        first.Summary.OutletSegments != second.Summary.OutletSegments ||
        first.Summary.MaximumOrder != second.Summary.MaximumOrder)
    {
        return false;
    }
    for (usize i = 0; i < first.Summary.PointCount; ++i)
    {
        if (first.Points[i] != second.Points[i])
        {
            return false;
        }
    }
    for (usize i = 0; i < first.Summary.SegmentCount; ++i)
    {
        const terrain::TerrainRiverSegment& a = first.Segments[i];
        const terrain::TerrainRiverSegment& b = second.Segments[i];
        if (a.FirstPoint != b.FirstPoint || a.PointCount != b.PointCount ||
            a.DownstreamSegment != b.DownstreamSegment || a.Order != b.Order)
        {
            return false;
        }
    }
    return true;
}

/// Whether two solves accumulate contributing samples within a tolerance at every channel sample of a network.
[[nodiscard]] bool
AreChannelAccumulationsWithin(const Network& network, const Drainage& first, const Drainage& second, uint64 tolerance)
{
    for (usize i = 0; i < network.Summary.PointCount; ++i)
    {
        const uint64 a = first.Accumulation[network.Points[i]];
        const uint64 b = second.Accumulation[network.Points[i]];
        if ((a > b ? a - b : b - a) > tolerance)
        {
            return false;
        }
    }
    return true;
}

/// The branching structure: one outlet segment that starts at the confluence, with exactly two upstream segments
/// ending there, one reaching up each tributary's side of the basin. Returns the outlet segment's order.
[[nodiscard]] uint32 CheckConfluence(const terrain::TerrainFieldGrid& grid, const Network& network, usize outlet)
{
    const uint32 confluence = static_cast<uint32>(GetSampleIndex(grid, CONFLUENCE_COLUMN, CONFLUENCE_ROW));
    usize outletSegments = 0;
    uint32 outletSegment = terrain::TERRAIN_RIVER_NONE;
    for (usize i = 0; i < network.Summary.SegmentCount; ++i)
    {
        const terrain::TerrainRiverSegment& segment = network.Segments[i];
        if (segment.DownstreamSegment == terrain::TERRAIN_RIVER_NONE)
        {
            ++outletSegments;
            outletSegment = static_cast<uint32>(i);
            CHECK(network.Points[segment.FirstPoint] == confluence);
            CHECK(network.Points[segment.FirstPoint + segment.PointCount - 1] == outlet);
        }
    }
    REQUIRE(outletSegments == 1);
    usize westInflow = 0;
    usize eastInflow = 0;
    for (usize i = 0; i < network.Summary.SegmentCount; ++i)
    {
        const terrain::TerrainRiverSegment& segment = network.Segments[i];
        if (segment.DownstreamSegment != outletSegment)
        {
            continue;
        }
        CHECK(network.Points[segment.FirstPoint + segment.PointCount - 1] == confluence);
        const uint32 upstream = network.Points[segment.FirstPoint];
        if (upstream % grid.Columns < CONFLUENCE_COLUMN)
        {
            ++westInflow;
        }
        else
        {
            ++eastInflow;
        }
    }
    CHECK(westInflow == 1);
    CHECK(eastInflow == 1);
    return network.Segments[outletSegment].Order;
}

/// Check every channel sample on a page edge, and both ends of every channel step that crosses one, against each
/// page whose core or halo holds them: the pages must decode identical heights within half a quantum of the source.
/// Returns the number of channel steps that cross between owning pages.
[[nodiscard]] usize CheckChannelPageEdges(const Field& source, const DecodedRevision& decoded, const Network& network)
{
    const terrain::TerrainRevisionLayout& layout = decoded.Manifest.GetLayout();
    const terrain::TerrainFieldGrid& grid = source.Grid;
    const usize pageCount = GetPageCount(layout);
    const auto checkSample = [&](uint32 sample) {
        const uint32 column = sample % grid.Columns;
        const uint32 row = sample / grid.Columns;
        const float64 owned = ReadPageSample(decoded, GetOwningPage(layout, {column, row}), {column, row});
        CHECK(math::Abs(owned - static_cast<float64>(source.Elevations[sample])) <= HALF_QUANTUM_METRES);
        for (usize page = 0; page < pageCount; ++page)
        {
            if (IsInsidePageWindow(layout, GetPageLocalSample(layout, page, {column, row}), HALO) == true)
            {
                CHECK(ReadPageSample(decoded, page, {column, row}) == owned);
            }
        }
    };
    usize crossings = 0;
    for (usize segment = 0; segment < network.Summary.SegmentCount; ++segment)
    {
        const terrain::TerrainRiverSegment& reach = network.Segments[segment];
        for (uint32 point = 0; point < reach.PointCount; ++point)
        {
            const uint32 sample = network.Points[reach.FirstPoint + point];
            const uint32 column = sample % grid.Columns;
            const uint32 row = sample / grid.Columns;
            if (column % layout.CellsPerPage == 0 || row % layout.CellsPerPage == 0)
            {
                checkSample(sample);
            }
            if (point + 1 == reach.PointCount)
            {
                continue;
            }
            const uint32 next = network.Points[reach.FirstPoint + point + 1];
            const usize upstreamPage = GetOwningPage(layout, {column, row});
            const usize downstreamPage = GetOwningPage(layout, {next % grid.Columns, next / grid.Columns});
            if (upstreamPage != downstreamPage)
            {
                ++crossings;
                // A D8 step moves one sample, so each end lies in the other page's one-sample halo.
                checkSample(sample);
                checkSample(next);
            }
        }
    }
    return crossings;
}

/// Whether the drainage path from a sample reaches the confluence after passing west of the divide sill.
[[nodiscard]] bool
IsRoutedAcrossDivide(const terrain::TerrainFieldGrid& grid, const Drainage& drainage, uint32 column, uint32 row)
{
    usize sample = GetSampleIndex(grid, column, row);
    bool hasCrossedDivide = false;
    for (usize step = 0; step < drainage.Direction.GetSize(); ++step)
    {
        const uint32 sampleColumn = static_cast<uint32>(sample % grid.Columns);
        const uint32 sampleRow = static_cast<uint32>(sample / grid.Columns);
        if (static_cast<float64>(sampleRow) >= DIVIDE_ROW - 1 && static_cast<float64>(sampleColumn) < DIVIDE_SILL_X)
        {
            hasCrossedDivide = true;
        }
        if (sample == GetSampleIndex(grid, CONFLUENCE_COLUMN, CONFLUENCE_ROW) ||
            drainage.Direction[sample] == TerrainFlowDirection::Outlet)
        {
            return hasCrossedDivide;
        }
        const terrain::TerrainFlowOffset offset = terrain::GetTerrainFlowOffset(drainage.Direction[sample]);
        sample = GetSampleIndex(grid,
                                static_cast<uint32>(static_cast<int64>(sampleColumn) + offset.X),
                                static_cast<uint32>(static_cast<int64>(sampleRow) + offset.Z));
    }
    FAIL("The routing has a cycle");
    return false;
}
} // namespace

TEST_CASE("A branching basin drains identically through cooked pages at every partition and order",
          "[terrain][cook][drainage]")
{
    const terrain::TerrainRevisionLayout coarseLayout =
        MakeLayout({BASIN_COARSE_PAGES, BASIN_COARSE_PAGES, COARSE_PAGE_CELLS}, BASE_REVISION);
    const terrain::TerrainRevisionLayout fineLayout =
        MakeLayout({BASIN_COARSE_PAGES, BASIN_COARSE_PAGES, FINE_PAGE_CELLS}, BASE_REVISION);
    Field source;
    FillBasin(coarseLayout, source);
    const terrain::TerrainFieldGrid& grid = source.Grid;
    const usize samples = terrain::GetTerrainSampleCount(grid);
    const usize outlet = GetSampleIndex(grid, OUTLET_COLUMN, OUTLET_ROW);
    const usize confluence = GetSampleIndex(grid, CONFLUENCE_COLUMN, CONFLUENCE_ROW);

    // The control: drainage and rivers of the complete source field. Everything drains to the one outlet, and the
    // network joins one tributary from each side of the basin in an order-two main stem.
    Drainage control;
    ComputeDrainage(grid, source.Elevations, control);
    CHECK(control.Summary.OutletSamples == 1);
    CHECK(control.Direction[outlet] == TerrainFlowDirection::Outlet);
    CHECK(control.Accumulation[outlet] == samples);
    CHECK(control.Summary.OutletAccumulation == samples);
    Network controlNetwork;
    ExtractNetwork(grid, control, controlNetwork);
    CHECK(controlNetwork.Summary.OutletSegments == 1);
    CHECK(CheckConfluence(grid, controlNetwork, outlet) == 2);
    CHECK(IsRoutedAcrossDivide(grid, control, TRIBUTARY_B_HEAD_COLUMN, TRIBUTARY_HEAD_ROW) == false);

    // Cook serially at coarse pages and shuffled at both partitions; shuffling never changes the bytes, and both
    // partitions decode the same field.
    terrain::TerrainCookedRevision coarseRevision;
    REQUIRE(terrain::TryCookTerrainRevision(coarseLayout, grid, source.Elevations.AsView(), coarseRevision) ==
            TerrainCookStatus::Success);
    terrain::TerrainCookedRevision coarseShuffled;
    CookShuffled(coarseLayout, source, coarseShuffled);
    CHECK(AreBytesEqual(coarseShuffled.GetManifestBytes(), coarseRevision.GetManifestBytes()));
    CHECK(AreBytesEqual(coarseShuffled.GetPagePackBytes(), coarseRevision.GetPagePackBytes()));
    terrain::TerrainCookedRevision fineShuffled;
    CookShuffled(fineLayout, source, fineShuffled);

    DecodedRevision coarseDecoded;
    DecodedRevision fineDecoded;
    DecodeRevision(coarseShuffled, grid, coarseDecoded);
    DecodeRevision(fineShuffled, grid, fineDecoded);
    for (usize i = 0; i < samples; ++i)
    {
        CHECK(coarseDecoded.Elevations[i] == fineDecoded.Elevations[i]);
    }
    CHECK(GetMaximumDecodeError(source, coarseDecoded) <= HALF_QUANTUM_METRES);

    // Drainage over the reassembled decoded field keeps the control's connectivity: the same branching network,
    // the same accumulation at every channel sample, confluence and outlet. A few hillslope samples whose two
    // steepest drops differ by less than the codec can resolve pick the other neighbour; each is a near-tie.
    Drainage decodedDrainage;
    ComputeDrainage(grid, coarseDecoded.Elevations, decodedDrainage);
    CHECK(decodedDrainage.Summary.OutletSamples == 1);
    CHECK(decodedDrainage.Accumulation[outlet] == control.Accumulation[outlet]);
    CHECK(decodedDrainage.Accumulation[confluence] == control.Accumulation[confluence]);
    Network decodedNetwork;
    ExtractNetwork(grid, decodedDrainage, decodedNetwork);
    CHECK(AreNetworksEqual(decodedNetwork, controlNetwork));
    const RoutingFlips flips = FindNearTieFlips(grid, control, decodedDrainage);
    CHECK(flips.Count <= MAXIMUM_NEAR_TIE_FLIPS);
    CHECK(AreChannelAccumulationsWithin(controlNetwork, decodedDrainage, control, flips.CarriedSamples));

    // Channel heights agree across every page edge the network crosses, at both partitions.
    CHECK(CheckChannelPageEdges(source, coarseDecoded, controlNetwork) >= 4);
    CHECK(CheckChannelPageEdges(source, fineDecoded, controlNetwork) >= 8);

    // Commit the dam: tributary B's upper reach ponds behind it and spills across the divide into A.
    Field edited;
    FillBasin(coarseLayout, edited);
    StampDam(edited);
    const terrain::TerrainSampleAddress origin = GetDomainOrigin(coarseLayout);
    const terrain::TerrainSampleRegion stamp{
        {origin.X + DAM_FIRST_COLUMN, origin.Z + DAM_FIRST_ROW},
        {origin.X + DAM_LAST_COLUMN, origin.Z + DAM_LAST_ROW},
    };
    Drainage editedControl;
    ComputeDrainage(grid, edited.Elevations, editedControl);
    CHECK(IsRoutedAcrossDivide(grid, editedControl, TRIBUTARY_B_HEAD_COLUMN, TRIBUTARY_HEAD_ROW) == true);
    CHECK(editedControl.Accumulation[outlet] == samples);
    Network editedControlNetwork;
    ExtractNetwork(grid, editedControl, editedControlNetwork);
    // Tributary B's lower reach still joins at the confluence, now fed only from below the dam.
    CHECK(CheckConfluence(grid, editedControlNetwork, outlet) == 2);

    for (const uint32 cellsPerPage : {COARSE_PAGE_CELLS, FINE_PAGE_CELLS})
    {
        DYNAMIC_SECTION("Recook at " << cellsPerPage << "-cell pages")
        {
            terrain::TerrainRevisionLayout baseLayout =
                MakeLayout({BASIN_COARSE_PAGES, BASIN_COARSE_PAGES, cellsPerPage}, BASE_REVISION);
            terrain::TerrainCookedRevision baseRevision;
            CookShuffled(baseLayout, source, baseRevision);
            terrain::TerrainManifest baseManifest;
            REQUIRE(terrain::TryReadTerrainManifest(baseRevision.GetManifestBytes(), baseManifest) ==
                    TerrainCookStatus::Success);
            DecodedRevision baseDecoded;
            DecodeRevision(baseRevision, grid, baseDecoded);
            Drainage baseDrainage;
            ComputeDrainage(grid, baseDecoded.Elevations, baseDrainage);

            terrain::TerrainRevisionLayout editedLayout =
                MakeLayout({BASIN_COARSE_PAGES, BASIN_COARSE_PAGES, cellsPerPage}, EDITED_REVISION);
            terrain::TerrainCookedRevision recooked;
            terrain::TerrainRecookSummary summary;
            REQUIRE(terrain::TryRecookTerrainRevision(baseManifest,
                                                      baseRevision.GetPagePackBytes(),
                                                      editedLayout,
                                                      grid,
                                                      edited.Elevations.AsView(),
                                                      stamp,
                                                      recooked,
                                                      summary) == TerrainCookStatus::Success);
            CHECK(summary.RecookedPages > 0);
            CHECK(summary.ReusedPages > 0);
            Array<uint32> editPages;
            REQUIRE(editPages.TryResize(GetPageCount(editedLayout)));
            usize editPageCount = 0;
            REQUIRE(terrain::TryCollectTerrainEditPages(editedLayout, stamp, editPages.AsView(), editPageCount) ==
                    TerrainCookStatus::Success);
            CHECK(editPageCount == summary.RecookedPages);

            // The recooked revision decodes exactly as a clean cook of the final terrain.
            terrain::TerrainCookedRevision clean;
            CookShuffled(editedLayout, edited, clean);
            DecodedRevision recookedDecoded;
            DecodedRevision cleanDecoded;
            DecodeRevision(recooked, grid, recookedDecoded);
            DecodeRevision(clean, grid, cleanDecoded);
            for (usize i = 0; i < GetPageCount(editedLayout); ++i)
            {
                CHECK(recookedDecoded.Manifest.GetPageRecords()[i].PayloadDigest ==
                      cleanDecoded.Manifest.GetPageRecords()[i].PayloadDigest);
            }
            for (usize i = 0; i < samples; ++i)
            {
                CHECK(recookedDecoded.Elevations[i] == cleanDecoded.Elevations[i]);
            }
            CHECK(GetMaximumDecodeError(edited, recookedDecoded) <= HALF_QUANTUM_METRES);

            // Drainage over the recooked pages equals drainage over the clean cook, including the confluence and
            // outlet, and keeps the final control's connectivity.
            Drainage recookedDrainage;
            Drainage cleanDrainage;
            ComputeDrainage(grid, recookedDecoded.Elevations, recookedDrainage);
            ComputeDrainage(grid, cleanDecoded.Elevations, cleanDrainage);
            CHECK(AreRoutingsEqual(recookedDrainage, cleanDrainage));
            CHECK(recookedDrainage.Accumulation[confluence] == cleanDrainage.Accumulation[confluence]);
            CHECK(recookedDrainage.Accumulation[outlet] == samples);
            Network recookedNetwork;
            ExtractNetwork(grid, recookedDrainage, recookedNetwork);
            CHECK(AreNetworksEqual(recookedNetwork, editedControlNetwork));
            const RoutingFlips editedFlips = FindNearTieFlips(grid, editedControl, recookedDrainage);
            CHECK(editedFlips.Count <= MAXIMUM_NEAR_TIE_FLIPS);
            CHECK(AreChannelAccumulationsWithin(editedControlNetwork,
                                                recookedDrainage,
                                                editedControl,
                                                editedFlips.CarriedSamples));
            CHECK(IsRoutedAcrossDivide(grid, recookedDrainage, TRIBUTARY_B_HEAD_COLUMN, TRIBUTARY_HEAD_ROW) == true);
            CHECK(CheckChannelPageEdges(edited, recookedDecoded, editedControlNetwork) > 0);

            // The diversion changes accumulation on pages the edit never reached, downstream along tributary A:
            // drainage is a regional product and must be recomputed over the whole field.
            usize changedOutsideEdit = 0;
            for (uint32 row = 0; row < grid.Rows; ++row)
            {
                for (uint32 column = 0; column < grid.Columns; ++column)
                {
                    const usize sample = GetSampleIndex(grid, column, row);
                    const usize page = GetOwningPage(editedLayout, {column, row});
                    const bool isRecooked =
                        recookedDecoded.Manifest.GetPageRecords()[page].ContentRevision == EDITED_REVISION;
                    if (isRecooked == false &&
                        recookedDrainage.Accumulation[sample] != baseDrainage.Accumulation[sample])
                    {
                        ++changedOutsideEdit;
                    }
                }
            }
            CHECK(changedOutsideEdit > 0);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Shallow saddle on a page boundary
// ---------------------------------------------------------------------------------------------------------------

namespace
{
/// The saddle domain covers 3 by 2 coarse pages: 49 by 33 samples at one metre.
constexpr uint32 SADDLE_COARSE_PAGES_X = 3;
constexpr uint32 SADDLE_COARSE_PAGES_Z = 2;
/// A closed lake west of a ridge on field column 16, a page edge at both partitions.
constexpr uint32 RIDGE_COLUMN = 16;
constexpr uint32 LAKE_CENTRE_COLUMN = 8;
constexpr uint32 LAKE_CENTRE_ROW = 16;
constexpr float32 LAKE_FLOOR_METRES = 5;
/// The lake floor rises this many metres per metre from its centre, so it has a single sink.
constexpr float32 LAKE_FLOOR_RISE = 0.125F;
/// Ridge, rim and every sample outside the lake and the spill channels.
constexpr float32 HIGHLAND_METRES = 40;
/// The two passes on the ridge. The first has the lower sample index, so it wins a tie in the flood's order.
constexpr uint32 FIRST_PASS_ROW = 10;
constexpr uint32 SECOND_PASS_ROW = 22;
/// Each spill channel runs from its pass along its row to an outlet on the +X border, falling this much per metre.
constexpr float32 SPILL_CHANNEL_FALL = 0.25F;
/// Pass heights are set relative to this exact code boundary.
constexpr float32 PASS_BASE_METRES = 20;

/// Pass height pairs on either side of one quantum.
constexpr usize SADDLE_CASE_COUNT = 4;

/// Two pass heights and the routing they produce through the decoded pages.
struct SaddleCase final
{
    const char* Name = "";
    float32 FirstPassMetres = 0;
    float32 SecondPassMetres = 0;
    /// Whether both passes quantize to one code.
    bool IsTieExpected = false;
    /// Whether the decoded lake drains through a different pass than the source lake.
    bool ShouldOutletChange = false;
    /// Without an outlet change, the samples the decoded routing moves from the other outlet to the expected one.
    int64 ExpectedOutletGain = 0;
};

[[nodiscard]] float32 ComputeSaddleElevation(const SaddleCase& saddle, uint32 column, uint32 row, uint32 rows)
{
    const bool isLake = column >= 1 && column < RIDGE_COLUMN && row >= 1 && row + 1 < rows;
    if (isLake == true)
    {
        const uint32 columnOffset =
            column > LAKE_CENTRE_COLUMN ? column - LAKE_CENTRE_COLUMN : LAKE_CENTRE_COLUMN - column;
        const uint32 rowOffset = row > LAKE_CENTRE_ROW ? row - LAKE_CENTRE_ROW : LAKE_CENTRE_ROW - row;
        return LAKE_FLOOR_METRES + (LAKE_FLOOR_RISE * static_cast<float32>(columnOffset + rowOffset));
    }
    if (column >= RIDGE_COLUMN && (row == FIRST_PASS_ROW || row == SECOND_PASS_ROW))
    {
        const float32 pass = row == FIRST_PASS_ROW ? saddle.FirstPassMetres : saddle.SecondPassMetres;
        return pass - (SPILL_CHANNEL_FALL * static_cast<float32>(column - RIDGE_COLUMN));
    }
    return HIGHLAND_METRES;
}

void FillSaddle(const terrain::TerrainRevisionLayout& layout, const SaddleCase& saddle, Field& outField)
{
    outField.Grid =
    {
        .Columns = (layout.PagesX * layout.CellsPerPage) + 1,
        .Rows = (layout.PagesZ * layout.CellsPerPage) + 1,
    };
    REQUIRE(outField.Elevations.TryResize(terrain::GetTerrainSampleCount(outField.Grid)));
    for (uint32 row = 0; row < outField.Grid.Rows; ++row)
    {
        for (uint32 column = 0; column < outField.Grid.Columns; ++column)
        {
            outField.Elevations[GetSampleIndex(outField.Grid, column, row)] =
                ComputeSaddleElevation(saddle, column, row, outField.Grid.Rows);
        }
    }
}
} // namespace

TEST_CASE("A shallow saddle on a page edge keeps its spill route only when the passes differ by over one quantum",
          "[terrain][cook][drainage]")
{
    constexpr float32 QUARTER = QUANTUM_METRES / 4;
    constexpr float32 EIGHTH = QUANTUM_METRES / 8;
    // Rounding to the nearest code is monotone, so quantization can merge two pass heights into one code but never
    // reverse them; a merged pair then resolves by the flood's tie rule, ascending sample index.
    const StaticArray<SaddleCase, SADDLE_CASE_COUNT> cases{{
        {"passes 1.25 quanta apart", PASS_BASE_METRES + QUANTUM_METRES, PASS_BASE_METRES - QUARTER, false, false, 0},
        {"passes half a quantum apart in one code, the tie favouring the other route",
         PASS_BASE_METRES + QUARTER,
         PASS_BASE_METRES - QUARTER,
         true,
         true,
         0},
        {"passes a quarter quantum apart across a code boundary",
         PASS_BASE_METRES + (5 * EIGHTH),
         PASS_BASE_METRES + (3 * EIGHTH),
         false,
         false,
         0},
        {"passes half a quantum apart in one code, the tie favouring the same route",
         PASS_BASE_METRES - QUARTER,
         PASS_BASE_METRES + QUARTER,
         true,
         false,
         -1},
    }};
    for (const SaddleCase& saddle : cases)
    {
        DYNAMIC_SECTION(saddle.Name)
        {
            const terrain::TerrainRevisionLayout coarseLayout =
                MakeLayout({SADDLE_COARSE_PAGES_X, SADDLE_COARSE_PAGES_Z, COARSE_PAGE_CELLS}, BASE_REVISION);
            const terrain::TerrainRevisionLayout fineLayout =
                MakeLayout({SADDLE_COARSE_PAGES_X, SADDLE_COARSE_PAGES_Z, FINE_PAGE_CELLS}, BASE_REVISION);
            Field source;
            FillSaddle(coarseLayout, saddle, source);
            const terrain::TerrainFieldGrid& grid = source.Grid;
            const usize samples = terrain::GetTerrainSampleCount(grid);
            const usize firstOutlet = GetSampleIndex(grid, grid.Columns - 1, FIRST_PASS_ROW);
            const usize secondOutlet = GetSampleIndex(grid, grid.Columns - 1, SECOND_PASS_ROW);
            const usize firstPass = GetSampleIndex(grid, RIDGE_COLUMN, FIRST_PASS_ROW);
            const usize secondPass = GetSampleIndex(grid, RIDGE_COLUMN, SECOND_PASS_ROW);

            // The expected outlet, declared on the unquantized field: the lake spills over the lower pass.
            const bool isFirstPassLower = saddle.FirstPassMetres < saddle.SecondPassMetres;
            const usize expectedOutlet = isFirstPassLower == true ? firstOutlet : secondOutlet;
            const usize otherOutlet = isFirstPassLower == true ? secondOutlet : firstOutlet;
            Drainage control;
            ComputeDrainage(grid, source.Elevations, control);
            CHECK(TraceToOutlet(grid, control, LAKE_CENTRE_COLUMN, LAKE_CENTRE_ROW) == expectedOutlet);
            const uint32 lakeSamples = (RIDGE_COLUMN - 1) * (grid.Rows - 2);
            CHECK(control.Accumulation[expectedOutlet] > lakeSamples);
            CHECK(control.Accumulation[otherOutlet] < lakeSamples);

            // Cook at both partitions in shuffled order, plus a serial coarse cook; every cook decodes one field,
            // within half a quantum of the source everywhere, and both pages on the ridge agree on each pass.
            terrain::TerrainCookedRevision serial;
            REQUIRE(terrain::TryCookTerrainRevision(coarseLayout, grid, source.Elevations.AsView(), serial) ==
                    TerrainCookStatus::Success);
            terrain::TerrainCookedRevision coarseShuffled;
            terrain::TerrainCookedRevision fineShuffled;
            CookShuffled(coarseLayout, source, coarseShuffled);
            CookShuffled(fineLayout, source, fineShuffled);
            CHECK(AreBytesEqual(coarseShuffled.GetManifestBytes(), serial.GetManifestBytes()));
            CHECK(AreBytesEqual(coarseShuffled.GetPagePackBytes(), serial.GetPagePackBytes()));
            DecodedRevision coarseDecoded;
            DecodedRevision fineDecoded;
            DecodeRevision(coarseShuffled, grid, coarseDecoded);
            DecodeRevision(fineShuffled, grid, fineDecoded);
            for (usize i = 0; i < samples; ++i)
            {
                CHECK(coarseDecoded.Elevations[i] == fineDecoded.Elevations[i]);
            }
            CHECK(GetMaximumDecodeError(source, coarseDecoded) <= HALF_QUANTUM_METRES);
            for (const DecodedRevision* decodedRevision : {&coarseDecoded, &fineDecoded})
            {
                const terrain::TerrainRevisionLayout& layout = decodedRevision->Manifest.GetLayout();
                for (const uint32 passRow : {FIRST_PASS_ROW, SECOND_PASS_ROW})
                {
                    const usize westPage = GetOwningPage(layout, {RIDGE_COLUMN - 1, passRow});
                    const usize eastPage = GetOwningPage(layout, {RIDGE_COLUMN, passRow});
                    REQUIRE(westPage != eastPage);
                    CHECK(ReadPageSample(*decodedRevision, westPage, {RIDGE_COLUMN, passRow}) ==
                          ReadPageSample(*decodedRevision, eastPage, {RIDGE_COLUMN, passRow}));
                }
            }
            const bool isTie = coarseDecoded.Elevations[firstPass] == coarseDecoded.Elevations[secondPass];
            CHECK(isTie == saddle.IsTieExpected);

            // The diagnostic: drainage over the decoded field. It is identical at every partition and order, but
            // the route follows the lower decoded pass, or the first pass on a tie.
            Drainage coarseDrainage;
            Drainage fineDrainage;
            ComputeDrainage(grid, coarseDecoded.Elevations, coarseDrainage);
            ComputeDrainage(grid, fineDecoded.Elevations, fineDrainage);
            CHECK(AreRoutingsEqual(coarseDrainage, fineDrainage));
            const bool isFirstDecodedPassTaken =
                coarseDecoded.Elevations[firstPass] <= coarseDecoded.Elevations[secondPass];
            const usize decodedOutlet = isFirstDecodedPassTaken == true ? firstOutlet : secondOutlet;
            CHECK(TraceToOutlet(grid, coarseDrainage, LAKE_CENTRE_COLUMN, LAKE_CENTRE_ROW) == decodedOutlet);
            CHECK((decodedOutlet != expectedOutlet) == saddle.ShouldOutletChange);
            CHECK(coarseDrainage.Summary.OutletAccumulation == samples);
            if (saddle.ShouldOutletChange == true)
            {
                // The whole lake catchment moves to the other outlet although every height passed its tolerance.
                CHECK(coarseDrainage.Accumulation[expectedOutlet] < lakeSamples);
                CHECK(coarseDrainage.Accumulation[otherOutlet] > lakeSamples);
                CHECK(AreRoutingsEqual(coarseDrainage, control) == false);
            }
            else
            {
                const int64 gain = static_cast<int64>(coarseDrainage.Accumulation[expectedOutlet]) -
                                   static_cast<int64>(control.Accumulation[expectedOutlet]);
                CHECK(gain == saddle.ExpectedOutletGain);
                CHECK(static_cast<int64>(coarseDrainage.Accumulation[otherOutlet]) -
                          static_cast<int64>(control.Accumulation[otherOutlet]) ==
                      -saddle.ExpectedOutletGain);
            }
            if (saddle.ExpectedOutletGain != 0)
            {
                // The ridge sample beside the second pass drops equally into the lake and onto the pass once both
                // decode to one code, and D8's direction order then sends it over the pass instead of the lake.
                const usize ridge = GetSampleIndex(grid, RIDGE_COLUMN, SECOND_PASS_ROW - 1);
                CHECK(control.Direction[ridge] == TerrainFlowDirection::NegativeX);
                CHECK(coarseDrainage.Direction[ridge] == TerrainFlowDirection::PositiveZ);
            }
        }
    }
}
