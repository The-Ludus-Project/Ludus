#include "internal/wave_scene_queries.h"
#include <ludus/foundation/containers/sort.hpp>

// Thanks to Shlomi Steinberg and Matt Pharr, "Wave Tracing: Generalizing The Path Integral To Wave Optics",
// Computer Graphics Forum 45(2), e70322, 2026, Section 4.2, https://doi.org/10.1111/cgf.70322 .
// Adopt the nearest-interaction frustum with depth twice the major radius. Use Ludus's binary BVH rather
// than the paper's specialized wide hierarchy; this changes traversal cost, not region membership.
namespace ludus::graphics::renderer::internal
{
namespace
{
math::Vector3d ConvertVector(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
struct RegionAxialBounds final
{
    float64 NearestWorld;
    float64 NearestFarWorld;
};
rhi::RasterStatus WalkRegion(const ReferenceSceneData& scene,
                             const math::EllipticalCone& cone,
                             bool shouldGather,
                             RegionAxialBounds& inoutBounds,
                             WaveInteractionRegion& inoutRegion) noexcept
{
    if (scene.Nodes.IsEmpty() == true)
    {
        return rhi::RasterStatus::Ready;
    }
    StaticArray<uint32, LUDUS_REFERENCE_STACK_SIZE> stack;
    usize stackCount = 1;
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
        math::EllipticalCone clipped = cone;
        if (shouldGather == false)
        {
            clipped.FarWorld = inoutBounds.NearestWorld;
        }
        if (math::TryTestEllipticalConeBounds(clipped,
                                              ConvertVector(node.Bounds.Min),
                                              ConvertVector(node.Bounds.Max),
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
            const math::ConeTriangle geometry{ConvertVector(triangle.Positions[0]),
                                              ConvertVector(triangle.Positions[1]),
                                              ConvertVector(triangle.Positions[2])};
            math::ConeTriangleIntersection intersection;
            if (math::TryIntersectEllipticalConeTriangle(clipped, geometry, intersection) == false)
            {
                return rhi::RasterStatus::InvalidDescription;
            }
            if (intersection.IsHit == false)
            {
                continue;
            }
            if (intersection.NearWorld < inoutBounds.NearestWorld)
            {
                inoutBounds.NearestWorld = intersection.NearWorld;
                inoutBounds.NearestFarWorld = intersection.FarWorld;
            }
            if (shouldGather == true)
            {
                if (inoutRegion.TriangleCount == LUDUS_WAVE_REGION_TRIANGLES)
                {
                    return rhi::RasterStatus::CapacityExceeded;
                }
                inoutRegion.Triangles[inoutRegion.TriangleCount++] = triangleIndex;
            }
        }
    }
    return rhi::RasterStatus::Ready;
}
} // namespace
rhi::RasterStatus FindWaveInteractionRegion(const ReferenceSceneData& scene,
                                            const math::EllipticalCone& cone,
                                            WaveInteractionRegion& outRegion) noexcept
{
    bool canIntersect = false;
    if (math::TryTestEllipticalConeBounds(cone, cone.OriginWorld, cone.OriginWorld, canIntersect) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    WaveInteractionRegion candidate;
    candidate.Frustum = cone;
    RegionAxialBounds bounds{ .NearestWorld = cone.FarWorld, .NearestFarWorld = cone.FarWorld };
    rhi::RasterStatus status = WalkRegion(scene, cone, false, bounds, candidate);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    candidate.Frustum.NearWorld = bounds.NearestWorld;
    candidate.Frustum.FarWorld =
        math::Min(cone.FarWorld, bounds.NearestWorld + 2 * (cone.RadiusWorld + cone.Slope * bounds.NearestWorld));
    if (cone.RadiusWorld + cone.Slope * bounds.NearestWorld == 0)
    {
        candidate.Frustum.FarWorld = bounds.NearestFarWorld;
    }
    status = WalkRegion(scene, candidate.Frustum, true, bounds, candidate);
    if (status == rhi::RasterStatus::Ready)
    {
        Sort(candidate.Triangles.GetData(), candidate.TriangleCount, [](uint32 first, uint32 second) noexcept {
            return first < second;
        });
        outRegion = candidate;
    }
    return status;
}
} // namespace ludus::graphics::renderer::internal
