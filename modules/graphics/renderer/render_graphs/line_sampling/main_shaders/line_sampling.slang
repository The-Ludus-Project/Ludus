#pragma once
// One line strategy on a triangle emitter (point-line design D6), in the direction U of the
// emitter triangle's first edge.
// Thanks to Niels Billen and Philip Dutre, "Line Sampling for Direct Illumination", Computer Graphics
// Forum 35(4) (EGSR 2016), pp. 93-102, https://doi.org/10/f84z2h : their surface-area line estimator
// places a line with density proportional to its chord length and integrates only the chord's
// visible parts, which FindVisibleLineIntervals finds from their shadow triangle (section 4).
// Thanks to Katherine Salesin and Wojciech Jarosz, "Combining Point and Line Samples for Direct
// Illumination", Computer Graphics Forum 38(4) (EGSR 2019), pp. 159-169,
// https://doi.org/10.1111/cgf.13779 , section 4 and equations 14-15: a line sample is a point
// sample with the joint density p(u, v) = p(v) p(u | v), with p(v) proportional to the chord
// length and p(u | v) uniform over the visible length.
// Departures (design section 2): lines are chords of one emitter triangle, not of a quad light; the
// chord's offset comes from an area-uniform SampleIncidentLight point, whose density is already
// c / A_T, so no extra inversion is needed; the point along the chord is drawn and evaluated instead
// of integrating the line analytically; and a chord that crosses alpha-tested triangles is confirmed
// with one TestVisibility ray (design D4). Directional, point, spot and environment lights keep the
// ordinary point proposal.

/// Receivers whose direction to the emitter plane has a cosine at most this (unitless) receive no
/// light from that triangle: the integrand carries the cosine, so it is negligible, and a chord seen
/// edge-on from its own plane would make the line query fail as degenerate.
static const float LINE_EMITTER_PLANE_COSINE_TOLERANCE = 0.000001;
/// Bound on chord end parameters before clipping to the triangle; any finite triangle lies inside it.
static const float LINE_CHORD_UNBOUNDED_PARAMETER = 1.0e30;

/// The chord of an emitter triangle through a point, as a line query segment and in barycentrics.
struct EmitterChord
{
    LineSegment Segment;
    float2 StartBarycentric;
    float2 EndBarycentric;
    float LengthWorld;
};

/// Draws of one line strategy: the selected light's point proposal and, for a mesh light, the chord
/// through that point, its visible length and the chord parameter drawn over its visible intervals.
/// The intervals themselves are read only inside ProposeLineU, right after the query: copying a
/// LineVisibility between functions lost its intervals on Metal in this merged kernel (PL1).
struct LineProposal
{
    LightSample PointSample;
    EmitterChord Chord;
    float EmitterAreaWorld;
    float VisibleLengthWorld;
    float DrawnParameter;
    bool HasMaskedOccluders;
    bool IsMeshChord;
    bool HasProposal;
};

/// Chord of the triangle through barycentric (b1, b2), along alongWorld in the triangle's plane.
/// Returns false for a degenerate triangle or nonfinite bounds. Where the line leaves the triangle at
/// once, as at a vertex whose interior angle excludes alongWorld, the chord is returned with zero
/// length; IsEmitterChordQueryable rejects it.
bool TryMakeEmitterChord(TriangleData triangle, float2 barycentric, float3 alongWorld, out EmitterChord outChord)
{
    outChord = (EmitterChord)0;
    float3 edge1World = triangle.Position1World - triangle.Position0World;
    float3 edge2World = triangle.Position2World - triangle.Position0World;
    float3 normalWorld = cross(edge1World, edge2World);
    float normalLengthSquared = dot(normalWorld, normalWorld);
    if (triangle.IsValid == false || normalLengthSquared <= 0)
        return false;
    // alongWorld = deltaB1 * edge1 + deltaB2 * edge2, so the barycentric weights move by these deltas.
    float deltaB1 = dot(cross(alongWorld, edge2World), normalWorld) / normalLengthSquared;
    float deltaB2 = dot(cross(edge1World, alongWorld), normalWorld) / normalLengthSquared;
    float3 weights = float3(1 - barycentric.x - barycentric.y, barycentric.x, barycentric.y);
    float3 deltas = float3(-deltaB1 - deltaB2, deltaB1, deltaB2);
    float minimum = -LINE_CHORD_UNBOUNDED_PARAMETER;
    float maximum = LINE_CHORD_UNBOUNDED_PARAMETER;
    for (uint k = 0; k < 3; ++k)
    {
        if (deltas[k] > 0)
            minimum = max(minimum, -weights[k] / deltas[k]);
        else if (deltas[k] < 0)
            maximum = min(maximum, -weights[k] / deltas[k]);
    }
    if (all(isfinite(float2(minimum, maximum))) == false)
        return false;
    // In exact arithmetic maximum == minimum only for a chord of zero length; rounding can reverse them.
    maximum = max(maximum, minimum);
    outChord.StartBarycentric = barycentric + float2(deltaB1, deltaB2) * minimum;
    outChord.EndBarycentric = barycentric + float2(deltaB1, deltaB2) * maximum;
    float3 pointWorld = triangle.Position0World + edge1World * barycentric.x + edge2World * barycentric.y;
    outChord.Segment.StartWorld = pointWorld + alongWorld * minimum;
    outChord.Segment.EndWorld = pointWorld + alongWorld * maximum;
    outChord.Segment.TriangleIndex = triangle.TriangleIndex;
    outChord.LengthWorld = length(outChord.Segment.EndWorld - outChord.Segment.StartWorld);
    return true;
}

/// True when FindVisibleLineIntervals accepts the chord's length, the same length it computes from
/// the segment. A shorter chord lies within SCENE_UNIT_TOLERANCE of a vertex where the line direction
/// leaves the triangle, or on an edge along the line that rounding makes it leave at once. The
/// strategy proposes nothing there and its density there is zero, so the set of such points, which
/// has zero area in exact arithmetic, only leaves the strategy's support (point-line design D6). The
/// area-uniform point lands on vertex P0 exactly when its radial draw is zero, a probability of 2^-24
/// with 24-bit draws, and the chord across the first edge through an acute P0 has zero length.
bool IsEmitterChordQueryable(EmitterChord chord)
{
    return chord.LengthWorld >= SCENE_UNIT_TOLERANCE;
}

/// Barycentrics of chord parameter t in [0, 1], clamped into the triangle against rounding.
float2 GetChordBarycentric(EmitterChord chord, float t)
{
    float2 barycentric = max(lerp(chord.StartBarycentric, chord.EndBarycentric, t), float2(0));
    float sum = barycentric.x + barycentric.y;
    return sum > 1 ? barycentric / sum : barycentric;
}

/// Select a light, propose a point on it and, for a mesh light, build its U chord, find the chord's
/// visible intervals and draw a chord parameter uniformly by length over them. Reads dimensions
/// firstDimension to firstDimension + 3, and positionDimension, at explicit addresses. Consumes one
/// line query for a mesh light unless the receiver lies in the emitter's plane, the chord is too short
/// to query (IsEmitterChordQueryable) or the receiver sees the whole chord below its horizon (all
/// contribute zero). A failed line query latches queries.HasFailed.
/// Returns false when there is nothing to estimate.
bool ProposeLineU(SceneContext scene,
                  RayLaunch launch,
                  SurfaceHit receiver,
                  float3 shadingNormalWorld,
                  bool isHorizonTested,
                  uint firstDimension,
                  uint positionDimension,
                  inout RayQueryContext queries,
                  out LineProposal outProposal)
{
    outProposal = (LineProposal)0;
    SelectedLight selected = SelectLight(scene, launch, firstDimension);
    if (selected.HasSample == false)
        return false;
    float3 shapeSample = float3(SampleSceneRandom(launch, firstDimension + 1),
                                SampleSceneRandom(launch, firstDimension + 2),
                                SampleSceneRandom(launch, firstDimension + 3));
    outProposal.PointSample = SampleIncidentLight(scene, selected, receiver, shapeSample);
    if (outProposal.PointSample.HasSample == false)
        return false;
    outProposal.HasProposal = true;
    uint triangleIndex = outProposal.PointSample.TriangleIndex;
    if (triangleIndex == SCENE_INVALID_INDEX)
        return true;
    TriangleData triangle = GetTriangle(scene, triangleIndex);
    SurfaceHit emitter = GetSurfaceHit(scene, triangleIndex, outProposal.PointSample.Barycentric);
    float3 towardReceiverWorld = receiver.PositionWorld - outProposal.PointSample.PositionWorld;
    if (abs(dot(triangle.GeometricNormalWorld, towardReceiverWorld)) <=
        LINE_EMITTER_PLANE_COSINE_TOLERANCE * length(towardReceiverWorld))
    {
        outProposal.HasProposal = false;
        return false;
    }
    if (emitter.IsValid == false || emitter.EmitterIndex >= GetEmitterCount(scene) ||
        TryMakeEmitterChord(triangle,
                            outProposal.PointSample.Barycentric,
                            triangle.Position1World - triangle.Position0World,
                            outProposal.Chord) == false)
    {
        queries.HasFailed = true;
        return false;
    }
    if (IsEmitterChordQueryable(outProposal.Chord) == false)
    {
        outProposal.HasProposal = false;
        return false;
    }
    outProposal.IsMeshChord = true;
    outProposal.EmitterAreaWorld = GetEmitter(scene, emitter.EmitterIndex).z;
    // The chord is a segment, so the receiver's upper hemisphere cuts it in one interval: when both
    // endpoints are below the horizon, every point of it is, and the estimate is exactly zero.
    if (isHorizonTested == true &&
        dot(shadingNormalWorld, outProposal.Chord.Segment.StartWorld - receiver.PositionWorld) <= 0 &&
        dot(shadingNormalWorld, outProposal.Chord.Segment.EndWorld - receiver.PositionWorld) <= 0)
    {
        outProposal.HasProposal = false;
        return false;
    }
    LineVisibility visibility;
    if (FindVisibleLineIntervals(scene, queries, receiver, outProposal.Chord.Segment, visibility) ==
        SCENE_QUERY_FAILURE)
    {
        outProposal.HasProposal = false;
        return false;
    }
    outProposal.VisibleLengthWorld = visibility.VisibleLengthWorld;
    outProposal.HasMaskedOccluders = visibility.HasMaskedOccluders;
    if (visibility.IntervalCount == 0)
        return true;
    // Map one uniform draw linearly across the concatenated visible intervals.
    float remaining =
        SampleSceneRandom(launch, positionDimension) * visibility.VisibleLengthWorld / outProposal.Chord.LengthWorld;
    outProposal.DrawnParameter = visibility.Intervals[visibility.IntervalCount - 1].y;
    for (uint i = 0; i < visibility.IntervalCount; ++i)
    {
        float width = visibility.Intervals[i].y - visibility.Intervals[i].x;
        if (remaining < width)
        {
            outProposal.DrawnParameter = visibility.Intervals[i].x + remaining;
            break;
        }
        remaining -= width;
    }
    return true;
}

/// Fraction of the proposal's chord that the receiver sees, L_visible / c; zero for a non-mesh light.
float GetVisibleChordFraction(LineProposal proposal)
{
    return proposal.IsMeshChord == true && proposal.Chord.LengthWorld > 0
               ? proposal.VisibleLengthWorld / proposal.Chord.LengthWorld
               : 0;
}

/// Estimate direct lighting from a proposal: for a mesh chord, evaluate the point drawn over its
/// visible intervals and weight it by the joint density
/// P(light) * c / (A_light * L_visible), converted to solid angle; for a non-mesh light, trace the
/// point proposal's shadow ray. Consumes at most one visibility query: the non-mesh shadow ray, or
/// the confirmation of a point drawn past alpha-tested triangles (HasMaskedOccluders).
float3 EstimateLineProposal(SceneContext scene,
                            RayLaunch launch,
                            SurfaceHit receiver,
                            MaterialSample material,
                            float3 shadingNormalWorld,
                            float3 viewDirectionWorld,
                            LineProposal proposal,
                            inout RayQueryContext queries)
{
    if (proposal.HasProposal == false)
        return float3(0);
    LightSample light = proposal.PointSample;
    float3 bsdfValue;
    float bsdfDensity;
    if (proposal.IsMeshChord == false)
    {
        float lightDensity = light.SelectionPmf * light.ConditionalPdf;
        float cosSurface = dot(shadingNormalWorld, light.DirectionWorld);
        if (lightDensity <= 0 || any(light.IncidentCoefficient > 0) == false || cosSurface <= 0)
            return float3(0);
        EvaluateStandardBsdf(material, viewDirectionWorld, light.DirectionWorld, bsdfValue, bsdfDensity);
        if (any(bsdfValue > 0) == false || TestVisibility(scene, queries, receiver, light) != SCENE_QUERY_MISS)
            return float3(0);
        return bsdfValue * light.IncidentCoefficient * (cosSurface / lightDensity);
    }
    if (proposal.VisibleLengthWorld <= 0)
        return float3(0);
    SurfaceHit endpoint = GetSurfaceHit(scene,
                                        proposal.Chord.Segment.TriangleIndex,
                                        GetChordBarycentric(proposal.Chord, proposal.DrawnParameter));
    if (endpoint.IsValid == false)
    {
        queries.HasFailed = true;
        return float3(0);
    }
    float3 offsetWorld = endpoint.PositionWorld - receiver.PositionWorld;
    float distanceWorld = length(offsetWorld);
    if (distanceWorld <= 0)
        return float3(0);
    float3 directionWorld = offsetWorld / distanceWorld;
    float cosSurface = dot(shadingNormalWorld, directionWorld);
    float cosLight = abs(dot(endpoint.GeometricNormalWorld, directionWorld));
    float3 emission = EvaluateEmission(scene, endpoint, -directionWorld);
    if (cosSurface <= 0 || cosLight <= 0 || any(emission > 0) == false)
        return float3(0);
    EvaluateStandardBsdf(material, viewDirectionWorld, directionWorld, bsdfValue, bsdfDensity);
    if (any(bsdfValue > 0) == false)
        return float3(0);
    if (proposal.HasMaskedOccluders == true)
    {
        LightSample confirmation = (LightSample)0;
        confirmation.TriangleIndex = endpoint.TriangleIndex;
        confirmation.Barycentric = endpoint.Barycentric;
        confirmation.PositionWorld = endpoint.PositionWorld;
        confirmation.DirectionWorld = directionWorld;
        confirmation.DistanceWorld = distanceWorld;
        confirmation.HasFiniteEndpoint = true;
        confirmation.HasSample = true;
        if (TestVisibility(scene, queries, receiver, confirmation) != SCENE_QUERY_MISS)
            return float3(0);
    }
    // Joint area density P(light) * c / (A_light * L_visible), in solid angle d^2 / |cos_light|.
    float densitySolidAngle = light.SelectionPmf * proposal.Chord.LengthWorld /
                              (proposal.EmitterAreaWorld * proposal.VisibleLengthWorld) * distanceWorld *
                              distanceWorld / cosLight;
    if (isfinite(densitySolidAngle) == false || densitySolidAngle <= 0)
        return float3(0);
    return bsdfValue * emission * (cosSurface / densitySolidAngle);
}
