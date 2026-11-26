#include "internal/wave_connection.h"
#include "../shaders/wave_connection_layout.h"
#include <ludus/foundation/math/enclosure.hpp>

// Thanks to Shlomi Steinberg and Matt Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics",
// CGF45(2), e70322 (2026), Sections3.1 and4.2, https://doi.org/10.1111/cgf.70322, for coherent field sums
// and scene-derived interaction frusta. This scalar connection specialization uses a periodic paraxial
// angular-spectrum lattice instead of their analytic Fraunhofer/UTD operators and bidirectional cone paths;
// wave_edge_diffraction.cpp implements the selectable UTD operator.
namespace ludus::graphics::renderer::internal
{
namespace
{
constexpr usize AXIS = LUDUS_WAVE_CONNECTION_AXIS;
constexpr usize CELLS = static_cast<usize>(LUDUS_WAVE_CONNECTION_CELLS);
constexpr usize MASK_SAMPLES = static_cast<usize>(LUDUS_WAVE_CONNECTION_MASK_SAMPLES);
constexpr float64 FRAME_HELPER_COSINE = 0.9;
constexpr float64 MIN_WAVELENGTH_WORLD =
    static_cast<float64>(static_cast<float32>(LUDUS_WAVE_CONNECTION_MIN_WAVELENGTH_WORLD));
constexpr float64 UNIT_DIRECTION_TOLERANCE = 1e-6;
constexpr float64 MAX_WAVELENGTH_TO_DISTANCE = 1.0 / 32;
using WaveGrid = StaticArray<WaveField, CELLS>;
WaveField MultiplyField(WaveField first, WaveField second) noexcept
{
    return {first.Real * second.Real - first.Imaginary * second.Imaginary,
            first.Real * second.Imaginary + first.Imaginary * second.Real};
}
WaveField MakePhase(float64 phase) noexcept
{
    return {math::Cos(phase), math::Sin(phase)};
}
void TransformLine(WaveGrid& inoutGrid, usize offset, usize stride, bool isInverse) noexcept
{
    usize reversed = 0;
    for (usize index = 1; index < AXIS; ++index)
    {
        usize bit = AXIS / 2;
        while ((reversed & bit) != 0)
        {
            reversed ^= bit;
            bit >>= 1;
        }
        reversed ^= bit;
        if (index < reversed)
        {
            const WaveField saved = inoutGrid[offset + index * stride];
            inoutGrid[offset + index * stride] = inoutGrid[offset + reversed * stride];
            inoutGrid[offset + reversed * stride] = saved;
        }
    }
    for (usize length = 2; length <= AXIS; length *= 2)
    {
        const WaveField step = MakePhase((isInverse == true ? 2 : -2) * math::kPi / static_cast<float64>(length));
        for (usize first = 0; first < AXIS; first += length)
        {
            WaveField twiddle{1, 0};
            for (usize index = 0; index < length / 2; ++index)
            {
                const usize left = offset + (first + index) * stride;
                const usize right = left + length / 2 * stride;
                const WaveField value = MultiplyField(inoutGrid[right], twiddle);
                const WaveField original = inoutGrid[left];
                inoutGrid[left] = {original.Real + value.Real, original.Imaginary + value.Imaginary};
                inoutGrid[right] = {original.Real - value.Real, original.Imaginary - value.Imaginary};
                twiddle = MultiplyField(twiddle, step);
            }
        }
    }
    if (isInverse == true)
    {
        for (usize index = 0; index < AXIS; ++index)
        {
            WaveField& value = inoutGrid[offset + index * stride];
            value.Real /= static_cast<float64>(AXIS);
            value.Imaginary /= static_cast<float64>(AXIS);
        }
    }
}
struct WavePropagation final
{
    float64 DistanceWorld;
    float64 WavelengthWorld;
    float64 HalfWidthWorld;
};
void PropagateGrid(WaveGrid& inoutGrid, const WavePropagation& propagation) noexcept
{
    for (usize index = 0; index < AXIS; ++index)
    {
        TransformLine(inoutGrid, index * AXIS, 1, false);
    }
    for (usize index = 0; index < AXIS; ++index)
    {
        TransformLine(inoutGrid, index, AXIS, false);
    }
    for (usize y = 0; y < AXIS; ++y)
    {
        const float64 frequencyY =
            static_cast<float64>(y <= AXIS / 2 ? static_cast<isize>(y)
                                               : static_cast<isize>(y) - static_cast<isize>(AXIS)) /
            (2 * propagation.HalfWidthWorld);
        for (usize x = 0; x < AXIS; ++x)
        {
            const float64 frequencyX =
                static_cast<float64>(x <= AXIS / 2 ? static_cast<isize>(x)
                                                   : static_cast<isize>(x) - static_cast<isize>(AXIS)) /
                (2 * propagation.HalfWidthWorld);
            const float64 phase = -math::kPi * propagation.WavelengthWorld * propagation.DistanceWorld *
                                  (frequencyX * frequencyX + frequencyY * frequencyY);
            inoutGrid[y * AXIS + x] = MultiplyField(inoutGrid[y * AXIS + x], MakePhase(phase));
        }
    }
    for (usize index = 0; index < AXIS; ++index)
    {
        TransformLine(inoutGrid, index * AXIS, 1, true);
    }
    for (usize index = 0; index < AXIS; ++index)
    {
        TransformLine(inoutGrid, index, AXIS, true);
    }
}
struct GaussianWaveParameters final
{
    float64 DistanceWorld;
    float64 SigmaWorldSquared;
    float64 Wavenumber;
};
WaveField EvaluateGaussian(float64 radiusSquaredWorld, const GaussianWaveParameters& gaussian) noexcept
{
    const float64 correlation = gaussian.DistanceWorld / gaussian.Wavenumber;
    const float64 denominator = gaussian.SigmaWorldSquared * gaussian.SigmaWorldSquared + correlation * correlation;
    const float64 amplitude = math::Exp(-radiusSquaredWorld * gaussian.SigmaWorldSquared / (2 * denominator));
    const WaveField normalization{gaussian.SigmaWorldSquared * gaussian.SigmaWorldSquared / denominator,
                                  -gaussian.SigmaWorldSquared * correlation / denominator};
    const WaveField phase = MakePhase(radiusSquaredWorld * correlation / (2 * denominator));
    const WaveField value = MultiplyField(normalization, phase);
    return {amplitude * value.Real, amplitude * value.Imaginary};
}
math::Vector3d WidenVector(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
} // namespace
bool IsProjectedPointBlocked(const ReferenceSceneData& scene,
                             const WaveInteractionRegion& region,
                             const ProjectedPointFrustum& positionFrustum) noexcept
{
    const math::Vector3d axisXWorld = region.Frustum.AxisXWorld;
    const math::Vector3d axisYWorld = math::Cross(region.Frustum.DirectionWorld, axisXWorld);
    for (uint32 slot = 0; slot < region.TriangleCount; ++slot)
    {
        const ReferenceTriangle& triangle = scene.Triangles[region.Triangles[slot]];
        const math::Vector3d first = WidenVector(triangle.Positions[0]) - region.Frustum.OriginWorld;
        const math::Vector3d second = WidenVector(triangle.Positions[1]) - region.Frustum.OriginWorld;
        const math::Vector3d third = WidenVector(triangle.Positions[2]) - region.Frustum.OriginWorld;
        const float64 ax = math::Dot(first, axisXWorld) - positionFrustum.X;
        const float64 ay = math::Dot(first, axisYWorld) - positionFrustum.Y;
        const float64 bx = math::Dot(second, axisXWorld) - positionFrustum.X;
        const float64 by = math::Dot(second, axisYWorld) - positionFrustum.Y;
        const float64 cx = math::Dot(third, axisXWorld) - positionFrustum.X;
        const float64 cy = math::Dot(third, axisYWorld) - positionFrustum.Y;
        const float64 firstWeight = bx * cy - by * cx;
        const float64 secondWeight = cx * ay - cy * ax;
        const float64 thirdWeight = ax * by - ay * bx;
        const float64 sum = firstWeight + secondWeight + thirdWeight;
        const bool hasNegative = firstWeight < 0 || secondWeight < 0 || thirdWeight < 0;
        const bool hasPositive = firstWeight > 0 || secondWeight > 0 || thirdWeight > 0;
        if (sum == 0 || (hasNegative == true && hasPositive == true))
        {
            continue;
        }
        // Geometry-only fixtures omit instances. Retained renderer scenes always supply the material owner.
        if (scene.Instances.IsEmpty() == true)
        {
            return true;
        }
        if (triangle.Instance < scene.Instances.GetSize())
        {
            const float32 u = static_cast<float32>(secondWeight / sum);
            const float32 v = static_cast<float32>(thirdWeight / sum);
            if (ReferenceOpacity(scene.Instances[triangle.Instance],
                                 InterpolateReferenceUv(triangle, u, v),
                                 InterpolateReferenceColor(triangle, u, v).W) == true)
            {
                return true;
            }
        }
    }
    return false;
}
rhi::RasterStatus TraceWaveBallistic(const ReferenceSceneData& scene,
                                     math::EllipticalCone rayCone,
                                     const WaveExcludedTriangles& excluded,
                                     bool& outIsBlocked) noexcept
{
    rayCone.RadiusWorld = 0;
    rayCone.Slope = 0;
    StaticArray<uint32, LUDUS_REFERENCE_STACK_SIZE> stack;
    usize stackCount = scene.Nodes.IsEmpty() == true ? 0 : 1;
    stack[0] = 0;
    while (stackCount > 0)
    {
        const uint32 nodeIndex = stack[--stackCount];
        if (nodeIndex >= scene.Nodes.GetSize())
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        const ReferenceBvhNode& node = scene.Nodes[nodeIndex];
        bool canIntersect = false;
        if (math::TryTestEllipticalConeBounds(rayCone,
                                              WidenVector(node.Bounds.Min),
                                              WidenVector(node.Bounds.Max),
                                              canIntersect) == false)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        if (canIntersect == false)
        {
            continue;
        }
        if (node.Count == 0)
        {
            if (stackCount > stack.GetSize() - 2)
            {
                return rhi::RasterStatus::InvalidDescription;
            }
            stack[stackCount++] = node.Right;
            stack[stackCount++] = node.Left;
            continue;
        }
        if (node.First > scene.Order.GetSize() || node.Count > scene.Order.GetSize() - node.First)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        for (uint32 slot = 0; slot < node.Count; ++slot)
        {
            const uint32 triangleIndex = scene.Order[node.First + slot];
            if (triangleIndex >= scene.Triangles.GetSize())
            {
                return rhi::RasterStatus::InvalidDescription;
            }
            if (excluded.Contains(triangleIndex) == true)
            {
                continue;
            }
            const ReferenceTriangle& triangle = scene.Triangles[triangleIndex];
            const math::ConeTriangle geometry{WidenVector(triangle.Positions[0]),
                                              WidenVector(triangle.Positions[1]),
                                              WidenVector(triangle.Positions[2])};
            math::ConeTriangleIntersection intersection;
            if (math::TryIntersectEllipticalConeTriangle(rayCone, geometry, intersection) == false)
            {
                return rhi::RasterStatus::InvalidDescription;
            }
            if (intersection.IsHit == false)
            {
                continue;
            }
            if (scene.Instances.IsEmpty() == true)
            {
                outIsBlocked = true;
                return rhi::RasterStatus::Ready;
            }
            if (triangle.Instance >= scene.Instances.GetSize())
            {
                return rhi::RasterStatus::InvalidDescription;
            }
            const float32 u = static_cast<float32>(intersection.NearBarycentric.Y);
            const float32 v = static_cast<float32>(intersection.NearBarycentric.Z);
            if (ReferenceOpacity(scene.Instances[triangle.Instance],
                                 InterpolateReferenceUv(triangle, u, v),
                                 InterpolateReferenceColor(triangle, u, v).W) == true)
            {
                outIsBlocked = true;
                return rhi::RasterStatus::Ready;
            }
        }
    }
    outIsBlocked = false;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus EscapeWaveOrigin(const ReferenceSceneData& scene,
                                   const math::EllipticalCone& cone,
                                   float64 wavelengthWorld,
                                   bool shouldTraceBallistic,
                                   WaveOriginEscape& outEscape) noexcept
{
    float64 distanceWorld = cone.NearWorld;
    float64 previousWorld = distanceWorld;
    const float64 maximumBallisticWorld =
        math::Max(cone.NearWorld,
                  math::Min(cone.FarWorld, LUDUS_WAVE_CONNECTION_MAX_BALLISTIC_WAVELENGTHS * wavelengthWorld));
    outEscape.IsBlocked = false;
    outEscape.IsEntirelyBallistic = false;
    for (uint32 attempt = 0; attempt < LUDUS_WAVE_CONNECTION_ESCAPE_ATTEMPTS; ++attempt)
    {
        math::EllipticalCone segment = cone;
        segment.NearWorld = previousWorld;
        segment.FarWorld = distanceWorld;
        if (shouldTraceBallistic == true && distanceWorld > previousWorld)
        {
            const rhi::RasterStatus status = TraceWaveBallistic(scene, segment, {}, outEscape.IsBlocked);
            if (status != rhi::RasterStatus::Ready || outEscape.IsBlocked == true)
            {
                return status;
            }
        }
        segment.NearWorld = distanceWorld;
        segment.FarWorld = math::Min(cone.FarWorld, distanceWorld + cone.NearWorld);
        bool hasHit = false;
        const rhi::RasterStatus status = TestWaveInteraction(scene, segment, hasHit);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        if (hasHit == false)
        {
            outEscape.DistanceWorld = distanceWorld;
            return rhi::RasterStatus::Ready;
        }
        if (distanceWorld >= maximumBallisticWorld)
        {
            // At the escape cap retain a declared ballistic connection, rather than repeatedly sampling
            // the same grazing surface. This scalar profile does not model its near-wall guided modes.
            segment.NearWorld = distanceWorld;
            segment.FarWorld = cone.FarWorld;
            outEscape.IsEntirelyBallistic = true;
            if (shouldTraceBallistic == false)
            {
                return rhi::RasterStatus::Ready;
            }
            return TraceWaveBallistic(scene, segment, {}, outEscape.IsBlocked);
        }
        previousWorld = distanceWorld;
        distanceWorld = math::Min(maximumBallisticWorld, distanceWorld * LUDUS_WAVE_CONNECTION_BALLISTIC_SCALE);
    }
    return rhi::RasterStatus::Unsupported;
}

bool TryAppendWaveConnectionLeg(WaveConnectionChain& inoutChain,
                                math::Vector3d beginWorld,
                                math::Vector3d endWorld,
                                float64 refractiveIndex,
                                bool isReflectedEntry) noexcept
{
    const math::Vector3d deltaWorld = endWorld - beginWorld;
    const float64 lengthWorld = math::Sqrt(math::Dot(deltaWorld, deltaWorld));
    if (inoutChain.LegCount >= WAVE_MAX_CONNECTION_LEGS || math::IsFinite(beginWorld) == false ||
        math::IsFinite(endWorld) == false || math::IsFinite(lengthWorld) == false || lengthWorld <= 0 ||
        math::IsFinite(refractiveIndex) == false || refractiveIndex <= 0)
    {
        return false;
    }
    inoutChain.Legs[inoutChain.LegCount++] =
    {
        .BeginWorld = beginWorld,
        .EndWorld = endWorld,
        .DirectionWorld = deltaWorld * (1 / lengthWorld),
        .LengthWorld = lengthWorld,
        .RefractiveIndex = refractiveIndex,
        .IsReflectedEntry = isReflectedEntry,
    };
    return true;
}
bool TryMakeWaveConnectionFrames(const WaveConnectionChain& chain, WaveConnectionFrames& outFrames) noexcept
{
    if (chain.LegCount == 0 || chain.LegCount > WAVE_MAX_CONNECTION_LEGS)
    {
        return false;
    }
    WaveConnectionFrames frames;
    const math::Vector3d firstWorld = chain.Legs[0].DirectionWorld;
    const math::Vector3d helperWorld =
        math::Abs(firstWorld.Y) < FRAME_HELPER_COSINE ? math::Vector3d{0, 1, 0} : math::Vector3d{1, 0, 0};
    const math::Vector3d crossWorld = math::Cross(helperWorld, firstWorld);
    frames.AxisXWorld[0] = crossWorld * (1 / math::Sqrt(math::Dot(crossWorld, crossWorld)));
    frames.Handedness[0] = 1;
    for (uint32 leg = 1; leg < chain.LegCount; ++leg)
    {
        const math::Vector3d incidentWorld = chain.Legs[leg - 1].DirectionWorld;
        const math::Vector3d outgoingWorld = chain.Legs[leg].DirectionWorld;
        math::Vector3d axisWorld = frames.AxisXWorld[leg - 1];
        frames.Handedness[leg] = frames.Handedness[leg - 1];
        if (chain.Legs[leg].IsReflectedEntry == true)
        {
            // A mirror reflection maps the incident direction to the outgoing one with the Householder transform of
            // the interface normal, which lies along their difference; it reverses the transverse handedness.
            const math::Vector3d normalWorld = incidentWorld - outgoingWorld;
            const float64 normalLength = math::Sqrt(math::Dot(normalWorld, normalWorld));
            if (normalLength <= 0 || math::IsFinite(normalLength) == false)
            {
                return false;
            }
            const math::Vector3d unitNormalWorld = normalWorld * (1 / normalLength);
            axisWorld = axisWorld - unitNormalWorld * (2 * math::Dot(axisWorld, unitNormalWorld));
            frames.Handedness[leg] = -frames.Handedness[leg];
        }
        else
        {
            const float64 cosine = math::Dot(incidentWorld, outgoingWorld);
            const math::Vector3d rotationWorld = math::Cross(incidentWorld, outgoingWorld);
            const float64 sine = math::Sqrt(math::Dot(rotationWorld, rotationWorld));
            if (cosine <= 0 || math::IsFinite(cosine) == false || math::IsFinite(sine) == false)
            {
                return false;
            }
            if (sine > 0)
            {
                // Rodrigues' rotation about the normal of the plane of incidence, by the refraction angle.
                const math::Vector3d unitAxisWorld = rotationWorld * (1 / sine);
                axisWorld = axisWorld * cosine + math::Cross(unitAxisWorld, axisWorld) * sine +
                            unitAxisWorld * (math::Dot(unitAxisWorld, axisWorld) * (1 - cosine));
            }
        }
        // Remove the rounding left by offset endpoints, so the frame stays transverse to the traced leg.
        axisWorld = axisWorld - outgoingWorld * math::Dot(axisWorld, outgoingWorld);
        const float64 axisLength = math::Sqrt(math::Dot(axisWorld, axisWorld));
        if (axisLength <= 0 || math::IsFinite(axisLength) == false)
        {
            return false;
        }
        frames.AxisXWorld[leg] = axisWorld * (1 / axisLength);
    }
    outFrames = frames;
    return true;
}
void BeginWaveRelay(ReferenceOpticalRelay& outRelay, math::Vector3 originWorld) noexcept
{
    outRelay = {};
    outRelay.OriginsWorld[0] = originWorld;
    outRelay.SegmentCount = 1;
}
void AppendWaveRelayEvent(ReferenceOpticalRelay& inoutRelay,
                          bool isSolid,
                          float32 eta,
                          bool isViewerInside,
                          bool isReflected,
                          math::Vector3 nextOriginWorld) noexcept
{
    if (inoutRelay.SegmentCount == 0 || inoutRelay.SegmentCount >= WAVE_MAX_CONNECTION_LEGS)
    {
        inoutRelay.HasConflict = true;
        return;
    }
    const float32 incomingIndex = isSolid == true && isViewerInside == true ? eta : 1;
    const bool isOutgoingInside = isReflected == true ? isViewerInside : isViewerInside == false;
    const float32 outgoingIndex = isSolid == true && isOutgoingInside == true ? eta : 1;
    const uint32 segment = inoutRelay.SegmentCount - 1;
    if (inoutRelay.RefractiveIndices[segment] != 0 && inoutRelay.RefractiveIndices[segment] != incomingIndex)
    {
        inoutRelay.HasConflict = true;
    }
    inoutRelay.RefractiveIndices[segment] = incomingIndex;
    inoutRelay.IsReflected[segment] = isReflected;
    inoutRelay.OriginsWorld[segment + 1] = nextOriginWorld;
    inoutRelay.RefractiveIndices[segment + 1] = outgoingIndex;
    ++inoutRelay.SegmentCount;
}
bool TryMakeWaveRelayChain(const ReferenceOpticalRelay& relay,
                           math::Vector3 sourceWorld,
                           WaveConnectionChain& outChain) noexcept
{
    if (relay.HasConflict == true || relay.SegmentCount < 2 || relay.SegmentCount > WAVE_MAX_CONNECTION_LEGS)
    {
        return false;
    }
    // Light order: the emitter, then the glass events from the last to the first, then the receiver.
    WaveConnectionChain chain;
    math::Vector3d beginWorld = WidenVector(sourceWorld);
    for (uint32 leg = 0; leg < relay.SegmentCount; ++leg)
    {
        const uint32 segment = relay.SegmentCount - 1 - leg;
        const math::Vector3d endWorld = WidenVector(relay.OriginsWorld[segment]);
        if (TryAppendWaveConnectionLeg(chain,
                                       beginWorld,
                                       endWorld,
                                       static_cast<float64>(relay.RefractiveIndices[segment]),
                                       leg > 0 && relay.IsReflected[segment] == true) == false)
        {
            return false;
        }
        beginWorld = endWorld;
    }
    outChain = chain;
    return true;
}
rhi::RasterStatus EvaluateWaveChainConnection(const ReferenceSceneData& scene,
                                              const WaveConnectionChain& chain,
                                              float64 wavelengthWorld,
                                              WaveField& outField) noexcept
{
    if (chain.LegCount == 0 || chain.LegCount > WAVE_MAX_CONNECTION_LEGS || math::IsFinite(wavelengthWorld) == false ||
        wavelengthWorld < MIN_WAVELENGTH_WORLD)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    // Paraxially, a leg of geometric length l in a medium of index n propagates as l / n in vacuum at the vacuum
    // wavelength: the reduced distance. Transverse coordinates are unchanged at the interfaces (Section 5.2's rule
    // that an interaction keeps the beam's solid angle and eccentricity), so one Gaussian mode spans the chain.
    float64 distanceWorld = 0;
    float64 geometricLengthWorld = 0;
    float64 coordinateMagnitudeWorld = 0;
    for (uint32 index = 0; index < chain.LegCount; ++index)
    {
        const WaveConnectionLeg& leg = chain.Legs[index];
        if (math::IsFinite(leg.BeginWorld) == false || math::IsFinite(leg.EndWorld) == false ||
            math::IsFinite(leg.DirectionWorld) == false || math::IsFinite(leg.LengthWorld) == false ||
            leg.LengthWorld <= 0 || math::IsFinite(leg.RefractiveIndex) == false || leg.RefractiveIndex <= 0 ||
            math::Abs(math::Dot(leg.DirectionWorld, leg.DirectionWorld) - 1) > UNIT_DIRECTION_TOLERANCE)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        distanceWorld += leg.LengthWorld / leg.RefractiveIndex;
        geometricLengthWorld += leg.LengthWorld;
        coordinateMagnitudeWorld =
            math::Max(coordinateMagnitudeWorld,
                      math::Max(math::Max(math::Abs(leg.BeginWorld.X),
                                          math::Max(math::Abs(leg.BeginWorld.Y), math::Abs(leg.BeginWorld.Z))),
                                math::Max(math::Abs(leg.EndWorld.X),
                                          math::Max(math::Abs(leg.EndWorld.Y), math::Abs(leg.EndWorld.Z)))));
    }
    if (math::IsFinite(distanceWorld) == false || wavelengthWorld / distanceWorld > MAX_WAVELENGTH_TO_DISTANCE)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    WaveConnectionFrames frames;
    if (TryMakeWaveConnectionFrames(chain, frames) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const float64 wavenumber = 2 * math::kPi / wavelengthWorld;
    const float64 sigmaWorldSquared = distanceWorld / wavenumber;
    const float64 sigmaWorld = math::Sqrt(sigmaWorldSquared);
    const float64 halfWidthWorld = LUDUS_WAVE_CONNECTION_TAIL_DEVIATIONS * math::Sqrt(2.0) * sigmaWorld;
    const float64 coordinateScaleWorld = math::Max(geometricLengthWorld, coordinateMagnitudeWorld);
    const float64 endpointEscapeWorld = math::Max(LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld,
                                                  LUDUS_WAVE_CONNECTION_ENDPOINT_ROUNDOFF_SCALE * coordinateScaleWorld);
    if (2 * endpointEscapeWorld >= distanceWorld)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    WaveGrid grid;
    bool hasField = false;
    float64 fieldDistanceWorld = 0;
    // Reduced distance from the source to the start of the current leg.
    float64 legStartWorld = 0;
    uint32 interaction = 0;
    for (uint32 index = 0; index < chain.LegCount; ++index)
    {
        const WaveConnectionLeg& leg = chain.Legs[index];
        const float64 handedness = frames.Handedness[index];
        const float64 legStartReducedWorld = legStartWorld;
        legStartWorld += leg.LengthWorld / leg.RefractiveIndex;
        if (2 * endpointEscapeWorld >= leg.LengthWorld)
        {
            // Inside both endpoint escapes: no interaction region is resolvable on this leg.
            continue;
        }
        math::EllipticalCone cone;
        cone.OriginWorld = leg.BeginWorld;
        cone.DirectionWorld = leg.DirectionWorld;
        cone.AxisXWorld = frames.AxisXWorld[index];
        // The cylinder encloses the propagated Gaussian at every z in this finite connection, with margin
        // beyond the three-sigma intensity ellipse. The same window bounds the periodic propagation lattice.
        cone.RadiusWorld = halfWidthWorld;
        cone.NearWorld = endpointEscapeWorld;
        cone.FarWorld = leg.LengthWorld - cone.NearWorld;
        WaveOriginEscape escapeResult;
        rhi::RasterStatus escapeStatus = EscapeWaveOrigin(scene, cone, wavelengthWorld, true, escapeResult);
        const float64 sourceEscapeWorld = escapeResult.DistanceWorld;
        if (escapeStatus != rhi::RasterStatus::Ready)
        {
            return escapeStatus;
        }
        if (escapeResult.IsBlocked == true)
        {
            outField = {};
            return rhi::RasterStatus::Ready;
        }
        if (escapeResult.IsEntirelyBallistic == true)
        {
            continue;
        }
        math::EllipticalCone reverse = cone;
        reverse.OriginWorld = leg.EndWorld;
        reverse.DirectionWorld = -leg.DirectionWorld;
        escapeStatus = EscapeWaveOrigin(scene, reverse, wavelengthWorld, true, escapeResult);
        const float64 receiverEscapeWorld = escapeResult.DistanceWorld;
        if (escapeStatus != rhi::RasterStatus::Ready)
        {
            return escapeStatus;
        }
        if (escapeResult.IsBlocked == true)
        {
            outField = {};
            return rhi::RasterStatus::Ready;
        }
        if (escapeResult.IsEntirelyBallistic == true)
        {
            continue;
        }
        cone.NearWorld = sourceEscapeWorld;
        cone.FarWorld = math::Max(cone.NearWorld, leg.LengthWorld - receiverEscapeWorld);
        for (; interaction < LUDUS_WAVE_CONNECTION_REGIONS; ++interaction)
        {
            WaveInteractionRegion region;
            const rhi::RasterStatus status = cone.NearWorld < cone.FarWorld
                                                 ? FindWaveInteractionRegion(scene, cone, region)
                                                 : rhi::RasterStatus::Ready;
            if (status != rhi::RasterStatus::Ready)
            {
                return status;
            }
            if (region.TriangleCount == 0)
            {
                break;
            }
            float64 screenDistanceWorld = region.Frustum.NearWorld * 0.5 + region.Frustum.FarWorld * 0.5;
            if (chain.LegCount > 1)
            {
                // A relay projects each region onto the midpoint of its slab in reduced depth, as the unfolded
                // straight connection would: twice the envelope radius from the nearest intersection, clipped at the
                // chain's far end rather than at the end of the leg.
                const float64 nearReducedWorld = legStartReducedWorld + region.Frustum.NearWorld / leg.RefractiveIndex;
                const float64 farReducedWorld =
                    math::Max(nearReducedWorld,
                              math::Min(distanceWorld - endpointEscapeWorld, nearReducedWorld + 2 * halfWidthWorld));
                screenDistanceWorld = nearReducedWorld * 0.5 + farReducedWorld * 0.5;
            }
            StaticArray<float64, CELLS> transmission;
            usize blockedCount = 0;
            for (usize y = 0; y < AXIS; ++y)
            {
                for (usize x = 0; x < AXIS; ++x)
                {
                    const float64 positionXWorld =
                        (2 * static_cast<float64>(x) / static_cast<float64>(AXIS) - 1) * halfWidthWorld;
                    const float64 positionYWorld =
                        (2 * static_cast<float64>(y) / static_cast<float64>(AXIS) - 1) * halfWidthWorld;
                    const usize cell = y * AXIS + x;
                    usize coveredSamples = 0;
                    const float64 cellWidthWorld = 2 * halfWidthWorld / static_cast<float64>(AXIS);
                    for (uint32 sampleY = 0; sampleY < LUDUS_WAVE_CONNECTION_MASK_AXIS; ++sampleY)
                    {
                        for (uint32 sampleX = 0; sampleX < LUDUS_WAVE_CONNECTION_MASK_AXIS; ++sampleX)
                        {
                            const float64 offsetXWorld =
                                ((static_cast<float64>(sampleX) + 0.5) / LUDUS_WAVE_CONNECTION_MASK_AXIS - 0.5) *
                                cellWidthWorld;
                            const float64 offsetYWorld =
                                ((static_cast<float64>(sampleY) + 0.5) / LUDUS_WAVE_CONNECTION_MASK_AXIS - 0.5) *
                                cellWidthWorld;
                            // The lattice keeps the first leg's transverse coordinates; a mirrored leg's frame has
                            // the opposite handedness, so its second coordinate changes sign.
                            coveredSamples +=
                                IsProjectedPointBlocked(scene,
                                                        region,
                                                        {
                                                            .X = positionXWorld + offsetXWorld,
                                                            .Y = (positionYWorld + offsetYWorld) * handedness,
                                                        }) == true
                                    ? 1
                                    : 0;
                        }
                    }
                    transmission[cell] = 1 - static_cast<float64>(coveredSamples) / static_cast<float64>(MASK_SAMPLES);
                    blockedCount += coveredSamples;
                    if (hasField == false)
                    {
                        grid[cell] = EvaluateGaussian(positionXWorld * positionXWorld + positionYWorld * positionYWorld,
                                                      {
                                                          .DistanceWorld = screenDistanceWorld,
                                                          .SigmaWorldSquared = sigmaWorldSquared,
                                                          .Wavenumber = wavenumber,
                                                      });
                    }
                }
            }
            if (blockedCount == CELLS * MASK_SAMPLES)
            {
                outField = {};
                return rhi::RasterStatus::Ready;
            }
            if (blockedCount > 0)
            {
                if (hasField == true)
                {
                    PropagateGrid(grid,
                                  {
                                      .DistanceWorld = screenDistanceWorld - fieldDistanceWorld,
                                      .WavelengthWorld = wavelengthWorld,
                                      .HalfWidthWorld = halfWidthWorld,
                                  });
                }
                for (usize cell = 0; cell < CELLS; ++cell)
                {
                    grid[cell].Real *= transmission[cell];
                    grid[cell].Imaginary *= transmission[cell];
                }
                hasField = true;
                fieldDistanceWorld = screenDistanceWorld;
            }
            const float64 nextNearWorld = math::NextFloat64Up(region.Frustum.FarWorld);
            if (nextNearWorld > cone.FarWorld)
            {
                cone.NearWorld = cone.FarWorld;
            }
            else
            {
                cone.NearWorld = nextNearWorld;
            }
            if (cone.FarWorld - cone.NearWorld > LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld)
            {
                math::EllipticalCone escape = cone;
                escape.OriginWorld = leg.BeginWorld + leg.DirectionWorld * cone.NearWorld;
                escape.FarWorld = cone.FarWorld - cone.NearWorld;
                escape.NearWorld = math::Min(escape.FarWorld, endpointEscapeWorld);
                const rhi::RasterStatus escaped = EscapeWaveOrigin(scene, escape, wavelengthWorld, true, escapeResult);
                const float64 escapedWorld = escapeResult.DistanceWorld;
                if (escaped != rhi::RasterStatus::Ready)
                {
                    return escaped;
                }
                if (escapeResult.IsBlocked == true)
                {
                    outField = {};
                    return rhi::RasterStatus::Ready;
                }
                cone.NearWorld = escapeResult.IsEntirelyBallistic == true
                                     ? cone.FarWorld
                                     : math::Min(cone.FarWorld, cone.NearWorld + escapedWorld);
            }
            // No remaining interval: finish this leg through the empty-region path on the next iteration.
            if (nextNearWorld > cone.FarWorld)
            {
                cone.FarWorld = cone.NearWorld;
            }
        }
        if (interaction == LUDUS_WAVE_CONNECTION_REGIONS)
        {
            return rhi::RasterStatus::Unsupported;
        }
        // The empty region that ended this leg used one step, as it does for a one-leg connection.
        ++interaction;
    }
    if (hasField == false)
    {
        outField = {1, 0};
        return rhi::RasterStatus::Ready;
    }
    PropagateGrid(grid,
                  {
                      .DistanceWorld = distanceWorld - fieldDistanceWorld,
                      .WavelengthWorld = wavelengthWorld,
                      .HalfWidthWorld = halfWidthWorld,
                  });
    const WaveField field = grid[(AXIS / 2) * AXIS + AXIS / 2];
    const WaveField normalization = EvaluateGaussian(
        0,
        { .DistanceWorld = distanceWorld, .SigmaWorldSquared = sigmaWorldSquared, .Wavenumber = wavenumber });
    const float64 power = normalization.Real * normalization.Real + normalization.Imaginary * normalization.Imaginary;
    const WaveField result{(field.Real * normalization.Real + field.Imaginary * normalization.Imaginary) / power,
                           (field.Imaginary * normalization.Real - field.Real * normalization.Imaginary) / power};
    if (math::IsFinite(result.Real) == false || math::IsFinite(result.Imaginary) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    outField = result;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus EvaluateWaveConnectionToReceiver(const ReferenceSceneData& scene,
                                                   math::Vector3d sourceWorld,
                                                   math::Vector3d receiverWorld,
                                                   math::Vector3d directionWorld,
                                                   float64 distanceWorld,
                                                   float64 wavelengthWorld,
                                                   WaveField& outField) noexcept
{
    WaveConnectionChain chain;
    chain.Legs[0] =
    {
        .BeginWorld = sourceWorld,
        .EndWorld = receiverWorld,
        .DirectionWorld = directionWorld,
        .LengthWorld = distanceWorld,
        .RefractiveIndex = 1,
        .IsReflectedEntry = false,
    };
    chain.LegCount = 1;
    return EvaluateWaveChainConnection(scene, chain, wavelengthWorld, outField);
}
rhi::RasterStatus EvaluateWaveConnection(const ReferenceSceneData& scene,
                                         math::Vector3d sourceWorld,
                                         math::Vector3d directionWorld,
                                         float64 distanceWorld,
                                         float64 wavelengthWorld,
                                         WaveField& outField) noexcept
{
    return EvaluateWaveConnectionToReceiver(scene,
                                            sourceWorld,
                                            sourceWorld + directionWorld * distanceWorld,
                                            directionWorld,
                                            distanceWorld,
                                            wavelengthWorld,
                                            outField);
}
} // namespace ludus::graphics::renderer::internal
