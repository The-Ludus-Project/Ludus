#pragma once

// Shared skeleton fixtures for the Animation tests.

#include <ludus/animation/skeleton.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>

#include <catch2/catch_test_macros.hpp>

namespace ludus::animation::test
{
using foundation::float32;
using foundation::uint32;
using foundation::uint64;
using foundation::usize;
namespace math = foundation::math;

inline constexpr float32 QUARTER_TURN = 0.5F * math::kPiF;
inline constexpr uint64 CHAIN_AUTHORING_ID = 0xA0C4A1;
inline constexpr uint64 BRANCH_AUTHORING_ID = 0xA0B4A2;

inline math::Quaternion MakeRotation(math::Vector3 axis, float32 angleRadians)
{
    math::Quaternion rotation{};
    REQUIRE(math::TryFromAxisAngle(axis, angleRadians, rotation) == math::MathStatus::Success);
    return rotation;
}

inline math::TransformTRS MakeTransform(math::Vector3 translation, math::Quaternion rotation, math::Vector3 scale)
{
    return { .Translation = translation, .Rotation = rotation, .Scale = scale };
}

/// A four-joint chain with nonuniform scale, so the parent scale shears its rotated children.
inline constexpr usize CHAIN_JOINT_COUNT = 4;
using ChainJoints = foundation::StaticArray<SkeletonJointRecord, CHAIN_JOINT_COUNT>;

inline ChainJoints MakeChainJoints()
{
    return ChainJoints{{
        {
            .JointId = 100,
            .ParentIndex = NO_PARENT_JOINT,
            .SubtreeEnd = 4,
            .RestLocal = MakeTransform({0.5F, 1, -0.25F}, MakeRotation({0, 1, 0}, 0.3F), {1, 1, 1}),
        },
        {
            .JointId = 200,
            .ParentIndex = 0,
            .SubtreeEnd = 4,
            .RestLocal = MakeTransform({0, 1, 0}, MakeRotation({0, 0, 1}, 0.5F), {2, 1, 0.5F}),
        },
        {
            .JointId = 300,
            .ParentIndex = 1,
            .SubtreeEnd = 4,
            .RestLocal = MakeTransform({1, 0.5F, 0}, MakeRotation({1, 1, 0}, 0.8F), {1, 3, 0.75F}),
        },
        {
            .JointId = 400,
            .ParentIndex = 2,
            .SubtreeEnd = 4,
            .RestLocal = MakeTransform({0, 0.25F, 2}, MakeRotation({0, 1, 1}, -0.6F), {0.5F, 0.5F, 1.5F}),
        },
    }};
}

/// A branching humanoid-like hierarchy in depth-first order:
/// 0 root; 1 spine; 2 left arm; 3 left hand; 4 right arm; 5 right hand; 6 head.
inline constexpr usize BRANCH_JOINT_COUNT = 7;
using BranchJoints = foundation::StaticArray<SkeletonJointRecord, BRANCH_JOINT_COUNT>;

inline BranchJoints MakeBranchJoints()
{
    const math::Quaternion identity = math::QuaternionIdentity();
    return BranchJoints{{
        { .JointId = 1, .ParentIndex = NO_PARENT_JOINT, .SubtreeEnd = 7, .RestLocal = {} },
        { .JointId = 2, .ParentIndex = 0, .SubtreeEnd = 7, .RestLocal = MakeTransform({0, 1, 0}, identity, {1, 1, 1}) },
        // Rest pose: arms hang down (an "A pose"), rotated a quarter turn about Z from the T pose.
        {
            .JointId = 3,
            .ParentIndex = 1,
            .SubtreeEnd = 4,
            .RestLocal = MakeTransform({0.2F, 0.5F, 0}, MakeRotation({0, 0, 1}, -QUARTER_TURN), {1, 1, 1}),
        },
        {
            .JointId = 4,
            .ParentIndex = 2,
            .SubtreeEnd = 4,
            .RestLocal = MakeTransform({0.6F, 0, 0}, identity, {1, 1, 1}),
        },
        {
            .JointId = 5,
            .ParentIndex = 1,
            .SubtreeEnd = 6,
            .RestLocal = MakeTransform({-0.2F, 0.5F, 0}, MakeRotation({0, 0, 1}, QUARTER_TURN), {1, 1, 1}),
        },
        {
            .JointId = 6,
            .ParentIndex = 4,
            .SubtreeEnd = 6,
            .RestLocal = MakeTransform({-0.6F, 0, 0}, identity, {1, 1, 1}),
        },
        {
            .JointId = 7,
            .ParentIndex = 1,
            .SubtreeEnd = 7,
            .RestLocal = MakeTransform({0, 0.6F, 0}, identity, {1, 1, 1}),
        },
    }};
}

template <usize Count>
SkeletonPackage MakePackage(const foundation::StaticArray<SkeletonJointRecord, Count>& joints, uint64 authoringId)
{
    return
    {
        .Identity = { .AuthoringId = authoringId, .Revision = 1 },
        .MetersPerUnit = 1,
        .Joints = {joints.GetData(), Count},
    };
}

template <usize Count>
Skeleton PrepareSkeleton(const foundation::StaticArray<SkeletonJointRecord, Count>& joints, uint64 authoringId)
{
    Skeleton skeleton;
    REQUIRE(TryPrepareSkeleton(MakePackage(joints, authoringId), {}, skeleton) == AnimationStatus::Success);
    return skeleton;
}

inline bool AreBitwiseEqual(const math::Affine3& left, const math::Affine3& right)
{
    return left == right;
}
} // namespace ludus::animation::test
