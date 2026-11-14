#include "skeleton_fixtures.hpp"

#include <ludus/animation/pose.hpp>
#include <ludus/foundation/math/matrix.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::animation;
using namespace ludus::animation::test;
namespace foundation = ludus::foundation;

namespace
{
constexpr float32 MATRIX_TOLERANCE = 1e-5F;

template <usize Count>
foundation::StaticArray<math::Affine3, Count> ComposeOracle(const Skeleton& skeleton,
                                                            foundation::BorrowedView<const math::TransformTRS> locals)
{
    foundation::StaticArray<math::Affine3, Count> oracle{};
    for (usize index = 0; index < Count; ++index)
    {
        const uint32 parent = skeleton.GetJoints()[index].ParentIndex;
        const math::Affine3 local = math::ToAffine3(locals[index]);
        oracle[index] = parent == NO_PARENT_JOINT ? local : math::Compose(oracle[parent], local);
    }
    return oracle;
}

bool IsNear(float32 left, float32 right)
{
    const float32 scale = math::Abs(left) > 1 ? math::Abs(left) : 1.0F;
    return math::Abs(left - right) <= MATRIX_TOLERANCE * scale;
}

void RequireNear(const math::Matrix4& left, const math::Matrix4& right)
{
    for (usize column = 0; column < 4; ++column)
    {
        REQUIRE(IsNear(left.Columns[column].X, right.Columns[column].X));
        REQUIRE(IsNear(left.Columns[column].Y, right.Columns[column].Y));
        REQUIRE(IsNear(left.Columns[column].Z, right.Columns[column].Z));
        REQUIRE(IsNear(left.Columns[column].W, right.Columns[column].W));
    }
}

struct PosedChain final
{
    Skeleton ChainSkeleton;
    LocalPose Local;
    ComponentPose Component;
};

PosedChain MakePosedChain()
{
    PosedChain posed
    {
        .ChainSkeleton = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID),
        .Local = {},
        .Component = {},
    };
    REQUIRE(TryPrepareLocalPose(posed.ChainSkeleton, posed.Local) == AnimationStatus::Success);
    REQUIRE(TryPrepareComponentPose(posed.ChainSkeleton, posed.Component) == AnimationStatus::Success);
    return posed;
}
} // namespace

TEST_CASE("Prepared poses start at the rest pose and are not yet evaluated", "[animation][pose]")
{
    PosedChain posed = MakePosedChain();
    REQUIRE(posed.Local.GetJointCount() == CHAIN_JOINT_COUNT);
    REQUIRE(posed.Local.GetRevision() == 1);
    REQUIRE(posed.Local.GetCompatibility() == posed.ChainSkeleton.GetCompatibility());
    for (usize index = 0; index < CHAIN_JOINT_COUNT; ++index)
    {
        const math::TransformTRS& rest = posed.ChainSkeleton.GetJoints()[index].RestLocal;
        REQUIRE(posed.Local.GetJoints()[index].Translation == rest.Translation);
        REQUIRE(posed.Local.GetJoints()[index].Rotation == rest.Rotation);
        REQUIRE(posed.Local.GetJoints()[index].Scale == rest.Scale);
    }
    REQUIRE(posed.Component.IsPrepared() == true);
    REQUIRE(posed.Component.IsEvaluated() == false);
    REQUIRE(posed.Component.GetJointCount() == CHAIN_JOINT_COUNT);
    REQUIRE(posed.Component.GetJoints().IsEmpty() == true);
    REQUIRE(posed.Component.GetSourceRevision() == 0);

    Skeleton unprepared;
    LocalPose local;
    ComponentPose component;
    REQUIRE(TryPrepareLocalPose(unprepared, local) == AnimationStatus::InvalidArgument);
    REQUIRE(TryPrepareComponentPose(unprepared, component) == AnimationStatus::InvalidArgument);
    REQUIRE(local.IsPrepared() == false);
    REQUIRE(component.IsPrepared() == false);
}

TEST_CASE("Forward kinematics matches FoundationMath affine composition with nonuniform scale", "[animation][pose]")
{
    PosedChain posed = MakePosedChain();
    REQUIRE(TryEvaluateComponentPose(posed.ChainSkeleton, posed.Local, posed.Component) == AnimationStatus::Success);
    REQUIRE(posed.Component.IsEvaluated() == true);
    REQUIRE(posed.Component.GetSourceRevision() == posed.Local.GetRevision());

    const foundation::BorrowedView<const math::Affine3> components = posed.Component.GetJoints();
    const foundation::StaticArray<math::Affine3, CHAIN_JOINT_COUNT> oracle =
        ComposeOracle<CHAIN_JOINT_COUNT>(posed.ChainSkeleton, posed.Local.GetJoints());
    for (usize index = 0; index < CHAIN_JOINT_COUNT; ++index)
    {
        // Same operations in the same order: bitwise equality, not a tolerance.
        REQUIRE(AreBitwiseEqual(components[index], oracle[index]) == true);
    }

    // Independent check through homogeneous matrix products.
    math::Matrix4 product = math::Matrix4::Identity();
    for (usize index = 0; index < CHAIN_JOINT_COUNT; ++index)
    {
        product = product * math::ToMatrix4(math::ToAffine3(posed.Local.GetJoints()[index]));
        RequireNear(math::ToMatrix4(components[index]), product);
    }

    // The parent's nonuniform scale shears the rotated child's basis: it is not orthogonal,
    // so no TRS value could reproduce it.
    const math::Affine3& sheared = components[2];
    REQUIRE(math::Abs(math::Dot(sheared.Columns[0], sheared.Columns[1])) > 1e-2F);

    // A point in the hand's space reaches component space through each parent in turn.
    const math::Vector3 handPoint{0.25F, -0.5F, 1};
    math::Vector3 nested = handPoint;
    for (usize index = CHAIN_JOINT_COUNT; index > 0; --index)
    {
        nested = math::TransformPoint(math::ToAffine3(posed.Local.GetJoints()[index - 1]), nested);
    }
    const math::Vector3 composed = math::TransformPoint(components[CHAIN_JOINT_COUNT - 1], handPoint);
    REQUIRE(IsNear(composed.X, nested.X));
    REQUIRE(IsNear(composed.Y, nested.Y));
    REQUIRE(IsNear(composed.Z, nested.Z));
}

TEST_CASE("Forward kinematics composes branches through their own parents", "[animation][pose]")
{
    const Skeleton skeleton = PrepareSkeleton(MakeBranchJoints(), BRANCH_AUTHORING_ID);
    LocalPose local;
    ComponentPose component;
    REQUIRE(TryPrepareLocalPose(skeleton, local) == AnimationStatus::Success);
    REQUIRE(TryPrepareComponentPose(skeleton, component) == AnimationStatus::Success);
    REQUIRE(local.TrySetJoint(1, MakeTransform({0, 1, 0}, MakeRotation({0, 1, 0}, 0.4F), {1.5F, 1, 1})) ==
            AnimationStatus::Success);
    REQUIRE(TryEvaluateComponentPose(skeleton, local, component) == AnimationStatus::Success);
    const foundation::StaticArray<math::Affine3, BRANCH_JOINT_COUNT> oracle =
        ComposeOracle<BRANCH_JOINT_COUNT>(skeleton, local.GetJoints());
    for (usize index = 0; index < BRANCH_JOINT_COUNT; ++index)
    {
        REQUIRE(AreBitwiseEqual(component.GetJoints()[index], oracle[index]) == true);
    }
    // The right hand composes onto the right arm, not onto the preceding left hand.
    REQUIRE(AreBitwiseEqual(component.GetJoints()[5],
                            math::Compose(component.GetJoints()[4], math::ToAffine3(local.GetJoints()[5]))) == true);
}

TEST_CASE("Local pose edits are validated and revisioned", "[animation][pose]")
{
    PosedChain posed = MakePosedChain();
    const math::TransformTRS edited = MakeTransform({1, 2, 3}, MakeRotation({1, 0, 0}, 0.25F), {1, 2, 3});
    REQUIRE(posed.Local.TrySetJoint(2, edited) == AnimationStatus::Success);
    REQUIRE(posed.Local.GetRevision() == 2);
    REQUIRE(posed.Local.GetJoints()[2].Translation == edited.Translation);

    const math::TransformTRS before = posed.Local.GetJoints()[1];
    math::TransformTRS invalid = edited;
    invalid.Scale = {1, 0, 1};
    REQUIRE(posed.Local.TrySetJoint(1, invalid) == AnimationStatus::UnsupportedScale);
    invalid = edited;
    invalid.Rotation = {0, 0, 0, 3};
    REQUIRE(posed.Local.TrySetJoint(1, invalid) == AnimationStatus::InvalidTransform);
    invalid = edited;
    invalid.Translation.Z = std::numeric_limits<float32>::quiet_NaN();
    REQUIRE(posed.Local.TrySetJoint(1, invalid) == AnimationStatus::InvalidTransform);
    REQUIRE(posed.Local.TrySetJoint(CHAIN_JOINT_COUNT, edited) == AnimationStatus::InvalidArgument);
    REQUIRE(posed.Local.GetRevision() == 2);
    REQUIRE(posed.Local.GetJoints()[1].Translation == before.Translation);
    REQUIRE(posed.Local.GetJoints()[1].Rotation == before.Rotation);
    REQUIRE(posed.Local.GetJoints()[1].Scale == before.Scale);

    // A component pose evaluated before an edit reports the older source revision.
    REQUIRE(TryEvaluateComponentPose(posed.ChainSkeleton, posed.Local, posed.Component) == AnimationStatus::Success);
    REQUIRE(posed.Local.TrySetJoint(3, edited) == AnimationStatus::Success);
    REQUIRE(posed.Component.GetSourceRevision() == 2);
    REQUIRE(posed.Local.GetRevision() == 3);

    REQUIRE(posed.Local.TryResetToRest(posed.ChainSkeleton) == AnimationStatus::Success);
    REQUIRE(posed.Local.GetRevision() == 4);
    REQUIRE(posed.Local.GetJoints()[2].Translation == posed.ChainSkeleton.GetJoints()[2].RestLocal.Translation);
}

TEST_CASE("Failed evaluation preserves the published component pose", "[animation][pose]")
{
    PosedChain posed = MakePosedChain();

    SECTION("before any evaluation")
    {
        REQUIRE(posed.Local.TrySetJoint(0, MakeTransform({}, math::QuaternionIdentity(), {1e4F, 1e4F, 1e4F})) ==
                AnimationStatus::Success);
        REQUIRE(posed.Local.TrySetJoint(1, MakeTransform({3e38F, 0, 0}, math::QuaternionIdentity(), {1, 1, 1})) ==
                AnimationStatus::Success);
        REQUIRE(TryEvaluateComponentPose(posed.ChainSkeleton, posed.Local, posed.Component) ==
                AnimationStatus::OutOfRange);
        REQUIRE(posed.Component.IsEvaluated() == false);
        REQUIRE(posed.Component.GetJoints().IsEmpty() == true);
    }
    SECTION("after a published evaluation")
    {
        REQUIRE(TryEvaluateComponentPose(posed.ChainSkeleton, posed.Local, posed.Component) ==
                AnimationStatus::Success);
        const uint64 sourceRevision = posed.Component.GetSourceRevision();
        foundation::StaticArray<math::Affine3, CHAIN_JOINT_COUNT> published{};
        for (usize index = 0; index < CHAIN_JOINT_COUNT; ++index)
        {
            published[index] = posed.Component.GetJoints()[index];
        }
        const auto requirePublishedUnchanged = [&]() {
            REQUIRE(posed.Component.IsEvaluated() == true);
            REQUIRE(posed.Component.GetSourceRevision() == sourceRevision);
            for (usize index = 0; index < CHAIN_JOINT_COUNT; ++index)
            {
                REQUIRE(AreBitwiseEqual(posed.Component.GetJoints()[index], published[index]) == true);
            }
        };

        // Composition overflows partway down the chain.
        REQUIRE(posed.Local.TrySetJoint(0, MakeTransform({}, math::QuaternionIdentity(), {1e4F, 1e4F, 1e4F})) ==
                AnimationStatus::Success);
        REQUIRE(posed.Local.TrySetJoint(2, MakeTransform({3e38F, 0, 0}, math::QuaternionIdentity(), {1, 1, 1})) ==
                AnimationStatus::Success);
        REQUIRE(TryEvaluateComponentPose(posed.ChainSkeleton, posed.Local, posed.Component) ==
                AnimationStatus::OutOfRange);
        requirePublishedUnchanged();

        // A same-count skeleton with a different hierarchy is incompatible.
        ChainJoints siblings = MakeChainJoints();
        siblings[2].SubtreeEnd = 3;
        siblings[3].ParentIndex = 1;
        const Skeleton other = PrepareSkeleton(siblings, CHAIN_AUTHORING_ID);
        REQUIRE(other.GetJointCount() == posed.ChainSkeleton.GetJointCount());
        REQUIRE(TryEvaluateComponentPose(other, posed.Local, posed.Component) == AnimationStatus::IncompatibleSkeleton);
        REQUIRE(posed.Local.TryResetToRest(other) == AnimationStatus::IncompatibleSkeleton);
        requirePublishedUnchanged();

        // Unprepared inputs.
        const Skeleton unprepared;
        const LocalPose unpreparedLocal;
        REQUIRE(TryEvaluateComponentPose(unprepared, posed.Local, posed.Component) == AnimationStatus::InvalidArgument);
        REQUIRE(TryEvaluateComponentPose(posed.ChainSkeleton, unpreparedLocal, posed.Component) ==
                AnimationStatus::InvalidArgument);
        requirePublishedUnchanged();

        // A successful evaluation after repair replaces the published pose.
        REQUIRE(posed.Local.TryResetToRest(posed.ChainSkeleton) == AnimationStatus::Success);
        REQUIRE(TryEvaluateComponentPose(posed.ChainSkeleton, posed.Local, posed.Component) ==
                AnimationStatus::Success);
        REQUIRE(posed.Component.GetSourceRevision() == posed.Local.GetRevision());
        for (usize index = 0; index < CHAIN_JOINT_COUNT; ++index)
        {
            REQUIRE(AreBitwiseEqual(posed.Component.GetJoints()[index], published[index]) == true);
        }
    }
}
