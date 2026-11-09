#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::editor
{
struct RenderScene;

/// Transient collision for Explore as Player. Owns a bounded copy of the World
/// triangles and sweeps one upright box against them. It has no simulation bodies.
class ViewportWalkCollision final
{
public:
    enum class StartStatus : foundation::uint8
    {
        Ready,
        NoGeometry,
        GeometryUnavailable,
        SpawnObstructed,
    };

    /// Build from the current scene. Failure preserves the previous session.
    [[nodiscard]] StartStatus Start(const RenderScene& scene, foundation::math::Vector3d eyeWorld) noexcept;
    void Reset() noexcept;
    /// Advance by a bounded interval in seconds. A horizontal velocity is in metres per second.
    [[nodiscard]] foundation::math::Vector3d
    Advance(foundation::math::Vector3d velocityWorld, foundation::float64 deltaSeconds, bool shouldJump) noexcept;

private:
    struct Triangle final
    {
        foundation::math::Vector3 FirstWorld;
        foundation::math::Vector3 SecondWorld;
        foundation::math::Vector3 ThirdWorld;
    };
    struct Node final
    {
        foundation::math::Aabb3 BoundsWorld;
        foundation::uint32 First = 0;
        foundation::uint32 Count = 0;
        foundation::uint32 Right = 0;
    };
    struct Hit final
    {
        foundation::math::Vector3d NormalWorld;
        foundation::float64 Fraction = 1;
        bool HasContact = false;
        bool IsPenetrating = false;
        bool IsWalkable = false;
    };

    [[nodiscard]] bool build(const RenderScene& scene) noexcept;
    [[nodiscard]] bool buildNode(foundation::uint32 first, foundation::uint32 count) noexcept;
    [[nodiscard]] Hit cast(foundation::math::Vector3d centerWorld,
                           foundation::math::Vector3d movementWorld) const noexcept;
    void castNode(foundation::uint32 node,
                  foundation::math::Vector3d centerWorld,
                  foundation::math::Vector3d movementWorld,
                  Hit& inoutHit) const noexcept;
    static void castTriangle(const Triangle& triangle,
                             foundation::math::Vector3d centerWorld,
                             foundation::math::Vector3d movementWorld,
                             Hit& inoutHit) noexcept;
    [[nodiscard]] bool tryStep(foundation::math::Vector3d movementWorld,
                               foundation::math::Vector3d& outCenterWorld) const noexcept;
    void walk(foundation::math::Vector3d movementWorld) noexcept;
    void tick(foundation::math::Vector3d velocityWorld, bool shouldJump) noexcept;

    foundation::Array<Triangle> mTriangles;
    foundation::Array<Node> mNodes;
    foundation::math::Vector3d mSpawnCenterWorld{};
    foundation::math::Vector3d mCenterWorld{};
    foundation::float64 mVerticalSpeed = 0;
    foundation::float64 mPendingSeconds = 0;
    bool mIsGrounded = false;
    bool mShouldJump = false;
};
} // namespace ludus::editor
