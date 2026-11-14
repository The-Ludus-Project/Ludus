#include "skeleton_fixtures.hpp"

#include <ludus/animation/inspection.hpp>
#include <ludus/animation/pose.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::animation;
using namespace ludus::animation::test;
namespace foundation = ludus::foundation;

TEST_CASE("The skeleton inspector summarizes a validated generation", "[animation][inspection]")
{
    const Skeleton skeleton = PrepareSkeleton(MakeBranchJoints(), BRANCH_AUTHORING_ID);
    SkeletonInspection inspection{};
    REQUIRE(TryInspectSkeleton(skeleton, inspection) == AnimationStatus::Success);
    REQUIRE(inspection.Identity == skeleton.GetIdentity());
    REQUIRE(inspection.Compatibility == skeleton.GetCompatibility());
    REQUIRE(inspection.JointCount == BRANCH_JOINT_COUNT);
    REQUIRE(inspection.LeafCount == 3);
    REQUIRE(inspection.MaxDepth == 3);
    REQUIRE(inspection.MetersPerUnit == 1);
    REQUIRE(inspection.PreparedBytes == skeleton.GetPreparedBytes());

    const Skeleton unprepared;
    SkeletonInspection preserved{ .JointCount = 42 };
    REQUIRE(TryInspectSkeleton(unprepared, preserved) == AnimationStatus::InvalidArgument);
    REQUIRE(preserved.JointCount == 42);
}

TEST_CASE("The joint inspector reports hierarchy, rest, local and component values", "[animation][inspection]")
{
    const Skeleton skeleton = PrepareSkeleton(MakeBranchJoints(), BRANCH_AUTHORING_ID);
    LocalPose local;
    ComponentPose component;
    REQUIRE(TryPrepareLocalPose(skeleton, local) == AnimationStatus::Success);
    REQUIRE(TryPrepareComponentPose(skeleton, component) == AnimationStatus::Success);

    constexpr uint32 RIGHT_ARM = 4;
    JointInspection inspection{};
    REQUIRE(TryInspectJoint(skeleton, local, &component, RIGHT_ARM, inspection) == AnimationStatus::Success);
    REQUIRE(inspection.JointIndex == RIGHT_ARM);
    REQUIRE(inspection.JointId == 5);
    REQUIRE(inspection.ParentIndex == 1);
    REQUIRE(inspection.ParentJointId == 2);
    REQUIRE(inspection.SubtreeEnd == 6);
    REQUIRE(inspection.Depth == 2);
    REQUIRE(inspection.ChildCount == 1);
    REQUIRE(inspection.HasComponent == false);
    REQUIRE(inspection.IsComponentCurrent == false);

    REQUIRE(TryInspectJoint(skeleton, local, nullptr, 0, inspection) == AnimationStatus::Success);
    REQUIRE(inspection.ParentIndex == NO_PARENT_JOINT);
    REQUIRE(inspection.ParentJointId == 0);
    REQUIRE(inspection.Depth == 0);
    REQUIRE(inspection.ChildCount == 1);
    constexpr uint32 SPINE = 1;
    REQUIRE(TryInspectJoint(skeleton, local, nullptr, SPINE, inspection) == AnimationStatus::Success);
    REQUIRE(inspection.ChildCount == 3);

    // Edit the arm, evaluate, then edit again: the inspector shows rest and current values
    // and reports that the component pose is stale.
    math::TransformTRS raised = local.GetJoints()[RIGHT_ARM];
    raised.Rotation = math::QuaternionIdentity();
    REQUIRE(local.TrySetJoint(RIGHT_ARM, raised) == AnimationStatus::Success);
    REQUIRE(TryEvaluateComponentPose(skeleton, local, component) == AnimationStatus::Success);
    REQUIRE(TryInspectJoint(skeleton, local, &component, RIGHT_ARM, inspection) == AnimationStatus::Success);
    REQUIRE(inspection.HasComponent == true);
    REQUIRE(inspection.IsComponentCurrent == true);
    REQUIRE(inspection.CurrentLocal.Rotation == math::QuaternionIdentity());
    REQUIRE((inspection.RestLocal.Rotation == inspection.CurrentLocal.Rotation) == false);
    REQUIRE(inspection.Component == component.GetJoints()[RIGHT_ARM]);
    REQUIRE(inspection.ComponentSourceRevision == local.GetRevision());

    const uint64 inspectedRevision = local.GetRevision();
    REQUIRE(local.TryResetToRest(skeleton) == AnimationStatus::Success);
    REQUIRE(TryInspectJoint(skeleton, local, &component, RIGHT_ARM, inspection) == AnimationStatus::Success);
    REQUIRE(inspection.IsComponentCurrent == false);
    REQUIRE(inspection.ComponentSourceRevision == inspectedRevision);
    // Inspection reads only: it neither evaluates nor edits.
    REQUIRE(component.GetSourceRevision() == inspectedRevision);
    REQUIRE(local.GetRevision() == inspectedRevision + 1);
}

TEST_CASE("The joint inspector rejects invalid requests and preserves its output", "[animation][inspection]")
{
    const Skeleton skeleton = PrepareSkeleton(MakeBranchJoints(), BRANCH_AUTHORING_ID);
    const Skeleton chain = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID);
    LocalPose local;
    LocalPose chainLocal;
    ComponentPose chainComponent;
    REQUIRE(TryPrepareLocalPose(skeleton, local) == AnimationStatus::Success);
    REQUIRE(TryPrepareLocalPose(chain, chainLocal) == AnimationStatus::Success);
    REQUIRE(TryPrepareComponentPose(chain, chainComponent) == AnimationStatus::Success);

    JointInspection inspection{ .JointId = 1234 };
    REQUIRE(TryInspectJoint(skeleton, local, nullptr, BRANCH_JOINT_COUNT, inspection) ==
            AnimationStatus::InvalidArgument);
    REQUIRE(TryInspectJoint(skeleton, chainLocal, nullptr, 0, inspection) == AnimationStatus::IncompatibleSkeleton);
    REQUIRE(TryInspectJoint(skeleton, local, &chainComponent, 0, inspection) == AnimationStatus::IncompatibleSkeleton);
    const LocalPose unprepared;
    REQUIRE(TryInspectJoint(skeleton, unprepared, nullptr, 0, inspection) == AnimationStatus::InvalidArgument);
    REQUIRE(inspection.JointId == 1234);
}
