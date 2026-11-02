#pragma once
#include "wave_connection_layout.h"
#include "wave_scene_queries.slang"
// See wave_connection.cpp for attribution, declared Gaussian-mode coupling and the scalar paraxial departure.
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
[noinline] void PropagateWaveGrid(inout WaveConnectionGrid inoutGrid,
                                  float distanceWorld,
                                  float wavelengthWorld,
                                  float halfWidthWorld) {
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
} float2 EvaluateWaveGaussian(float radiusSquaredWorld, float distanceWorld, float sigmaWorldSquared, float wavenumber)
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
[noinline] uint TraceWaveBallistic(SceneContext scene, EllipticalCone rayCone, out bool outIsBlocked) {
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
            return WAVE_QUERY_INVALID;
        bool canIntersect;
        if (TryTestEllipticalConeBounds(rayCone, node.MinimumWorld, node.MaximumWorld, canIntersect) == false)
            return WAVE_QUERY_INVALID;
        if (canIntersect == false)
            continue;
        if (node.TriangleCount == 0)
        {
            if (stackCount > LUDUS_REFERENCE_STACK_SIZE - 2)
                return WAVE_QUERY_INVALID;
            stack[stackCount++] = node.Right;
            stack[stackCount++] = node.Left;
            continue;
        }
        for (uint slot = 0; slot < node.TriangleCount; ++slot)
        {
            uint triangleIndex = GetGeometryHierarchyTriangle(scene, node.FirstTriangle + slot);
            TriangleData triangle = GetTriangle(scene, triangleIndex);
            if (triangle.IsValid == false)
                return WAVE_QUERY_INVALID;
            ConeTriangle geometry;
            geometry.FirstWorld = triangle.Position0World;
            geometry.SecondWorld = triangle.Position1World;
            geometry.ThirdWorld = triangle.Position2World;
            ConeTriangleIntersection intersection = (ConeTriangleIntersection)0;
            if (TryIntersectEllipticalConeTriangle(rayCone, geometry, intersection) == false)
                return WAVE_QUERY_INVALID;
            if (intersection.IsHit == true &&
                IsWaveTriangleCovered(scene, triangleIndex, intersection.NearBarycentric.yz) == true)
            {
                outIsBlocked = true;
                return WAVE_QUERY_READY;
            }
        }
    }
    return WAVE_QUERY_READY;
}[noinline] uint EscapeWaveOrigin(SceneContext scene,
                                  EllipticalCone cone,
                                  float wavelengthWorld,
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
        if (distanceWorld > previousWorld)
        {
            uint status = TraceWaveBallistic(scene, segment, outIsBlocked);
            if (status != WAVE_QUERY_READY || outIsBlocked == true)
                return status;
        }
        segment.NearWorld = distanceWorld;
        segment.FarWorld = min(cone.FarWorld, distanceWorld + cone.NearWorld);
        WaveInteractionRegion region = (WaveInteractionRegion)0;
        uint status = FindWaveInteractionRegion(scene, segment, region);
        if (status == WAVE_QUERY_READY && region.TriangleCount == 0)
        {
            outDistanceWorld = distanceWorld;
            return status;
        }
        if (status != WAVE_QUERY_READY && status != WAVE_QUERY_CAPACITY)
            return status;
        if (distanceWorld >= maximumBallisticWorld)
        {
            segment.NearWorld = distanceWorld;
            segment.FarWorld = cone.FarWorld;
            outIsEntirelyBallistic = true;
            return TraceWaveBallistic(scene, segment, outIsBlocked);
        }
        previousWorld = distanceWorld;
        distanceWorld = min(maximumBallisticWorld, distanceWorld * LUDUS_WAVE_CONNECTION_BALLISTIC_SCALE);
    }
    return WAVE_QUERY_ESCAPE_CAPACITY;
}
/// Field / unoccluded Gaussian field. Failure preserves the output and reports invalid input/capacity.
[noinline] uint EvaluateWaveConnectionToReceiver(SceneContext scene,
                                                 float3 sourceWorld,
                                                 float3 receiverWorld,
                                                 float3 directionWorld,
                                                 float distanceWorld,
                                                 float wavelengthWorld,
                                                 inout float2 inoutField) {
    if (IsFiniteBits(sourceWorld) == false || IsFiniteBits(receiverWorld) == false ||
        IsFiniteBits(directionWorld) == false || IsFiniteBits(distanceWorld) == false || distanceWorld <= 0 ||
        IsFiniteBits(wavelengthWorld) == false || wavelengthWorld < LUDUS_WAVE_CONNECTION_MIN_WAVELENGTH_WORLD ||
        wavelengthWorld / distanceWorld > WAVE_CONNECTION_MAX_WAVELENGTH_TO_DISTANCE ||
        abs(dot(directionWorld, directionWorld) - 1) > WAVE_CONNECTION_UNIT_TOLERANCE)
        return WAVE_QUERY_INVALID;
    float wavenumber = 2 * PI / wavelengthWorld;
    float sigmaWorldSquared = distanceWorld / wavenumber;
    float halfWidthWorld = LUDUS_WAVE_CONNECTION_TAIL_DEVIATIONS * sqrt(2 * sigmaWorldSquared);
    EllipticalCone cone = (EllipticalCone)0;
    cone.OriginWorld = sourceWorld;
    cone.DirectionWorld = directionWorld;
    float3 helperWorld =
        abs(directionWorld.y) < WAVE_CONNECTION_FRAME_HELPER_COSINE ? float3(0, 1, 0) : float3(1, 0, 0);
    cone.AxisXWorld = normalize(cross(helperWorld, directionWorld));
    cone.RadiusWorld = halfWidthWorld;
    cone.MinorToMajor = 1;
    float coordinateScaleWorld = max(distanceWorld,
                                     max(max(abs(sourceWorld.x), max(abs(sourceWorld.y), abs(sourceWorld.z))),
                                         max(abs(receiverWorld.x), max(abs(receiverWorld.y), abs(receiverWorld.z)))));
    float endpointEscapeWorld = max(LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld,
                                    LUDUS_WAVE_CONNECTION_ENDPOINT_ROUNDOFF_SCALE * coordinateScaleWorld);
    if (2 * endpointEscapeWorld >= distanceWorld)
        return WAVE_QUERY_INVALID;
    cone.NearWorld = endpointEscapeWorld;
    cone.FarWorld = distanceWorld - cone.NearWorld;
    float sourceEscapeWorld;
    bool isBlocked;
    bool isEntirelyBallistic;
    uint escapeStatus =
        EscapeWaveOrigin(scene, cone, wavelengthWorld, sourceEscapeWorld, isBlocked, isEntirelyBallistic);
    if (escapeStatus != WAVE_QUERY_READY)
        return escapeStatus;
    if (isBlocked == true || isEntirelyBallistic == true)
    {
        inoutField = float2(isBlocked == true ? 0 : 1, 0);
        return WAVE_QUERY_READY;
    }
    EllipticalCone reverse = cone;
    reverse.OriginWorld = receiverWorld;
    reverse.DirectionWorld = -directionWorld;
    float receiverEscapeWorld;
    escapeStatus =
        EscapeWaveOrigin(scene, reverse, wavelengthWorld, receiverEscapeWorld, isBlocked, isEntirelyBallistic);
    if (escapeStatus != WAVE_QUERY_READY)
        return escapeStatus;
    if (isBlocked == true || isEntirelyBallistic == true)
    {
        inoutField = float2(isBlocked == true ? 0 : 1, 0);
        return WAVE_QUERY_READY;
    }
    cone.NearWorld = sourceEscapeWorld;
    cone.FarWorld = max(cone.NearWorld, distanceWorld - receiverEscapeWorld);
    WaveConnectionGrid grid;
    bool hasField = false;
    float fieldDistanceWorld = 0;
    for (uint interaction = 0; interaction < LUDUS_WAVE_CONNECTION_REGIONS; ++interaction)
    {
        WaveInteractionRegion region = (WaveInteractionRegion)0;
        uint status =
            cone.NearWorld < cone.FarWorld ? FindWaveInteractionRegion(scene, cone, region) : WAVE_QUERY_READY;
        if (status != WAVE_QUERY_READY)
            return status;
        if (region.TriangleCount == 0)
        {
            if (hasField == false)
            {
                inoutField = float2(1, 0);
                return WAVE_QUERY_READY;
            }
            PropagateWaveGrid(grid, distanceWorld - fieldDistanceWorld, wavelengthWorld, halfWidthWorld);
            float2 field = grid.Values[(LUDUS_WAVE_CONNECTION_AXIS / 2) * LUDUS_WAVE_CONNECTION_AXIS +
                                       LUDUS_WAVE_CONNECTION_AXIS / 2];
            float2 normalization = EvaluateWaveGaussian(0, distanceWorld, sigmaWorldSquared, wavenumber);
            float2 result =
                MultiplyWaveField(field, float2(normalization.x, -normalization.y)) / dot(normalization, normalization);
            if (IsFiniteBits(result.x) == false || IsFiniteBits(result.y) == false)
                return WAVE_QUERY_INVALID;
            inoutField = result;
            return WAVE_QUERY_READY;
        }
        float screenDistanceWorld = region.Frustum.NearWorld * 0.5 + region.Frustum.FarWorld * 0.5;
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
                        coveredSamples += IsWavePointBlocked(scene,
                                                             region,
                                                             float2(positionXWorld + offsetXWorld,
                                                                    positionYWorld + offsetYWorld)) == true
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
            return WAVE_QUERY_READY;
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
            escape.OriginWorld = sourceWorld + directionWorld * cone.NearWorld;
            escape.FarWorld = cone.FarWorld - cone.NearWorld;
            escape.NearWorld = min(escape.FarWorld, endpointEscapeWorld);
            float escapedWorld;
            uint escaped =
                EscapeWaveOrigin(scene, escape, wavelengthWorld, escapedWorld, isBlocked, isEntirelyBallistic);
            if (escaped != WAVE_QUERY_READY)
                return escaped;
            if (isBlocked == true)
            {
                inoutField = float2(0);
                return WAVE_QUERY_READY;
            }
            cone.NearWorld =
                isEntirelyBallistic == true ? cone.FarWorld : min(cone.FarWorld, cone.NearWorld + escapedWorld);
        }
    }
    return WAVE_QUERY_PROPAGATION_CAPACITY;
}

uint EvaluateWaveConnection(SceneContext scene,
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
