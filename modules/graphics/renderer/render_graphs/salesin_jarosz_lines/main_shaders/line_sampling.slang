#pragma once
// Line strategies on a triangle emitter (point-line design D6) in two directions, and the density of
// either at a given point (design D7). U runs along the emitter triangle's first edge and V across
// it, in the triangle's plane. This extends the line-sampling template's single U strategy (PL1) and
// is copied here because a template directory imports only its own include closure.
// Thanks to Niels Billen and Philip Dutre, "Line Sampling for Direct Illumination", Computer Graphics
// Forum 35(4) (EGSR 2016), pp. 93-102, https://doi.org/10/f84z2h : their surface-area line estimator
// places a line with density proportional to its chord length and integrates only the chord's
// visible parts, which FindVisibleLineIntervals finds from their shadow triangle (section 4).
// Thanks to Katherine Salesin and Wojciech Jarosz, "Combining Point and Line Samples for Direct
// Illumination", Computer Graphics Forum 38(4) (EGSR 2019), pp. 159-169,
// https://doi.org/10.1111/cgf.13779 , section 4 and equations 14-15: a line sample is a point
// sample with the joint density p(u, v) = p(v) p(u | v), with p(v) proportional to the chord
// length and p(u | v) uniform over the visible length. Section 4.2: evaluating a line strategy's
// density at a point drawn by another strategy takes a line query through that point.
// Departures (design section 2): lines are chords of one emitter triangle, not of a quad light; the
// chord's offset comes from an area-uniform SampleIncidentLight point, whose density is already
// c / A_T, so no extra inversion is needed; the point along the chord is drawn and evaluated instead
// of integrating the line analytically; and a chord that crosses alpha-tested triangles is confirmed
// with one TestVisibility ray (design D4). Directional, point, spot and environment lights keep the
// ordinary point proposal, in both directions.

// Out-of-line line queries. Thanks to the Slang authors, core module noinline attribute reference:
// https://docs.shader-slang.org/en/latest/external/core-module-reference/attributes/noinline.html
// ProposeLine and EvaluateLineDensity each run a line query. Inlined at every call path of the three
// shade passes, the ten copies made the merged Metal stage exceed the 2 MiB generated-artifact limit.
// Their bodies are [noinline] functions that take the query context by value and return it through an
// out parameter: Slang 2026.1.2 inlines a [noinline] function with an inout parameter regardless. The
// inout wrappers copy the whole context back, so budgets, failure and the automatic random stream
// advance exactly as if the body ran inline. SPIRV-Cross still marks the MSL functions always_inline,
// so Metal compiles the same kernel, and each body still reads its LineVisibility only where it was
// queried.

/// Receivers whose direction to the emitter plane has a cosine at most this (unitless) receive no
/// light from that triangle: the integrand carries the cosine, so it is negligible, and a chord seen
/// edge-on from its own plane would make the line query fail as degenerate.
static const float LINE_EMITTER_PLANE_COSINE_TOLERANCE = 0.000001;
/// Bound on chord end parameters before clipping to the triangle; any finite triangle lies inside it.
static const float LINE_CHORD_UNBOUNDED_PARAMETER = 1.0e30;
/// The line direction along the emitter triangle's first edge, P1 - P0.
static const uint LINE_DIRECTION_U = 0;
/// The line direction across the first edge, cross(n, P1 - P0) with n the geometric normal.
static const uint LINE_DIRECTION_V = 1;

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
/// The intervals themselves are read only inside FindLineProposal, right after the query: copying a
/// LineVisibility between functions lost its intervals on Metal in merged kernels (PL1).
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

/// A line strategy's drawn sample, evaluated at its point or direction: the unweighted integrand
/// BSDF * incident radiance * cos, the strategy's own density there (solid angle at the receiver, or
/// the selection probability of a delta light), and BSDF sampling's density for the same direction.
struct LineSampleValue
{
    float3 IntegrandValue;
    float3 DirectionWorld;
    /// The drawn point on TriangleIndex, for a mesh light.
    float2 Barycentric;
    /// The emitter triangle of a mesh light's point, or SCENE_INVALID_INDEX for a non-mesh light.
    uint TriangleIndex;
    float Density;
    /// Zero for a delta light, which BSDF sampling never proposes.
    float BsdfDensity;
};

/// Chord of the triangle through barycentric (b1, b2), along alongWorld in the triangle's plane.
/// Returns false for a degenerate triangle or a chord of zero length.
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
    if (maximum <= minimum || all(isfinite(float2(minimum, maximum))) == false)
        return false;
    outChord.StartBarycentric = barycentric + float2(deltaB1, deltaB2) * minimum;
    outChord.EndBarycentric = barycentric + float2(deltaB1, deltaB2) * maximum;
    float3 pointWorld = triangle.Position0World + edge1World * barycentric.x + edge2World * barycentric.y;
    outChord.Segment.StartWorld = pointWorld + alongWorld * minimum;
    outChord.Segment.EndWorld = pointWorld + alongWorld * maximum;
    outChord.Segment.TriangleIndex = triangle.TriangleIndex;
    outChord.LengthWorld = length(outChord.Segment.EndWorld - outChord.Segment.StartWorld);
    return outChord.LengthWorld > 0;
}

/// Barycentrics of chord parameter t in [0, 1], clamped into the triangle against rounding.
float2 GetChordBarycentric(EmitterChord chord, float t)
{
    float2 barycentric = max(lerp(chord.StartBarycentric, chord.EndBarycentric, t), float2(0));
    float sum = barycentric.x + barycentric.y;
    return sum > 1 ? barycentric / sum : barycentric;
}

/// A vector along line direction U or V in the triangle's plane. Its length does not matter: the
/// chord through a point is the same for any positive multiple.
float3 GetLineAlongWorld(TriangleData triangle, uint direction)
{
    float3 alongUWorld = triangle.Position1World - triangle.Position0World;
    return direction == LINE_DIRECTION_U ? alongUWorld : cross(triangle.GeometricNormalWorld, alongUWorld);
}

/// Probability with which SelectLight chose one light. Selection is uniform, as in
/// EvaluateLightSelectionPmf and EvaluateLightPdfSolidAngle.
float GetUniformLightSelectionPmf(SceneContext scene)
{
    uint count = GetLightCount(scene);
    return count > 0 ? 1.0 / float(count) : 0;
}

/// Joint density P(light) * c / (A_light * L_visible) of a mesh line sample (design D6 step 6), in
/// solid angle at the receiver: times distance^2 / |cos_light|, as EvaluateLightPdfSolidAngle
/// converts area density. Zero for any nonpositive or nonfinite input or result.
float ComputeLineDensitySolidAngle(float selectionPmf,
                                   float chordLengthWorld,
                                   float emitterAreaWorld,
                                   float visibleLengthWorld,
                                   float distanceWorld,
                                   float cosLight)
{
    if (selectionPmf <= 0 || chordLengthWorld <= 0 || emitterAreaWorld <= 0 || visibleLengthWorld <= 0 ||
        cosLight <= 0)
        return 0;
    float densitySolidAngle = selectionPmf * chordLengthWorld / (emitterAreaWorld * visibleLengthWorld) *
                              distanceWorld * distanceWorld / cosLight;
    return isfinite(densitySolidAngle) == true && densitySolidAngle > 0 ? densitySolidAngle : 0;
}

/// Select a light, propose a point on it and, for a mesh light, build its chord in direction
/// LINE_DIRECTION_U or LINE_DIRECTION_V, find the chord's visible intervals and draw a chord parameter
/// uniformly by length over them. Reads dimensions firstDimension to firstDimension + 3, and
/// positionDimension, at explicit addresses. Consumes one line query for a mesh light unless the
/// receiver lies in the emitter's plane or sees the whole chord below its horizon (both contribute
/// zero). A failed line query latches queries.HasFailed. Returns false when there is nothing to estimate.
bool FindLineProposal(SceneContext scene,
                      RayLaunch launch,
                      SurfaceHit receiver,
                      float3 shadingNormalWorld,
                      bool isHorizonTested,
                      uint direction,
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
                            GetLineAlongWorld(triangle, direction),
                            outProposal.Chord) == false)
    {
        queries.HasFailed = true;
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

/// FindLineProposal out of line, with the query context copied in and out (see the note above).
[noinline] bool ProposeLineOutOfLine(SceneContext scene,
                                     RayLaunch launch,
                                     SurfaceHit receiver,
                                     float3 shadingNormalWorld,
                                     bool isHorizonTested,
                                     uint direction,
                                     uint firstDimension,
                                     uint positionDimension,
                                     RayQueryContext initialQueries,
                                     out RayQueryContext outQueries,
                                     out LineProposal outProposal) {
    RayQueryContext queries = initialQueries;
    bool hasProposal = FindLineProposal(scene,
                                        launch,
                                        receiver,
                                        shadingNormalWorld,
                                        isHorizonTested,
                                        direction,
                                        firstDimension,
                                        positionDimension,
                                        queries,
                                        outProposal);
    outQueries = queries;
    return hasProposal;
}

/// FindLineProposal through its single out-of-line copy; same contract.
bool ProposeLine(SceneContext scene,
                 RayLaunch launch,
                 SurfaceHit receiver,
                 float3 shadingNormalWorld,
                 bool isHorizonTested,
                 uint direction,
                 uint firstDimension,
                 uint positionDimension,
                 inout RayQueryContext queries,
                 out LineProposal outProposal)
{
    RayQueryContext updatedQueries;
    bool hasProposal = ProposeLineOutOfLine(scene,
                                            launch,
                                            receiver,
                                            shadingNormalWorld,
                                            isHorizonTested,
                                            direction,
                                            firstDimension,
                                            positionDimension,
                                            queries,
                                            updatedQueries,
                                            outProposal);
    queries = updatedQueries;
    return hasProposal;
}

/// Evaluate a proposal at its drawn point: for a mesh chord, the point drawn over its visible
/// intervals with the joint line density; for a non-mesh light, the point proposal and its density
/// after its shadow ray. Consumes at most one visibility query: the non-mesh shadow ray, or the
/// confirmation of a point drawn past alpha-tested triangles (HasMaskedOccluders). Returns false,
/// with a zero outValue, when the sample contributes nothing.
bool EvaluateLineProposal(SceneContext scene,
                          SurfaceHit receiver,
                          MaterialSample material,
                          float3 shadingNormalWorld,
                          float3 viewDirectionWorld,
                          LineProposal proposal,
                          inout RayQueryContext queries,
                          out LineSampleValue outValue)
{
    outValue = (LineSampleValue)0;
    outValue.TriangleIndex = SCENE_INVALID_INDEX;
    if (proposal.HasProposal == false)
        return false;
    LightSample light = proposal.PointSample;
    float3 bsdfValue;
    float bsdfDensity;
    if (proposal.IsMeshChord == false)
    {
        float lightDensity = light.SelectionPmf * light.ConditionalPdf;
        float cosSurface = dot(shadingNormalWorld, light.DirectionWorld);
        if (lightDensity <= 0 || any(light.IncidentCoefficient > 0) == false || cosSurface <= 0)
            return false;
        EvaluateStandardBsdf(material, viewDirectionWorld, light.DirectionWorld, bsdfValue, bsdfDensity);
        if (any(bsdfValue > 0) == false || TestVisibility(scene, queries, receiver, light) != SCENE_QUERY_MISS)
            return false;
        outValue.IntegrandValue = bsdfValue * light.IncidentCoefficient * cosSurface;
        outValue.DirectionWorld = light.DirectionWorld;
        outValue.Density = lightDensity;
        outValue.BsdfDensity = light.IsDiscrete == true ? 0 : bsdfDensity;
        return true;
    }
    if (proposal.VisibleLengthWorld <= 0)
        return false;
    float2 barycentric = GetChordBarycentric(proposal.Chord, proposal.DrawnParameter);
    SurfaceHit endpoint = GetSurfaceHit(scene, proposal.Chord.Segment.TriangleIndex, barycentric);
    if (endpoint.IsValid == false)
    {
        queries.HasFailed = true;
        return false;
    }
    float3 offsetWorld = endpoint.PositionWorld - receiver.PositionWorld;
    float distanceWorld = length(offsetWorld);
    if (distanceWorld <= 0)
        return false;
    float3 directionWorld = offsetWorld / distanceWorld;
    float cosSurface = dot(shadingNormalWorld, directionWorld);
    float cosLight = abs(dot(endpoint.GeometricNormalWorld, directionWorld));
    float3 emission = EvaluateEmission(scene, endpoint, -directionWorld);
    if (cosSurface <= 0 || cosLight <= 0 || any(emission > 0) == false)
        return false;
    EvaluateStandardBsdf(material, viewDirectionWorld, directionWorld, bsdfValue, bsdfDensity);
    if (any(bsdfValue > 0) == false)
        return false;
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
            return false;
    }
    outValue.Density = ComputeLineDensitySolidAngle(light.SelectionPmf,
                                                    proposal.Chord.LengthWorld,
                                                    proposal.EmitterAreaWorld,
                                                    proposal.VisibleLengthWorld,
                                                    distanceWorld,
                                                    cosLight);
    if (outValue.Density <= 0)
        return false;
    outValue.IntegrandValue = bsdfValue * emission * cosSurface;
    outValue.DirectionWorld = directionWorld;
    outValue.Barycentric = barycentric;
    outValue.TriangleIndex = endpoint.TriangleIndex;
    outValue.BsdfDensity = bsdfDensity;
    return true;
}

/// Density in solid angle at the receiver with which the line strategy in direction draws the point
/// at barycentric on triangle triangleIndex (design D7): build the chord of the triangle through the
/// point in that direction, find its visible intervals and, when the point's chord parameter lies in
/// one of them, return the joint density of ComputeLineDensitySolidAngle. Returns zero without a query
/// for a triangle that is not a mesh emitter, and for a receiver in the emitter's plane, where
/// ProposeLine proposes nothing. Otherwise consumes one line query; a failed query latches
/// queries.HasFailed and returns zero. The intervals are read here, where they are queried (PL1).
float FindLineDensity(SceneContext scene,
                      SurfaceHit receiver,
                      uint triangleIndex,
                      float2 barycentric,
                      uint direction,
                      inout RayQueryContext queries)
{
    TriangleData triangle = GetTriangle(scene, triangleIndex);
    SurfaceHit emitter = GetSurfaceHit(scene, triangleIndex, barycentric);
    if (triangle.IsValid == false || emitter.IsValid == false || emitter.EmitterIndex >= GetEmitterCount(scene))
        return 0;
    float3 towardReceiverWorld = receiver.PositionWorld - emitter.PositionWorld;
    float distanceWorld = length(towardReceiverWorld);
    float projectedDistanceWorld = abs(dot(triangle.GeometricNormalWorld, towardReceiverWorld));
    if (distanceWorld <= 0 || projectedDistanceWorld <= LINE_EMITTER_PLANE_COSINE_TOLERANCE * distanceWorld)
        return 0;
    EmitterChord chord;
    if (TryMakeEmitterChord(triangle, barycentric, GetLineAlongWorld(triangle, direction), chord) == false)
    {
        queries.HasFailed = true;
        return 0;
    }
    float3 chordWorld = chord.Segment.EndWorld - chord.Segment.StartWorld;
    float parameter = dot(emitter.PositionWorld - chord.Segment.StartWorld, chordWorld) / dot(chordWorld, chordWorld);
    LineVisibility visibility;
    if (FindVisibleLineIntervals(scene, queries, receiver, chord.Segment, visibility) == SCENE_QUERY_FAILURE)
        return 0;
    bool isVisible = false;
    for (uint i = 0; i < visibility.IntervalCount; ++i)
    {
        if (parameter >= visibility.Intervals[i].x && parameter <= visibility.Intervals[i].y)
            isVisible = true;
    }
    if (isVisible == false)
        return 0;
    return ComputeLineDensitySolidAngle(GetUniformLightSelectionPmf(scene),
                                        chord.LengthWorld,
                                        GetEmitter(scene, emitter.EmitterIndex).z,
                                        visibility.VisibleLengthWorld,
                                        distanceWorld,
                                        projectedDistanceWorld / distanceWorld);
}

/// FindLineDensity out of line, with the query context copied in and out (see the note above).
[noinline] float EvaluateLineDensityOutOfLine(SceneContext scene,
                                              SurfaceHit receiver,
                                              uint triangleIndex,
                                              float2 barycentric,
                                              uint direction,
                                              RayQueryContext initialQueries,
                                              out RayQueryContext outQueries) {
    RayQueryContext queries = initialQueries;
    float density = FindLineDensity(scene, receiver, triangleIndex, barycentric, direction, queries);
    outQueries = queries;
    return density;
}

/// FindLineDensity through its single out-of-line copy; same contract.
float EvaluateLineDensity(SceneContext scene,
                          SurfaceHit receiver,
                          uint triangleIndex,
                          float2 barycentric,
                          uint direction,
                          inout RayQueryContext queries)
{
    RayQueryContext updatedQueries;
    float density =
        EvaluateLineDensityOutOfLine(scene, receiver, triangleIndex, barycentric, direction, queries, updatedQueries);
    queries = updatedQueries;
    return density;
}
