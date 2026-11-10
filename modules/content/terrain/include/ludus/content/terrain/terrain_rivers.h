#pragma once

// River channels of terrain slice T2: channel extraction from D8 drainage into
// a segment network with Strahler order, and carving of a channel profile with
// a water surface field. Both read the outputs of TryComputeTerrainDrainage
// for one complete drainage domain. Callers rerun drainage on the carved
// surface before deriving slope, wetness or materials, because carving can move
// a watershed. Contracts: docs/architecture/terrain-generation.md#ordered-pipeline
// and docs/modules/content/terrain.md#river-channels.
//
// Thanks to Luna B. Leopold and Thomas Maddock Jr., "The hydraulic geometry of
// stream channels and some physiographic implications", U.S. Geological Survey
// Professional Paper 252, 1953 (https://doi.org/10.3133/pp252), for channel
// width and depth as power laws of discharge. Ludus uses contributing area as
// the discharge proxy and authored coefficients rather than fitted regional
// constants. Thanks to Arthur N. Strahler, "Quantitative analysis of watershed
// geomorphology", Transactions, American Geophysical Union 38(6), pp. 913-920,
// 1957 (https://doi.org/10.1029/TR038i006p00913), for stream ordering. No source
// code is copied from these works.

#include <ludus/foundation/base/core.h>

#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::content::terrain
{
/// Version of the channel rule, segment order, profile and water surface below; store it with cooked data.
inline constexpr uint32 TERRAIN_RIVERS_LAYOUT_VERSION = 1;
/// Segment index meaning "no segment": an outlet has no downstream segment.
inline constexpr uint32 TERRAIN_RIVER_NONE = 0xffffffffU;
/// Most samples a channel profile reaches from its centre sample, in each direction.
inline constexpr uint32 MAX_TERRAIN_RIVER_STENCIL_SAMPLES = 16;

/// Default contributing area at which a channel starts, in square metres.
inline constexpr float32 DEFAULT_TERRAIN_RIVER_CATCHMENT_SQUARE_METRES = 2000;
/// Default depth at one square metre of contributing area, in metres.
inline constexpr float32 DEFAULT_TERRAIN_RIVER_DEPTH_COEFFICIENT_METRES = 0.02F;
/// Default depth exponent; Leopold and Maddock report about 0.4 for discharge.
inline constexpr float32 DEFAULT_TERRAIN_RIVER_DEPTH_EXPONENT = 0.4F;
/// Default half-width at one square metre of contributing area, in metres.
inline constexpr float32 DEFAULT_TERRAIN_RIVER_WIDTH_COEFFICIENT_METRES = 0.02F;
/// Default width exponent; Leopold and Maddock report about 0.5 for discharge.
inline constexpr float32 DEFAULT_TERRAIN_RIVER_WIDTH_EXPONENT = 0.5F;
/// Default bank slope (rise over run) between the channel bed and the surrounding terrain.
inline constexpr float32 DEFAULT_TERRAIN_RIVER_BANK_SLOPE = 0.5F;
/// Default water level as a fraction of channel depth above the bed.
inline constexpr float32 DEFAULT_TERRAIN_RIVER_WATER_FRACTION = 0.8F;

/// @brief Authored channel rule and profile, shared by extraction and carving.
///
/// A sample is a channel when its contributing area, accumulation times spacing squared, reaches
/// MinimumCatchmentSquareMetres. Contributing area grows downstream, so the channel set is closed downstream.
/// Channel depth and half-width are Coefficient * area^Exponent with area in square metres.
struct TerrainRiverParameters final
{
    /// Contributing area that starts a channel, in square metres; finite and positive.
    float32 MinimumCatchmentSquareMetres = DEFAULT_TERRAIN_RIVER_CATCHMENT_SQUARE_METRES;
    /// Depth at one square metre, in metres; finite and nonnegative.
    float32 DepthCoefficientMetres = DEFAULT_TERRAIN_RIVER_DEPTH_COEFFICIENT_METRES;
    /// Depth exponent, 0 through 1.
    float32 DepthExponent = DEFAULT_TERRAIN_RIVER_DEPTH_EXPONENT;
    /// Half-width at one square metre, in metres; finite and nonnegative.
    float32 WidthCoefficientMetres = DEFAULT_TERRAIN_RIVER_WIDTH_COEFFICIENT_METRES;
    /// Half-width exponent, 0 through 1.
    float32 WidthExponent = DEFAULT_TERRAIN_RIVER_WIDTH_EXPONENT;
    /// Bank rise over run; the bank spans depth / BankSlope metres beyond the half-width. Finite and positive.
    float32 BankSlope = DEFAULT_TERRAIN_RIVER_BANK_SLOPE;
    /// Water level above the bed as a fraction of depth, 0 through 1.
    float32 WaterFraction = DEFAULT_TERRAIN_RIVER_WATER_FRACTION;
};

/// @brief Outcome of a river operation. Any failure leaves every output unchanged.
enum class TerrainRiverStatus : uint8
{
    /// The operation succeeded.
    Success,
    /// The grid fails IsTerrainFieldGridValid.
    InvalidGrid,
    /// A view is invalid or does not hold the documented count.
    InvalidStorage,
    /// A parameter violates its documented range.
    InvalidArgument,
    /// An input sample is NaN or infinite.
    NonFiniteInput,
    /// The routing does not drain every sample to an outlet, so it did not come from TryComputeTerrainDrainage.
    InvalidRouting,
    /// The caller's point or segment storage is too small; the summary reports the required counts.
    CapacityExceeded,
    /// Scratch storage could not be allocated.
    AllocationFailed,
};

/// @brief Whether every parameter lies in its documented range.
[[nodiscard]] bool AreTerrainRiverParametersValid(const TerrainRiverParameters& parameters) noexcept;

/// @brief One unbranched reach between a source or confluence and the next confluence or outlet.
///
/// Points are sample indices from upstream to downstream. A segment that ends at a confluence repeats the
/// confluence sample as its last point, which is also the first point of the downstream segment, so the
/// polylines join.
struct TerrainRiverSegment final
{
    /// Index of the first point in the point storage.
    uint32 FirstPoint = 0;
    /// Points in the segment, at least one.
    uint32 PointCount = 0;
    /// Segment this one flows into, or TERRAIN_RIVER_NONE when it ends at an outlet.
    uint32 DownstreamSegment = TERRAIN_RIVER_NONE;
    /// Strahler order: one at a source; at a confluence, the largest upstream order, plus one when two or more
    /// upstream segments share it.
    uint32 Order = 0;
};

/// @brief Counts of one network extraction.
struct TerrainRiverNetworkSummary final
{
    /// Channel samples in the domain.
    usize ChannelSamples = 0;
    /// Points written, or required when CapacityExceeded.
    usize PointCount = 0;
    /// Segments written, or required when CapacityExceeded.
    usize SegmentCount = 0;
    /// Segments that end at an outlet.
    usize OutletSegments = 0;
    /// Largest Strahler order in the network; zero without channels.
    uint32 MaximumOrder = 0;
};

/// @brief Extract the channel network as joined polylines with downstream links and Strahler order.
///
/// Segments start at channel samples whose channel inflow is not exactly one (sources and confluences), visited
/// in ascending sample index, so the output depends only on the inputs.
/// @param grid The complete drainage domain.
/// @param flowDirection Routing from TryComputeTerrainDrainage.
/// @param accumulation Contributing samples from TryComputeTerrainDrainage.
/// @param parameters The channel rule; only MinimumCatchmentSquareMetres is read, but all fields are validated.
/// @param outPoints Sample indices of every segment, concatenated in segment order.
/// @param outSegments The segments, in head-sample order.
/// @param outSummary Counts; written on Success and, with the required counts, on CapacityExceeded.
/// @return Success, CapacityExceeded with nothing but the summary written, or a validation failure.
[[nodiscard]] TerrainRiverStatus
TryExtractTerrainRivers(const TerrainFieldGrid& grid,
                        foundation::BorrowedView<const TerrainFlowDirection> flowDirection,
                        foundation::BorrowedView<const uint32> accumulation,
                        const TerrainRiverParameters& parameters,
                        foundation::BorrowedView<uint32> outPoints,
                        foundation::BorrowedView<TerrainRiverSegment> outSegments,
                        TerrainRiverNetworkSummary& outSummary) noexcept;

/// @brief Diagnostics of one carve.
struct TerrainRiverCarveSummary final
{
    /// Channel samples that cut a profile.
    usize ChannelSamples = 0;
    /// Samples lowered by any channel.
    usize CarvedSamples = 0;
    /// Largest lowering, in metres.
    float32 MaximumCarveMetres = 0;
    /// Samples with water above the carved surface.
    usize WetSamples = 0;
    /// Channels whose profile was clamped to MAX_TERRAIN_RIVER_STENCIL_SAMPLES.
    usize ClampedChannels = 0;
};

/// @brief Cut each channel's profile into the terrain and compute the water surface.
///
/// A channel sample c has bed b = filled(c) - depth(c), so the bed descends downstream wherever the filled
/// surface does. Each sample p within the half-width h of c is lowered toward b; beyond it, a smooth bank of
/// width depth / BankSlope blends back to the original elevation, so the cut has no step. The carved elevation
/// is the minimum over every channel and the original, which is independent of traversal order and never
/// raises terrain. Each sample takes its water level, b + WaterFraction * depth, from the channel whose profile
/// sets its lowest target (ties to the lower sample index), so the cut and the water agree: a zero depth cuts at
/// most a flat floor of the half-width and holds no water. Water stands wherever the carved surface lies below
/// that level.
/// @param grid The complete drainage domain.
/// @param elevationMetres The uncarved elevation.
/// @param filledElevationMetres The filled surface from TryComputeTerrainDrainage of elevationMetres.
/// @param accumulation Contributing samples from the same drainage solve.
/// @param parameters Channel rule and profile.
/// @param outCarvedElevationMetres The carved surface, at most the input elevation.
/// @param outWaterSurfaceMetres Water level where wet; the carved elevation where dry.
/// @param outWaterDepthMetres Water surface minus carved elevation, zero where dry.
/// @param outSummary Carve and water diagnostics.
[[nodiscard]] TerrainRiverStatus TryCarveTerrainRivers(const TerrainFieldGrid& grid,
                                                       foundation::BorrowedView<const float32> elevationMetres,
                                                       foundation::BorrowedView<const float32> filledElevationMetres,
                                                       foundation::BorrowedView<const uint32> accumulation,
                                                       const TerrainRiverParameters& parameters,
                                                       foundation::BorrowedView<float32> outCarvedElevationMetres,
                                                       foundation::BorrowedView<float32> outWaterSurfaceMetres,
                                                       foundation::BorrowedView<float32> outWaterDepthMetres,
                                                       TerrainRiverCarveSummary& outSummary) noexcept;
} // namespace ludus::content::terrain
