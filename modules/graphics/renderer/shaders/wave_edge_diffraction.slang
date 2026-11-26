#pragma once
// GPU UTD connection operator; included by wave_connection.slang. See wave_edge_diffraction.cpp for the declared
// field, attribution (Steinberg and Pharr 2026, Section 5.2.2; Kouyoumjian and Pathak 1974) and limits. Edge
// enumeration, coverage, visibility and coherent accumulation execute here on the GPU, mirroring the CPU oracle.
static const float WAVE_UTD_EIGHTH_TURN = 0.25 * PI;
static const float WAVE_UTD_SINC_SECOND_TERM = 1.0 / 6;
static const uint WAVE_UTD_TRIANGLE_CORNERS = 3;
static const uint WAVE_UTD_NO_CORNER = WAVE_UTD_TRIANGLE_CORNERS;
/// 0 + 1 + 2: subtracting two corner indices from it gives the third.
static const uint WAVE_UTD_CORNER_INDEX_SUM = 3;
/// One face is a thin half-plane; two faces bound a wedge.
static const uint WAVE_UTD_MAX_EDGE_FACES = 2;
static const uint WAVE_UTD_NO_TRIANGLE = 0xffffffff;

float2 DivideWaveField(float2 numerator, float2 denominator)
{
    return MultiplyWaveField(numerator, float2(denominator.x, -denominator.y)) / dot(denominator, denominator);
}
float EvaluateWaveSinc(float value)
{
    return abs(value) < LUDUS_WAVE_UTD_SINC_LIMIT ? 1 - value * value * WAVE_UTD_SINC_SECOND_TERM : sin(value) / value;
}
/// Angle in [0, 2 pi) rotating the unit `reference` to the unit `direction` about the unit `axis`.
float MeasureWaveAngleAbout(float3 axis, float3 reference, float3 direction)
{
    float angle = atan2(dot(cross(reference, direction), axis), dot(reference, direction));
    return angle < 0 ? angle + 2 * PI : angle;
}
/// G(X) = F(X) / sqrt(X) of Kouyoumjian and Pathak's transition function, for X >= 0.
float2 EvaluateUtdTransitionRatio(float argument)
{
    float root = sqrt(argument);
    if (argument < LUDUS_WAVE_UTD_SERIES_LIMIT)
    {
        float2 sum = float2(0);
        float2 rotation = float2(1, 0);
        float magnitude = root;
        for (uint term = 0; term < LUDUS_WAVE_UTD_SERIES_TERMS; ++term)
        {
            sum += rotation * (magnitude / float(2 * term + 1));
            rotation = float2(rotation.y, -rotation.x);
            magnitude *= argument / float(term + 1);
        }
        float halfRootPi = sqrt(PI) * 0.5;
        float2 tail = float2(halfRootPi * cos(WAVE_UTD_EIGHTH_TURN), -halfRootPi * sin(WAVE_UTD_EIGHTH_TURN)) - sum;
        float2 rotated = MultiplyWaveField(MakeWavePhase(argument), tail);
        return float2(-2 * rotated.y, 2 * rotated.x);
    }
    float2 rotation = MakeWavePhase(WAVE_UTD_EIGHTH_TURN);
    float2 z = rotation * root;
    float2 denominator = z;
    for (uint level = LUDUS_WAVE_UTD_FRACTION_DEPTH; level > 0; --level)
        denominator = z + DivideWaveField(float2(float(level) * 0.5, 0), denominator);
    return DivideWaveField(rotation, denominator);
}
/// One incident shadow-boundary term divided by sqrt(kL); epsilon is its distance from the boundary.
float2 EvaluateUtdIncidentTerm(float epsilon, float wedgeIndex, float distanceWavenumber)
{
    float halfEpsilon = epsilon * 0.5;
    float sign = epsilon < 0 ? -1.0 : 1.0;
    float factor = sqrt(2.0) * wedgeIndex * sign * cos(halfEpsilon / wedgeIndex) * EvaluateWaveSinc(halfEpsilon) /
                   EvaluateWaveSinc(halfEpsilon / wedgeIndex);
    float sine = sin(halfEpsilon);
    return EvaluateUtdTransitionRatio(distanceWavenumber * 2 * sine * sine) * factor;
}
/// A straight edge segment and the unit in-face directions, perpendicular to it, of the faces meeting along it.
struct UtdEdge
{
    float3 BeginWorld;
    float3 EndWorld;
    float3 FaceDirectionsWorld[WAVE_UTD_MAX_EDGE_FACES];
    uint FaceCount;
};
/// One edge's diffracted field relative to the unobstructed field, and its diffraction point.
struct UtdEdgeField
{
    float2 Field;
    float3 PointWorld;
    bool IsDiffracting;
};
/// Diffracted field of `edge` for a point source and receiver; matches EvaluateUtdEdgeField in the CPU oracle.
UtdEdgeField EvaluateUtdEdgeField(UtdEdge edge, float3 sourceWorld, float3 receiverWorld, float wavelengthWorld)
{
    UtdEdgeField result = (UtdEdgeField)0;
    if (edge.FaceCount == 0 || edge.FaceCount > WAVE_UTD_MAX_EDGE_FACES || wavelengthWorld <= 0)
        return result;
    float3 edgeWorld = edge.EndWorld - edge.BeginWorld;
    float lengthWorld = length(edgeWorld);
    if (lengthWorld <= 0 || IsFiniteBits(lengthWorld) == false)
        return result;
    float3 tangent = edgeWorld / lengthWorld;
    float3 sourceOffsetWorld = sourceWorld - edge.BeginWorld;
    float3 receiverOffsetWorld = receiverWorld - edge.BeginWorld;
    float sourceAlongWorld = dot(sourceOffsetWorld, tangent);
    float receiverAlongWorld = dot(receiverOffsetWorld, tangent);
    float3 sourceRadialWorld = sourceOffsetWorld - tangent * sourceAlongWorld;
    float3 receiverRadialWorld = receiverOffsetWorld - tangent * receiverAlongWorld;
    float sourceRadiusWorld = length(sourceRadialWorld);
    float receiverRadiusWorld = length(receiverRadialWorld);
    if (sourceRadiusWorld <= 0 || receiverRadiusWorld <= 0)
        return result;
    float pointAlongWorld = sourceAlongWorld + (receiverAlongWorld - sourceAlongWorld) * sourceRadiusWorld /
                                                   (sourceRadiusWorld + receiverRadiusWorld);
    if (pointAlongWorld <= 0 || pointAlongWorld >= lengthWorld || IsFiniteBits(pointAlongWorld) == false)
        return result;
    float3 sourceDirection = sourceRadialWorld / sourceRadiusWorld;
    float3 receiverDirection = receiverRadialWorld / receiverRadiusWorld;
    float3 firstFace = edge.FaceDirectionsWorld[0];
    float exteriorAngle = 2 * PI;
    if (edge.FaceCount == WAVE_UTD_MAX_EDGE_FACES)
    {
        float between = MeasureWaveAngleAbout(tangent, edge.FaceDirectionsWorld[0], edge.FaceDirectionsWorld[1]);
        float sourceFromFirst = MeasureWaveAngleAbout(tangent, edge.FaceDirectionsWorld[0], sourceDirection);
        if (sourceFromFirst < between)
        {
            exteriorAngle = between;
        }
        else
        {
            firstFace = edge.FaceDirectionsWorld[1];
            exteriorAngle = 2 * PI - between;
        }
    }
    float sourceAngle = MeasureWaveAngleAbout(tangent, firstFace, sourceDirection);
    float receiverAngle = MeasureWaveAngleAbout(tangent, firstFace, receiverDirection);
    if (sourceAngle <= 0 || sourceAngle >= exteriorAngle || receiverAngle <= 0 || receiverAngle >= exteriorAngle)
        return result;
    float wedgeIndex = exteriorAngle / PI;
    bool isSourceSilhouette = (sourceAngle < PI) != (sourceAngle > (wedgeIndex - 1) * PI);
    bool isReceiverSilhouette = (receiverAngle < PI) != (receiverAngle > (wedgeIndex - 1) * PI);
    if (isSourceSilhouette == false && isReceiverSilhouette == false)
        return result;
    float incidentAlongWorld = pointAlongWorld - sourceAlongWorld;
    float diffractedAlongWorld = receiverAlongWorld - pointAlongWorld;
    float incidentWorld = sqrt(sourceRadiusWorld * sourceRadiusWorld + incidentAlongWorld * incidentAlongWorld);
    float diffractedWorld =
        sqrt(receiverRadiusWorld * receiverRadiusWorld + diffractedAlongWorld * diffractedAlongWorld);
    float pathWorld = incidentWorld + diffractedWorld;
    float directDistanceWorld = length(receiverWorld - sourceWorld);
    float wavenumber = 2 * PI / wavelengthWorld;
    float sineIncidence = sourceRadiusWorld / incidentWorld;
    float distanceParameterWorld = diffractedWorld * incidentWorld * sineIncidence * sineIncidence / pathWorld;
    float distanceWavenumber = wavenumber * distanceParameterWorld;
    float angleDifference = receiverAngle - sourceAngle;
    float period = 2 * PI * wedgeIndex;
    float plusIndex = floor((angleDifference + PI) / period + 0.5);
    float minusIndex = floor((angleDifference - PI) / period + 0.5);
    float2 terms = EvaluateUtdIncidentTerm(PI + angleDifference - period * plusIndex, wedgeIndex, distanceWavenumber) +
                   EvaluateUtdIncidentTerm(PI - angleDifference + period * minusIndex, wedgeIndex, distanceWavenumber);
    float3 directionSum = sourceDirection + receiverDirection;
    float pathDifferenceWorld =
        sourceRadiusWorld * receiverRadiusWorld * dot(directionSum, directionSum) / (pathWorld + directDistanceWorld);
    float cycles = pathDifferenceWorld / wavelengthWorld;
    float2 delay = MakeWavePhase(-2 * PI * (cycles - floor(cycles)));
    float scale = -directDistanceWorld / pathWorld / (2 * wedgeIndex * sqrt(2 * PI));
    float2 leading = MakeWavePhase(-WAVE_UTD_EIGHTH_TURN) * scale;
    result.Field = MultiplyWaveField(MultiplyWaveField(leading, terms), delay);
    result.PointWorld = edge.BeginWorld + tangent * pointAlongWorld;
    result.IsDiffracting = IsFiniteBits(result.Field.x) == true && IsFiniteBits(result.Field.y) == true;
    if (result.IsDiffracting == false)
        result.Field = float2(0);
    return result;
}
/// Inputs an edge operator shares across the regions of one connection; mirrors WaveEdgeConnection in C++.
struct WaveEdgeConnection
{
    float3 SourceWorld;
    float3 ReceiverWorld;
    float3 DirectionWorld;
    /// Unit transverse axis of the connection frame; the other is cross(DirectionWorld, AxisXWorld).
    float3 AxisXWorld;
    float DistanceWorld;
    float WavelengthWorld;
    float SigmaWorldSquared;
    float EnvelopeRadiusWorld;
    float EndpointEscapeWorld;
    float RoundoffWorld;
    bool IsBallisticBlocked;
};
/// An edge connection between interaction regions: its inputs, the remaining cone and the field so far.
struct WaveEdgeWalk
{
    WaveEdgeConnection Connection;
    EllipticalCone Cone;
    float2 Field;
    /// The connection needs no further regions; Field is final.
    bool IsComplete;
};
/// Whether the segment from nearWorld past `beginWorld` to farMarginWorld before `endWorld` is clear, ignoring
/// the `excluded` faces.
[noinline] WaveQuery IsWaveSegmentClear(SceneContext scene,
                                        float3 beginWorld,
                                        float3 endWorld,
                                        float nearWorld,
                                        float farMarginWorld,
                                        uint2 excluded,
                                        out bool outIsClear)
{
    outIsClear = true;
    float3 deltaWorld = endWorld - beginWorld;
    float lengthWorld = length(deltaWorld);
    if (lengthWorld - farMarginWorld <= nearWorld)
        return WaveQuery::Ready;
    EllipticalCone ray = (EllipticalCone)0;
    ray.OriginWorld = beginWorld;
    ray.DirectionWorld = deltaWorld / lengthWorld;
    float3 helperWorld =
        abs(ray.DirectionWorld.y) < WAVE_CONNECTION_FRAME_HELPER_COSINE ? float3(0, 1, 0) : float3(1, 0, 0);
    ray.AxisXWorld = normalize(cross(helperWorld, ray.DirectionWorld));
    ray.MinorToMajor = 1;
    ray.NearWorld = nearWorld;
    ray.FarWorld = lengthWorld - farMarginWorld;
    bool isBlocked;
    WaveQuery status = TraceWaveBallistic(scene, ray, excluded, isBlocked);
    outIsClear = isBlocked == false;
    return status;
}
/// Corner of the triangle whose position equals `position` bit for bit, or WAVE_UTD_NO_CORNER.
uint FindUtdCorner(TriangleData triangle, float3 position)
{
    if (all(asuint(triangle.Position0World) == asuint(position)) == true)
        return 0;
    if (all(asuint(triangle.Position1World) == asuint(position)) == true)
        return 1;
    if (all(asuint(triangle.Position2World) == asuint(position)) == true)
        return 2;
    return WAVE_UTD_NO_CORNER;
}
float3 GetUtdCorner(TriangleData triangle, uint corner)
{
    return corner == 0 ? triangle.Position0World : (corner == 1 ? triangle.Position1World : triangle.Position2World);
}
/// Add the diffracted fields of the edges in `region` whose diffraction points lie in its slab and envelope.
WaveQuery AddUtdRegionEdgeFields(SceneContext scene,
                                 WaveEdgeConnection connection,
                                 WaveInteractionRegion region,
                                 inout float2 inoutField)
{
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
            float3 edgeWorld = endWorld - beginWorld;
            float lengthWorld = length(edgeWorld);
            float3 tangent = edgeWorld / lengthWorld;
            UtdEdge edge = (UtdEdge)0;
            edge.BeginWorld = beginWorld;
            edge.EndWorld = endWorld;
            bool isDegenerate = false;
            for (uint face = 0; face < faceCount; ++face)
            {
                TriangleData owner = GetTriangle(scene, faceTriangles[face]);
                uint third = WAVE_UTD_CORNER_INDEX_SUM - faceBegins[face] - faceEnds[face];
                float3 offsetWorld = GetUtdCorner(owner, third) - beginWorld;
                float3 inFaceWorld = offsetWorld - tangent * dot(offsetWorld, tangent);
                float inFaceLength = length(inFaceWorld);
                if (inFaceLength <= 0 || IsFiniteBits(inFaceLength) == false)
                {
                    isDegenerate = true;
                    break;
                }
                edge.FaceDirectionsWorld[face] = inFaceWorld / inFaceLength;
            }
            if (isDegenerate == true)
                continue;
            edge.FaceCount = faceCount;
            UtdEdgeField diffracted = EvaluateUtdEdgeField(edge,
                                                           connection.SourceWorld,
                                                           connection.ReceiverWorld,
                                                           connection.WavelengthWorld);
            if (diffracted.IsDiffracting == false)
                continue;
            float3 pointOffsetWorld = diffracted.PointWorld - connection.SourceWorld;
            float axialWorld = dot(pointOffsetWorld, connection.DirectionWorld);
            float3 radialWorld = pointOffsetWorld - connection.DirectionWorld * axialWorld;
            if (axialWorld < region.Frustum.NearWorld || axialWorld > region.Frustum.FarWorld ||
                dot(radialWorld, radialWorld) > connection.EnvelopeRadiusWorld * connection.EnvelopeRadiusWorld)
                continue;
            float edgeCoordinate = dot(diffracted.PointWorld - beginWorld, tangent) / lengthWorld;
            UtdEdge covered = edge;
            covered.FaceCount = 0;
            for (uint face = 0; face < faceCount; ++face)
            {
                float3 weights = float3(0);
                weights[faceBegins[face]] = 1 - edgeCoordinate;
                weights[faceEnds[face]] = edgeCoordinate;
                if (IsWaveTriangleCovered(scene, faceTriangles[face], weights.yz) == true)
                {
                    covered.FaceDirectionsWorld[covered.FaceCount] = edge.FaceDirectionsWorld[face];
                    ++covered.FaceCount;
                }
            }
            if (covered.FaceCount == 0)
                continue;
            if (covered.FaceCount != edge.FaceCount)
            {
                diffracted = EvaluateUtdEdgeField(covered,
                                                  connection.SourceWorld,
                                                  connection.ReceiverWorld,
                                                  connection.WavelengthWorld);
                if (diffracted.IsDiffracting == false)
                    continue;
            }
            uint2 excluded = uint2(faceTriangles[0], faceTriangles[1]);
            bool isIncidentClear;
            WaveQuery status = IsWaveSegmentClear(scene,
                                                  connection.SourceWorld,
                                                  diffracted.PointWorld,
                                                  connection.EndpointEscapeWorld,
                                                  0,
                                                  excluded,
                                                  isIncidentClear);
            if (status != WaveQuery::Ready)
                return status;
            if (isIncidentClear == false)
                continue;
            bool isDiffractedClear;
            status = IsWaveSegmentClear(scene,
                                        diffracted.PointWorld,
                                        connection.ReceiverWorld,
                                        0,
                                        connection.EndpointEscapeWorld,
                                        excluded,
                                        isDiffractedClear);
            if (status != WaveQuery::Ready)
                return status;
            if (isDiffractedClear == true)
                inoutField += diffracted.Field;
        }
    }
    return WaveQuery::Ready;
}
/// Validate a connection, trace its ballistic centre ray and escape both endpoints, as the shared C++ driver
/// EvaluateWaveEdgeConnection does. Accepts and rejects the same inputs as EvaluateWaveConnectionToReceiver.
WaveQuery BeginWaveEdgeConnection(SceneContext scene,
                                  float3 sourceWorld,
                                  float3 receiverWorld,
                                  float3 directionWorld,
                                  float distanceWorld,
                                  float wavelengthWorld,
                                  out WaveEdgeWalk outWalk)
{
    outWalk = (WaveEdgeWalk)0;
    if (IsFiniteBits(sourceWorld) == false || IsFiniteBits(receiverWorld) == false ||
        IsFiniteBits(directionWorld) == false || IsFiniteBits(distanceWorld) == false || distanceWorld <= 0 ||
        IsFiniteBits(wavelengthWorld) == false || wavelengthWorld < LUDUS_WAVE_CONNECTION_MIN_WAVELENGTH_WORLD ||
        wavelengthWorld / distanceWorld > WAVE_CONNECTION_MAX_WAVELENGTH_TO_DISTANCE ||
        abs(dot(directionWorld, directionWorld) - 1) > WAVE_CONNECTION_UNIT_TOLERANCE)
        return WaveQuery::Invalid;
    float wavenumber = 2 * PI / wavelengthWorld;
    float sigmaWorldSquared = distanceWorld / wavenumber;
    float halfWidthWorld = LUDUS_WAVE_CONNECTION_TAIL_DEVIATIONS * sqrt(2 * sigmaWorldSquared);
    float coordinateScaleWorld = max(distanceWorld,
                                     max(max(abs(sourceWorld.x), max(abs(sourceWorld.y), abs(sourceWorld.z))),
                                         max(abs(receiverWorld.x), max(abs(receiverWorld.y), abs(receiverWorld.z)))));
    float endpointEscapeWorld = max(LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld,
                                    LUDUS_WAVE_CONNECTION_ENDPOINT_ROUNDOFF_SCALE * coordinateScaleWorld);
    if (2 * endpointEscapeWorld >= distanceWorld)
        return WaveQuery::Invalid;
    EllipticalCone cone = (EllipticalCone)0;
    cone.OriginWorld = sourceWorld;
    cone.DirectionWorld = directionWorld;
    float3 helperWorld =
        abs(directionWorld.y) < WAVE_CONNECTION_FRAME_HELPER_COSINE ? float3(0, 1, 0) : float3(1, 0, 0);
    cone.AxisXWorld = normalize(cross(helperWorld, directionWorld));
    cone.RadiusWorld = halfWidthWorld;
    cone.MinorToMajor = 1;
    cone.NearWorld = endpointEscapeWorld;
    cone.FarWorld = distanceWorld - endpointEscapeWorld;
    bool isBlocked;
    WaveQuery status = TraceWaveBallistic(scene, cone, uint2(WAVE_CONNECTION_NO_EXCLUDED_TRIANGLE), isBlocked);
    if (status != WaveQuery::Ready)
        return status;
    outWalk.Field = float2(isBlocked == true ? 0 : 1, 0);
    float sourceEscapeWorld;
    bool isEscapeBlocked;
    bool isEntirelyBallistic;
    status =
        EscapeWaveOrigin(scene, cone, wavelengthWorld, false, sourceEscapeWorld, isEscapeBlocked, isEntirelyBallistic);
    if (status != WaveQuery::Ready)
        return status;
    if (isEntirelyBallistic == true)
    {
        outWalk.IsComplete = true;
        return WaveQuery::Ready;
    }
    EllipticalCone reverse = cone;
    reverse.OriginWorld = receiverWorld;
    reverse.DirectionWorld = -directionWorld;
    float receiverEscapeWorld;
    status = EscapeWaveOrigin(scene,
                              reverse,
                              wavelengthWorld,
                              false,
                              receiverEscapeWorld,
                              isEscapeBlocked,
                              isEntirelyBallistic);
    if (status != WaveQuery::Ready)
        return status;
    if (isEntirelyBallistic == true)
    {
        outWalk.IsComplete = true;
        return WaveQuery::Ready;
    }
    outWalk.Connection.SourceWorld = sourceWorld;
    outWalk.Connection.ReceiverWorld = receiverWorld;
    outWalk.Connection.DirectionWorld = directionWorld;
    outWalk.Connection.AxisXWorld = cone.AxisXWorld;
    outWalk.Connection.DistanceWorld = distanceWorld;
    outWalk.Connection.WavelengthWorld = wavelengthWorld;
    outWalk.Connection.SigmaWorldSquared = sigmaWorldSquared;
    outWalk.Connection.EnvelopeRadiusWorld = halfWidthWorld;
    outWalk.Connection.EndpointEscapeWorld = endpointEscapeWorld;
    outWalk.Connection.RoundoffWorld = LUDUS_WAVE_CONNECTION_ENDPOINT_ROUNDOFF_SCALE * coordinateScaleWorld;
    outWalk.Connection.IsBallisticBlocked = isBlocked;
    cone.NearWorld = sourceEscapeWorld;
    cone.FarWorld = max(cone.NearWorld, distanceWorld - receiverEscapeWorld);
    outWalk.Cone = cone;
    return WaveQuery::Ready;
}
/// Move the walk's cone past `region`, skipping geometry that continues straight past it as the angular-spectrum
/// operator does.
WaveQuery AdvanceWaveEdgeWalk(SceneContext scene, WaveInteractionRegion region, inout WaveEdgeWalk inoutWalk)
{
    float nextNearWorld = asfloat(asuint(region.Frustum.FarWorld) + 1);
    inoutWalk.Cone.NearWorld = min(inoutWalk.Cone.FarWorld, nextNearWorld);
    float wavelengthWorld = inoutWalk.Connection.WavelengthWorld;
    if (inoutWalk.Cone.FarWorld - inoutWalk.Cone.NearWorld <=
        LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld)
        return WaveQuery::Ready;
    EllipticalCone skip = inoutWalk.Cone;
    skip.OriginWorld =
        inoutWalk.Connection.SourceWorld + inoutWalk.Connection.DirectionWorld * inoutWalk.Cone.NearWorld;
    skip.FarWorld = inoutWalk.Cone.FarWorld - inoutWalk.Cone.NearWorld;
    skip.NearWorld = min(skip.FarWorld, inoutWalk.Connection.EndpointEscapeWorld);
    float skippedWorld;
    bool isEscapeBlocked;
    bool isEntirelyBallistic;
    WaveQuery status =
        EscapeWaveOrigin(scene, skip, wavelengthWorld, false, skippedWorld, isEscapeBlocked, isEntirelyBallistic);
    if (status != WaveQuery::Ready)
        return status;
    inoutWalk.Cone.NearWorld = isEntirelyBallistic == true
                                   ? inoutWalk.Cone.FarWorld
                                   : min(inoutWalk.Cone.FarWorld, inoutWalk.Cone.NearWorld + skippedWorld);
    return WaveQuery::Ready;
}
/// Ballistic field plus coherent single-edge UTD fields, relative to the unobstructed free-space field.
/// Accepts and rejects the same inputs as EvaluateWaveConnectionToReceiver; failure preserves the output.
[noinline] WaveQuery EvaluateUtdConnectionToReceiver(SceneContext scene,
                                                     float3 sourceWorld,
                                                     float3 receiverWorld,
                                                     float3 directionWorld,
                                                     float distanceWorld,
                                                     float wavelengthWorld,
                                                     inout float2 inoutField)
{
    WaveEdgeWalk walk;
    WaveQuery status = BeginWaveEdgeConnection(scene,
                                               sourceWorld,
                                               receiverWorld,
                                               directionWorld,
                                               distanceWorld,
                                               wavelengthWorld,
                                               walk);
    if (status != WaveQuery::Ready)
        return status;
    for (uint interaction = 0; interaction < LUDUS_WAVE_CONNECTION_REGIONS; ++interaction)
    {
        if (walk.IsComplete == true || walk.Cone.NearWorld >= walk.Cone.FarWorld)
        {
            inoutField = walk.Field;
            return WaveQuery::Ready;
        }
        WaveInteractionRegion region = (WaveInteractionRegion)0;
        status = FindWaveInteractionRegion(scene, walk.Cone, region);
        if (status != WaveQuery::Ready)
            return status;
        if (region.TriangleCount == 0)
        {
            inoutField = walk.Field;
            return WaveQuery::Ready;
        }
        status = AddUtdRegionEdgeFields(scene, walk.Connection, region, walk.Field);
        if (status != WaveQuery::Ready)
            return status;
        status = AdvanceWaveEdgeWalk(scene, region, walk);
        if (status != WaveQuery::Ready)
            return status;
    }
    return WaveQuery::PropagationCapacity;
}
#include "wave_fraunhofer_diffraction.slang"
