#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/triangle_mesh.h>

namespace ludus::editor
{
struct RenderScene;

/// Transient collision for Explore as Player. Cooks the Static Collider triangles into PhysicsCollision meshes and
/// sweeps one upright box against them with TriangleMesh::CastBoxClosest. It has no simulation bodies; the step,
/// slide, jump and support rules stay here.
class ViewportWalkCollision final
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

    /// Build from the current scene. Failure preserves the previous session.
    [[nodiscard]] StartStatus Start(const RenderScene& scene, foundation::math::Vector3d eyeWorld) noexcept;
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

    [[nodiscard]] StartStatus build(const RenderScene& scene) noexcept;
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
} // namespace ludus::editor
