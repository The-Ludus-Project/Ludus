#include "internal/wave_fraunhofer_diffraction.h"
#include "../shaders/wave_connection_layout.h"

// Thanks to Shlomi Steinberg, Ravi Ramamoorthi, Benedikt Bitterli, Arshiya Mollazainali, Eugene d'Eon and Matt
// Pharr, "A Free-Space Diffraction BSDF", ACM Transactions on Graphics 43(4), Article 113 (SIGGRAPH 2024),
// https://doi.org/10.1145/3658166 , for the edge-based formulation of Fraunhofer diffraction: the obstacle is the
// union of the scene triangles near the interaction, projected onto a screen across the propagation direction
// (their Eq. 17); the divergence theorem turns its diffraction integral into a line integral over the boundary
// (Eq. 22); and each straight boundary edge, with the incident field linearly interpolated between its ends,
// integrates in closed form to an edge-diffracted wave built from sinc(x/2) and (cos(x/2) - sinc(x/2)) / x of the
// phase change along the edge (Eqs. 23, 26 and 27), with triangle edges shared by two faces cancelling. Thanks to
// Shlomi Steinberg and Matt Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics", Computer
// Graphics Forum 45(2), e70322 (2026), Section 5.2.2, https://doi.org/10.1111/cgf.70322 , for applying that
// operator to the silhouette edges found in a beam's interaction region and summing the edge waves, with their
// mutual interference, coherently.
//
// Departures, each declared in render-graph-authoring.md ("Fraunhofer edge diffraction connections"):
// - A connection observes the field at a receiver on the beam axis, inside the beam's central lobe, where the BSDF
//   paper's clamped diffracted term (its Eqs. 28-30) removes nearly all of the diffraction. This operator instead
//   adds the edge waves coherently to the ballistic term. That sum is Kirchhoff's boundary-diffraction-wave
//   decomposition (Maggi and Rubinowicz): for the connection's Gaussian beam over a thin screen the obstacle
//   integral reduces exactly to the ballistic step plus (1/2pi) times the boundary integral of
//   exp(-beta rho^2 / 2) (rho . m) / rho^2, with m the normal pointing away from the obstacle.
// - The Fraunhofer weight (rho . m) / rho^2 is taken as its exact average over each segment, the angle the
//   segment subtends at the axis divided by its length, rather than its value at one point. This keeps the field
//   continuous across shadow boundaries, where the pointwise weight diverges.
// - Each edge is cut into segments that are short near its stationary point, the closest point to the axis, and
//   lengthen with distance from it; the paper tessellates triangles to a fixed size instead.
// - Coverage samples on both sides of each segment find the boundary of the projected obstacle, including
//   back-facing and alpha-tested faces, in place of the paper's front-face culling. Each point of an edge uses the
//   beam kernel at its own depth.
// - Edges are admitted whole when the source and receiver see their stationary point past the region's slab:
//   single diffraction, as the UTD operator declares. The cos/sinc terms and the graded segmentation are original.
namespace ludus::graphics::renderer::internal
{
namespace
{
/// Taylor coefficients of (cos(x) - sin(x)/x) / 2x: -x/6 + x^3/60 - x^5/1680 + x^7/90720.
constexpr float64 SERIES_FIRST = 1.0 / 6;
constexpr float64 SERIES_SECOND = 1.0 / 60;
constexpr float64 SERIES_THIRD = 1.0 / 1680;
constexpr float64 SERIES_FOURTH = 1.0 / 90720;
/// Second Taylor coefficient of sin(x) / x.
constexpr float64 SINC_SECOND_TERM = 1.0 / 6;
/// Field an edge through the axis adds: half the ballistic step.
constexpr float64 HALF = 0.5;
/// An edge is walked from its stationary point towards each end.
constexpr uint32 WALK_DIRECTIONS = 2;
/// 0 + 1 + 2: subtracting two corner indices from it gives the third.
constexpr uint32 CORNER_INDEX_SUM = 3;

WaveField MakePhase(float64 phase) noexcept
{
    return {math::Cos(phase), math::Sin(phase)};
}
float64 EvaluateSinc(float64 value) noexcept
{
    return math::Abs(value) < LUDUS_WAVE_UTD_SINC_LIMIT ? 1 - value * value * SINC_SECOND_TERM
                                                        : math::Sin(value) / value;
}
/// (cos(x) - sinc(x)) / 2x for x = halfPhase: the weight of a linear amplitude change along a segment.
float64 EvaluateLinearWeight(float64 halfPhase) noexcept
{
    if (math::Abs(halfPhase) < LUDUS_WAVE_FRAUNHOFER_SERIES_LIMIT)
    {
        const float64 square = halfPhase * halfPhase;
        return halfPhase *
               (-SERIES_FIRST + square * (SERIES_SECOND + square * (-SERIES_THIRD + square * SERIES_FOURTH)));
    }
    return (math::Cos(halfPhase) - math::Sin(halfPhase) / halfPhase) / (2 * halfPhase);
}
/// Re(beta) and -Im(beta) of the beam's transverse kernel exp(-beta rho^2 / 2) at one depth.
struct BeamKernel final
{
    /// Envelope decay, in inverse square metres.
    float64 Damping = 0;
    /// Phase curvature, in radians per square metre.
    float64 Curvature = 0;
};
BeamKernel EvaluateBeamKernel(const FraunhoferBeam& beam, float64 depthWorld) noexcept
{
    // beta = 1 / (Sigma^2 + i z / k) - i k / (D - z): the Gaussian beam at depth z, then Fresnel propagation to the
    // receiver on the axis. (beta / 2pi) times the integral of exp(-beta rho^2 / 2) over the plane is one, so the
    // obstacle's share of the unobstructed field needs no other normalization at any depth.
    const float64 correlationWorldSquared = depthWorld / beam.Wavenumber;
    const float64 denominator =
        beam.SigmaWorldSquared * beam.SigmaWorldSquared + correlationWorldSquared * correlationWorldSquared;
    return
    {
        .Damping = beam.SigmaWorldSquared / denominator,
        .Curvature = correlationWorldSquared / denominator + beam.Wavenumber / (beam.DistanceWorld - depthWorld),
    };
}
/// An edge's projected line in the connection frame.
struct EdgeLine final
{
    /// Unit direction from the edge's begin to its end.
    ProjectedPointFrustum TangentFrustum{};
    /// Signed distance of the line from the axis along the left normal (-tangent y, tangent x).
    float64 OffsetWorld = 0;
    /// Coordinates along the tangent of the edge's ends, measured from the line's closest point to the axis.
    float64 BeginAlongWorld = 0;
    float64 EndAlongWorld = 0;
    float64 BeginDepthWorld = 0;
    float64 EndDepthWorld = 0;
};
/// Which side of an edge segment the projected obstacle covers, if exactly one.
enum class EdgeSide : uint8
{
    None,
    ObstacleLeft,
    ObstacleRight,
};
float64 InterpolateDepth(const EdgeLine& line, float64 alongWorld) noexcept
{
    const float64 fraction = (alongWorld - line.BeginAlongWorld) / (line.EndAlongWorld - line.BeginAlongWorld);
    return line.BeginDepthWorld + (line.EndDepthWorld - line.BeginDepthWorld) * fraction;
}
/// Envelope amplitude and phase of the beam kernel at one point of the line.
struct KernelSample final
{
    float64 Amplitude = 0;
    float64 Phase = 0;
};
KernelSample SampleKernel(const EdgeLine& line, const FraunhoferBeam& beam, float64 alongWorld) noexcept
{
    const BeamKernel kernel = EvaluateBeamKernel(beam, InterpolateDepth(line, alongWorld));
    const float64 radiusSquaredWorld = line.OffsetWorld * line.OffsetWorld + alongWorld * alongWorld;
    return
    {
        .Amplitude = math::Exp(-kernel.Damping * radiusSquaredWorld * 0.5),
        .Phase = kernel.Curvature * radiusSquaredWorld * 0.5,
    };
}
/// Span of an edge segment along its line.
struct SegmentSpan final
{
    float64 BeginAlongWorld = 0;
    float64 EndAlongWorld = 0;
};
/// (1/2pi) times the boundary integral over one segment: the angle it subtends at the axis times the closed-form
/// Fraunhofer edge wave of the linearly interpolated kernel, per unit length.
WaveField EvaluateSegment(const EdgeLine& line,
                          const FraunhoferBeam& beam,
                          EdgeSide side,
                          const SegmentSpan& span,
                          bool isAxisBlocked) noexcept
{
    // rho . m with m pointing away from the obstacle: positive when the axis lies behind the obstacle.
    const float64 heightWorld = side == EdgeSide::ObstacleLeft ? -line.OffsetWorld : line.OffsetWorld;
    const float64 begin = span.BeginAlongWorld;
    const float64 end = span.EndAlongWorld;
    if (heightWorld == 0)
    {
        // An edge through the axis subtends a half turn at the axis point, where the kernel is one; its sign
        // completes the ballistic step to one half.
        if (begin <= 0 && 0 < end)
        {
            return {isAxisBlocked == true ? HALF : -HALF, 0};
        }
        return {};
    }
    // atan(u / h) = sgn(h) atan2(u, |h|), continuous in u for either sign of h.
    const float64 distanceWorld = math::Abs(heightWorld);
    const float64 heightSign = heightWorld < 0 ? -1.0 : 1.0;
    const float64 angle = heightSign * (math::Atan2(end, distanceWorld) - math::Atan2(begin, distanceWorld));
    if (angle == 0)
    {
        return {};
    }
    const KernelSample first = SampleKernel(line, beam, begin);
    const KernelSample last = SampleKernel(line, beam, end);
    const KernelSample middle = SampleKernel(line, beam, (begin + end) * 0.5);
    const float64 halfPhase = (last.Phase - first.Phase) * 0.5;
    const float64 mean = (first.Amplitude + last.Amplitude) * 0.5;
    const float64 change = last.Amplitude - first.Amplitude;
    // exp(i phase(mid)) (mean sinc(x) - i change (cos(x) - sinc(x)) / 2x), x half the phase change.
    const WaveField wave{mean * EvaluateSinc(halfPhase), -change * EvaluateLinearWeight(halfPhase)};
    const WaveField rotation = MakePhase(middle.Phase);
    const float64 weight = angle / (2 * math::kPi);
    return {weight * (wave.Real * rotation.Real - wave.Imaginary * rotation.Imaginary),
            weight * (wave.Real * rotation.Imaginary + wave.Imaginary * rotation.Real)};
}
/// Accumulated contribution of one edge and where its stationary point lies.
struct EdgeAccumulation final
{
    WaveField Field{};
    /// Coordinate along the line of the clipped edge's point closest to the axis.
    float64 StationaryAlongWorld = 0;
    /// Some segment bounds the obstacle.
    bool HasBoundary = false;
};
/// Walk the edge, clipped to the envelope radius, outwards from its stationary point and add each segment that
/// `classify` places on the obstacle boundary.
template <typename Classify>
rhi::RasterStatus AccumulateEdge(const EdgeLine& line,
                                 const FraunhoferBeam& beam,
                                 bool isAxisBlocked,
                                 const Classify& classify,
                                 EdgeAccumulation& outAccumulation) noexcept
{
    outAccumulation = {};
    const float64 envelopeRadiusWorld = LUDUS_WAVE_CONNECTION_TAIL_DEVIATIONS * math::Sqrt(2 * beam.SigmaWorldSquared);
    const float64 chordSquaredWorld = envelopeRadiusWorld * envelopeRadiusWorld - line.OffsetWorld * line.OffsetWorld;
    if (chordSquaredWorld <= 0)
    {
        return rhi::RasterStatus::Ready;
    }
    const float64 chordWorld = math::Sqrt(chordSquaredWorld);
    const float64 lowWorld = math::Max(line.BeginAlongWorld, -chordWorld);
    const float64 highWorld = math::Min(line.EndAlongWorld, chordWorld);
    if (lowWorld >= highWorld)
    {
        return rhi::RasterStatus::Ready;
    }
    // The kernel's shortest length scale over the edge bounds the segment length near the stationary point.
    const BeamKernel first = EvaluateBeamKernel(beam, line.BeginDepthWorld);
    const BeamKernel last = EvaluateBeamKernel(beam, line.EndDepthWorld);
    const float64 magnitude = math::Max(math::Sqrt(first.Damping * first.Damping + first.Curvature * first.Curvature),
                                        math::Sqrt(last.Damping * last.Damping + last.Curvature * last.Curvature));
    const float64 shortestWorld = LUDUS_WAVE_FRAUNHOFER_SEGMENT_SCALE / math::Sqrt(magnitude);
    const float64 startWorld = math::Min(math::Max(0.0, lowWorld), highWorld);
    outAccumulation.StationaryAlongWorld = startWorld;
    for (uint32 direction = 0; direction < WALK_DIRECTIONS; ++direction)
    {
        const float64 walkSign = direction == 0 ? 1.0 : -1.0;
        float64 alongWorld = startWorld;
        uint32 segment = 0;
        while (walkSign > 0 ? alongWorld < highWorld : alongWorld > lowWorld)
        {
            if (segment == LUDUS_WAVE_FRAUNHOFER_SIDE_SEGMENTS)
            {
                return rhi::RasterStatus::Unsupported;
            }
            ++segment;
            const float64 stepWorld =
                math::Max(shortestWorld, LUDUS_WAVE_FRAUNHOFER_SEGMENT_GROWTH * math::Abs(alongWorld));
            const float64 nextWorld = walkSign > 0 ? math::Min(highWorld, alongWorld + stepWorld)
                                                   : math::Max(lowWorld, alongWorld - stepWorld);
            const SegmentSpan span
            {
                .BeginAlongWorld = math::Min(alongWorld, nextWorld),
                .EndAlongWorld = math::Max(alongWorld, nextWorld),
            };
            alongWorld = nextWorld;
            const EdgeSide side = classify(span);
            if (side == EdgeSide::None)
            {
                continue;
            }
            const WaveField value = EvaluateSegment(line, beam, side, span, isAxisBlocked);
            outAccumulation.Field.Real += value.Real;
            outAccumulation.Field.Imaginary += value.Imaginary;
            outAccumulation.HasBoundary = true;
        }
    }
    if (math::IsFinite(outAccumulation.Field.Real) == false || math::IsFinite(outAccumulation.Field.Imaginary) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    return rhi::RasterStatus::Ready;
}
/// The projected line through two points in the connection frame; false for a degenerate projection.
bool TryMakeEdgeLine(const FraunhoferEdge& edge, EdgeLine& outLine) noexcept
{
    const float64 deltaX = edge.EndFrustum.X - edge.BeginFrustum.X;
    const float64 deltaY = edge.EndFrustum.Y - edge.BeginFrustum.Y;
    const float64 lengthWorld = math::Sqrt(deltaX * deltaX + deltaY * deltaY);
    if (lengthWorld <= 0 || math::IsFinite(lengthWorld) == false)
    {
        return false;
    }
    const ProjectedPointFrustum tangent{ .X = deltaX / lengthWorld, .Y = deltaY / lengthWorld };
    const float64 beginAlongWorld = edge.BeginFrustum.X * tangent.X + edge.BeginFrustum.Y * tangent.Y;
    outLine =
    {
        .TangentFrustum = tangent,
        .OffsetWorld = edge.BeginFrustum.Y * tangent.X - edge.BeginFrustum.X * tangent.Y,
        .BeginAlongWorld = beginAlongWorld,
        .EndAlongWorld = beginAlongWorld + lengthWorld,
        .BeginDepthWorld = edge.BeginDepthWorld,
        .EndDepthWorld = edge.EndDepthWorld,
    };
    return true;
}
bool IsBeamValid(const FraunhoferBeam& beam) noexcept
{
    return math::IsFinite(beam.Wavenumber) == true && beam.Wavenumber > 0 &&
           math::IsFinite(beam.SigmaWorldSquared) == true && beam.SigmaWorldSquared > 0 &&
           math::IsFinite(beam.DistanceWorld) == true && beam.DistanceWorld > 0;
}
bool IsDepthInside(const FraunhoferBeam& beam, float64 depthWorld) noexcept
{
    return math::IsFinite(depthWorld) == true && depthWorld > 0 && depthWorld < beam.DistanceWorld;
}
/// A face of an edge found in a region: the triangle, and its corners at the edge's begin and end.
struct RegionEdgeFace final
{
    uint32 TriangleIndex = 0;
    uint32 BeginCorner = 0;
    uint32 EndCorner = 0;
    /// Side of the projected edge on which the face's third corner lies.
    EdgeSide Side = EdgeSide::None;
};
/// Add the boundary-diffraction waves of the region's projected obstacle, edge by edge. Each triangle edge is
/// visited once, from the lowest region slot among its faces; edges with three or more faces are skipped.
rhi::RasterStatus AddFraunhoferRegionFields(const ReferenceSceneData& scene,
                                            const WaveEdgeConnection& connection,
                                            const WaveInteractionRegion& region,
                                            WaveField& inoutField) noexcept
{
    const math::Vector3d axisYWorld = math::Cross(connection.DirectionWorld, connection.AxisXWorld);
    const FraunhoferBeam beam
    {
        .Wavenumber = 2 * math::kPi / connection.WavelengthWorld,
        .SigmaWorldSquared = connection.SigmaWorldSquared,
        .DistanceWorld = connection.DistanceWorld,
    };
    const auto project = [&](math::Vector3 position, float64& outDepthWorld) noexcept {
        const math::Vector3d offsetWorld = math::Vector3d{static_cast<float64>(position.X),
                                                          static_cast<float64>(position.Y),
                                                          static_cast<float64>(position.Z)} -
                                           connection.SourceWorld;
        outDepthWorld = math::Dot(offsetWorld, connection.DirectionWorld);
        return ProjectedPointFrustum
        {
            .X = math::Dot(offsetWorld, connection.AxisXWorld),
            .Y = math::Dot(offsetWorld, axisYWorld),
        };
    };
    for (uint32 slot = 0; slot < region.TriangleCount; ++slot)
    {
        const ReferenceTriangle& triangle = scene.Triangles[region.Triangles[slot]];
        for (uint32 corner = 0; corner < WAVE_TRIANGLE_CORNERS; ++corner)
        {
            const uint32 endCorner = (corner + 1) % WAVE_TRIANGLE_CORNERS;
            const math::Vector3 begin = triangle.Positions[corner];
            const math::Vector3 end = triangle.Positions[endCorner];
            if (begin.X == end.X && begin.Y == end.Y && begin.Z == end.Z)
            {
                continue;
            }
            StaticArray<RegionEdgeFace, WAVE_MAX_EXCLUDED_TRIANGLES> faces{};
            faces[0] = { .TriangleIndex = region.Triangles[slot], .BeginCorner = corner, .EndCorner = endCorner };
            uint32 faceCount = 1;
            bool isVisitedElsewhere = false;
            for (uint32 other = 0; other < region.TriangleCount; ++other)
            {
                if (other == slot)
                {
                    continue;
                }
                const ReferenceTriangle& candidate = scene.Triangles[region.Triangles[other]];
                const uint32 candidateBegin = FindWaveTriangleCorner(candidate, begin);
                const uint32 candidateEnd = FindWaveTriangleCorner(candidate, end);
                if (candidateBegin == WAVE_NO_TRIANGLE_CORNER || candidateEnd == WAVE_NO_TRIANGLE_CORNER)
                {
                    continue;
                }
                if (other < slot || faceCount == WAVE_MAX_EXCLUDED_TRIANGLES)
                {
                    isVisitedElsewhere = true;
                    break;
                }
                faces[faceCount++] =
                {
                    .TriangleIndex = region.Triangles[other],
                    .BeginCorner = candidateBegin,
                    .EndCorner = candidateEnd,
                };
            }
            if (isVisitedElsewhere == true)
            {
                continue;
            }
            FraunhoferEdge edge;
            edge.BeginFrustum = project(begin, edge.BeginDepthWorld);
            edge.EndFrustum = project(end, edge.EndDepthWorld);
            EdgeLine line;
            if (IsDepthInside(beam, edge.BeginDepthWorld) == false ||
                IsDepthInside(beam, edge.EndDepthWorld) == false || TryMakeEdgeLine(edge, line) == false)
            {
                continue;
            }
            const ProjectedPointFrustum normal{ .X = -line.TangentFrustum.Y, .Y = line.TangentFrustum.X };
            for (uint32 face = 0; face < faceCount; ++face)
            {
                const ReferenceTriangle& owner = scene.Triangles[faces[face].TriangleIndex];
                const uint32 third = CORNER_INDEX_SUM - faces[face].BeginCorner - faces[face].EndCorner;
                float64 thirdDepthWorld = 0;
                const ProjectedPointFrustum thirdFrustum = project(owner.Positions[third], thirdDepthWorld);
                const float64 side = (thirdFrustum.X - edge.BeginFrustum.X) * normal.X +
                                     (thirdFrustum.Y - edge.BeginFrustum.Y) * normal.Y;
                faces[face].Side =
                    side > 0 ? EdgeSide::ObstacleLeft : (side < 0 ? EdgeSide::ObstacleRight : EdgeSide::None);
            }
            const float64 lengthWorld = line.EndAlongWorld - line.BeginAlongWorld;
            const auto classify = [&](const SegmentSpan& span) noexcept {
                const float64 middleWorld = (span.BeginAlongWorld + span.EndAlongWorld) * 0.5;
                const float64 edgeCoordinate = (middleWorld - line.BeginAlongWorld) / lengthWorld;
                bool isLeftOwned = false;
                bool isRightOwned = false;
                for (uint32 face = 0; face < faceCount; ++face)
                {
                    if (faces[face].Side == EdgeSide::None)
                    {
                        continue;
                    }
                    StaticArray<float64, WAVE_TRIANGLE_CORNERS> weights{};
                    weights[faces[face].BeginCorner] = 1 - edgeCoordinate;
                    weights[faces[face].EndCorner] = edgeCoordinate;
                    if (IsWaveTriangleCovered(scene, scene.Triangles[faces[face].TriangleIndex], weights) == false)
                    {
                        continue;
                    }
                    if (faces[face].Side == EdgeSide::ObstacleLeft)
                    {
                        isLeftOwned = true;
                    }
                    else
                    {
                        isRightOwned = true;
                    }
                }
                // Only an edge whose own faces cover exactly one side can bound the obstacle there; the open side
                // must also be clear of every other projected triangle.
                if (isLeftOwned == isRightOwned)
                {
                    return EdgeSide::None;
                }
                const float64 offsetWorld =
                    math::Max(LUDUS_WAVE_FRAUNHOFER_SIDE_OFFSET * (span.EndAlongWorld - span.BeginAlongWorld),
                              connection.RoundoffWorld);
                const float64 openSign = isLeftOwned == true ? -1.0 : 1.0;
                const ProjectedPointFrustum sampleFrustum
                {
                    .X = normal.X * (line.OffsetWorld + openSign * offsetWorld) + line.TangentFrustum.X * middleWorld,
                    .Y = normal.Y * (line.OffsetWorld + openSign * offsetWorld) + line.TangentFrustum.Y * middleWorld,
                };
                if (IsProjectedPointBlocked(scene, region, sampleFrustum) == true)
                {
                    return EdgeSide::None;
                }
                return isLeftOwned == true ? EdgeSide::ObstacleLeft : EdgeSide::ObstacleRight;
            };
            EdgeAccumulation accumulation;
            rhi::RasterStatus status =
                AccumulateEdge(line, beam, connection.IsBallisticBlocked, classify, accumulation);
            if (status != rhi::RasterStatus::Ready)
            {
                return status;
            }
            if (accumulation.HasBoundary == false)
            {
                continue;
            }
            // Single diffraction: the source and the receiver must see the stationary point past the region's slab.
            const math::Vector3d beginWorld{static_cast<float64>(begin.X),
                                            static_cast<float64>(begin.Y),
                                            static_cast<float64>(begin.Z)};
            const math::Vector3d endWorld{static_cast<float64>(end.X),
                                          static_cast<float64>(end.Y),
                                          static_cast<float64>(end.Z)};
            const float64 stationaryCoordinate =
                (accumulation.StationaryAlongWorld - line.BeginAlongWorld) / lengthWorld;
            const math::Vector3d pointWorld = beginWorld + (endWorld - beginWorld) * stationaryCoordinate;
            const float64 pointDepthWorld = InterpolateDepth(line, accumulation.StationaryAlongWorld);
            WaveExcludedTriangles excluded;
            for (uint32 face = 0; face < faceCount; ++face)
            {
                excluded.Triangles[excluded.Count++] = faces[face].TriangleIndex;
            }
            const math::Vector3d incidentWorld = pointWorld - connection.SourceWorld;
            const float64 incidentLengthWorld = math::Sqrt(math::Dot(incidentWorld, incidentWorld));
            const float64 incidentFraction = math::Min(1.0, region.Frustum.NearWorld / pointDepthWorld);
            bool isIncidentClear = false;
            status = IsWaveSegmentClear(scene,
                                        connection.SourceWorld,
                                        pointWorld,
                                        connection.EndpointEscapeWorld,
                                        incidentLengthWorld * (1 - incidentFraction),
                                        excluded,
                                        isIncidentClear);
            if (status != rhi::RasterStatus::Ready)
            {
                return status;
            }
            if (isIncidentClear == false)
            {
                continue;
            }
            const math::Vector3d diffractedWorld = connection.ReceiverWorld - pointWorld;
            const float64 diffractedLengthWorld = math::Sqrt(math::Dot(diffractedWorld, diffractedWorld));
            const float64 diffractedFraction =
                math::Max(0.0,
                          (region.Frustum.FarWorld - pointDepthWorld) / (connection.DistanceWorld - pointDepthWorld));
            bool isDiffractedClear = false;
            status = IsWaveSegmentClear(scene,
                                        pointWorld,
                                        connection.ReceiverWorld,
                                        diffractedLengthWorld * diffractedFraction,
                                        connection.EndpointEscapeWorld,
                                        excluded,
                                        isDiffractedClear);
            if (status != rhi::RasterStatus::Ready)
            {
                return status;
            }
            if (isDiffractedClear == true)
            {
                inoutField.Real += accumulation.Field.Real;
                inoutField.Imaginary += accumulation.Field.Imaginary;
            }
        }
    }
    return rhi::RasterStatus::Ready;
}
} // namespace

rhi::RasterStatus EvaluateFraunhoferEdgeField(const FraunhoferEdge& edge,
                                              const FraunhoferBeam& beam,
                                              bool isAxisBlocked,
                                              WaveField& outField) noexcept
{
    EdgeLine line;
    if (IsBeamValid(beam) == false || IsDepthInside(beam, edge.BeginDepthWorld) == false ||
        IsDepthInside(beam, edge.EndDepthWorld) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    if (TryMakeEdgeLine(edge, line) == false)
    {
        outField = {};
        return rhi::RasterStatus::Ready;
    }
    const EdgeSide side = edge.IsObstacleLeft == true ? EdgeSide::ObstacleLeft : EdgeSide::ObstacleRight;
    EdgeAccumulation accumulation;
    const rhi::RasterStatus status =
        AccumulateEdge(line, beam, isAxisBlocked, [side](const SegmentSpan&) noexcept { return side; }, accumulation);
    if (status == rhi::RasterStatus::Ready)
    {
        outField = accumulation.Field;
    }
    return status;
}

rhi::RasterStatus EvaluateFraunhoferConnectionToReceiver(const ReferenceSceneData& scene,
                                                         math::Vector3d sourceWorld,
                                                         math::Vector3d receiverWorld,
                                                         math::Vector3d directionWorld,
                                                         float64 distanceWorld,
                                                         float64 wavelengthWorld,
                                                         WaveField& outField) noexcept
{
    return EvaluateWaveEdgeConnection(scene,
                                      sourceWorld,
                                      receiverWorld,
                                      directionWorld,
                                      distanceWorld,
                                      wavelengthWorld,
                                      AddFraunhoferRegionFields,
                                      outField);
}
} // namespace ludus::graphics::renderer::internal
