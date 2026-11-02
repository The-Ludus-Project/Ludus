#include "internal/wave_connection.h"
#include "../shaders/wave_connection_layout.h"
#include <ludus/foundation/math/enclosure.hpp>

// Thanks to Shlomi Steinberg and Matt Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics",
// CGF45(2), e70322 (2026), Sections3.1 and4.2, https://doi.org/10.1111/cgf.70322, for coherent field sums
// and scene-derived interaction frusta. This scalar connection specialization uses a periodic paraxial
// angular-spectrum lattice instead of their analytic Fraunhofer/UTD operators and bidirectional cone paths.
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
/// Coordinates in the interaction frustum's transverse basis, in world-distance units.
struct ProjectedPointFrustum final
{
    float64 X;
    float64 Y;
};
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
rhi::RasterStatus
TraceBallistic(const ReferenceSceneData& scene, math::EllipticalCone rayCone, bool& outIsBlocked) noexcept
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
struct WaveOriginEscape final
{
    float64 DistanceWorld = 0;
    bool IsBlocked = false;
    bool IsEntirelyBallistic = false;
};
rhi::RasterStatus EscapeWaveOrigin(const ReferenceSceneData& scene,
                                   const math::EllipticalCone& cone,
                                   float64 wavelengthWorld,
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
        if (distanceWorld > previousWorld)
        {
            const rhi::RasterStatus status = TraceBallistic(scene, segment, outEscape.IsBlocked);
            if (status != rhi::RasterStatus::Ready || outEscape.IsBlocked == true)
            {
                return status;
            }
        }
        segment.NearWorld = distanceWorld;
        segment.FarWorld = math::Min(cone.FarWorld, distanceWorld + cone.NearWorld);
        WaveInteractionRegion region;
        const rhi::RasterStatus status = FindWaveInteractionRegion(scene, segment, region);
        if (status == rhi::RasterStatus::Ready && region.TriangleCount == 0)
        {
            outEscape.DistanceWorld = distanceWorld;
            return status;
        }
        if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::CapacityExceeded)
        {
            return status;
        }
        if (distanceWorld >= maximumBallisticWorld)
        {
            // At the escape cap retain a declared ballistic connection, rather than repeatedly sampling
            // the same grazing surface. This scalar profile does not model its near-wall guided modes.
            segment.NearWorld = distanceWorld;
            segment.FarWorld = cone.FarWorld;
            outEscape.IsEntirelyBallistic = true;
            return TraceBallistic(scene, segment, outEscape.IsBlocked);
        }
        previousWorld = distanceWorld;
        distanceWorld = math::Min(maximumBallisticWorld, distanceWorld * LUDUS_WAVE_CONNECTION_BALLISTIC_SCALE);
    }
    return rhi::RasterStatus::Unsupported;
}

} // namespace
rhi::RasterStatus EvaluateWaveConnectionToReceiver(const ReferenceSceneData& scene,
                                                   math::Vector3d sourceWorld,
                                                   math::Vector3d receiverWorld,
                                                   math::Vector3d directionWorld,
                                                   float64 distanceWorld,
                                                   float64 wavelengthWorld,
                                                   WaveField& outField) noexcept
{
    if (math::IsFinite(sourceWorld) == false || math::IsFinite(receiverWorld) == false ||
        math::IsFinite(directionWorld) == false || math::IsFinite(distanceWorld) == false || distanceWorld <= 0 ||
        math::IsFinite(wavelengthWorld) == false || wavelengthWorld < MIN_WAVELENGTH_WORLD ||
        wavelengthWorld / distanceWorld > MAX_WAVELENGTH_TO_DISTANCE ||
        math::Abs(math::Dot(directionWorld, directionWorld) - 1) > UNIT_DIRECTION_TOLERANCE)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const float64 wavenumber = 2 * math::kPi / wavelengthWorld;
    const float64 sigmaWorldSquared = distanceWorld / wavenumber;
    const float64 sigmaWorld = math::Sqrt(sigmaWorldSquared);
    const float64 halfWidthWorld = LUDUS_WAVE_CONNECTION_TAIL_DEVIATIONS * math::Sqrt(2.0) * sigmaWorld;
    math::EllipticalCone cone;
    cone.OriginWorld = sourceWorld;
    cone.DirectionWorld = directionWorld;
    const math::Vector3d helperWorld =
        math::Abs(directionWorld.Y) < FRAME_HELPER_COSINE ? math::Vector3d{0, 1, 0} : math::Vector3d{1, 0, 0};
    const math::Vector3d crossWorld = math::Cross(helperWorld, directionWorld);
    cone.AxisXWorld = crossWorld * (1 / math::Sqrt(math::Dot(crossWorld, crossWorld)));
    // The cylinder encloses the propagated Gaussian at every z in this finite connection, with margin
    // beyond the three-sigma intensity ellipse. The same window bounds the periodic propagation lattice.
    cone.RadiusWorld = halfWidthWorld;
    const float64 coordinateScaleWorld = math::Max(
        distanceWorld,
        math::Max(
            math::Max(math::Abs(sourceWorld.X), math::Max(math::Abs(sourceWorld.Y), math::Abs(sourceWorld.Z))),
            math::Max(math::Abs(receiverWorld.X), math::Max(math::Abs(receiverWorld.Y), math::Abs(receiverWorld.Z)))));
    const float64 endpointEscapeWorld = math::Max(LUDUS_WAVE_CONNECTION_BALLISTIC_WAVELENGTHS * wavelengthWorld,
                                                  LUDUS_WAVE_CONNECTION_ENDPOINT_ROUNDOFF_SCALE * coordinateScaleWorld);
    if (2 * endpointEscapeWorld >= distanceWorld)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    cone.NearWorld = endpointEscapeWorld;
    cone.FarWorld = distanceWorld - cone.NearWorld;
    WaveOriginEscape escapeResult;
    rhi::RasterStatus escapeStatus = EscapeWaveOrigin(scene, cone, wavelengthWorld, escapeResult);
    const float64 sourceEscapeWorld = escapeResult.DistanceWorld;
    if (escapeStatus != rhi::RasterStatus::Ready)
    {
        return escapeStatus;
    }
    if (escapeResult.IsBlocked == true || escapeResult.IsEntirelyBallistic == true)
    {
        outField = {escapeResult.IsBlocked == true ? 0.0 : 1.0, 0};
        return rhi::RasterStatus::Ready;
    }
    math::EllipticalCone reverse = cone;
    reverse.OriginWorld = receiverWorld;
    reverse.DirectionWorld = -directionWorld;
    escapeStatus = EscapeWaveOrigin(scene, reverse, wavelengthWorld, escapeResult);
    const float64 receiverEscapeWorld = escapeResult.DistanceWorld;
    if (escapeStatus != rhi::RasterStatus::Ready)
    {
        return escapeStatus;
    }
    if (escapeResult.IsBlocked == true || escapeResult.IsEntirelyBallistic == true)
    {
        outField = {escapeResult.IsBlocked == true ? 0.0 : 1.0, 0};
        return rhi::RasterStatus::Ready;
    }
    cone.NearWorld = sourceEscapeWorld;
    cone.FarWorld = math::Max(cone.NearWorld, distanceWorld - receiverEscapeWorld);
    WaveGrid grid;
    bool hasField = false;
    float64 fieldDistanceWorld = 0;
    for (uint32 interaction = 0; interaction < LUDUS_WAVE_CONNECTION_REGIONS; ++interaction)
    {
        WaveInteractionRegion region;
        const rhi::RasterStatus status =
            cone.NearWorld < cone.FarWorld ? FindWaveInteractionRegion(scene, cone, region) : rhi::RasterStatus::Ready;
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        if (region.TriangleCount == 0)
        {
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
            const float64 power =
                normalization.Real * normalization.Real + normalization.Imaginary * normalization.Imaginary;
            const WaveField result{
                (field.Real * normalization.Real + field.Imaginary * normalization.Imaginary) / power,
                (field.Imaginary * normalization.Real - field.Real * normalization.Imaginary) / power};
            if (math::IsFinite(result.Real) == false || math::IsFinite(result.Imaginary) == false)
            {
                return rhi::RasterStatus::InvalidDescription;
            }
            outField = result;
            return rhi::RasterStatus::Ready;
        }
        const float64 screenDistanceWorld = region.Frustum.NearWorld * 0.5 + region.Frustum.FarWorld * 0.5;
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
                        coveredSamples += IsProjectedPointBlocked(scene,
                                                                  region,
                                                                  {
                                                                      .X = positionXWorld + offsetXWorld,
                                                                      .Y = positionYWorld + offsetYWorld,
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
            escape.OriginWorld = sourceWorld + directionWorld * cone.NearWorld;
            escape.FarWorld = cone.FarWorld - cone.NearWorld;
            escape.NearWorld = math::Min(escape.FarWorld, endpointEscapeWorld);
            const rhi::RasterStatus escaped = EscapeWaveOrigin(scene, escape, wavelengthWorld, escapeResult);
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
        // No remaining interval: finish through the normal empty-scene path on the next iteration.
        if (nextNearWorld > cone.FarWorld)
        {
            cone.FarWorld = cone.NearWorld;
        }
    }
    return rhi::RasterStatus::Unsupported;
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
