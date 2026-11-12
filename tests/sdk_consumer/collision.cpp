#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/triangle_mesh.h>

bool ExerciseInstalledCollision() noexcept
{
    using namespace ludus::physics::collision;
    using ludus::foundation::BorrowedView;
    using ludus::foundation::StaticArray;
    using ludus::foundation::math::Vector3;
    constexpr ludus::foundation::usize CORNER_COUNT = 4;
    constexpr ludus::foundation::usize INDEX_COUNT = 6;
    const StaticArray<Vector3, CORNER_COUNT> positions{Vector3{0.0f, 0.0f, 0.0f},
                                                       Vector3{0.0f, 0.0f, 1.0f},
                                                       Vector3{1.0f, 0.0f, 1.0f},
                                                       Vector3{1.0f, 0.0f, 0.0f}};
    const StaticArray<uint32, INDEX_COUNT> indices{0, 1, 2, 0, 2, 3};
    const TriangleMeshSource source
    {
        .PositionsMesh = BorrowedView<const Vector3>(positions.GetData(), CORNER_COUNT),
        .Indices = BorrowedView<const uint32>(indices.GetData(), INDEX_COUNT),
    };
    TriangleMesh mesh;
    if (TriangleMesh::TryCook(source, {}, mesh).Result != Status::Success)
    {
        return false;
    }
    const RaycastInput ray
    {
        .OriginMesh = {0.25, 2.0, 0.75},
        .DirectionMesh = {0.0, -1.0, 0.0},
        .MaxDistanceMeters = 5.0,
    };
    RaycastResult hit;
    const BoxCastInput box
    {
        .CenterMesh = {0.5, 2.0, 0.5},
        .HalfExtentsMesh = {0.25, 0.5, 0.25},
        .TranslationMesh = {0.0, -3.0, 0.0},
    };
    BoxCastResult contact;
    return mesh.RaycastClosest(ray, hit) == Status::Success && hit.HasHit == true && hit.DistanceMeters == 2.0 &&
           hit.TriangleIndex == 0 && mesh.CastBoxClosest(box, contact) == Status::Success && contact.HasHit == true &&
           contact.Fraction == 0.5;
}
