#pragma once
#include "wave_connection_layout.h"
#include "wave_scene_queries.slang"
// See wave_connection.cpp for attribution, declared Gaussian-mode coupling and the scalar paraxial departure, and
// wave_edge_diffraction.cpp for the UTD edge operator included at the end of this file.
// All FFT propagation, geometry coverage and coherent accumulation execute here on the GPU.
static const float WAVE_CONNECTION_FRAME_HELPER_COSINE = 0.9;
static const float WAVE_CONNECTION_MAX_WAVELENGTH_TO_DISTANCE = 1.0 / 32;
static const float WAVE_CONNECTION_UNIT_TOLERANCE = 0.000001;
struct WaveConnectionGrid
{
    float2 Values[LUDUS_WAVE_CONNECTION_CELLS];
};
/// Adapter uses the same fixed-mip endpoint coverage as ordinary scene visibility.
bool IsWaveTriangleCovered(SceneContext scene, uint triangleIndex, float2 barycentric);
float2 MultiplyWaveField(float2 first, float2 second)
{
    return float2(first.x * second.x - first.y * second.y, first.x * second.y + first.y * second.x);
}
float2 MakeWavePhase(float phase)
{
    return float2(cos(phase), sin(phase));
}
void TransformWaveLine(inout WaveConnectionGrid inoutGrid, uint offset, uint stride, bool isInverse)
{
    uint reversed = 0;
    for (uint index = 1; index < LUDUS_WAVE_CONNECTION_AXIS; ++index)
    {
        uint bit = LUDUS_WAVE_CONNECTION_AXIS / 2;
        while ((reversed & bit) != 0)
        {
            reversed ^= bit;
            bit >>= 1;
        }
        reversed ^= bit;
        if (index < reversed)
        {
            float2 saved = inoutGrid.Values[offset + index * stride];
            inoutGrid.Values[offset + index * stride] = inoutGrid.Values[offset + reversed * stride];
            inoutGrid.Values[offset + reversed * stride] = saved;
        }
    }
    for (uint length = 2; length <= LUDUS_WAVE_CONNECTION_AXIS; length *= 2)
    {
        float2 step = MakeWavePhase((isInverse == true ? 2 : -2) * PI / float(length));
        for (uint first = 0; first < LUDUS_WAVE_CONNECTION_AXIS; first += length)
        {
            float2 twiddle = float2(1, 0);
            for (uint index = 0; index < length / 2; ++index)
            {
                uint left = offset + (first + index) * stride;
                uint right = left + length / 2 * stride;
                float2 value = MultiplyWaveField(inoutGrid.Values[right], twiddle);
                float2 original = inoutGrid.Values[left];
                inoutGrid.Values[left] = original + value;
                inoutGrid.Values[right] = original - value;
                twiddle = MultiplyWaveField(twiddle, step);
            }
        }
    }
    if (isInverse == true)
    {
        for (uint index = 0; index < LUDUS_WAVE_CONNECTION_AXIS; ++index)
            inoutGrid.Values[offset + index * stride] /= float(LUDUS_WAVE_CONNECTION_AXIS);
    }
}
[noinline] void
PropagateWaveGrid(inout WaveConnectionGrid inoutGrid, float distanceWorld, float wavelengthWorld, float halfWidthWorld)
{
    for (uint index = 0; index < LUDUS_WAVE_CONNECTION_AXIS; ++index)
        TransformWaveLine(inoutGrid, index * LUDUS_WAVE_CONNECTION_AXIS, 1, false);
    for (uint index = 0; index < LUDUS_WAVE_CONNECTION_AXIS; ++index)
        TransformWaveLine(inoutGrid, index, LUDUS_WAVE_CONNECTION_AXIS, false);
    for (uint y = 0; y < LUDUS_WAVE_CONNECTION_AXIS; ++y)
    {
        float frequencyY = float(y <= LUDUS_WAVE_CONNECTION_AXIS / 2 ? int(y) : int(y) - LUDUS_WAVE_CONNECTION_AXIS) /
                           (2 * halfWidthWorld);
        for (uint x = 0; x < LUDUS_WAVE_CONNECTION_AXIS; ++x)
        {
            float frequencyX =
                float(x <= LUDUS_WAVE_CONNECTION_AXIS / 2 ? int(x) : int(x) - LUDUS_WAVE_CONNECTION_AXIS) /
                (2 * halfWidthWorld);
            float phase = -PI * wavelengthWorld * distanceWorld * (frequencyX * frequencyX + frequencyY * frequencyY);
            uint cell = y * LUDUS_WAVE_CONNECTION_AXIS + x;
            inoutGrid.Values[cell] = MultiplyWaveField(inoutGrid.Values[cell], MakeWavePhase(phase));
        }
    }
    for (uint index = 0; index < LUDUS_WAVE_CONNECTION_AXIS; ++index)
        TransformWaveLine(inoutGrid, index * LUDUS_WAVE_CONNECTION_AXIS, 1, true);
    for (uint index = 0; index < LUDUS_WAVE_CONNECTION_AXIS; ++index)
        TransformWaveLine(inoutGrid, index, LUDUS_WAVE_CONNECTION_AXIS, true);
}
float2 EvaluateWaveGaussian(float radiusSquaredWorld, float distanceWorld, float sigmaWorldSquared, float wavenumber)
{
    float correlation = distanceWorld / wavenumber;
    float denominator = sigmaWorldSquared * sigmaWorldSquared + correlation * correlation;
    float amplitude = exp(-radiusSquaredWorld * sigmaWorldSquared / (2 * denominator));
    float2 normalization =
        float2(sigmaWorldSquared * sigmaWorldSquared, -sigmaWorldSquared * correlation) / denominator;
    return amplitude *
           MultiplyWaveField(normalization, MakeWavePhase(radiusSquaredWorld * correlation / (2 * denominator)));
}
bool IsWavePointBlocked(SceneContext scene, WaveInteractionRegion region, float2 positionPlaneWorld)
{
    float3 axisXWorld = region.Frustum.AxisXWorld;
    float3 axisYWorld = cross(region.Frustum.DirectionWorld, axisXWorld);
    for (uint slot = 0; slot < region.TriangleCount; ++slot)
    {
        uint triangleIndex = region.Triangles[slot];
        TriangleData triangle = GetTriangle(scene, triangleIndex);
        float3 first = triangle.Position0World - region.Frustum.OriginWorld;
        float3 second = triangle.Position1World - region.Frustum.OriginWorld;
        float3 third = triangle.Position2World - region.Frustum.OriginWorld;
        float2 a = float2(dot(first, axisXWorld), dot(first, axisYWorld)) - positionPlaneWorld;
        float2 b = float2(dot(second, axisXWorld), dot(second, axisYWorld)) - positionPlaneWorld;
        float2 c = float2(dot(third, axisXWorld), dot(third, axisYWorld)) - positionPlaneWorld;
        float3 weights = float3(ComputeDifferenceOfProducts(b.x, c.y, b.y, c.x),
                                ComputeDifferenceOfProducts(c.x, a.y, c.y, a.x),
                                ComputeDifferenceOfProducts(a.x, b.y, a.y, b.x));
        float sum = weights.x + weights.y + weights.z;
        bool hasNegative = any(weights < 0) == true;
        bool hasPositive = any(weights > 0) == true;
        if (sum == 0 || (hasNegative == true && hasPositive == true))
            continue;
        if (IsWaveTriangleCovered(scene, triangleIndex, weights.yz / sum) == true)
            return true;
    }
    return false;
}
/// No excluded triangle: the ballistic segment tests every triangle.
static const uint WAVE_CONNECTION_NO_EXCLUDED_TRIANGLE = 0xffffffff;
/// Trace the zero-radius centre ray of rayCone, ignoring the one or two triangles in `excluded`.
[noinline] WaveQuery
TraceWaveBallistic(SceneContext scene, EllipticalCone rayCone, uint2 excluded, out bool outIsBlocked)
{
    outIsBlocked = false;
    rayCone.RadiusWorld = 0;
    rayCone.Slope = 0;
    uint nodeCount = GetGeometryHierarchyNodeCount(scene);
    uint stack[LUDUS_REFERENCE_STACK_SIZE];
    uint stackCount = nodeCount == 0 ? 0 : 1;
    stack[0] = 0;
    while (stackCount > 0)
    {
        uint nodeIndex = stack[--stackCount];
        GeometryHierarchyNode node = GetGeometryHierarchyNode(scene, nodeIndex);
        if (nodeIndex >= nodeCount || node.IsValid == false)
            return WaveQuery::Invalid;
        bool canIntersect;
        if (TryTestEllipticalConeBounds(rayCone, node.MinimumWorld, node.MaximumWorld, canIntersect) == false)
            return WaveQuery::Invalid;
        if (canIntersect == false)
            continue;
        if (node.TriangleCount == 0)
        {
            if (stackCount > LUDUS_REFERENCE_STACK_SIZE - 2)
                return WaveQuery::Invalid;
            stack[stackCount++] = node.Right;
            stack[stackCount++] = node.Left;
            continue;
        }
        for (uint slot = 0; slot < node.TriangleCount; ++slot)
        {
            uint triangleIndex = GetGeometryHierarchyTriangle(scene, node.FirstTriangle + slot);
            TriangleData triangle = GetTriangle(scene, triangleIndex);
            if (triangle.IsValid == false)
                return WaveQuery::Invalid;
            if (triangleIndex == excluded.x || triangleIndex == excluded.y)
                continue;
            ConeTriangle geometry;
            geometry.FirstWorld = triangle.Position0World;
            geometry.SecondWorld = triangle.Position1World;
            geometry.ThirdWorld = triangle.Position2World;
            ConeTriangleIntersection intersection = (ConeTriangleIntersection)0;
            if (TryIntersectEllipticalConeTriangle(rayCone, geometry, intersection) == false)
                return WaveQuery::Invalid;
            if (intersection.IsHit == true &&
                IsWaveTriangleCovered(scene, triangleIndex, intersection.NearBarycentric.yz) == true)
            {
                outIsBlocked = true;
                return WaveQuery::Ready;
            }
        }
    }
    return WaveQuery::Ready;
}
/// shouldTraceBallistic also tests the centre ray across the skipped span and stops when it is blocked.
[noinline] WaveQuery EscapeWaveOrigin(SceneContext scene,
                                      EllipticalCone cone,
                                      float wavelengthWorld,
                                      bool shouldTraceBallistic,
                                      out float outDistanceWorld,
                                      out bool outIsBlocked,
                                      out bool outIsEntirelyBallistic)
{
    float distanceWorld = cone.NearWorld;
    float previousWorld = distanceWorld;
    float maximumBallisticWorld =
        max(cone.NearWorld, min(cone.FarWorld, LUDUS_WAVE_CONNECTION_MAX_BALLISTIC_WAVELENGTHS * wavelengthWorld));
    outDistanceWorld = 0;
    outIsBlocked = false;
    outIsEntirelyBallistic = false;
    for (uint attempt = 0; attempt < LUDUS_WAVE_CONNECTION_ESCAPE_ATTEMPTS; ++attempt)
    {
        EllipticalCone segment = cone;
        segment.NearWorld = previousWorld;
        segment.FarWorld = distanceWorld;
        if (shouldTraceBallistic == true && distanceWorld > previousWorld)
        {
            WaveQuery status =
                TraceWaveBallistic(scene, segment, uint2(WAVE_CONNECTION_NO_EXCLUDED_TRIANGLE), outIsBlocked);
            if (status != WaveQuery::Ready || outIsBlocked == true)
                return status;
        }
        segment.NearWorld = distanceWorld;
        segment.FarWorld = min(cone.FarWorld, distanceWorld + cone.NearWorld);
        bool hasHit;
        WaveQuery status = TestWaveInteraction(scene, segment, hasHit);
        if (status != WaveQuery::Ready)
            return status;
        if (hasHit == false)
        {
            outDistanceWorld = distanceWorld;
            return WaveQuery::Ready;
        }
        if (distanceWorld >= maximumBallisticWorld)
        {
            segment.NearWorld = distanceWorld;
            segment.FarWorld = cone.FarWorld;
            outIsEntirelyBallistic = true;
            if (shouldTraceBallistic == false)
                return WaveQuery::Ready;
            return TraceWaveBallistic(scene, segment, uint2(WAVE_CONNECTION_NO_EXCLUDED_TRIANGLE), outIsBlocked);
        }
        previousWorld = distanceWorld;
        distanceWorld = min(maximumBallisticWorld, distanceWorld * LUDUS_WAVE_CONNECTION_BALLISTIC_SCALE);
    }
    return WaveQuery::EscapeCapacity;
}
/// A light-to-receiver connection through zero or more specular interface events, as straight legs ordered from the
/// light; see WaveConnectionChain in wave_connection.h.
struct WaveConnectionChain
{
    float3 BeginWorld[LUDUS_WAVE_CONNECTION_LEGS];
    /// Offset end of each leg, where its far endpoint escape starts.
    float3 EndWorld[LUDUS_WAVE_CONNECTION_LEGS];
    float3 DirectionWorld[LUDUS_WAVE_CONNECTION_LEGS];
    float LengthWorld[LUDUS_WAVE_CONNECTION_LEGS];
    /// Real index of each leg's medium; paraxial propagation uses LengthWorld / RefractiveIndex.
    float RefractiveIndex[LUDUS_WAVE_CONNECTION_LEGS];
    /// Bit k: the interface event that begins leg k reflects; otherwise it transmits.
    uint ReflectedEntryMask;
    uint LegCount;
};
/// The one-leg chain of a straight connection, with its explicit axis and length.
WaveConnectionChain
MakeStraightWaveConnection(float3 sourceWorld, float3 receiverWorld, float3 directionWorld, float distanceWorld)
{
    WaveConnectionChain chain = (WaveConnectionChain)0;
    chain.BeginWorld[0] = sourceWorld;
    chain.EndWorld[0] = receiverWorld;
    chain.DirectionWorld[0] = directionWorld;
    chain.LengthWorld[0] = distanceWorld;
    chain.RefractiveIndex[0] = 1;
    chain.LegCount = 1;
    return chain;
}
/// Append the leg from beginWorld to endWorld. False, leaving the chain unchanged, when it is full, the endpoints
/// coincide or are not finite, or refractiveIndex is not finite and positive.
bool TryAppendWaveConnectionLeg(inout WaveConnectionChain inoutChain,
                                float3 beginWorld,
                                float3 endWorld,
                                float refractiveIndex,
                                bool isReflectedEntry)
{
    float3 deltaWorld = endWorld - beginWorld;
    float lengthWorld = length(deltaWorld);
    if (inoutChain.LegCount >= LUDUS_WAVE_CONNECTION_LEGS || IsFiniteBits(beginWorld) == false ||
        IsFiniteBits(endWorld) == false || IsFiniteBits(lengthWorld) == false || lengthWorld <= 0 ||
        IsFiniteBits(refractiveIndex) == false || refractiveIndex <= 0)
        return false;
    uint leg = inoutChain.LegCount;
    inoutChain.BeginWorld[leg] = beginWorld;
    inoutChain.EndWorld[leg] = endWorld;
    inoutChain.DirectionWorld[leg] = deltaWorld / lengthWorld;
    inoutChain.LengthWorld[leg] = lengthWorld;
    inoutChain.RefractiveIndex[leg] = refractiveIndex;
    if (isReflectedEntry == true)
        inoutChain.ReflectedEntryMask |= 1u << leg;
    inoutChain.LegCount = leg + 1;
    return true;
}
/// First transverse axis of a straight connection along directionWorld.
float3 MakeWaveConnectionAxis(float3 directionWorld)
{
    float3 helperWorld =
        abs(directionWorld.y) < WAVE_CONNECTION_FRAME_HELPER_COSINE ? float3(0, 1, 0) : float3(1, 0, 0);
    return normalize(cross(helperWorld, directionWorld));
}
/// Carry a leg's transverse frame across the interface event into the next leg, as TryMakeWaveConnectionFrames does:
/// a reflection mirrors it about the interface normal (the difference of the directions) and reverses its handedness,
/// a transmission rotates it with the direction. False for a reflection that does not turn the direction or a
/// transmission that reverses it.
bool TryCarryWaveConnectionFrame(float3 incidentWorld,
                                 float3 outgoingWorld,
                                 bool isReflected,
                                 inout float3 inoutAxisXWorld,
                                 inout float inoutHandedness)
{
    float3 axisWorld = inoutAxisXWorld;
    if (isReflected == true)
    {
        float3 normalWorld = incidentWorld - outgoingWorld;
        float normalLength = length(normalWorld);
        if (normalLength <= 0 || IsFiniteBits(normalLength) == false)
            return false;
        float3 unitNormalWorld = normalWorld / normalLength;
        axisWorld -= unitNormalWorld * (2 * dot(axisWorld, unitNormalWorld));
        inoutHandedness = -inoutHandedness;
    }
    else
    {
        float cosine = dot(incidentWorld, outgoingWorld);
        float3 rotationWorld = cross(incidentWorld, outgoingWorld);
        float sine = length(rotationWorld);
        if (cosine <= 0 || IsFiniteBits(cosine) == false || IsFiniteBits(sine) == false)
            return false;
        if (sine > 0)
        {
            float3 unitAxisWorld = rotationWorld / sine;
            axisWorld = axisWorld * cosine + cross(unitAxisWorld, axisWorld) * sine +
                        unitAxisWorld * (dot(unitAxisWorld, axisWorld) * (1 - cosine));
        }
    }
    axisWorld -= outgoingWorld * dot(axisWorld, outgoingWorld);
    float axisLength = length(axisWorld);
    if (axisLength <= 0 || IsFiniteBits(axisLength) == false)
        return false;
    inoutAxisXWorld = axisWorld / axisLength;
    return true;
}
/// Validate a chain and measure its reduced length (the sum of LengthWorld / RefractiveIndex), its geometric length
/// and its endpoint escape, as both relayed operators do.
WaveQuery MeasureWaveConnectionChain(WaveConnectionChain chain,
                                     float wavelengthWorld,
                                     out float outDistanceWorld,
                                     out float outEndpointEscapeWorld,
                                     out float outRoundoffWorld)
{
    outDistanceWorld = 0;
    outEndpointEscapeWorld = 0;
    outRoundoffWorld = 0;
    if (chain.LegCount == 0 || chain.LegCount > LUDUS_WAVE_CONNECTION_LEGS || IsFiniteBits(wavelengthWorld) == false ||
        wavelengthWorld < LUDUS_WAVE_CONNECTION_MIN_WAVELENGTH_WORLD)
        return WaveQuery::Invalid;
    float distanceWorld = 0;
    float geometricLengthWorld = 0;
    float coordinateMagnitudeWorld = 0;
    for (uint leg = 0; leg < chain.LegCount; ++leg)
    {
        float3 beginWorld = chain.BeginWorld[leg];
        float3 endWorld = chain.EndWorld[leg];
        float3 directionWorld = chain.DirectionWorld[leg];
        if (IsFiniteBits(beginWorld) == false || IsFiniteBits(endWorld) == false ||
            IsFiniteBits(directionWorld) == false || IsFiniteBits(chain.LengthWorld[leg]) == false ||
            chain.LengthWorld[leg] <= 0 || IsFiniteBits(chain.RefractiveIndex[leg]) == false ||
            chain.RefractiveIndex[leg] <= 0 ||
            abs(dot(directionWorld, directionWorld) - 1) > WAVE_CONNECTION_UNIT_TOLERANCE)
            return WaveQuery::Invalid;
        distanceWorld += chain.LengthWorld[leg] / chain.RefractiveIndex[leg];
        geometricLengthWorld += chain.LengthWorld[leg];
        coordinateMagnitudeWorld = max(coordinateMagnitudeWorld,
                                       max(max(abs(beginWorld.x), max(abs(beginWorld.y), abs(beginWorld.z))),
                                           max(abs(endWorld.x), max(abs(endWorld.y), abs(endWorld.z)))));
    }
    if (IsFiniteBits(distanceWorld) == false ||
        wavelengthWorld / distanceWorld > WAVE_CONNECTION_MAX_WAVELENGTH_TO_DISTANCE)
        return WaveQuery::Invalid;
    float coordinateScaleWorld = max(geometricLengthWorld, coordinateMagnitudeWorld);
    float endpointEscapeWorld = max(LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld,
                                    LUDUS_WAVE_CONNECTION_ENDPOINT_ROUNDOFF_SCALE * coordinateScaleWorld);
    if (2 * endpointEscapeWorld >= distanceWorld)
        return WaveQuery::Invalid;
    outDistanceWorld = distanceWorld;
    outEndpointEscapeWorld = endpointEscapeWorld;
    outRoundoffWorld = LUDUS_WAVE_CONNECTION_ENDPOINT_ROUNDOFF_SCALE * coordinateScaleWorld;
    return WaveQuery::Ready;
}
/// Coherent angular-spectrum connection relayed through the chain's interface events (EvaluateWaveChainConnection in
/// wave_connection.cpp): field / unoccluded field of the same chain. Failure preserves the output.
[noinline] WaveQuery EvaluateWaveChainConnection(SceneContext scene,
                                                 WaveConnectionChain chain,
                                                 float wavelengthWorld,
                                                 inout float2 inoutField)
{
    float distanceWorld;
    float endpointEscapeWorld;
    float roundoffWorld;
    WaveQuery measured =
        MeasureWaveConnectionChain(chain, wavelengthWorld, distanceWorld, endpointEscapeWorld, roundoffWorld);
    if (measured != WaveQuery::Ready)
        return measured;
    float wavenumber = 2 * PI / wavelengthWorld;
    float sigmaWorldSquared = distanceWorld / wavenumber;
    float halfWidthWorld = LUDUS_WAVE_CONNECTION_TAIL_DEVIATIONS * sqrt(2 * sigmaWorldSquared);
    float3 axisXWorld = MakeWaveConnectionAxis(chain.DirectionWorld[0]);
    float handedness = 1;
    WaveConnectionGrid grid;
    bool hasField = false;
    float fieldDistanceWorld = 0;
    float legStartWorld = 0;
    uint interaction = 0;
    for (uint leg = 0; leg < chain.LegCount; ++leg)
    {
        if (leg > 0 && TryCarryWaveConnectionFrame(chain.DirectionWorld[leg - 1],
                                                   chain.DirectionWorld[leg],
                                                   (chain.ReflectedEntryMask & (1u << leg)) != 0,
                                                   axisXWorld,
                                                   handedness) == false)
            return WaveQuery::Invalid;
        float refractiveIndex = chain.RefractiveIndex[leg];
        float legLengthWorld = chain.LengthWorld[leg];
        float legStartReducedWorld = legStartWorld;
        legStartWorld += legLengthWorld / refractiveIndex;
        // Inside both endpoint escapes: no interaction region is resolvable on this leg.
        if (2 * endpointEscapeWorld >= legLengthWorld)
            continue;
        EllipticalCone cone = (EllipticalCone)0;
        cone.OriginWorld = chain.BeginWorld[leg];
        cone.DirectionWorld = chain.DirectionWorld[leg];
        cone.AxisXWorld = axisXWorld;
        cone.RadiusWorld = halfWidthWorld;
        cone.MinorToMajor = 1;
        cone.NearWorld = endpointEscapeWorld;
        cone.FarWorld = legLengthWorld - cone.NearWorld;
        float sourceEscapeWorld;
        bool isBlocked;
        bool isEntirelyBallistic;
        WaveQuery escapeStatus =
            EscapeWaveOrigin(scene, cone, wavelengthWorld, true, sourceEscapeWorld, isBlocked, isEntirelyBallistic);
        if (escapeStatus != WaveQuery::Ready)
            return escapeStatus;
        if (isBlocked == true)
        {
            inoutField = float2(0);
            return WaveQuery::Ready;
        }
        if (isEntirelyBallistic == true)
            continue;
        EllipticalCone reverse = cone;
        reverse.OriginWorld = chain.EndWorld[leg];
        reverse.DirectionWorld = -chain.DirectionWorld[leg];
        float receiverEscapeWorld;
        escapeStatus = EscapeWaveOrigin(scene,
                                        reverse,
                                        wavelengthWorld,
                                        true,
                                        receiverEscapeWorld,
                                        isBlocked,
                                        isEntirelyBallistic);
        if (escapeStatus != WaveQuery::Ready)
            return escapeStatus;
        if (isBlocked == true)
        {
            inoutField = float2(0);
            return WaveQuery::Ready;
        }
        if (isEntirelyBallistic == true)
            continue;
        cone.NearWorld = sourceEscapeWorld;
        cone.FarWorld = max(cone.NearWorld, legLengthWorld - receiverEscapeWorld);
        for (; interaction < LUDUS_WAVE_CONNECTION_REGIONS; ++interaction)
        {
            WaveInteractionRegion region = (WaveInteractionRegion)0;
            WaveQuery status =
                cone.NearWorld < cone.FarWorld ? FindWaveInteractionRegion(scene, cone, region) : WaveQuery::Ready;
            if (status != WaveQuery::Ready)
                return status;
            if (region.TriangleCount == 0)
                break;
            float screenDistanceWorld = region.Frustum.NearWorld * 0.5 + region.Frustum.FarWorld * 0.5;
            if (chain.LegCount > 1)
            {
                // Project onto the midpoint of the region's slab in reduced depth, as the unfolded connection would.
                float nearReducedWorld = legStartReducedWorld + region.Frustum.NearWorld / refractiveIndex;
                float farReducedWorld =
                    max(nearReducedWorld,
                        min(distanceWorld - endpointEscapeWorld, nearReducedWorld + 2 * halfWidthWorld));
                screenDistanceWorld = nearReducedWorld * 0.5 + farReducedWorld * 0.5;
            }
            float transmission[LUDUS_WAVE_CONNECTION_CELLS];
            uint blockedCount = 0;
            for (uint y = 0; y < LUDUS_WAVE_CONNECTION_AXIS; ++y)
            {
                for (uint x = 0; x < LUDUS_WAVE_CONNECTION_AXIS; ++x)
                {
                    float positionXWorld = (2 * float(x) / LUDUS_WAVE_CONNECTION_AXIS - 1) * halfWidthWorld;
                    float positionYWorld = (2 * float(y) / LUDUS_WAVE_CONNECTION_AXIS - 1) * halfWidthWorld;
                    uint cell = y * LUDUS_WAVE_CONNECTION_AXIS + x;
                    uint coveredSamples = 0;
                    float cellWidthWorld = 2 * halfWidthWorld / LUDUS_WAVE_CONNECTION_AXIS;
                    for (uint sampleY = 0; sampleY < LUDUS_WAVE_CONNECTION_MASK_AXIS; ++sampleY)
                    {
                        for (uint sampleX = 0; sampleX < LUDUS_WAVE_CONNECTION_MASK_AXIS; ++sampleX)
                        {
                            float offsetXWorld =
                                ((float(sampleX) + 0.5) / LUDUS_WAVE_CONNECTION_MASK_AXIS - 0.5) * cellWidthWorld;
                            float offsetYWorld =
                                ((float(sampleY) + 0.5) / LUDUS_WAVE_CONNECTION_MASK_AXIS - 0.5) * cellWidthWorld;
                            // A mirrored leg's frame has the opposite handedness: its second coordinate changes sign.
                            coveredSamples +=
                                IsWavePointBlocked(scene,
                                                   region,
                                                   float2(positionXWorld + offsetXWorld,
                                                          (positionYWorld + offsetYWorld) * handedness)) == true
                                    ? 1
                                    : 0;
                        }
                    }
                    transmission[cell] = 1 - float(coveredSamples) / LUDUS_WAVE_CONNECTION_MASK_SAMPLES;
                    blockedCount += coveredSamples;
                    if (hasField == false)
                        grid.Values[cell] =
                            EvaluateWaveGaussian(positionXWorld * positionXWorld + positionYWorld * positionYWorld,
                                                 screenDistanceWorld,
                                                 sigmaWorldSquared,
                                                 wavenumber);
                }
            }
            if (blockedCount == LUDUS_WAVE_CONNECTION_CELLS * LUDUS_WAVE_CONNECTION_MASK_SAMPLES)
            {
                inoutField = float2(0);
                return WaveQuery::Ready;
            }
            if (blockedCount > 0)
            {
                if (hasField == true)
                    PropagateWaveGrid(grid, screenDistanceWorld - fieldDistanceWorld, wavelengthWorld, halfWidthWorld);
                for (uint cell = 0; cell < LUDUS_WAVE_CONNECTION_CELLS; ++cell)
                {
                    grid.Values[cell] *= transmission[cell];
                }
                hasField = true;
                fieldDistanceWorld = screenDistanceWorld;
            }
            float nextNearWorld = asfloat(asuint(region.Frustum.FarWorld) + 1);
            cone.NearWorld = min(cone.FarWorld, nextNearWorld);
            if (cone.FarWorld - cone.NearWorld > LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld)
            {
                EllipticalCone escape = cone;
                escape.OriginWorld = chain.BeginWorld[leg] + chain.DirectionWorld[leg] * cone.NearWorld;
                escape.FarWorld = cone.FarWorld - cone.NearWorld;
                escape.NearWorld = min(escape.FarWorld, endpointEscapeWorld);
                float escapedWorld;
                WaveQuery escaped = EscapeWaveOrigin(scene,
                                                     escape,
                                                     wavelengthWorld,
                                                     true,
                                                     escapedWorld,
                                                     isBlocked,
                                                     isEntirelyBallistic);
                if (escaped != WaveQuery::Ready)
                    return escaped;
                if (isBlocked == true)
                {
                    inoutField = float2(0);
                    return WaveQuery::Ready;
                }
                cone.NearWorld =
                    isEntirelyBallistic == true ? cone.FarWorld : min(cone.FarWorld, cone.NearWorld + escapedWorld);
            }
        }
        if (interaction == LUDUS_WAVE_CONNECTION_REGIONS)
            return WaveQuery::PropagationCapacity;
        // The empty region that ended this leg used one step, as it does for a one-leg connection.
        ++interaction;
    }
    if (hasField == false)
    {
        inoutField = float2(1, 0);
        return WaveQuery::Ready;
    }
    PropagateWaveGrid(grid, distanceWorld - fieldDistanceWorld, wavelengthWorld, halfWidthWorld);
    float2 field =
        grid.Values[(LUDUS_WAVE_CONNECTION_AXIS / 2) * LUDUS_WAVE_CONNECTION_AXIS + LUDUS_WAVE_CONNECTION_AXIS / 2];
    float2 normalization = EvaluateWaveGaussian(0, distanceWorld, sigmaWorldSquared, wavenumber);
    float2 result =
        MultiplyWaveField(field, float2(normalization.x, -normalization.y)) / dot(normalization, normalization);
    if (IsFiniteBits(result.x) == false || IsFiniteBits(result.y) == false)
        return WaveQuery::Invalid;
    inoutField = result;
    return WaveQuery::Ready;
}
/// Field / unoccluded Gaussian field. Failure preserves the output and reports invalid input/capacity.
WaveQuery EvaluateWaveConnectionToReceiver(SceneContext scene,
                                           float3 sourceWorld,
                                           float3 receiverWorld,
                                           float3 directionWorld,
                                           float distanceWorld,
                                           float wavelengthWorld,
                                           inout float2 inoutField)
{
    return EvaluateWaveChainConnection(
        scene,
        MakeStraightWaveConnection(sourceWorld, receiverWorld, directionWorld, distanceWorld),
        wavelengthWorld,
        inoutField);
}

WaveQuery EvaluateWaveConnection(SceneContext scene,
                                 float3 sourceWorld,
                                 float3 directionWorld,
                                 float distanceWorld,
                                 float wavelengthWorld,
                                 inout float2 inoutField)
{
    return EvaluateWaveConnectionToReceiver(scene,
                                            sourceWorld,
                                            sourceWorld + directionWorld * distanceWorld,
                                            directionWorld,
                                            distanceWorld,
                                            wavelengthWorld,
                                            inoutField);
}
#include "wave_edge_diffraction.slang"
