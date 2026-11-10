// Serial reference river kernels; see terrain_rivers.h for contracts and attribution.
// Both operations validate, allocate scratch and compute into it before writing any output.

#include <ludus/content/terrain/terrain_rivers.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::terrain
{
namespace
{
namespace math = foundation::math;
using foundation::Array;
using foundation::BorrowedView;
using foundation::core::float64;
using foundation::core::int64;

template <typename ElementType>
[[nodiscard]] bool HoldsOnePerSample(BorrowedView<ElementType> view, usize samples) noexcept
{
    return view.IsValid() == true && view.GetSize() == samples;
}

[[nodiscard]] bool AreAllFinite(BorrowedView<const float32> values) noexcept
{
    for (usize index = 0; index < values.GetSize(); ++index)
    {
        if (math::IsFinite(values[index]) == false)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsUnitInterval(float32 value) noexcept
{
    return math::IsFinite(value) == true && value >= 0 && value <= 1;
}

[[nodiscard]] float64 GetCatchmentSquareMetres(const TerrainFieldGrid& grid, uint32 accumulation) noexcept
{
    return static_cast<float64>(accumulation) * static_cast<float64>(GetTerrainSampleAreaSquareMetres(grid));
}

[[nodiscard]] bool
IsChannel(const TerrainFieldGrid& grid, const TerrainRiverParameters& parameters, uint32 accumulation) noexcept
{
    return GetCatchmentSquareMetres(grid, accumulation) >=
           static_cast<float64>(parameters.MinimumCatchmentSquareMetres);
}

/// Downstream sample of a routed sample; false for an outlet or a direction that leaves the grid.
[[nodiscard]] bool TryGetDownstream(const TerrainFieldGrid& grid,
                                    uint32 sample,
                                    TerrainFlowDirection direction,
                                    uint32& outTarget) noexcept
{
    if (direction == TerrainFlowDirection::Outlet)
    {
        return false;
    }
    const TerrainFlowOffset offset = GetTerrainFlowOffset(direction);
    const int64 x = static_cast<int64>(sample % grid.Columns) + offset.X;
    const int64 z = static_cast<int64>(sample / grid.Columns) + offset.Z;
    if (x < 0 || z < 0 || x >= grid.Columns || z >= grid.Rows)
    {
        return false;
    }
    outTarget = static_cast<uint32>(z * grid.Columns + x);
    return true;
}

/// Routing from TryComputeTerrainDrainage strictly increases accumulation downstream, which also rules out cycles.
[[nodiscard]] bool IsRoutingConsistent(const TerrainFieldGrid& grid,
                                       BorrowedView<const TerrainFlowDirection> flowDirection,
                                       BorrowedView<const uint32> accumulation) noexcept
{
    const usize samples = GetTerrainSampleCount(grid);
    for (uint32 sample = 0; sample < samples; ++sample)
    {
        const TerrainFlowDirection direction = flowDirection[sample];
        if (static_cast<uint32>(direction) > static_cast<uint32>(TerrainFlowDirection::Outlet) ||
            accumulation[sample] == 0)
        {
            return false;
        }
        if (direction == TerrainFlowDirection::Outlet)
        {
            continue;
        }
        uint32 target = 0;
        if (TryGetDownstream(grid, sample, direction, target) == false || accumulation[target] <= accumulation[sample])
        {
            return false;
        }
    }
    return true;
}

/// Network built in scratch before any output is written.
struct NetworkScratch final
{
    Array<uint8> ChannelInflow;
    Array<uint32> HeadSegment;
    Array<uint32> Points;
    Array<TerrainRiverSegment> Segments;
    Array<uint32> UpstreamRemaining;
    Array<uint32> UpstreamMaximumOrder;
    Array<uint32> UpstreamAtMaximum;
    Array<uint32> Queue;
};

/// Walk one segment from its head; appends its points and returns the confluence it ends at, or NONE.
[[nodiscard]] uint32 WalkSegment(const TerrainFieldGrid& grid,
                                 BorrowedView<const TerrainFlowDirection> flowDirection,
                                 BorrowedView<const uint8> channelInflow,
                                 uint32 head,
                                 Array<uint32>& inoutPoints)
{
    uint32 sample = head;
    inoutPoints.Add(sample);
    while (true)
    {
        uint32 target = 0;
        if (TryGetDownstream(grid, sample, flowDirection[sample], target) == false)
        {
            return TERRAIN_RIVER_NONE;
        }
        inoutPoints.Add(target);
        if (channelInflow[target] >= 2)
        {
            return target;
        }
        sample = target;
    }
}

void ComputeStrahlerOrders(NetworkScratch& inoutScratch) noexcept
{
    const usize segments = inoutScratch.Segments.GetSize();
    for (usize segment = 0; segment < segments; ++segment)
    {
        const uint32 downstream = inoutScratch.Segments[segment].DownstreamSegment;
        if (downstream != TERRAIN_RIVER_NONE)
        {
            ++inoutScratch.UpstreamRemaining[downstream];
        }
    }
    usize tail = 0;
    for (uint32 segment = 0; segment < segments; ++segment)
    {
        if (inoutScratch.UpstreamRemaining[segment] == 0)
        {
            inoutScratch.Segments[segment].Order = 1;
            inoutScratch.Queue[tail] = segment;
            ++tail;
        }
    }
    for (usize head = 0; head < tail; ++head)
    {
        const TerrainRiverSegment& current = inoutScratch.Segments[inoutScratch.Queue[head]];
        const uint32 downstream = current.DownstreamSegment;
        if (downstream == TERRAIN_RIVER_NONE)
        {
            continue;
        }
        if (current.Order > inoutScratch.UpstreamMaximumOrder[downstream])
        {
            inoutScratch.UpstreamMaximumOrder[downstream] = current.Order;
            inoutScratch.UpstreamAtMaximum[downstream] = 1;
        }
        else if (current.Order == inoutScratch.UpstreamMaximumOrder[downstream])
        {
            ++inoutScratch.UpstreamAtMaximum[downstream];
        }
        --inoutScratch.UpstreamRemaining[downstream];
        if (inoutScratch.UpstreamRemaining[downstream] == 0)
        {
            const uint32 maximum = inoutScratch.UpstreamMaximumOrder[downstream];
            inoutScratch.Segments[downstream].Order =
                inoutScratch.UpstreamAtMaximum[downstream] >= 2 ? maximum + 1 : maximum;
            inoutScratch.Queue[tail] = downstream;
            ++tail;
        }
    }
    // Every segment drains to an outlet segment, so the segment graph is a forest and every segment is reached.
    LUDUS_ASSERT(tail == segments);
}

/// Channel profile of one sample, clamped to the stencil limit.
struct ChannelProfile final
{
    float32 BedMetres = 0;
    float32 WaterLevelMetres = 0;
    float32 HalfWidthMetres = 0;
    float32 BankWidthMetres = 0;
    uint32 RadiusXSamples = 0;
    uint32 RadiusZSamples = 0;
    bool IsClamped = false;
};

[[nodiscard]] ChannelProfile ComputeProfile(const TerrainFieldGrid& grid,
                                            const TerrainRiverParameters& parameters,
                                            uint32 accumulation,
                                            float32 filledMetres) noexcept
{
    const float64 area = GetCatchmentSquareMetres(grid, accumulation);
    const float64 depth = static_cast<float64>(parameters.DepthCoefficientMetres) *
                          math::Pow(area, static_cast<float64>(parameters.DepthExponent));
    float64 halfWidth = static_cast<float64>(parameters.WidthCoefficientMetres) *
                        math::Pow(area, static_cast<float64>(parameters.WidthExponent));
    float64 bankWidth = depth / static_cast<float64>(parameters.BankSlope);
    const float64 spacingX = static_cast<float64>(grid.SpacingXMetres);
    const float64 spacingZ = static_cast<float64>(grid.SpacingZMetres);
    // The reach fits the stencil on the finer axis, so neither axis exceeds it.
    const float64 limit = static_cast<float64>(MAX_TERRAIN_RIVER_STENCIL_SAMPLES) * math::Min(spacingX, spacingZ);
    ChannelProfile profile;
    if (halfWidth + bankWidth > limit)
    {
        profile.IsClamped = true;
        halfWidth = math::Min(halfWidth, limit);
        bankWidth = limit - halfWidth;
    }
    profile.BedMetres = static_cast<float32>(static_cast<float64>(filledMetres) - depth);
    profile.WaterLevelMetres = static_cast<float32>(static_cast<float64>(profile.BedMetres) +
                                                    static_cast<float64>(parameters.WaterFraction) * depth);
    profile.HalfWidthMetres = static_cast<float32>(halfWidth);
    profile.BankWidthMetres = static_cast<float32>(bankWidth);
    const float64 reach = halfWidth + bankWidth;
    profile.RadiusXSamples =
        math::Min(static_cast<uint32>(math::Ceil(reach / spacingX)), MAX_TERRAIN_RIVER_STENCIL_SAMPLES);
    profile.RadiusZSamples =
        math::Min(static_cast<uint32>(math::Ceil(reach / spacingZ)), MAX_TERRAIN_RIVER_STENCIL_SAMPLES);
    return profile;
}

/// Elevation the profile allows at distance from the channel centre: the bed inside the half-width, a smooth
/// bank blending to the original beyond it.
[[nodiscard]] float32 ComputeProfileTarget(const ChannelProfile& profile, float32 distance, float32 original) noexcept
{
    if (distance <= profile.HalfWidthMetres)
    {
        return profile.BedMetres;
    }
    if (profile.BankWidthMetres <= 0)
    {
        return original;
    }
    const float32 blend = math::SmoothStep01((distance - profile.HalfWidthMetres) / profile.BankWidthMetres);
    return math::Lerp(profile.BedMetres, original, blend);
}

/// Carve scratch: the lowest profile target per sample and the water level of the channel that set it.
struct CarveScratch final
{
    Array<float32> LowestTarget;
    Array<float32> WaterLevel;
    Array<uint8> IsReached;
};
} // namespace

bool AreTerrainRiverParametersValid(const TerrainRiverParameters& parameters) noexcept
{
    return math::IsFinite(parameters.MinimumCatchmentSquareMetres) == true &&
           parameters.MinimumCatchmentSquareMetres > 0 && math::IsFinite(parameters.DepthCoefficientMetres) == true &&
           parameters.DepthCoefficientMetres >= 0 && IsUnitInterval(parameters.DepthExponent) == true &&
           math::IsFinite(parameters.WidthCoefficientMetres) == true && parameters.WidthCoefficientMetres >= 0 &&
           IsUnitInterval(parameters.WidthExponent) == true && math::IsFinite(parameters.BankSlope) == true &&
           parameters.BankSlope > 0 && IsUnitInterval(parameters.WaterFraction) == true;
}

TerrainRiverStatus TryExtractTerrainRivers(const TerrainFieldGrid& grid,
                                           BorrowedView<const TerrainFlowDirection> flowDirection,
                                           BorrowedView<const uint32> accumulation,
                                           const TerrainRiverParameters& parameters,
                                           BorrowedView<uint32> outPoints,
                                           BorrowedView<TerrainRiverSegment> outSegments,
                                           TerrainRiverNetworkSummary& outSummary) noexcept
{
    if (IsTerrainFieldGridValid(grid) == false)
    {
        return TerrainRiverStatus::InvalidGrid;
    }
    const usize samples = GetTerrainSampleCount(grid);
    if (HoldsOnePerSample(flowDirection, samples) == false || HoldsOnePerSample(accumulation, samples) == false ||
        outPoints.IsValid() == false || outSegments.IsValid() == false)
    {
        return TerrainRiverStatus::InvalidStorage;
    }
    if (AreTerrainRiverParametersValid(parameters) == false)
    {
        return TerrainRiverStatus::InvalidArgument;
    }
    if (IsRoutingConsistent(grid, flowDirection, accumulation) == false)
    {
        return TerrainRiverStatus::InvalidRouting;
    }
    NetworkScratch scratch;
    if (scratch.ChannelInflow.TryResize(samples, 0) == false ||
        scratch.HeadSegment.TryResize(samples, TERRAIN_RIVER_NONE) == false)
    {
        return TerrainRiverStatus::AllocationFailed;
    }
    usize channelSamples = 0;
    for (uint32 sample = 0; sample < samples; ++sample)
    {
        if (IsChannel(grid, parameters, accumulation[sample]) == false)
        {
            continue;
        }
        ++channelSamples;
        uint32 target = 0;
        if (TryGetDownstream(grid, sample, flowDirection[sample], target) == true)
        {
            // Accumulation grows downstream, so the target of a channel is a channel.
            ++scratch.ChannelInflow[target];
        }
    }
    // A segment adds at most one shared confluence point beyond its own samples.
    if (scratch.Points.TryEnsureCapacity(2 * channelSamples) == false ||
        scratch.Segments.TryEnsureCapacity(channelSamples) == false)
    {
        return TerrainRiverStatus::AllocationFailed;
    }
    Array<uint32> endConfluence;
    if (endConfluence.TryEnsureCapacity(channelSamples) == false)
    {
        return TerrainRiverStatus::AllocationFailed;
    }
    for (uint32 sample = 0; sample < samples; ++sample)
    {
        if (IsChannel(grid, parameters, accumulation[sample]) == false || scratch.ChannelInflow[sample] == 1)
        {
            continue;
        }
        TerrainRiverSegment segment;
        segment.FirstPoint = static_cast<uint32>(scratch.Points.GetSize());
        scratch.HeadSegment[sample] = static_cast<uint32>(scratch.Segments.GetSize());
        const BorrowedView<const uint8> channelInflow(scratch.ChannelInflow.GetData(), samples);
        endConfluence.Add(WalkSegment(grid, flowDirection, channelInflow, sample, scratch.Points));
        segment.PointCount = static_cast<uint32>(scratch.Points.GetSize()) - segment.FirstPoint;
        scratch.Segments.Add(segment);
    }
    const usize segments = scratch.Segments.GetSize();
    TerrainRiverNetworkSummary summary;
    summary.ChannelSamples = channelSamples;
    summary.PointCount = scratch.Points.GetSize();
    summary.SegmentCount = segments;
    for (usize segment = 0; segment < segments; ++segment)
    {
        const uint32 confluence = endConfluence[segment];
        scratch.Segments[segment].DownstreamSegment =
            confluence == TERRAIN_RIVER_NONE ? TERRAIN_RIVER_NONE : scratch.HeadSegment[confluence];
        if (confluence == TERRAIN_RIVER_NONE)
        {
            ++summary.OutletSegments;
        }
    }
    if (summary.PointCount > outPoints.GetSize() || segments > outSegments.GetSize())
    {
        outSummary = summary;
        return TerrainRiverStatus::CapacityExceeded;
    }
    if (scratch.UpstreamRemaining.TryResize(segments, 0) == false ||
        scratch.UpstreamMaximumOrder.TryResize(segments, 0) == false ||
        scratch.UpstreamAtMaximum.TryResize(segments, 0) == false || scratch.Queue.TryResize(segments, 0) == false)
    {
        return TerrainRiverStatus::AllocationFailed;
    }
    ComputeStrahlerOrders(scratch);
    for (usize point = 0; point < summary.PointCount; ++point)
    {
        outPoints[point] = scratch.Points[point];
    }
    for (usize segment = 0; segment < segments; ++segment)
    {
        outSegments[segment] = scratch.Segments[segment];
        summary.MaximumOrder = math::Max(summary.MaximumOrder, scratch.Segments[segment].Order);
    }
    outSummary = summary;
    return TerrainRiverStatus::Success;
}

TerrainRiverStatus TryCarveTerrainRivers(const TerrainFieldGrid& grid,
                                         BorrowedView<const float32> elevationMetres,
                                         BorrowedView<const float32> filledElevationMetres,
                                         BorrowedView<const uint32> accumulation,
                                         const TerrainRiverParameters& parameters,
                                         BorrowedView<float32> outCarvedElevationMetres,
                                         BorrowedView<float32> outWaterSurfaceMetres,
                                         BorrowedView<float32> outWaterDepthMetres,
                                         TerrainRiverCarveSummary& outSummary) noexcept
{
    if (IsTerrainFieldGridValid(grid) == false)
    {
        return TerrainRiverStatus::InvalidGrid;
    }
    const usize samples = GetTerrainSampleCount(grid);
    if (HoldsOnePerSample(elevationMetres, samples) == false ||
        HoldsOnePerSample(filledElevationMetres, samples) == false ||
        HoldsOnePerSample(accumulation, samples) == false ||
        HoldsOnePerSample(outCarvedElevationMetres, samples) == false ||
        HoldsOnePerSample(outWaterSurfaceMetres, samples) == false ||
        HoldsOnePerSample(outWaterDepthMetres, samples) == false)
    {
        return TerrainRiverStatus::InvalidStorage;
    }
    if (AreTerrainRiverParametersValid(parameters) == false)
    {
        return TerrainRiverStatus::InvalidArgument;
    }
    if (AreAllFinite(elevationMetres) == false || AreAllFinite(filledElevationMetres) == false)
    {
        return TerrainRiverStatus::NonFiniteInput;
    }
    CarveScratch scratch;
    if (scratch.LowestTarget.TryResize(samples, 0.0F) == false ||
        scratch.WaterLevel.TryResize(samples, 0.0F) == false || scratch.IsReached.TryResize(samples, 0) == false)
    {
        return TerrainRiverStatus::AllocationFailed;
    }
    TerrainRiverCarveSummary summary;
    for (uint32 channel = 0; channel < samples; ++channel)
    {
        if (IsChannel(grid, parameters, accumulation[channel]) == false)
        {
            continue;
        }
        ++summary.ChannelSamples;
        const ChannelProfile profile =
            ComputeProfile(grid, parameters, accumulation[channel], filledElevationMetres[channel]);
        if (profile.IsClamped == true)
        {
            ++summary.ClampedChannels;
        }
        const int64 centreX = channel % grid.Columns;
        const int64 centreZ = channel / grid.Columns;
        const int64 radiusX = profile.RadiusXSamples;
        const int64 radiusZ = profile.RadiusZSamples;
        const float32 reach = profile.HalfWidthMetres + profile.BankWidthMetres;
        for (int64 z = math::Max<int64>(centreZ - radiusZ, 0); z <= math::Min<int64>(centreZ + radiusZ, grid.Rows - 1);
             ++z)
        {
            for (int64 x = math::Max<int64>(centreX - radiusX, 0);
                 x <= math::Min<int64>(centreX + radiusX, grid.Columns - 1);
                 ++x)
            {
                const float32 stepX = static_cast<float32>(x - centreX) * grid.SpacingXMetres;
                const float32 stepZ = static_cast<float32>(z - centreZ) * grid.SpacingZMetres;
                const float32 distance = math::Sqrt(stepX * stepX + stepZ * stepZ);
                if (distance > reach && distance > 0)
                {
                    continue;
                }
                const usize sample = static_cast<usize>(z * grid.Columns + x);
                const float32 target = ComputeProfileTarget(profile, distance, elevationMetres[sample]);
                // Channels are visited in ascending index, so a strict comparison ties to the lower index.
                if (scratch.IsReached[sample] == 0 || target < scratch.LowestTarget[sample])
                {
                    scratch.IsReached[sample] = 1;
                    scratch.LowestTarget[sample] = target;
                    scratch.WaterLevel[sample] = profile.WaterLevelMetres;
                }
            }
        }
    }
    for (usize sample = 0; sample < samples; ++sample)
    {
        const bool isReached = scratch.IsReached[sample] != 0;
        const float32 carved = isReached == true ? math::Min(elevationMetres[sample], scratch.LowestTarget[sample])
                                                 : elevationMetres[sample];
        const float32 lowering = elevationMetres[sample] - carved;
        if (lowering > 0)
        {
            ++summary.CarvedSamples;
            summary.MaximumCarveMetres = math::Max(summary.MaximumCarveMetres, lowering);
        }
        float32 depth = 0;
        if (isReached == true)
        {
            depth = math::Max(scratch.WaterLevel[sample] - carved, 0.0F);
        }
        if (depth > 0)
        {
            ++summary.WetSamples;
        }
        outCarvedElevationMetres[sample] = carved;
        outWaterDepthMetres[sample] = depth;
        outWaterSurfaceMetres[sample] = carved + depth;
    }
    outSummary = summary;
    return TerrainRiverStatus::Success;
}
} // namespace ludus::content::terrain
