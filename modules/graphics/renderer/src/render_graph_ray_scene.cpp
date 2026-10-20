#include "internal/render_graph_ray_scene.h"

#include <ludus/content/render_graph_format/plan.h>
#include <ludus/foundation/base/checked_integer.hpp>

#include <new>

namespace ludus::graphics::renderer::internal
{
rhi::RasterStatus BuildGraphRayScene(BorrowedView<const GraphRaySceneInput> inputs,
                                     const ReferenceCaptureDescription& description,
                                     usize byteBudget,
                                     core::UniquePtr<ReferenceJob>& outJob) noexcept
{
    using Status = rhi::RasterStatus;
    constexpr usize CORNER_COUNT = 3;
    constexpr float32 MAX_COORDINATE_METERS = 1000000;
    constexpr float32 TRIANGLE_EXTENT_ROUNDING_ALLOWANCE = 2e-6F;
    if (inputs.IsValid() == false || outJob.Get() != nullptr)
    {
        return Status::InvalidDescription;
    }
    if (inputs.GetSize() > content::render_graph_format::MAX_ITEMS)
    {
        return Status::CapacityExceeded;
    }
    uint64 triangleCountWide = 0;
    for (usize i = 0; i < inputs.GetSize(); ++i)
    {
        const GraphRaySceneInput& input = inputs[i];
        if (input.MeshOrNull == nullptr || ReferenceColor(input.Color, 1) == false)
        {
            return Status::InvalidDescription;
        }
        triangleCountWide += input.MeshOrNull->Indices.GetSize() / CORNER_COUNT;
    }
    if (triangleCountWide > (~uint32{0}) / 2)
    {
        return Status::CapacityExceeded;
    }
    const uint64 peakBytesWide =
        sizeof(ReferenceJob) + uint64(inputs.GetSize()) * (sizeof(ReferenceInstance) + sizeof(ReferenceSurfaceData)) +
        triangleCountWide * (sizeof(ReferenceTriangle) + 2 * sizeof(ReferenceBvhNode) + 2 * sizeof(uint32));
    usize peakBytes = 0;
    usize triangleCount = 0;
    if (TryIntegerCast(peakBytesWide, peakBytes) == false ||
        TryIntegerCast(triangleCountWide, triangleCount) == false || peakBytes > byteBudget)
    {
        return Status::CapacityExceeded;
    }
    ReferenceJob* allocationOrNull = new (std::nothrow) ReferenceJob;
    core::UniquePtr<ReferenceJob> candidate{allocationOrNull};
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->IsCpuOnly = true;
    candidate->Description = description;
    if (candidate->FlatSurfaces.TryEnsureCapacity(inputs.GetSize()) == false ||
        candidate->FlatSurfaces.TryResize(inputs.GetSize()) == false ||
        candidate->Scene.Instances.TryEnsureCapacity(inputs.GetSize()) == false ||
        candidate->Scene.Instances.TryResize(inputs.GetSize()) == false ||
        candidate->Scene.Triangles.TryEnsureCapacity(triangleCount) == false ||
        candidate->Scene.Triangles.TryResize(triangleCount) == false)
    {
        return Status::OutOfMemory;
    }
    usize triangleIndex = 0;
    for (usize i = 0; i < inputs.GetSize(); ++i)
    {
        const GraphRaySceneInput& input = inputs[i];
        const ReferenceMeshData& mesh = *input.MeshOrNull;
        ReferenceSurfaceData& surface = candidate->FlatSurfaces[i];
        surface.Bsdf.BaseColor = input.Color;
        ReferenceInstance& instance = candidate->Scene.Instances[i];
        instance.Surface = &surface;
        instance.SourceId = input.SourceId;
        instance.FirstTriangle = triangleIndex;
        instance.TriangleCount = mesh.Indices.GetSize() / CORNER_COUNT;
        math::Matrix3 normalMatrix;
        if (math::TryNormalMatrix(input.MeshToWorld, {}, normalMatrix) != math::MathStatus::Success)
        {
            return Status::InvalidDescription;
        }
        for (usize primitive = 0; primitive < instance.TriangleCount; ++primitive)
        {
            ReferenceTriangle& triangle = candidate->Scene.Triangles[triangleIndex++];
            triangle.Instance = static_cast<uint32>(i);
            triangle.Primitive = static_cast<uint32>(primitive);
            for (usize corner = 0; corner < CORNER_COUNT; ++corner)
            {
                const uint32 vertex = mesh.Indices[primitive * CORNER_COUNT + corner];
                const math::Vector3 positionWorld = math::TransformPoint(input.MeshToWorld, mesh.Positions[vertex]);
                if (math::IsFinite(positionWorld) == false || math::Abs(positionWorld.X) > MAX_COORDINATE_METERS ||
                    math::Abs(positionWorld.Y) > MAX_COORDINATE_METERS ||
                    math::Abs(positionWorld.Z) > MAX_COORDINATE_METERS)
                {
                    return Status::Unsupported;
                }
                triangle.Positions[corner] = positionWorld;
                triangle.Colors[corner] = {1, 1, 1, 1};
                const math::Vector3 errorWorld =
                    ComputeReferencePositionError(input.MeshToWorld, mesh.Positions[vertex]);
                triangle.Error = {math::Max(triangle.Error.X, errorWorld.X),
                                  math::Max(triangle.Error.Y, errorWorld.Y),
                                  math::Max(triangle.Error.Z, errorWorld.Z)};
            }
            const math::Vector3 edgeWorld = triangle.Positions[1] - triangle.Positions[0];
            const math::Vector3 otherEdgeWorld = triangle.Positions[2] - triangle.Positions[0];
            triangle.Area = math::Length(math::Cross(edgeWorld, otherEdgeWorld)) * 0.5F;
            const math::Vector3 positionMesh = mesh.Positions[mesh.Indices[primitive * CORNER_COUNT]];
            const math::Vector3 edgeMesh = mesh.Positions[mesh.Indices[primitive * CORNER_COUNT + 1]] - positionMesh;
            const math::Vector3 otherEdgeMesh =
                mesh.Positions[mesh.Indices[primitive * CORNER_COUNT + 2]] - positionMesh;
            triangle.GeometricNormal = ReferenceNormalize(normalMatrix * math::Cross(edgeMesh, otherEdgeMesh));
            if (math::IsFinite(triangle.Area) == false || triangle.Area <= 0 ||
                math::Length(triangle.GeometricNormal) == 0)
            {
                return Status::Unsupported;
            }
            for (usize corner = 0; corner < CORNER_COUNT; ++corner)
            {
                triangle.Normals[corner] = triangle.GeometricNormal;
            }
            const float32 extentWorld =
                math::Max(math::Max(math::Abs(edgeWorld.X), math::Abs(otherEdgeWorld.X)),
                          math::Max(math::Max(math::Abs(edgeWorld.Y), math::Abs(otherEdgeWorld.Y)),
                                    math::Max(math::Abs(edgeWorld.Z), math::Abs(otherEdgeWorld.Z))));
            const float32 roundingWorld = extentWorld * TRIANGLE_EXTENT_ROUNDING_ALLOWANCE;
            triangle.Error = triangle.Error + math::Vector3{roundingWorld, roundingWorld, roundingWorld};
        }
    }
    candidate->Scene.InstanceCount = inputs.GetSize();
    Status status = BuildReferenceBvh(candidate->Scene);
    if (status != Status::Ready)
    {
        return status;
    }
    status = PrepareReferenceCamera(*candidate);
    if (status != Status::Ready)
    {
        return status;
    }
    candidate->Info.CpuBytes = peakBytes;
    outJob.Swap(candidate);
    return Status::Ready;
}
} // namespace ludus::graphics::renderer::internal
