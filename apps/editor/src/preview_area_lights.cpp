#include "internal/preview_area_lights.h"

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace rr = graphics::renderer;
namespace rhi = graphics::rhi;
constexpr usize CORNERS = 3;
struct Edge final
{
    math::Vector3 A{};
    math::Vector3 B{};
    uint32 Triangle = 0;
};
struct Plane final
{
    math::Vector3 Normal{};
    math::Vector3 Origin{};
    float32 Tolerance = 0;
    /// Zero-area triangle: it has no normal and emits no power, so it joins no patch.
    bool IsDegenerate = false;
};
[[nodiscard]] bool IsPositionLess(math::Vector3 a, math::Vector3 b) noexcept
{
    return a.X < b.X || (a.X == b.X && (a.Y < b.Y || (a.Y == b.Y && a.Z < b.Z)));
}
[[nodiscard]] uint32 FindRoot(Array<uint32>& inoutParents, uint32 triangle) noexcept
{
    while (inoutParents[triangle] != triangle)
    {
        inoutParents[triangle] = inoutParents[inoutParents[triangle]];
        triangle = inoutParents[triangle];
    }
    return triangle;
}
[[nodiscard]] bool AreCoplanar(const Plane& a, const Plane& b) noexcept
{
    return math::Dot(a.Normal, b.Normal) >= .99999F &&
           math::Abs(math::Dot(a.Normal, b.Origin - a.Origin)) <= math::Max(a.Tolerance, b.Tolerance);
}
[[nodiscard]] uint64 ComputePatchId(uint64 source, uint32 firstTriangle) noexcept
{
    // A deterministic avalanche of the persistent instance and first source
    // triangle; topology edits invalidate the scene key before publication.
    uint64 value = source ^ ((static_cast<uint64>(firstTriangle) + 1) * 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value == 0 ? 1 : value;
}
} // namespace
rhi::RasterStatus AppendPreviewAreaLights(const resources::SceneMesh& mesh,
                                          const math::Affine3& meshToWorld,
                                          math::Vector3 resolvedEmission,
                                          bool isTwoSided,
                                          uint64 objectSourceId,
                                          float32 range,
                                          rr::LightSelection& inoutLights,
                                          uint32& outAdded) noexcept
{
    if (objectSourceId == 0 || mesh.Indices.IsEmpty() == true || mesh.Indices.GetSize() % CORNERS != 0 ||
        mesh.Indices.GetSize() > content::scene_format::IMPORT_LIMITS.Vertices ||
        inoutLights.Count > content::scene_format::MAX_LIGHTS)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const usize count = mesh.Indices.GetSize() / CORNERS;
    Array<Edge> edges;
    Array<Plane> planes;
    Array<uint32> parents;
    if (edges.TryResize(mesh.Indices.GetSize()) == false || planes.TryResize(count) == false ||
        parents.TryResize(count) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    usize edgeCount = 0;
    for (usize triangle = 0; triangle < count; ++triangle)
    {
        parents[triangle] = static_cast<uint32>(triangle);
        for (usize corner = 0; corner < CORNERS; ++corner)
        {
            if (mesh.Indices[triangle * CORNERS + corner] >= mesh.Positions.GetSize())
            {
                return rhi::RasterStatus::InvalidDescription;
            }
        }
        const math::Vector3 a = math::TransformPoint(meshToWorld, mesh.Positions[mesh.Indices[triangle * CORNERS]]);
        const math::Vector3 b = math::TransformPoint(meshToWorld, mesh.Positions[mesh.Indices[triangle * CORNERS + 1]]);
        const math::Vector3 c = math::TransformPoint(meshToWorld, mesh.Positions[mesh.Indices[triangle * CORNERS + 2]]);
        Plane& plane = planes[triangle];
        const math::Vector3 cross = math::Cross(b - a, c - a);
        const float32 length = math::Length(cross);
        if (math::IsFinite(length) == false)
        {
            return rhi::RasterStatus::InvalidDescription;
        }
        if (length <= 0)
        {
            // Imported emitters commonly carry collapsed triangles. They add no
            // area or power; excluding their edges also keeps them from making
            // a neighbouring edge look nonmanifold.
            plane.IsDegenerate = true;
            continue;
        }
        plane.Normal = cross * (1 / length);
        plane.Origin = a;
        plane.Tolerance = 1e-4F + 1e-6F * math::Max(math::Length(a), math::Max(math::Length(b), math::Length(c)));
        for (usize corner = 0; corner < CORNERS; ++corner)
        {
            Edge& edge = edges[edgeCount];
            ++edgeCount;
            edge.A = mesh.Positions[mesh.Indices[triangle * CORNERS + corner]];
            edge.B = mesh.Positions[mesh.Indices[triangle * CORNERS + (corner + 1) % CORNERS]];
            edge.Triangle = static_cast<uint32>(triangle);
            if (IsPositionLess(edge.B, edge.A) == true)
            {
                const math::Vector3 temporary = edge.A;
                edge.A = edge.B;
                edge.B = temporary;
            }
        }
    }
    Sort(edges.GetData(), edgeCount, [](const Edge& a, const Edge& b) noexcept {
        return IsPositionLess(a.A, b.A) ||
               (a.A == b.A && (IsPositionLess(a.B, b.B) || (a.B == b.B && a.Triangle < b.Triangle)));
    });
    for (usize first = 0; first < edgeCount;)
    {
        usize end = first + 1;
        while (end < edgeCount && edges[end].A == edges[first].A && edges[end].B == edges[first].B)
        {
            ++end;
        }
        // Nonmanifold edges are not an admitted planar source. Reject before
        // constructing any candidate rather than performing quadratic joins.
        if (end - first > 2)
        {
            return rhi::RasterStatus::Unsupported;
        }
        if (end - first == 2 && AreCoplanar(planes[edges[first].Triangle], planes[edges[first + 1].Triangle]) == true)
        {
            const uint32 a = FindRoot(parents, edges[first].Triangle);
            const uint32 b = FindRoot(parents, edges[first + 1].Triangle);
            parents[math::Max(a, b)] = math::Min(a, b);
        }
        first = end;
    }
    Array<uint32> roots;
    Array<uint32> patchIndices;
    if (roots.TryResize(count) == false || patchIndices.TryEnsureCapacity(mesh.Indices.GetSize()) == false)
    {
        return rhi::RasterStatus::OutOfMemory;
    }
    for (usize i = 0; i < count; ++i)
    {
        roots[i] = FindRoot(parents, static_cast<uint32>(i));
    }
    rr::LightSelection candidate = inoutLights;
    const uint32 initialCount = candidate.Count;
    for (usize root = 0; root < count; ++root)
    {
        if (roots[root] != root || planes[root].IsDegenerate == true)
        {
            continue;
        }
        if (candidate.Count == 128)
        {
            return rhi::RasterStatus::CapacityExceeded;
        }
        patchIndices.Clear();
        for (usize triangle = 0; triangle < count; ++triangle)
        {
            if (roots[triangle] == root)
            {
                for (usize corner = 0; corner < CORNERS; ++corner)
                {
                    if (patchIndices.TryAdd(mesh.Indices[triangle * CORNERS + corner]) == false)
                    {
                        return rhi::RasterStatus::OutOfMemory;
                    }
                }
            }
        }
        const rhi::RasterStatus status = rr::BuildPlanarAreaLight(
            {
                .PositionsMesh = {mesh.Positions.GetData(), mesh.Positions.GetSize()},
                .Indices = {patchIndices.GetData(), patchIndices.GetSize()},
                .MeshToWorld = meshToWorld,
                .Emission = resolvedEmission,
                .SourceId = ComputePatchId(objectSourceId, static_cast<uint32>(root)),
                .Range = range,
                .IsTwoSided = isTwoSided,
            },
            candidate.Lights[candidate.Count]);
        if (status != rhi::RasterStatus::Ready)
        {
            return status;
        }
        ++candidate.Count;
    }
    rr::LightSelection normalized;
    const rhi::RasterStatus status = rr::NormalizeDirectLights(candidate.Lights, candidate.Count, normalized);
    if (status != rhi::RasterStatus::Ready)
    {
        return status;
    }
    outAdded = candidate.Count - initialCount;
    inoutLights = normalized;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::editor
