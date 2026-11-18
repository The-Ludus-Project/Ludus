#include "internal/viewport_walk_collision.h"

#include "internal/render_scene.h"

#include <ludus/foundation/containers/array.hpp>

namespace ludus::editor
{
using namespace foundation;

first_person::WalkingController::StartStatus
StartWorldWalk(const RenderScene& scene, math::Vector3d eyeWorld, first_person::WalkingController& inoutWalk) noexcept
{
    using StartStatus = first_person::WalkingController::StartStatus;
    if (scene.Definition != nullptr && scene.Definition->Meshes.GetSize() != static_cast<usize>(scene.Meshes.size()))
    {
        return StartStatus::GeometryUnavailable;
    }
    Array<first_person::StaticColliderInstance> instances;
    for (const PreviewObject& object : scene.Objects)
    {
        if (scene.StaticColliderEntityIds.contains(object.Id) == false)
        {
            continue;
        }
        if (object.Mesh < 0 || object.Mesh >= scene.Meshes.size())
        {
            return StartStatus::GeometryUnavailable;
        }
        const first_person::StaticColliderInstance instance
        {
            .PositionsMesh = GetRenderSceneMeshPositions(scene, object.Mesh),
            .TriangleIndices = GetRenderSceneMeshIndices(scene, object.Mesh),
            .MeshToWorld = object.Transform,
        };
        if (instances.TryAdd(instance) == false)
        {
            return StartStatus::GeometryUnavailable;
        }
    }
    return inoutWalk.Start({instances.GetData(), instances.GetSize()}, eyeWorld);
}
} // namespace ludus::editor
