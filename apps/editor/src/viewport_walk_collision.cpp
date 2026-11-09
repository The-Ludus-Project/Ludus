#include "internal/viewport_walk_collision.h"

#include "internal/render_scene.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;

namespace
{
constexpr usize TRIANGLE_CORNER_COUNT = 3;
constexpr usize MAX_COLLISION_TRIANGLES = 8000000;
constexpr uint32 TRIANGLES_PER_LEAF = 8;
constexpr uint32 AXIS_COUNT = 3;
constexpr StaticArray<math::Vector3d, AXIS_COUNT> WORLD_AXES{math::Vector3d{1, 0, 0},
                                                             math::Vector3d{0, 1, 0},
                                                             math::Vector3d{0, 0, 1}};
constexpr math::Vector3d BODY_HALF_EXTENT_METRES{0.24, 0.875, 0.24};
constexpr math::Vector3d CENTER_TO_EYE_METRES{0, 0.725, 0};
constexpr float64 STEP_HEIGHT_METRES = 0.35;
constexpr float64 GROUND_SNAP_METRES = 0.09;
constexpr float64 CONTACT_SKIN_METRES = 0.002;
// Equal contacts on a shared edge can use axes scaled by different triangle areas.
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

void IncludePoint(math::Aabb3& inoutBoundsWorld, math::Vector3 pointWorld) noexcept
{
    inoutBoundsWorld.Min = {math::Min(inoutBoundsWorld.Min.X, pointWorld.X),
                            math::Min(inoutBoundsWorld.Min.Y, pointWorld.Y),
                            math::Min(inoutBoundsWorld.Min.Z, pointWorld.Z)};
    inoutBoundsWorld.Max = {math::Max(inoutBoundsWorld.Max.X, pointWorld.X),
                            math::Max(inoutBoundsWorld.Max.Y, pointWorld.Y),
                            math::Max(inoutBoundsWorld.Max.Z, pointWorld.Z)};
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

float64 GetAxis(math::Vector3 value, uint32 axis) noexcept
{
    return static_cast<float64>(axis == 0 ? value.X : (axis == 1 ? value.Y : value.Z));
}

float64 ComputeSafeFraction(math::Vector3d movementWorld, float64 fraction) noexcept
{
    const float64 length = math::Length(movementWorld);
    return length > CONTACT_SKIN_METRES ? math::Max(0.0, fraction - CONTACT_SKIN_METRES / length) : 0;
}

bool OverlapsSweep(const math::Aabb3& boundsWorld, math::Vector3d startWorld, math::Vector3d endWorld) noexcept
{
    const math::Vector3d minimumWorld = Widen(boundsWorld.Min);
    const math::Vector3d maximumWorld = Widen(boundsWorld.Max);
    return minimumWorld.X <= math::Max(startWorld.X, endWorld.X) + BODY_HALF_EXTENT_METRES.X &&
           maximumWorld.X >= math::Min(startWorld.X, endWorld.X) - BODY_HALF_EXTENT_METRES.X &&
           minimumWorld.Y <= math::Max(startWorld.Y, endWorld.Y) + BODY_HALF_EXTENT_METRES.Y &&
           maximumWorld.Y >= math::Min(startWorld.Y, endWorld.Y) - BODY_HALF_EXTENT_METRES.Y &&
           minimumWorld.Z <= math::Max(startWorld.Z, endWorld.Z) + BODY_HALF_EXTENT_METRES.Z &&
           maximumWorld.Z >= math::Min(startWorld.Z, endWorld.Z) - BODY_HALF_EXTENT_METRES.Z;
}

// Thanks to David Eberly, "Intersection of Convex Objects: The Method of
// Separating Axes", Geometric Tools, sections 4 and 6, for intersecting time
// intervals on the face normals and edge cross products of two convex shapes.
// https://www.geometrictools.com/Documentation/MethodOfSeparatingAxes.pdf
// Here one shape is a stationary triangle and the other a translating AABB.
struct SweepInterval final
{
    float64 Enter = -1;
    float64 Exit = 1;
    math::Vector3d NormalWorld{};
};

bool ClipAxis(math::Vector3d axisWorld,
              math::Vector3d firstWorld,
              math::Vector3d secondWorld,
              math::Vector3d thirdWorld,
              math::Vector3d movementWorld,
              SweepInterval& inoutInterval) noexcept
{
    if (math::LengthSquared(axisWorld) == 0)
    {
        return true;
    }
    const float64 first = math::Dot(axisWorld, firstWorld);
    const float64 second = math::Dot(axisWorld, secondWorld);
    const float64 third = math::Dot(axisWorld, thirdWorld);
    const float64 minimum = math::Min(first, math::Min(second, third));
    const float64 maximum = math::Max(first, math::Max(second, third));
    const float64 radius = math::Abs(axisWorld.X) * BODY_HALF_EXTENT_METRES.X +
                           math::Abs(axisWorld.Y) * BODY_HALF_EXTENT_METRES.Y +
                           math::Abs(axisWorld.Z) * BODY_HALF_EXTENT_METRES.Z;
    const float64 projectedMovement = math::Dot(axisWorld, movementWorld);
    if (projectedMovement == 0)
    {
        // A stationary axis that merely touches allows parallel movement.
        return minimum < radius && maximum > -radius;
    }
    const float64 firstTime = (minimum - radius) / projectedMovement;
    const float64 lastTime = (maximum + radius) / projectedMovement;
    const float64 enter = math::Min(firstTime, lastTime);
    if (enter > inoutInterval.Enter)
    {
        inoutInterval.Enter = enter;
        inoutInterval.NormalWorld = projectedMovement > 0 ? -axisWorld : axisWorld;
    }
    inoutInterval.Exit = math::Min(inoutInterval.Exit, math::Max(firstTime, lastTime));
    return inoutInterval.Exit > 0 && inoutInterval.Enter < inoutInterval.Exit;
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
    if (candidate.build(scene) == false)
    {
        return StartStatus::GeometryUnavailable;
    }
    if (candidate.mNodes.IsEmpty() == true)
    {
        return StartStatus::NoGeometry;
    }
    candidate.mCenterWorld = eyeWorld - CENTER_TO_EYE_METRES;
    if (math::IsFinite(eyeWorld) == false || candidate.cast(candidate.mCenterWorld, {}).IsPenetrating == true)
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

bool ViewportWalkCollision::build(const RenderScene& scene) noexcept
{
    if (scene.Definition != nullptr && scene.Definition->Meshes.GetSize() != static_cast<usize>(scene.Meshes.size()))
    {
        return false;
    }
    usize triangleCount = 0;
    for (const PreviewObject& object : scene.Objects)
    {
        if (scene.StaticColliderEntityIds.contains(object.Id) == false)
        {
            continue;
        }
        if (object.Mesh < 0 || object.Mesh >= scene.Meshes.size())
        {
            return false;
        }
        const usize count = GetRenderSceneMeshIndices(scene, object.Mesh).GetSize();
        if (count % TRIANGLE_CORNER_COUNT != 0 ||
            count / TRIANGLE_CORNER_COUNT > MAX_COLLISION_TRIANGLES - triangleCount)
        {
            return false;
        }
        triangleCount += count / TRIANGLE_CORNER_COUNT;
    }
    if (mTriangles.TryEnsureCapacity(triangleCount) == false)
    {
        return false;
    }
    for (const PreviewObject& object : scene.Objects)
    {
        if (scene.StaticColliderEntityIds.contains(object.Id) == false)
        {
            continue;
        }
        const BorrowedView<const math::Vector3> positionsMesh = GetRenderSceneMeshPositions(scene, object.Mesh);
        const BorrowedView<const uint32> indices = GetRenderSceneMeshIndices(scene, object.Mesh);
        for (usize index = 0; index < indices.GetSize(); index += TRIANGLE_CORNER_COUNT)
        {
            const uint32 first = indices[index];
            const uint32 second = indices[index + 1];
            const uint32 third = indices[index + 2];
            if (first >= positionsMesh.GetSize() || second >= positionsMesh.GetSize() ||
                third >= positionsMesh.GetSize())
            {
                return false;
            }
            const Triangle triangle
            {
                .FirstWorld = math::TransformPoint(object.Transform, positionsMesh[first]),
                .SecondWorld = math::TransformPoint(object.Transform, positionsMesh[second]),
                .ThirdWorld = math::TransformPoint(object.Transform, positionsMesh[third]),
            };
            if (math::IsFinite(triangle.FirstWorld) == false || math::IsFinite(triangle.SecondWorld) == false ||
                math::IsFinite(triangle.ThirdWorld) == false)
            {
                return false;
            }
            if (math::LengthSquared(math::Cross(Widen(triangle.SecondWorld) - Widen(triangle.FirstWorld),
                                                Widen(triangle.ThirdWorld) - Widen(triangle.FirstWorld))) > 0 &&
                mTriangles.TryAdd(triangle) == false)
            {
                return false;
            }
        }
    }
    return mTriangles.IsEmpty() == true || buildNode(0, static_cast<uint32>(mTriangles.GetSize())) == true;
}

bool ViewportWalkCollision::buildNode(uint32 first, uint32 count) noexcept
{
    const uint32 node = static_cast<uint32>(mNodes.GetSize());
    math::Aabb3 boundsWorld = math::EmptyAabb3();
    for (uint32 index = first; index < first + count; ++index)
    {
        const Triangle& triangle = mTriangles[index];
        IncludePoint(boundsWorld, triangle.FirstWorld);
        IncludePoint(boundsWorld, triangle.SecondWorld);
        IncludePoint(boundsWorld, triangle.ThirdWorld);
    }
    if (mNodes.TryAdd({ .BoundsWorld = boundsWorld, .First = first, .Count = count }) == false)
    {
        return false;
    }
    if (count <= TRIANGLES_PER_LEAF)
    {
        return true;
    }
    const math::Vector3d spanWorld = Widen(boundsWorld.Max) - Widen(boundsWorld.Min);
    constexpr uint32 Z_AXIS = 2;
    const uint32 horizontalAxis = spanWorld.X >= spanWorld.Z ? 0 : Z_AXIS;
    const uint32 axis = spanWorld.Y > math::Max(spanWorld.X, spanWorld.Z) ? 1 : horizontalAxis;
    const float64 pivot = 0.5 * (GetAxis(boundsWorld.Min, axis) + GetAxis(boundsWorld.Max, axis));
    uint32 middle = first;
    for (uint32 index = first; index < first + count; ++index)
    {
        const Triangle& triangle = mTriangles[index];
        constexpr float64 CENTROID_WEIGHT = 1.0 / TRIANGLE_CORNER_COUNT;
        const float64 centroid = (GetAxis(triangle.FirstWorld, axis) + GetAxis(triangle.SecondWorld, axis) +
                                  GetAxis(triangle.ThirdWorld, axis)) *
                                 CENTROID_WEIGHT;
        if (centroid < pivot)
        {
            const Triangle saved = triangle;
            mTriangles[index] = mTriangles[middle];
            mTriangles[middle] = saved;
            ++middle;
        }
    }
    // Split by count on a poor partition to bound build and traversal depth.
    constexpr uint32 MIN_PARTITION_DIVISOR = 4;
    if (middle - first < count / MIN_PARTITION_DIVISOR || first + count - middle < count / MIN_PARTITION_DIVISOR)
    {
        middle = first + count / 2;
    }
    if (buildNode(first, middle - first) == false)
    {
        return false;
    }
    mNodes[node].Count = 0;
    mNodes[node].Right = static_cast<uint32>(mNodes.GetSize());
    return buildNode(middle, first + count - middle);
}

ViewportWalkCollision::Hit ViewportWalkCollision::cast(math::Vector3d centerWorld,
                                                       math::Vector3d movementWorld) const noexcept
{
    Hit hit;
    castNode(0, centerWorld, movementWorld, hit);
    return hit;
}

void ViewportWalkCollision::castNode(uint32 node,
                                     math::Vector3d centerWorld,
                                     math::Vector3d movementWorld,
                                     Hit& inoutHit) const noexcept
{
    const Node& bounds = mNodes[node];
    const float64 fraction = math::Min(1.0, inoutHit.Fraction + CONTACT_FRACTION_TOLERANCE);
    if (inoutHit.IsPenetrating == true ||
        OverlapsSweep(bounds.BoundsWorld, centerWorld, centerWorld + movementWorld * fraction) == false)
    {
        return;
    }
    if (bounds.Count == 0)
    {
        castNode(node + 1, centerWorld, movementWorld, inoutHit);
        castNode(bounds.Right, centerWorld, movementWorld, inoutHit);
        return;
    }
    for (uint32 index = bounds.First; index < bounds.First + bounds.Count; ++index)
    {
        castTriangle(mTriangles[index], centerWorld, movementWorld, inoutHit);
        if (inoutHit.IsPenetrating == true)
        {
            return;
        }
    }
}

void ViewportWalkCollision::castTriangle(const Triangle& triangle,
                                         math::Vector3d centerWorld,
                                         math::Vector3d movementWorld,
                                         Hit& inoutHit) noexcept
{
    const math::Vector3d firstWorld = Widen(triangle.FirstWorld) - centerWorld;
    const math::Vector3d secondWorld = Widen(triangle.SecondWorld) - centerWorld;
    const math::Vector3d thirdWorld = Widen(triangle.ThirdWorld) - centerWorld;
    const StaticArray<math::Vector3d, TRIANGLE_CORNER_COUNT> edgesWorld{secondWorld - firstWorld,
                                                                        thirdWorld - secondWorld,
                                                                        firstWorld - thirdWorld};
    const math::Vector3d faceWorld = math::Cross(edgesWorld[0], edgesWorld[1]);
    SweepInterval interval;
    if (ClipAxis(faceWorld, firstWorld, secondWorld, thirdWorld, movementWorld, interval) == false)
    {
        return;
    }
    for (const math::Vector3d axisWorld : WORLD_AXES)
    {
        if (ClipAxis(axisWorld, firstWorld, secondWorld, thirdWorld, movementWorld, interval) == false)
        {
            return;
        }
        for (const math::Vector3d edgeWorld : edgesWorld)
        {
            if (ClipAxis(math::Cross(axisWorld, edgeWorld),
                         firstWorld,
                         secondWorld,
                         thirdWorld,
                         movementWorld,
                         interval) == false)
            {
                return;
            }
        }
    }
    const bool isDownward = movementWorld.X == 0 && movementWorld.Z == 0 && movementWorld.Y < 0;
    const bool isSharedContact = inoutHit.HasContact == true && isDownward == true &&
                                 math::Abs(interval.Enter - inoutHit.Fraction) <= CONTACT_FRACTION_TOLERANCE;
    if (isSharedContact == false && interval.Enter >= inoutHit.Fraction)
    {
        return;
    }
    Hit candidate;
    candidate.HasContact = true;
    candidate.IsPenetrating = interval.Enter < 0;
    candidate.Fraction = math::Max(0.0, interval.Enter);
    math::Vector3d surfaceWorld;
    if (candidate.IsPenetrating == false && TryNormalize(interval.NormalWorld, candidate.NormalWorld) == true &&
        TryNormalize(faceWorld, surfaceWorld) == true)
    {
        candidate.IsWalkable =
            candidate.NormalWorld.Y >= MIN_WALKABLE_NORMAL_Y && math::Abs(surfaceWorld.Y) >= MIN_WALKABLE_NORMAL_Y;
    }
    if (isSharedContact == true && candidate.IsPenetrating == false)
    {
        // A stair riser and its tread share the landing time. Keep the earliest
        // obstruction, but prefer the tread's support normal over the riser edge.
        candidate.Fraction = math::Min(candidate.Fraction, inoutHit.Fraction);
        if (inoutHit.IsWalkable == true)
        {
            candidate.IsWalkable = true;
            candidate.NormalWorld = inoutHit.NormalWorld;
        }
    }
    inoutHit = candidate;
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
    if (mCenterWorld.Y < static_cast<float64>(mNodes[0].BoundsWorld.Min.Y) - FALL_LIMIT_BELOW_WORLD_METRES)
    {
        mCenterWorld = mSpawnCenterWorld;
        mVerticalSpeed = 0;
        mIsGrounded = false;
    }
}

math::Vector3d
ViewportWalkCollision::Advance(math::Vector3d velocityWorld, float64 deltaSeconds, bool shouldJump) noexcept
{
    if (mNodes.IsEmpty() == false && math::IsFinite(deltaSeconds) == true && deltaSeconds > 0 &&
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
