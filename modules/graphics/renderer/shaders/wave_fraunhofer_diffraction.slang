#pragma once
// GPU Fraunhofer edge connection operator; included by wave_edge_diffraction.slang. See
// wave_fraunhofer_diffraction.cpp for the declared field, attribution (Steinberg et al. 2024, "A Free-Space
// Diffraction BSDF", Eqs. 17, 22, 23, 26 and 27; Steinberg and Pharr 2026, Section 5.2.2) and departures. Edge
// enumeration, coverage classification, segmentation, visibility and coherent accumulation execute here on the
// GPU, mirroring the CPU oracle.
/// Taylor coefficients of (cos(x) - sin(x)/x) / 2x: -x/6 + x^3/60 - x^5/1680 + x^7/90720.
static const float WAVE_FRAUNHOFER_SERIES_FIRST = 1.0 / 6;
static const float WAVE_FRAUNHOFER_SERIES_SECOND = 1.0 / 60;
static const float WAVE_FRAUNHOFER_SERIES_THIRD = 1.0 / 1680;
static const float WAVE_FRAUNHOFER_SERIES_FOURTH = 1.0 / 90720;
/// Field an edge through the axis adds: half the ballistic step.
static const float WAVE_FRAUNHOFER_HALF = 0.5;
/// An edge is walked from its stationary point towards each end.
static const uint WAVE_FRAUNHOFER_WALK_DIRECTIONS = 2;

/// Which side of an edge segment the projected obstacle covers, if exactly one.
enum WaveEdgeSide : uint
{
    None = 0,
    ObstacleLeft = 1,
    ObstacleRight = 2,
};
/// (cos(x) - sinc(x)) / 2x for x = halfPhase.
float EvaluateFraunhoferLinearWeight(float halfPhase)
{
    if (abs(halfPhase) < LUDUS_WAVE_FRAUNHOFER_SERIES_LIMIT)
    {
        float square = halfPhase * halfPhase;
        return halfPhase *
               (-WAVE_FRAUNHOFER_SERIES_FIRST +
                square * (WAVE_FRAUNHOFER_SERIES_SECOND +
                          square * (-WAVE_FRAUNHOFER_SERIES_THIRD + square * WAVE_FRAUNHOFER_SERIES_FOURTH)));
    }
    return (cos(halfPhase) - sin(halfPhase) / halfPhase) / (2 * halfPhase);
}
/// Re(beta) and -Im(beta) of the beam kernel exp(-beta rho^2 / 2) at `depthWorld`; see EvaluateBeamKernel.
float2 EvaluateFraunhoferKernel(WaveEdgeConnection connection, float depthWorld)
{
    float wavenumber = 2 * PI / connection.WavelengthWorld;
    float correlationWorldSquared = depthWorld / wavenumber;
    float denominator =
        connection.SigmaWorldSquared * connection.SigmaWorldSquared + correlationWorldSquared * correlationWorldSquared;
    return float2(connection.SigmaWorldSquared / denominator,
                  correlationWorldSquared / denominator + wavenumber / (connection.DistanceWorld - depthWorld));
}
/// An edge's projected line in the connection frame; mirrors EdgeLine in the CPU oracle.
struct FraunhoferLine
{
    float2 TangentFrustum;
    float OffsetWorld;
    float BeginAlongWorld;
    float EndAlongWorld;
    float BeginDepthWorld;
    float EndDepthWorld;
};
/// Envelope amplitude and phase of the kernel at `alongWorld` on the line.
float2 SampleFraunhoferKernel(WaveEdgeConnection connection, FraunhoferLine line, float alongWorld)
{
    float fraction = (alongWorld - line.BeginAlongWorld) / (line.EndAlongWorld - line.BeginAlongWorld);
    float depthWorld = line.BeginDepthWorld + (line.EndDepthWorld - line.BeginDepthWorld) * fraction;
    float2 kernel = EvaluateFraunhoferKernel(connection, depthWorld);
    float radiusSquaredWorld = line.OffsetWorld * line.OffsetWorld + alongWorld * alongWorld;
    return float2(exp(-kernel.x * radiusSquaredWorld * 0.5), kernel.y * radiusSquaredWorld * 0.5);
}
/// (1/2pi) times the boundary integral over [beginWorld, endWorld]; mirrors EvaluateSegment in the CPU oracle.
float2 EvaluateFraunhoferSegment(WaveEdgeConnection connection,
                                 FraunhoferLine line,
                                 WaveEdgeSide side,
                                 float beginWorld,
                                 float endWorld)
{
    float heightWorld = side == WaveEdgeSide::ObstacleLeft ? -line.OffsetWorld : line.OffsetWorld;
    if (heightWorld == 0)
    {
        if (beginWorld <= 0 && 0 < endWorld)
            return float2(connection.IsBallisticBlocked == true ? WAVE_FRAUNHOFER_HALF : -WAVE_FRAUNHOFER_HALF, 0);
        return float2(0);
    }
    float distanceWorld = abs(heightWorld);
    float heightSign = heightWorld < 0 ? -1.0 : 1.0;
    float angle = heightSign * (atan2(endWorld, distanceWorld) - atan2(beginWorld, distanceWorld));
    if (angle == 0)
        return float2(0);
    float2 first = SampleFraunhoferKernel(connection, line, beginWorld);
    float2 last = SampleFraunhoferKernel(connection, line, endWorld);
    float2 middle = SampleFraunhoferKernel(connection, line, (beginWorld + endWorld) * 0.5);
    float halfPhase = (last.y - first.y) * 0.5;
    float mean = (first.x + last.x) * 0.5;
    float change = last.x - first.x;
    float2 wave = float2(mean * EvaluateWaveSinc(halfPhase), -change * EvaluateFraunhoferLinearWeight(halfPhase));
    return MultiplyWaveField(wave, MakeWavePhase(middle.y)) * (angle / (2 * PI));
}
/// Classify one segment from its own faces' coverage and the open side's projected coverage; mirrors the CPU
/// oracle's classifier.
WaveEdgeSide ClassifyFraunhoferSegment(SceneContext scene,
                                       WaveEdgeConnection connection,
                                       WaveInteractionRegion region,
                                       FraunhoferLine line,
                                       uint faceTriangles[WAVE_UTD_MAX_EDGE_FACES],
                                       uint faceBegins[WAVE_UTD_MAX_EDGE_FACES],
                                       uint faceEnds[WAVE_UTD_MAX_EDGE_FACES],
                                       WaveEdgeSide faceSides[WAVE_UTD_MAX_EDGE_FACES],
                                       uint faceCount,
                                       float beginWorld,
                                       float endWorld)
{
    float middleWorld = (beginWorld + endWorld) * 0.5;
    float edgeCoordinate = (middleWorld - line.BeginAlongWorld) / (line.EndAlongWorld - line.BeginAlongWorld);
    bool isLeftOwned = false;
    bool isRightOwned = false;
    for (uint face = 0; face < faceCount; ++face)
    {
        if (faceSides[face] == WaveEdgeSide::None)
            continue;
        float3 weights = float3(0);
        weights[faceBegins[face]] = 1 - edgeCoordinate;
        weights[faceEnds[face]] = edgeCoordinate;
        if (IsWaveTriangleCovered(scene, faceTriangles[face], weights.yz) == false)
            continue;
        if (faceSides[face] == WaveEdgeSide::ObstacleLeft)
            isLeftOwned = true;
        else
            isRightOwned = true;
    }
    if (isLeftOwned == isRightOwned)
        return WaveEdgeSide::None;
    float offsetWorld = max(LUDUS_WAVE_FRAUNHOFER_SIDE_OFFSET * (endWorld - beginWorld), connection.RoundoffWorld);
    float openSign = isLeftOwned == true ? -1.0 : 1.0;
    float2 normal = float2(-line.TangentFrustum.y, line.TangentFrustum.x);
    float2 samplePlaneWorld = normal * (line.OffsetWorld + openSign * offsetWorld) + line.TangentFrustum * middleWorld;
    if (IsWavePointBlocked(scene, region, samplePlaneWorld) == true)
        return WaveEdgeSide::None;
    return isLeftOwned == true ? WaveEdgeSide::ObstacleLeft : WaveEdgeSide::ObstacleRight;
}
/// Add the boundary-diffraction waves of the region's projected obstacle; mirrors AddFraunhoferRegionFields.
WaveQuery AddFraunhoferRegionFields(SceneContext scene,
                                    WaveEdgeConnection connection,
                                    WaveInteractionRegion region,
                                    inout float2 inoutField)
{
    float3 axisYWorld = cross(connection.DirectionWorld, connection.AxisXWorld);
    for (uint slot = 0; slot < region.TriangleCount; ++slot)
    {
        TriangleData triangle = GetTriangle(scene, region.Triangles[slot]);
        for (uint corner = 0; corner < WAVE_UTD_TRIANGLE_CORNERS; ++corner)
        {
            uint endCorner = (corner + 1) % WAVE_UTD_TRIANGLE_CORNERS;
            float3 beginWorld = GetUtdCorner(triangle, corner);
            float3 endWorld = GetUtdCorner(triangle, endCorner);
            if (all(asuint(beginWorld) == asuint(endWorld)) == true)
                continue;
            uint faceTriangles[WAVE_UTD_MAX_EDGE_FACES] = {region.Triangles[slot], WAVE_UTD_NO_TRIANGLE};
            uint faceBegins[WAVE_UTD_MAX_EDGE_FACES] = {corner, 0};
            uint faceEnds[WAVE_UTD_MAX_EDGE_FACES] = {endCorner, 0};
            WaveEdgeSide faceSides[WAVE_UTD_MAX_EDGE_FACES] = {WaveEdgeSide::None, WaveEdgeSide::None};
            uint faceCount = 1;
            bool isVisitedElsewhere = false;
            for (uint other = 0; other < region.TriangleCount; ++other)
            {
                if (other == slot)
                    continue;
                TriangleData candidate = GetTriangle(scene, region.Triangles[other]);
                uint candidateBegin = FindUtdCorner(candidate, beginWorld);
                uint candidateEnd = FindUtdCorner(candidate, endWorld);
                if (candidateBegin == WAVE_UTD_NO_CORNER || candidateEnd == WAVE_UTD_NO_CORNER)
                    continue;
                if (other < slot || faceCount == WAVE_UTD_MAX_EDGE_FACES)
                {
                    isVisitedElsewhere = true;
                    break;
                }
                faceTriangles[faceCount] = region.Triangles[other];
                faceBegins[faceCount] = candidateBegin;
                faceEnds[faceCount] = candidateEnd;
                ++faceCount;
            }
            if (isVisitedElsewhere == true)
                continue;
            float3 beginOffsetWorld = beginWorld - connection.SourceWorld;
            float3 endOffsetWorld = endWorld - connection.SourceWorld;
            float beginDepthWorld = dot(beginOffsetWorld, connection.DirectionWorld);
            float endDepthWorld = dot(endOffsetWorld, connection.DirectionWorld);
            if (beginDepthWorld <= 0 || beginDepthWorld >= connection.DistanceWorld || endDepthWorld <= 0 ||
                endDepthWorld >= connection.DistanceWorld)
                continue;
            float2 beginPlaneWorld =
                float2(dot(beginOffsetWorld, connection.AxisXWorld), dot(beginOffsetWorld, axisYWorld));
            float2 endPlaneWorld = float2(dot(endOffsetWorld, connection.AxisXWorld), dot(endOffsetWorld, axisYWorld));
            float2 deltaPlaneWorld = endPlaneWorld - beginPlaneWorld;
            float lengthWorld = length(deltaPlaneWorld);
            if (lengthWorld <= 0 || IsFiniteBits(lengthWorld) == false)
                continue;
            FraunhoferLine line;
            line.TangentFrustum = deltaPlaneWorld / lengthWorld;
            line.OffsetWorld = beginPlaneWorld.y * line.TangentFrustum.x - beginPlaneWorld.x * line.TangentFrustum.y;
            line.BeginAlongWorld = dot(beginPlaneWorld, line.TangentFrustum);
            line.EndAlongWorld = line.BeginAlongWorld + lengthWorld;
            line.BeginDepthWorld = beginDepthWorld;
            line.EndDepthWorld = endDepthWorld;
            float2 normal = float2(-line.TangentFrustum.y, line.TangentFrustum.x);
            for (uint face = 0; face < faceCount; ++face)
            {
                TriangleData owner = GetTriangle(scene, faceTriangles[face]);
                uint third = WAVE_UTD_CORNER_INDEX_SUM - faceBegins[face] - faceEnds[face];
                float3 thirdOffsetWorld = GetUtdCorner(owner, third) - connection.SourceWorld;
                float2 thirdPlaneWorld =
                    float2(dot(thirdOffsetWorld, connection.AxisXWorld), dot(thirdOffsetWorld, axisYWorld));
                float side = dot(thirdPlaneWorld - beginPlaneWorld, normal);
                faceSides[face] = side > 0 ? WaveEdgeSide::ObstacleLeft
                                           : (side < 0 ? WaveEdgeSide::ObstacleRight : WaveEdgeSide::None);
            }
            float chordSquaredWorld =
                connection.EnvelopeRadiusWorld * connection.EnvelopeRadiusWorld - line.OffsetWorld * line.OffsetWorld;
            if (chordSquaredWorld <= 0)
                continue;
            float chordWorld = sqrt(chordSquaredWorld);
            float lowWorld = max(line.BeginAlongWorld, -chordWorld);
            float highWorld = min(line.EndAlongWorld, chordWorld);
            if (lowWorld >= highWorld)
                continue;
            float2 firstKernel = EvaluateFraunhoferKernel(connection, line.BeginDepthWorld);
            float2 lastKernel = EvaluateFraunhoferKernel(connection, line.EndDepthWorld);
            float shortestWorld =
                LUDUS_WAVE_FRAUNHOFER_SEGMENT_SCALE / sqrt(max(length(firstKernel), length(lastKernel)));
            float startWorld = min(max(0.0, lowWorld), highWorld);
            float2 edgeField = float2(0);
            bool hasBoundary = false;
            for (uint direction = 0; direction < WAVE_FRAUNHOFER_WALK_DIRECTIONS; ++direction)
            {
                float walkSign = direction == 0 ? 1.0 : -1.0;
                float alongWorld = startWorld;
                uint segment = 0;
                while (walkSign > 0 ? alongWorld < highWorld : alongWorld > lowWorld)
                {
                    if (segment == LUDUS_WAVE_FRAUNHOFER_SIDE_SEGMENTS)
                        return WaveQuery::PropagationCapacity;
                    ++segment;
                    float stepWorld = max(shortestWorld, LUDUS_WAVE_FRAUNHOFER_SEGMENT_GROWTH * abs(alongWorld));
                    float nextWorld =
                        walkSign > 0 ? min(highWorld, alongWorld + stepWorld) : max(lowWorld, alongWorld - stepWorld);
                    float spanBeginWorld = min(alongWorld, nextWorld);
                    float spanEndWorld = max(alongWorld, nextWorld);
                    alongWorld = nextWorld;
                    WaveEdgeSide side = ClassifyFraunhoferSegment(scene,
                                                                  connection,
                                                                  region,
                                                                  line,
                                                                  faceTriangles,
                                                                  faceBegins,
                                                                  faceEnds,
                                                                  faceSides,
                                                                  faceCount,
                                                                  spanBeginWorld,
                                                                  spanEndWorld);
                    if (side == WaveEdgeSide::None)
                        continue;
                    edgeField += EvaluateFraunhoferSegment(connection, line, side, spanBeginWorld, spanEndWorld);
                    hasBoundary = true;
                }
            }
            if (hasBoundary == false)
                continue;
            if (IsFiniteBits(edgeField.x) == false || IsFiniteBits(edgeField.y) == false)
                return WaveQuery::Invalid;
            float stationaryCoordinate = (startWorld - line.BeginAlongWorld) / lengthWorld;
            float3 pointWorld = beginWorld + (endWorld - beginWorld) * stationaryCoordinate;
            float pointDepthWorld = beginDepthWorld + (endDepthWorld - beginDepthWorld) * stationaryCoordinate;
            uint2 excluded = uint2(faceTriangles[0], faceTriangles[1]);
            float incidentLengthWorld = length(pointWorld - connection.SourceWorld);
            float incidentFraction = min(1.0, region.Frustum.NearWorld / pointDepthWorld);
            // Past an interface event the source is virtual: the incident ray is tested within the leg only.
            float incidentNearWorld =
                connection.LegBeginWorld > 0
                    ? connection.EndpointEscapeWorld + connection.LegBeginWorld * incidentLengthWorld / pointDepthWorld
                    : connection.EndpointEscapeWorld;
            bool isIncidentClear;
            WaveQuery status = IsWaveSegmentClear(scene,
                                                  connection.SourceWorld,
                                                  pointWorld,
                                                  incidentNearWorld,
                                                  incidentLengthWorld * (1 - incidentFraction),
                                                  excluded,
                                                  isIncidentClear);
            if (status != WaveQuery::Ready)
                return status;
            if (isIncidentClear == false)
                continue;
            float diffractedLengthWorld = length(connection.ReceiverWorld - pointWorld);
            float diffractedFraction =
                max(0.0, (region.Frustum.FarWorld - pointDepthWorld) / (connection.DistanceWorld - pointDepthWorld));
            float beyondLegWorld = connection.DistanceWorld - connection.LegEndWorld;
            float diffractedMarginWorld =
                beyondLegWorld > 0 ? connection.EndpointEscapeWorld + beyondLegWorld * diffractedLengthWorld /
                                                                          (connection.DistanceWorld - pointDepthWorld)
                                   : connection.EndpointEscapeWorld;
            bool isDiffractedClear;
            status = IsWaveSegmentClear(scene,
                                        pointWorld,
                                        connection.ReceiverWorld,
                                        diffractedLengthWorld * diffractedFraction,
                                        diffractedMarginWorld,
                                        excluded,
                                        isDiffractedClear);
            if (status != WaveQuery::Ready)
                return status;
            if (isDiffractedClear == true)
                inoutField += edgeField;
        }
    }
    return WaveQuery::Ready;
}
#include "wave_relay.slang"
