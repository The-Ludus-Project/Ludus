#include "skeleton_fixtures.hpp"

#include <ludus/animation/skeleton.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::animation;
using namespace ludus::animation::test;
namespace foundation = ludus::foundation;

namespace
{
AnimationStatus PrepareChain(const ChainJoints& joints, Skeleton& outSkeleton)
{
    return TryPrepareSkeleton(MakePackage(joints, CHAIN_AUTHORING_ID), {}, outSkeleton);
}

AnimationStatus PrepareBranch(const BranchJoints& joints, Skeleton& outSkeleton)
{
    return TryPrepareSkeleton(MakePackage(joints, BRANCH_AUTHORING_ID), {}, outSkeleton);
}

AnimationStatus PrepareChainWithScale(math::Vector3 scale)
{
    ChainJoints joints = MakeChainJoints();
    joints[2].RestLocal.Scale = scale;
    Skeleton skeleton;
    return PrepareChain(joints, skeleton);
}
} // namespace

TEST_CASE("A valid skeleton package publishes a normalized immutable generation", "[animation][skeleton]")
{
    BranchJoints joints = MakeBranchJoints();
    // Slightly nonunit rotation within tolerance: normalized in the owned copy only.
    joints[2].RestLocal.Rotation.W *= 1.00001F;
    const math::Quaternion authored = joints[2].RestLocal.Rotation;
    Skeleton skeleton;
    REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::Success);

    REQUIRE(skeleton.IsPrepared() == true);
    REQUIRE(skeleton.GetJointCount() == BRANCH_JOINT_COUNT);
    REQUIRE(skeleton.GetIdentity() == SkeletonIdentity{ .AuthoringId = BRANCH_AUTHORING_ID, .Revision = 1 });
    REQUIRE(skeleton.GetMetersPerUnit() == 1);
    REQUIRE(skeleton.GetMaxDepth() == 3);
    REQUIRE(skeleton.GetPreparedBytes() > BRANCH_JOINT_COUNT * sizeof(SkeletonJointRecord));
    REQUIRE((skeleton.GetCompatibility() == ludus::foundation::Fingerprint128{}) == false);
    REQUIRE(joints[2].RestLocal.Rotation == authored);
    const math::Quaternion stored = skeleton.GetJoints()[2].RestLocal.Rotation;
    REQUIRE(math::Abs(math::Dot(stored, stored) - 1.0F) < 1e-6F);
    for (usize index = 0; index < BRANCH_JOINT_COUNT; ++index)
    {
        uint32 found = NO_PARENT_JOINT;
        REQUIRE(skeleton.TryFindJointIndex(joints[index].JointId, found) == AnimationStatus::Success);
        REQUIRE(found == index);
        REQUIRE(skeleton.GetJoints()[index].SubtreeEnd == joints[index].SubtreeEnd);
    }
    uint32 missing = 77;
    REQUIRE(skeleton.TryFindJointIndex(999, missing) == AnimationStatus::InvalidJointId);
    REQUIRE(missing == 77);
}

TEST_CASE("A single-joint skeleton is a valid root", "[animation][skeleton]")
{
    const ludus::foundation::StaticArray<SkeletonJointRecord, 1> joints{{
        { .JointId = 9, .ParentIndex = NO_PARENT_JOINT, .SubtreeEnd = 1, .RestLocal = {} },
    }};
    Skeleton skeleton;
    REQUIRE(TryPrepareSkeleton(MakePackage(joints, CHAIN_AUTHORING_ID), {}, skeleton) == AnimationStatus::Success);
    REQUIRE(skeleton.GetMaxDepth() == 0);
}

TEST_CASE("Invalid hierarchies are rejected", "[animation][skeleton]")
{
    SECTION("the first joint must be the root")
    {
        BranchJoints joints = MakeBranchJoints();
        joints[0].ParentIndex = 0;
        Skeleton skeleton;
        REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::InvalidHierarchy);
    }
    SECTION("a second root requires an explicit synthetic root")
    {
        BranchJoints joints = MakeBranchJoints();
        joints[6].ParentIndex = NO_PARENT_JOINT;
        Skeleton skeleton;
        REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::InvalidHierarchy);
    }
    SECTION("a parent must precede its child")
    {
        BranchJoints joints = MakeBranchJoints();
        joints[3].ParentIndex = 5;
        Skeleton skeleton;
        REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::InvalidHierarchy);
        joints[3].ParentIndex = 3;
        REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::InvalidHierarchy);
    }
    SECTION("a subtree must be contiguous in depth-first order")
    {
        // The right hand (5) names the left hand (3) as parent after the right arm opened.
        BranchJoints joints = MakeBranchJoints();
        joints[5].ParentIndex = 3;
        Skeleton skeleton;
        REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::InvalidHierarchy);
    }
    SECTION("cooked subtree ends must match the hierarchy")
    {
        BranchJoints joints = MakeBranchJoints();
        joints[2].SubtreeEnd = 5;
        Skeleton skeleton;
        REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::InvalidHierarchy);
        joints = MakeBranchJoints();
        joints[0].SubtreeEnd = BRANCH_JOINT_COUNT + 1;
        REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::InvalidHierarchy);
        joints = MakeBranchJoints();
        joints[3].SubtreeEnd = 3;
        REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::InvalidHierarchy);
    }
}

TEST_CASE("Zero and duplicate joint identities are rejected", "[animation][skeleton]")
{
    BranchJoints joints = MakeBranchJoints();
    joints[4].JointId = 0;
    Skeleton skeleton;
    REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::InvalidJointId);
    joints = MakeBranchJoints();
    joints[6].JointId = joints[2].JointId;
    REQUIRE(PrepareBranch(joints, skeleton) == AnimationStatus::InvalidJointId);
    REQUIRE(skeleton.IsPrepared() == false);
}

TEST_CASE("Unsupported scale values are rejected", "[animation][skeleton]")
{
    REQUIRE(PrepareChainWithScale({1, 0, 1}) == AnimationStatus::UnsupportedScale);
    REQUIRE(PrepareChainWithScale({1, -1, 1}) == AnimationStatus::UnsupportedScale);
    REQUIRE(PrepareChainWithScale({-1, -1, -1}) == AnimationStatus::UnsupportedScale);
    REQUIRE(PrepareChainWithScale({1, 1, MIN_JOINT_SCALE * 0.5F}) == AnimationStatus::UnsupportedScale);
    REQUIRE(PrepareChainWithScale({MAX_JOINT_SCALE * 2, 1, 1}) == AnimationStatus::UnsupportedScale);
    REQUIRE(PrepareChainWithScale({std::numeric_limits<float32>::quiet_NaN(), 1, 1}) ==
            AnimationStatus::InvalidTransform);
    // Positive nonuniform scale at the domain bounds is supported.
    REQUIRE(PrepareChainWithScale({MIN_JOINT_SCALE, MAX_JOINT_SCALE, 1}) == AnimationStatus::Success);
}

TEST_CASE("Nonfinite or nonunit rest transforms are rejected", "[animation][skeleton]")
{
    ChainJoints joints = MakeChainJoints();
    joints[1].RestLocal.Translation.Y = std::numeric_limits<float32>::infinity();
    Skeleton skeleton;
    REQUIRE(PrepareChain(joints, skeleton) == AnimationStatus::InvalidTransform);
    joints = MakeChainJoints();
    joints[1].RestLocal.Rotation = {0, 0, 0, 0};
    REQUIRE(PrepareChain(joints, skeleton) == AnimationStatus::InvalidTransform);
    joints = MakeChainJoints();
    joints[1].RestLocal.Rotation = {0, 0, 0, 2};
    REQUIRE(PrepareChain(joints, skeleton) == AnimationStatus::InvalidTransform);
}

TEST_CASE("Invalid package arguments and budgets are rejected", "[animation][skeleton]")
{
    const ChainJoints joints = MakeChainJoints();
    Skeleton skeleton;
    SkeletonPackage package = MakePackage(joints, CHAIN_AUTHORING_ID);
    package.Joints = {};
    REQUIRE(TryPrepareSkeleton(package, {}, skeleton) == AnimationStatus::InvalidArgument);
    package.Joints = {nullptr, CHAIN_JOINT_COUNT};
    REQUIRE(TryPrepareSkeleton(package, {}, skeleton) == AnimationStatus::InvalidArgument);
    package = MakePackage(joints, 0);
    REQUIRE(TryPrepareSkeleton(package, {}, skeleton) == AnimationStatus::InvalidArgument);
    package = MakePackage(joints, CHAIN_AUTHORING_ID);
    package.MetersPerUnit = 0;
    REQUIRE(TryPrepareSkeleton(package, {}, skeleton) == AnimationStatus::InvalidArgument);
    package.MetersPerUnit = std::numeric_limits<float32>::quiet_NaN();
    REQUIRE(TryPrepareSkeleton(package, {}, skeleton) == AnimationStatus::InvalidArgument);
    package = MakePackage(joints, CHAIN_AUTHORING_ID);
    REQUIRE(TryPrepareSkeleton(package, { .MaxJoints = 0 }, skeleton) == AnimationStatus::InvalidArgument);
    REQUIRE(TryPrepareSkeleton(package, { .MaxJoints = CHAIN_JOINT_COUNT - 1 }, skeleton) ==
            AnimationStatus::BudgetExceeded);
    REQUIRE(TryPrepareSkeleton(package, { .MaxJoints = CHAIN_JOINT_COUNT }, skeleton) == AnimationStatus::Success);
}

TEST_CASE("Failed skeleton validation preserves the published generation", "[animation][skeleton]")
{
    const ChainJoints joints = MakeChainJoints();
    Skeleton skeleton;
    REQUIRE(PrepareChain(joints, skeleton) == AnimationStatus::Success);
    const ludus::foundation::Fingerprint128 compatibility = skeleton.GetCompatibility();
    const SkeletonJointRecord* data = skeleton.GetJoints().GetDataOrNull();

    BranchJoints invalid = MakeBranchJoints();
    invalid[3].RestLocal.Scale = {1, 0, 1};
    REQUIRE(PrepareBranch(invalid, skeleton) == AnimationStatus::UnsupportedScale);
    invalid = MakeBranchJoints();
    invalid[6].JointId = invalid[0].JointId;
    REQUIRE(PrepareBranch(invalid, skeleton) == AnimationStatus::InvalidJointId);
    invalid = MakeBranchJoints();
    invalid[2].SubtreeEnd = 6;
    REQUIRE(PrepareBranch(invalid, skeleton) == AnimationStatus::InvalidHierarchy);

    REQUIRE(skeleton.GetCompatibility() == compatibility);
    REQUIRE(skeleton.GetJointCount() == CHAIN_JOINT_COUNT);
    REQUIRE(skeleton.GetIdentity().AuthoringId == CHAIN_AUTHORING_ID);
    REQUIRE(skeleton.GetJoints().GetDataOrNull() == data);
}

TEST_CASE("Compatibility covers structure and rest values, not the authoring identity", "[animation][skeleton]")
{
    const ChainJoints joints = MakeChainJoints();
    Skeleton reference;
    REQUIRE(PrepareChain(joints, reference) == AnimationStatus::Success);

    SECTION("a recook with a new authoring identity and revision stays compatible")
    {
        SkeletonPackage package = MakePackage(joints, CHAIN_AUTHORING_ID + 1);
        package.Identity.Revision = 2;
        Skeleton skeleton;
        REQUIRE(TryPrepareSkeleton(package, {}, skeleton) == AnimationStatus::Success);
        REQUIRE(skeleton.GetCompatibility() == reference.GetCompatibility());
        REQUIRE((skeleton.GetIdentity() == reference.GetIdentity()) == false);
    }
    SECTION("negative zero hashes as positive zero")
    {
        ChainJoints signedZero = MakeChainJoints();
        signedZero[3].RestLocal.Translation.X = -0.0F;
        Skeleton skeleton;
        REQUIRE(PrepareChain(signedZero, skeleton) == AnimationStatus::Success);
        REQUIRE(skeleton.GetCompatibility() == reference.GetCompatibility());
    }
    SECTION("rest values, identities, units and hierarchy change compatibility")
    {
        ChainJoints changed = MakeChainJoints();
        changed[2].RestLocal.Translation.X += 0.125F;
        Skeleton skeleton;
        REQUIRE(PrepareChain(changed, skeleton) == AnimationStatus::Success);
        REQUIRE((skeleton.GetCompatibility() == reference.GetCompatibility()) == false);

        changed = MakeChainJoints();
        changed[3].JointId = 401;
        REQUIRE(PrepareChain(changed, skeleton) == AnimationStatus::Success);
        REQUIRE((skeleton.GetCompatibility() == reference.GetCompatibility()) == false);

        SkeletonPackage package = MakePackage(joints, CHAIN_AUTHORING_ID);
        package.MetersPerUnit = 0.01F;
        REQUIRE(TryPrepareSkeleton(package, {}, skeleton) == AnimationStatus::Success);
        REQUIRE((skeleton.GetCompatibility() == reference.GetCompatibility()) == false);

        // Same joint count and identities, different hierarchy: the last two joints become siblings.
        changed = MakeChainJoints();
        changed[2].SubtreeEnd = 3;
        changed[3].ParentIndex = 1;
        REQUIRE(PrepareChain(changed, skeleton) == AnimationStatus::Success);
        REQUIRE(skeleton.GetJointCount() == reference.GetJointCount());
        REQUIRE((skeleton.GetCompatibility() == reference.GetCompatibility()) == false);
    }
}

TEST_CASE("Moving a skeleton transfers the generation", "[animation][skeleton]")
{
    Skeleton skeleton = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID);
    const ludus::foundation::Fingerprint128 compatibility = skeleton.GetCompatibility();
    Skeleton moved(ludus::foundation::Move(skeleton));
    REQUIRE(moved.GetCompatibility() == compatibility);
    REQUIRE(skeleton.IsPrepared() == false);         // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    REQUIRE(skeleton.GetJointCount() == 0);          // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    REQUIRE(skeleton.GetJoints().IsEmpty() == true); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    uint32 index = 0;
    REQUIRE(skeleton.TryFindJointIndex(100, index) == // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            AnimationStatus::InvalidArgument);
}
