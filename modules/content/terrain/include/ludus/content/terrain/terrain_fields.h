#pragma once

// Derived terrain fields of slice T1: slope, finite-domain D8 drainage over a
// depression-filled surface, and a topographic wetness field. Every operation
// is a serial reference over one complete grid: a streamed page is not a
// hydrology boundary, so callers pass the whole drainage domain. Contracts:
// docs/architecture/terrain-generation.md#erosion-and-hydrology-domains and
// docs/modules/content/terrain.md.
//
// Thanks to John F. O'Callaghan and David M. Mark, "The extraction of drainage
// networks from digital elevation data", Computer Vision, Graphics, and Image
// Processing 28(3), pp. 323-344, 1984 (https://doi.org/10.1016/S0734-189X(84)80011-0),
// for steepest-descent (D8) routing and contributing-area accumulation. Thanks
// to Richard Barnes, Clarence Lehman and David Mulla, "Priority-Flood: An
// optimal depression-filling and watershed-labeling algorithm for digital
// elevation models", Computers & Geosciences 62, pp. 117-127, 2014
// (https://doi.org/10.1016/j.cageo.2013.04.024), for filling depressions from
// the domain border with a priority queue. Ludus departs by keeping the flood's
// spanning forest as the deterministic route across flats instead of adding an
// epsilon gradient, so filled heights stay exact spill levels. Thanks to Keith
// J. Beven and Mike J. Kirkby, "A physically based, variable contributing area
// model of basin hydrology", Hydrological Sciences Bulletin 24(1), pp. 43-69,
// 1979 (https://doi.org/10.1080/02626667909491834), for the wetness index
// ln(a / tan b). No source code is copied from these works.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::content::terrain
{
using foundation::core::float32;
using foundation::core::int32;
using foundation::core::uint32;
using foundation::core::uint64;
using foundation::core::uint8;
using foundation::core::usize;

/// Version of the neighbour order, fill rule, routing and wetness formulas below; store it with cooked fields.
inline constexpr uint32 TERRAIN_FIELDS_LAYOUT_VERSION = 1;
/// Most samples on one grid axis (4096 cells plus the shared edge).
inline constexpr uint32 MAX_TERRAIN_FIELD_AXIS_SAMPLES = 4097;
/// Fewest samples on one grid axis.
inline constexpr uint32 MIN_TERRAIN_FIELD_AXIS_SAMPLES = 2;
/// Neighbours a sample can drain to.
inline constexpr uint32 TERRAIN_FLOW_NEIGHBOURS = 8;

/// @brief Regular sample grid shared by every field: Columns along X, Rows along Z, row-major storage.
struct TerrainFieldGrid final
{
    /// Samples along X, MIN_TERRAIN_FIELD_AXIS_SAMPLES through MAX_TERRAIN_FIELD_AXIS_SAMPLES.
    uint32 Columns = 0;
    /// Samples along Z, MIN_TERRAIN_FIELD_AXIS_SAMPLES through MAX_TERRAIN_FIELD_AXIS_SAMPLES.
    uint32 Rows = 0;
    /// Distance between neighbouring samples along either axis, in metres; finite and positive.
    float32 SpacingMetres = 1;
};

/// @brief Samples in a grid; Columns times Rows, without range checks.
[[nodiscard]] constexpr usize GetTerrainSampleCount(const TerrainFieldGrid& grid) noexcept
{
    return static_cast<usize>(grid.Columns) * grid.Rows;
}

/// @brief Whether both axes lie within the admitted sample range and the spacing is finite and positive.
[[nodiscard]] bool IsTerrainFieldGridValid(const TerrainFieldGrid& grid) noexcept;

/// @brief Outcome of a field operation. Any failure leaves every output view unchanged.
enum class TerrainFieldStatus : uint8
{
    /// The operation succeeded.
    Success,
    /// The grid fails IsTerrainFieldGridValid.
    InvalidGrid,
    /// An input or output view is invalid or does not hold one value per sample.
    InvalidStorage,
    /// An input sample or parameter is NaN or infinite.
    NonFiniteInput,
    /// A parameter violates its documented range.
    InvalidArgument,
    /// Scratch storage could not be allocated.
    AllocationFailed,
};

/// @brief Where a sample drains. The first eight are the D8 neighbours in counter-clockwise order from +X.
enum class TerrainFlowDirection : uint8
{
    /// Column + 1.
    PositiveX,
    /// Column + 1, row + 1.
    PositiveXPositiveZ,
    /// Row + 1.
    PositiveZ,
    /// Column - 1, row + 1.
    NegativeXPositiveZ,
    /// Column - 1.
    NegativeX,
    /// Column - 1, row - 1.
    NegativeXNegativeZ,
    /// Row - 1.
    NegativeZ,
    /// Column + 1, row - 1.
    PositiveXNegativeZ,
    /// The sample leaves the domain; only border samples drain out.
    Outlet,
};

/// @brief Column and row step of one flow direction.
struct TerrainFlowOffset final
{
    /// Column step, -1 through 1.
    int32 X = 0;
    /// Row step, -1 through 1.
    int32 Z = 0;
};

/// @brief Column and row step of a direction; Outlet steps nowhere.
[[nodiscard]] constexpr TerrainFlowOffset GetTerrainFlowOffset(TerrainFlowDirection direction) noexcept
{
    switch (direction)
    {
        case TerrainFlowDirection::PositiveX:
            return {1, 0};
        case TerrainFlowDirection::PositiveXPositiveZ:
            return {1, 1};
        case TerrainFlowDirection::PositiveZ:
            return {0, 1};
        case TerrainFlowDirection::NegativeXPositiveZ:
            return {-1, 1};
        case TerrainFlowDirection::NegativeX:
            return {-1, 0};
        case TerrainFlowDirection::NegativeXNegativeZ:
            return {-1, -1};
        case TerrainFlowDirection::NegativeZ:
            return {0, -1};
        case TerrainFlowDirection::PositiveXNegativeZ:
            return {1, -1};
        case TerrainFlowDirection::Outlet:
            break;
    }
    return {0, 0};
}

/// @brief Slope magnitude (rise over run, the tangent of the slope angle) from central differences.
///
/// Border samples use one-sided differences. Elevations must be finite.
/// @param grid The sample grid.
/// @param elevationMetres One elevation per sample, in metres.
/// @param outSlope One dimensionless slope per sample; written only on Success.
[[nodiscard]] TerrainFieldStatus TryComputeTerrainSlope(const TerrainFieldGrid& grid,
                                                        foundation::BorrowedView<const float32> elevationMetres,
                                                        foundation::BorrowedView<float32> outSlope) noexcept;

/// @brief Diagnostics of one drainage solve.
struct TerrainDrainageSummary final
{
    /// Samples whose filled elevation exceeds their input elevation.
    usize FilledSamples = 0;
    /// Largest fill depth, in metres.
    float32 MaximumFillMetres = 0;
    /// Samples that drain out of the domain.
    usize OutletSamples = 0;
    /// Sum of the accumulation at the outlets; equals the sample count, since every sample drains out.
    uint64 OutletAccumulation = 0;
};

/// @brief Fill depressions from the border, route every sample downhill by D8 and count contributing samples.
///
/// Depressions fill to their spill level; flats and filled lakes drain along the flood's spanning forest toward
/// their spill point, so the routing has no cycles and every sample reaches an Outlet. Ties between equally steep
/// neighbours resolve in TerrainFlowDirection order. The result depends only on the inputs, never on scratch
/// or traversal order. Accumulation counts the sample itself plus every sample upstream of it.
/// @param grid The complete drainage domain.
/// @param elevationMetres One finite elevation per sample, in metres.
/// @param outFilledElevationMetres The depression-filled surface; at least the input elevation.
/// @param outFlowDirection Where each sample drains.
/// @param outAccumulation Contributing samples including the sample itself, at least one.
/// @param outSummary Fill and outlet diagnostics.
/// @return Success, or a failure that leaves every output unchanged.
[[nodiscard]] TerrainFieldStatus
TryComputeTerrainDrainage(const TerrainFieldGrid& grid,
                          foundation::BorrowedView<const float32> elevationMetres,
                          foundation::BorrowedView<float32> outFilledElevationMetres,
                          foundation::BorrowedView<TerrainFlowDirection> outFlowDirection,
                          foundation::BorrowedView<uint32> outAccumulation,
                          TerrainDrainageSummary& outSummary) noexcept;

/// Default wetness index mapped to 0: a one-sample catchment on a 1:7 slope at one metre spacing.
inline constexpr float32 DEFAULT_TERRAIN_WETNESS_MINIMUM_INDEX = 2;
/// Default wetness index mapped to 1: roughly a 160,000 m catchment on a 1:1 slope at one metre spacing.
inline constexpr float32 DEFAULT_TERRAIN_WETNESS_MAXIMUM_INDEX = 12;
/// Default slope floor: one millimetre of rise per metre, so flats keep a finite index.
inline constexpr float32 DEFAULT_TERRAIN_WETNESS_MINIMUM_SLOPE = 0.001F;

/// @brief Declared bounds that map the topographic wetness index onto [0, 1].
struct TerrainWetnessParameters final
{
    /// Index value mapped to wetness 0; finite.
    float32 MinimumIndex = DEFAULT_TERRAIN_WETNESS_MINIMUM_INDEX;
    /// Index value mapped to wetness 1; finite and above MinimumIndex.
    float32 MaximumIndex = DEFAULT_TERRAIN_WETNESS_MAXIMUM_INDEX;
    /// Smallest slope used in the quotient, so flat samples stay finite; finite and positive.
    float32 MinimumSlope = DEFAULT_TERRAIN_WETNESS_MINIMUM_SLOPE;
};

/// @brief Wetness in [0, 1] from ln(a / tan b): specific catchment area over slope, remapped by the parameters.
///
/// The specific catchment area is accumulation times spacing (upslope area over the contour length of one
/// sample). Slopes below MinimumSlope are clamped up to it.
/// @param grid The sample grid.
/// @param slope Dimensionless slope per sample, as TryComputeTerrainSlope writes it.
/// @param accumulation Contributing samples per sample, as TryComputeTerrainDrainage writes it; at least one.
/// @param parameters Index bounds and the slope floor.
/// @param outWetness One value in [0, 1] per sample; written only on Success.
[[nodiscard]] TerrainFieldStatus TryComputeTerrainWetness(const TerrainFieldGrid& grid,
                                                          foundation::BorrowedView<const float32> slope,
                                                          foundation::BorrowedView<const uint32> accumulation,
                                                          const TerrainWetnessParameters& parameters,
                                                          foundation::BorrowedView<float32> outWetness) noexcept;
} // namespace ludus::content::terrain
