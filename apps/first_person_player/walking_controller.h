#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/triangle_mesh.h>

namespace ludus::first_person
{
/// Indexed static triangles for one collision instance. The position and index storage must remain alive through Start.
struct StaticColliderInstance final
{
    foundation::BorrowedView<const foundation::math::Vector3> PositionsMesh;
    foundation::BorrowedView<const foundation::uint32> TriangleIndices;
    foundation::math::Affine3 MeshToWorld{};
};

/// Collision and fixed-step locomotion for one first-person player. Owns cooked support and obstacle meshes.
class WalkingController final
{
public:
    enum class StartStatus : foundation::uint8
    {
        Ready,
        NoGeometry,
        GeometryUnavailable,
        /// The Static Collider surfaces hold more than physics::collision::MAX_TRIANGLE_MESH_TRIANGLES triangles.
        GeometryTooLarge,
        /// A Static Collider triangle lies beyond physics::collision::MAX_COORDINATE_METERS from the World origin.
        GeometryOutOfRange,
        SpawnObstructed,
        /// The spawn's collision box centre lies beyond physics::collision::MAX_COORDINATE_METERS.
        SpawnOutOfRange,
    };

    /// Compute the upright collision box in world metres from the player's eye position.
    [[nodiscard]] static foundation::math::Aabb3 ComputeBodyBounds(foundation::math::Vector3d eyeWorld) noexcept;

    /// Cook the borrowed instances and start at the eye position in world metres. Failure preserves the current walk.
    [[nodiscard]] StartStatus Start(foundation::BorrowedView<const StaticColliderInstance> instances,
                                    foundation::math::Vector3d eyeWorld) noexcept;
    void Reset() noexcept;
    /// Advance by a bounded interval in seconds. A horizontal velocity is in metres per second.
    [[nodiscard]] foundation::math::Vector3d
    Advance(foundation::math::Vector3d velocityWorld, foundation::float64 deltaSeconds, bool shouldJump) noexcept;

private:
    struct Hit final
    {
        foundation::math::Vector3d NormalWorld;
        foundation::float64 Fraction = 1;
        bool HasContact = false;
        bool IsPenetrating = false;
        bool IsWalkable = false;
    };

    [[nodiscard]] StartStatus build(foundation::BorrowedView<const StaticColliderInstance> instances) noexcept;
    [[nodiscard]] bool hasGeometry() const noexcept;
    [[nodiscard]] static Hit castMesh(const physics::collision::TriangleMesh& mesh,
                                      foundation::math::Vector3d centerWorld,
                                      foundation::math::Vector3d movementWorld,
                                      bool isSupportMesh) noexcept;
    [[nodiscard]] Hit cast(foundation::math::Vector3d centerWorld,
                           foundation::math::Vector3d movementWorld) const noexcept;
    [[nodiscard]] bool tryStep(foundation::math::Vector3d movementWorld,
                               foundation::math::Vector3d& outCenterWorld) const noexcept;
    void walk(foundation::math::Vector3d movementWorld) noexcept;
    void tick(foundation::math::Vector3d velocityWorld, bool shouldJump) noexcept;

    /// Static Collider triangles, in world space, whose face can support the player (floors, treads, slopes up
    /// to 45 degrees). Uncooked when the World has none.
    physics::collision::TriangleMesh mSupportMesh;
    /// Every other admitted Static Collider triangle (walls, risers, steep slopes). Uncooked when the World has none.
    physics::collision::TriangleMesh mObstacleMesh;
    foundation::math::Vector3d mSpawnCenterWorld{};
    foundation::math::Vector3d mCenterWorld{};
    /// Lowest collider point; falling this far below it returns the player to the spawn.
    foundation::float64 mLowestWorldY = 0;
    foundation::float64 mVerticalSpeed = 0;
    foundation::float64 mPendingSeconds = 0;
    bool mIsGrounded = false;
    bool mShouldJump = false;
};
} // namespace ludus::first_person
