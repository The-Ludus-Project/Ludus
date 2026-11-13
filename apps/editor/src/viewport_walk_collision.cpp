#include "internal/viewport_walk_collision.h"

#include "internal/render_scene.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>

// Explore as Player is the first consumer of PhysicsCollision's closest box cast
// (docs/architecture/physics-collision.md). That sweep was lifted from this file;
// see the attribution beside CastTriangle in triangle_mesh_box_cast.cpp.

namespace ludus::editor
{
using namespace foundation;
namespace collision = physics::collision;
namespace math = foundation::math;

namespace
{
constexpr usize TRIANGLE_CORNER_COUNT = 3;
/// The cook source addresses world-space vertices with uint32 indices.
constexpr usize MAX_COLLISION_VERTICES = 0xFFFFFFFFU;
constexpr math::Vector3d BODY_HALF_EXTENT_METRES{0.24, 0.875, 0.24};
constexpr math::Vector3d CENTER_TO_EYE_METRES{0, 0.725, 0};
constexpr float64 STEP_HEIGHT_METRES = 0.35;
constexpr float64 GROUND_SNAP_METRES = 0.09;
constexpr float64 CONTACT_SKIN_METRES = 0.002;
// A riser and its tread meet the falling box at one landing time, but their
// separating axes are scaled by different triangle areas, so the two fractions
// can differ by rounding.
constexpr float64 CONTACT_FRACTION_TOLERANCE = 1e-12;
constexpr float64 MIN_WALKABLE_NORMAL_Y = 0.7071067811865475;
constexpr float64 GRAVITY_METRES_PER_SECOND_SQUARED = 9.81;
constexpr float64 JUMP_SPEED_METRES_PER_SECOND = 4.5;
constexpr float64 FALL_LIMIT_BELOW_WORLD_METRES = 5;
constexpr float64 TICK_SECONDS = 1.0 / 120.0;
constexpr float64 MAX_ELAPSED_SECONDS = 0.1;
constexpr uint32 MAX_SLIDE_CONTACTS = 3;

math::Vector3d Widen(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

bool TryNormalize(math::Vector3d directionWorld, math::Vector3d& outUnitWorld) noexcept
{
    const float64 length = math::Length(directionWorld);
    if (length <= 0 || math::IsFinite(length) == false)
    {
        return false;
    }
    outUnitWorld = directionWorld * (1 / length);
    return true;
}

float64 ComputeSafeFraction(math::Vector3d movementWorld, float64 fraction) noexcept
{
    const float64 length = math::Length(movementWorld);
    return length > CONTACT_SKIN_METRES ? math::Max(0.0, fraction - CONTACT_SKIN_METRES / length) : 0;
}

/// Whether a box centre lies inside the coordinate envelope PhysicsCollision queries admit.
bool IsWithinQueryEnvelope(math::Vector3d centerWorld) noexcept
{
    constexpr float64 LIMIT = static_cast<float64>(collision::MAX_COORDINATE_METERS);
    return math::Abs(centerWorld.X) <= LIMIT && math::Abs(centerWorld.Y) <= LIMIT && math::Abs(centerWorld.Z) <= LIMIT;
}

/// Whether a face can carry the player, by the same 45 degree limit as the contact normal.
bool IsSupportFace(math::Vector3 firstWorld, math::Vector3 secondWorld, math::Vector3 thirdWorld) noexcept
{
    const math::Vector3d faceWorld =
        math::Cross(Widen(secondWorld) - Widen(firstWorld), Widen(thirdWorld) - Widen(firstWorld));
    math::Vector3d unitWorld;
    return TryNormalize(faceWorld, unitWorld) == true && math::Abs(unitWorld.Y) >= MIN_WALKABLE_NORMAL_Y;
}

/// Cook one partition. An empty or entirely degenerate partition stays uncooked.
ViewportWalkCollision::StartStatus CookPartition(BorrowedView<const math::Vector3> positionsWorld,
                                                 BorrowedView<const uint32> indices,
                                                 collision::TriangleMesh& outMesh) noexcept
{
    using StartStatus = ViewportWalkCollision::StartStatus;
    if (indices.GetSize() == 0)
    {
        return StartStatus::Ready;
    }
    // Leave degenerate triangles out instead of failing the walk, as the preview always has:
    // imported meshes often carry zero-area slivers.
    constexpr collision::TriangleMeshCookSettings SETTINGS
    {
        .DegeneratePolicy = collision::DegenerateTrianglePolicy::Skip,
    };
    const collision::TriangleMeshSource source{ .PositionsMesh = positionsWorld, .Indices = indices };
    switch (collision::TriangleMesh::TryCook(source, SETTINGS, outMesh).Result)
    {
        case collision::Status::Success:
        case collision::Status::NoGeometry:
            return StartStatus::Ready;
        case collision::Status::CapacityExceeded:
            return StartStatus::GeometryTooLarge;
        case collision::Status::OutOfRange:
            return StartStatus::GeometryOutOfRange;
        default:
            return StartStatus::GeometryUnavailable;
    }
}
} // namespace

math::Aabb3 ViewportWalkCollision::ComputeBodyBounds(math::Vector3d eyeWorld) noexcept
{
    const math::Vector3d centerWorld = eyeWorld - CENTER_TO_EYE_METRES;
    const math::Vector3d minimumWorld = centerWorld - BODY_HALF_EXTENT_METRES;
    const math::Vector3d maximumWorld = centerWorld + BODY_HALF_EXTENT_METRES;
    return {{static_cast<float32>(minimumWorld.X),
             static_cast<float32>(minimumWorld.Y),
             static_cast<float32>(minimumWorld.Z)},
            {static_cast<float32>(maximumWorld.X),
             static_cast<float32>(maximumWorld.Y),
             static_cast<float32>(maximumWorld.Z)}};
}

ViewportWalkCollision::StartStatus ViewportWalkCollision::Start(const RenderScene& scene,
                                                                math::Vector3d eyeWorld) noexcept
{
    ViewportWalkCollision candidate;
    const StartStatus built = candidate.build(scene);
    if (built != StartStatus::Ready)
    {
        return built;
    }
    if (candidate.hasGeometry() == false)
    {
        return StartStatus::NoGeometry;
    }
    candidate.mCenterWorld = eyeWorld - CENTER_TO_EYE_METRES;
    if (math::IsFinite(eyeWorld) == false)
    {
        return StartStatus::SpawnObstructed;
    }
    if (IsWithinQueryEnvelope(candidate.mCenterWorld) == false)
    {
        return StartStatus::SpawnOutOfRange;
    }
    if (candidate.cast(candidate.mCenterWorld, {}).IsPenetrating == true)
    {
        return StartStatus::SpawnObstructed;
    }
    candidate.mSpawnCenterWorld = candidate.mCenterWorld;
    candidate.mIsGrounded = candidate.cast(candidate.mCenterWorld, {0, -GROUND_SNAP_METRES, 0}).IsWalkable;
    *this = Move(candidate);
    return StartStatus::Ready;
}

void ViewportWalkCollision::Reset() noexcept
{
    *this = {};
}

ViewportWalkCollision::StartStatus ViewportWalkCollision::build(const RenderScene& scene) noexcept
{
    if (scene.Definition != nullptr && scene.Definition->Meshes.GetSize() != static_cast<usize>(scene.Meshes.size()))
    {
        return StartStatus::GeometryUnavailable;
    }
    usize triangleCount = 0;
    usize vertexCount = 0;
    constexpr usize MAX_TRIANGLES = collision::MAX_TRIANGLE_MESH_TRIANGLES;
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
        const usize count = GetRenderSceneMeshIndices(scene, object.Mesh).GetSize();
        if (count % TRIANGLE_CORNER_COUNT != 0)
        {
            return StartStatus::GeometryUnavailable;
        }
        if (count / TRIANGLE_CORNER_COUNT > MAX_TRIANGLES - triangleCount)
        {
            return StartStatus::GeometryTooLarge;
        }
        triangleCount += count / TRIANGLE_CORNER_COUNT;
        const usize positionCount = GetRenderSceneMeshPositions(scene, object.Mesh).GetSize();
        // Each instance gets its own world-space copy of its mesh vertices.
        if (positionCount > MAX_COLLISION_VERTICES - vertexCount)
        {
            return StartStatus::GeometryTooLarge;
        }
        vertexCount += positionCount;
    }
    // One shared vertex array and one index array: support triangles fill it from the front and obstacle
    // triangles from the back, so neither partition needs a second reservation.
    Array<math::Vector3> positionsWorld;
    Array<uint32> indices;
    if (positionsWorld.TryEnsureCapacity(vertexCount) == false ||
        indices.TryResize(triangleCount * TRIANGLE_CORNER_COUNT) == false)
    {
        return StartStatus::GeometryUnavailable;
    }
    usize supportEnd = 0;
    usize obstacleBegin = indices.GetSize();
    for (const PreviewObject& object : scene.Objects)
    {
        if (scene.StaticColliderEntityIds.contains(object.Id) == false)
        {
            continue;
        }
        const BorrowedView<const math::Vector3> positionsMesh = GetRenderSceneMeshPositions(scene, object.Mesh);
        const BorrowedView<const uint32> meshIndices = GetRenderSceneMeshIndices(scene, object.Mesh);
        const uint32 baseVertex = static_cast<uint32>(positionsWorld.GetSize());
        for (const math::Vector3 positionMesh : positionsMesh)
        {
            if (positionsWorld.TryAdd(math::TransformPoint(object.Transform, positionMesh)) == false)
            {
                return StartStatus::GeometryUnavailable;
            }
        }
        for (usize index = 0; index < meshIndices.GetSize(); index += TRIANGLE_CORNER_COUNT)
        {
            const uint32 first = meshIndices[index];
            const uint32 second = meshIndices[index + 1];
            const uint32 third = meshIndices[index + 2];
            if (first >= positionsMesh.GetSize() || second >= positionsMesh.GetSize() ||
                third >= positionsMesh.GetSize())
            {
                return StartStatus::GeometryUnavailable;
            }
            const bool isSupport = IsSupportFace(positionsWorld[baseVertex + first],
                                                 positionsWorld[baseVertex + second],
                                                 positionsWorld[baseVertex + third]);
            usize corner = supportEnd;
            if (isSupport == true)
            {
                supportEnd += TRIANGLE_CORNER_COUNT;
            }
            else
            {
                obstacleBegin -= TRIANGLE_CORNER_COUNT;
                corner = obstacleBegin;
            }
            indices[corner] = baseVertex + first;
            indices[corner + 1] = baseVertex + second;
            indices[corner + 2] = baseVertex + third;
        }
    }
    const BorrowedView<const math::Vector3> positionsView(positionsWorld.GetData(), positionsWorld.GetSize());
    const StartStatus support =
        CookPartition(positionsView, BorrowedView<const uint32>(indices.GetData(), supportEnd), mSupportMesh);
    if (support != StartStatus::Ready)
    {
        return support;
    }
    const StartStatus obstacle =
        CookPartition(positionsView,
                      BorrowedView<const uint32>(indices.GetData() + obstacleBegin, indices.GetSize() - obstacleBegin),
                      mObstacleMesh);
    if (obstacle != StartStatus::Ready)
    {
        return obstacle;
    }
    float64 lowestWorldY = static_cast<float64>(collision::MAX_COORDINATE_METERS);
    if (mSupportMesh.IsCooked() == true)
    {
        lowestWorldY = static_cast<float64>(mSupportMesh.GetBoundsMesh().Min.Y);
    }
    if (mObstacleMesh.IsCooked() == true)
    {
        lowestWorldY = math::Min(lowestWorldY, static_cast<float64>(mObstacleMesh.GetBoundsMesh().Min.Y));
    }
    mLowestWorldY = lowestWorldY;
    return StartStatus::Ready;
}

bool ViewportWalkCollision::hasGeometry() const noexcept
{
    return mSupportMesh.IsCooked() == true || mObstacleMesh.IsCooked() == true;
}

ViewportWalkCollision::Hit ViewportWalkCollision::castMesh(const collision::TriangleMesh& mesh,
                                                           math::Vector3d centerWorld,
                                                           math::Vector3d movementWorld,
                                                           bool isSupportMesh) noexcept
{
    Hit hit;
    if (mesh.IsCooked() == false)
    {
        return hit;
    }
    const collision::BoxCastInput input
    {
        .CenterMesh = centerWorld,
        .HalfExtentsMesh = BODY_HALF_EXTENT_METRES,
        .TranslationMesh = movementWorld,
        .OverlapPolicy = collision::InitialOverlapPolicy::Report,
    };
    collision::BoxCastResult result;
    if (mesh.CastBoxClosest(input, result) != collision::Status::Success)
    {
        // A query the module cannot answer blocks like an overlap, so it can never let the player tunnel.
        hit.HasContact = true;
        hit.IsPenetrating = true;
        hit.Fraction = 0;
        return hit;
    }
    if (result.HasHit == false)
    {
        return hit;
    }
    hit.HasContact = true;
    hit.IsPenetrating = result.HasStartedOverlapping;
    hit.Fraction = result.Fraction;
    if (result.HasNormal == true)
    {
        hit.NormalWorld = result.NormalMesh;
        hit.IsWalkable = isSupportMesh == true && result.NormalMesh.Y >= MIN_WALKABLE_NORMAL_Y;
    }
    return hit;
}

ViewportWalkCollision::Hit ViewportWalkCollision::cast(math::Vector3d centerWorld,
                                                       math::Vector3d movementWorld) const noexcept
{
    const Hit support = castMesh(mSupportMesh, centerWorld, movementWorld, true);
    const Hit obstacle = castMesh(mObstacleMesh, centerWorld, movementWorld, false);
    if (support.HasContact == false || obstacle.IsPenetrating == true)
    {
        return obstacle.HasContact == true ? obstacle : support;
    }
    if (obstacle.HasContact == false || support.IsPenetrating == true)
    {
        return support;
    }
    Hit nearest = support.Fraction <= obstacle.Fraction ? support : obstacle;
    const bool isDownward = movementWorld.X == 0 && movementWorld.Z == 0 && movementWorld.Y < 0;
    if (isDownward == true && support.IsWalkable == true &&
        math::Abs(support.Fraction - obstacle.Fraction) <= CONTACT_FRACTION_TOLERANCE)
    {
        // A stair riser and its tread share the landing time. Keep the earliest
        // obstruction, but prefer the tread's support normal over the riser edge.
        nearest.NormalWorld = support.NormalWorld;
        nearest.IsWalkable = true;
    }
    return nearest;
}

bool ViewportWalkCollision::tryStep(math::Vector3d movementWorld, math::Vector3d& outCenterWorld) const noexcept
{
    constexpr math::Vector3d STEP_UP_WORLD{0, STEP_HEIGHT_METRES, 0};
    if (cast(mCenterWorld, STEP_UP_WORLD).HasContact == true)
    {
        return false;
    }
    const math::Vector3d raisedWorld = mCenterWorld + STEP_UP_WORLD;
    if (cast(raisedWorld, movementWorld).HasContact == true)
    {
        return false;
    }
    const math::Vector3d movedWorld = raisedWorld + movementWorld;
    constexpr math::Vector3d STEP_DOWN_WORLD{0, -STEP_HEIGHT_METRES - GROUND_SNAP_METRES, 0};
    const Hit landing = cast(movedWorld, STEP_DOWN_WORLD);
    if (landing.IsWalkable == false)
    {
        return false;
    }
    outCenterWorld = movedWorld + STEP_DOWN_WORLD * ComputeSafeFraction(STEP_DOWN_WORLD, landing.Fraction);
    return true;
}

void ViewportWalkCollision::walk(math::Vector3d movementWorld) noexcept
{
    for (uint32 contact = 0; contact < MAX_SLIDE_CONTACTS && math::LengthSquared(movementWorld) > 0; ++contact)
    {
        const Hit hit = cast(mCenterWorld, movementWorld);
        if (hit.HasContact == false)
        {
            mCenterWorld = mCenterWorld + movementWorld;
            return;
        }
        if (hit.IsPenetrating == true)
        {
            return;
        }
        math::Vector3d steppedWorld;
        if (contact == 0 && mIsGrounded == true && hit.IsWalkable == false &&
            tryStep(movementWorld, steppedWorld) == true)
        {
            mCenterWorld = steppedWorld;
            return;
        }
        const float64 fraction = ComputeSafeFraction(movementWorld, hit.Fraction);
        mCenterWorld = mCenterWorld + movementWorld * fraction;
        math::Vector3d slideNormalWorld = hit.NormalWorld;
        if (hit.IsWalkable == false)
        {
            slideNormalWorld.Y = 0;
            if (TryNormalize(slideNormalWorld, slideNormalWorld) == false)
            {
                return;
            }
        }
        movementWorld = movementWorld * (1 - fraction);
        const float64 intoSurface = math::Dot(movementWorld, slideNormalWorld);
        if (intoSurface >= 0)
        {
            return;
        }
        movementWorld = movementWorld - slideNormalWorld * intoSurface;
    }
}

void ViewportWalkCollision::tick(math::Vector3d velocityWorld, bool shouldJump) noexcept
{
    if (mIsGrounded == true && shouldJump == true)
    {
        mVerticalSpeed = JUMP_SPEED_METRES_PER_SECOND;
        mIsGrounded = false;
    }
    const bool wasGrounded = mIsGrounded;
    walk(velocityWorld * TICK_SECONDS);
    mVerticalSpeed -= GRAVITY_METRES_PER_SECOND_SQUARED * TICK_SECONDS;
    const float64 distance = mVerticalSpeed * TICK_SECONDS;
    const math::Vector3d verticalWorld{0, wasGrounded == true ? math::Min(distance, -GROUND_SNAP_METRES) : distance, 0};
    const Hit hit = cast(mCenterWorld, verticalWorld);
    mIsGrounded = hit.IsWalkable == true && verticalWorld.Y < 0;
    if (hit.HasContact == true)
    {
        mCenterWorld = mCenterWorld + verticalWorld * ComputeSafeFraction(verticalWorld, hit.Fraction);
        mVerticalSpeed = 0;
    }
    else
    {
        // The extra ground probe must never accelerate a fall over a ledge.
        mCenterWorld.Y += distance;
    }
    if (mCenterWorld.Y < mLowestWorldY - FALL_LIMIT_BELOW_WORLD_METRES || IsWithinQueryEnvelope(mCenterWorld) == false)
    {
        mCenterWorld = mSpawnCenterWorld;
        mVerticalSpeed = 0;
        mIsGrounded = false;
    }
}

math::Vector3d
ViewportWalkCollision::Advance(math::Vector3d velocityWorld, float64 deltaSeconds, bool shouldJump) noexcept
{
    if (hasGeometry() == true && math::IsFinite(deltaSeconds) == true && deltaSeconds > 0 &&
        math::IsFinite(velocityWorld) == true)
    {
        velocityWorld.Y = 0;
        mShouldJump = mShouldJump == true || shouldJump == true;
        mPendingSeconds += math::Min(deltaSeconds, MAX_ELAPSED_SECONDS);
        while (mPendingSeconds >= TICK_SECONDS)
        {
            tick(velocityWorld, mShouldJump);
            mShouldJump = false;
            mPendingSeconds -= TICK_SECONDS;
        }
    }
    return mCenterWorld + CENTER_TO_EYE_METRES;
}
} // namespace ludus::editor
