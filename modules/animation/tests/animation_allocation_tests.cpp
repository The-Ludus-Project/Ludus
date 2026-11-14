// Allocation injection for every Animation preparation path, and proof that pose
// edits, forward kinematics, skinning palettes and inspection never allocate.
// This executable replaces the global allocation functions, which FoundationMemory's
// container domain reaches through the aligned nothrow operator new; sanitizer
// builds use the ordinary suite because sanitizer runtimes replace the same functions.

#include "skeleton_fixtures.hpp"

#include <ludus/animation/inspection.hpp>
#include <ludus/animation/pose.hpp>
#include <ludus/animation/skeleton.hpp>
#include <ludus/animation/skin_binding.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{
ludus::foundation::int64 gAllocationCount = 0;
bool isTrackingAllocations = false;
ludus::foundation::int64 gFailAfter = -1;

void* AllocateAligned(std::size_t bytes, std::size_t alignment) noexcept
{
    if (isTrackingAllocations == true)
    {
        ++gAllocationCount;
    }
    if (gFailAfter == 0)
    {
        return nullptr;
    }
    if (gFailAfter > 0)
    {
        --gFailAfter;
    }
    if (alignment < alignof(std::max_align_t))
    {
        alignment = alignof(std::max_align_t);
    }
    std::size_t rounded = (bytes + alignment - 1) & ~(alignment - 1);
    if (rounded == 0)
    {
        rounded = alignment;
    }
    return std::aligned_alloc(alignment, rounded);
}

void FreeAligned(void* pointerOrNull) noexcept
{
    std::free(pointerOrNull);
}
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t bytes)
{
    void* pointerOrNull = AllocateAligned(bytes, alignof(std::max_align_t));
    if (pointerOrNull == nullptr)
    {
        throw std::bad_alloc{};
    }
    return pointerOrNull;
}
void* operator new(std::size_t bytes, std::align_val_t alignment)
{
    void* pointerOrNull = AllocateAligned(bytes, static_cast<std::size_t>(alignment));
    if (pointerOrNull == nullptr)
    {
        throw std::bad_alloc{};
    }
    return pointerOrNull;
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    return AllocateAligned(bytes, alignof(std::max_align_t));
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return AllocateAligned(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointerOrNull) noexcept
{
    FreeAligned(pointerOrNull);
}
void operator delete(void* pointerOrNull, std::size_t) noexcept
{
    FreeAligned(pointerOrNull);
}
void operator delete(void* pointerOrNull, std::align_val_t) noexcept
{
    FreeAligned(pointerOrNull);
}
void operator delete(void* pointerOrNull, std::size_t, std::align_val_t) noexcept
{
    FreeAligned(pointerOrNull);
}
void operator delete(void* pointerOrNull, const std::nothrow_t&) noexcept
{
    FreeAligned(pointerOrNull);
}
void operator delete(void* pointerOrNull, std::align_val_t, const std::nothrow_t&) noexcept
{
    FreeAligned(pointerOrNull);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

using namespace ludus::animation;
using namespace ludus::animation::test;
namespace foundation = ludus::foundation;

namespace
{
/// Upper bound on allocations attempted by any one preparation; the loops stop at success.
constexpr ludus::foundation::int64 MAX_INJECTED_FAILURES = 16;

template <typename Prepare, typename RequirePreserved>
void RequireEveryAllocationFailureIsRecoverable(const Prepare& prepare, const RequirePreserved& requirePreserved)
{
    bool hasSucceeded = false;
    for (ludus::foundation::int64 failedAllocation = 0;
         failedAllocation < MAX_INJECTED_FAILURES && hasSucceeded == false;
         ++failedAllocation)
    {
        gFailAfter = failedAllocation;
        const AnimationStatus status = prepare();
        gFailAfter = -1;
        if (status == AnimationStatus::Success)
        {
            hasSucceeded = true;
            REQUIRE(failedAllocation > 0);
        }
        else
        {
            REQUIRE(status == AnimationStatus::OutOfMemory);
            requirePreserved();
        }
    }
    REQUIRE(hasSucceeded == true);
}

ludus::foundation::int64 CountAllocations(const auto& operation)
{
    gAllocationCount = 0;
    isTrackingAllocations = true;
    operation();
    isTrackingAllocations = false;
    return gAllocationCount;
}
} // namespace

TEST_CASE("Every skeleton preparation allocation failure preserves the published generation", "[animation][alloc]")
{
    const ChainJoints chain = MakeChainJoints();
    const BranchJoints branch = MakeBranchJoints();
    Skeleton skeleton = PrepareSkeleton(chain, CHAIN_AUTHORING_ID);
    const ludus::foundation::Fingerprint128 compatibility = skeleton.GetCompatibility();
    RequireEveryAllocationFailureIsRecoverable(
        [&]() { return TryPrepareSkeleton(MakePackage(branch, BRANCH_AUTHORING_ID), {}, skeleton); },
        [&]() {
            REQUIRE(skeleton.GetCompatibility() == compatibility);
            REQUIRE(skeleton.GetJointCount() == CHAIN_JOINT_COUNT);
        });
    REQUIRE(skeleton.GetJointCount() == BRANCH_JOINT_COUNT);
}

TEST_CASE("Every pose and binding preparation allocation failure preserves its output", "[animation][alloc]")
{
    const Skeleton chain = PrepareSkeleton(MakeChainJoints(), CHAIN_AUTHORING_ID);
    const Skeleton branch = PrepareSkeleton(MakeBranchJoints(), BRANCH_AUTHORING_ID);

    LocalPose local;
    REQUIRE(TryPrepareLocalPose(chain, local) == AnimationStatus::Success);
    RequireEveryAllocationFailureIsRecoverable(
        [&]() { return TryPrepareLocalPose(branch, local); },
        [&]() { REQUIRE(local.GetCompatibility() == chain.GetCompatibility()); });
    REQUIRE(local.GetCompatibility() == branch.GetCompatibility());

    ComponentPose component;
    REQUIRE(TryPrepareComponentPose(chain, component) == AnimationStatus::Success);
    RequireEveryAllocationFailureIsRecoverable(
        [&]() { return TryPrepareComponentPose(branch, component); },
        [&]() { REQUIRE(component.GetCompatibility() == chain.GetCompatibility()); });
    REQUIRE(component.GetCompatibility() == branch.GetCompatibility());

    const SkinBindingJointRecord chainEntry{ .JointId = 100, .InverseBindComponent = {} };
    const SkinBindingJointRecord branchEntry{ .JointId = 7, .InverseBindComponent = {} };
    SkinBinding binding;
    REQUIRE(TryPrepareSkinBinding(chain,
                                  { .SkeletonCompatibility = chain.GetCompatibility(), .Palette = {&chainEntry, 1} },
                                  {},
                                  binding) == AnimationStatus::Success);
    RequireEveryAllocationFailureIsRecoverable(
        [&]() {
            return TryPrepareSkinBinding(
                branch,
                { .SkeletonCompatibility = branch.GetCompatibility(), .Palette = {&branchEntry, 1} },
                {},
                binding);
        },
        [&]() { REQUIRE(binding.GetCompatibility() == chain.GetCompatibility()); });
    REQUIRE(binding.GetCompatibility() == branch.GetCompatibility());
}

TEST_CASE("Pose edits, evaluation, skinning and inspection never allocate", "[animation][alloc]")
{
    const Skeleton skeleton = PrepareSkeleton(MakeBranchJoints(), BRANCH_AUTHORING_ID);
    LocalPose local;
    ComponentPose component;
    REQUIRE(TryPrepareLocalPose(skeleton, local) == AnimationStatus::Success);
    REQUIRE(TryPrepareComponentPose(skeleton, component) == AnimationStatus::Success);
    const SkinBindingJointRecord entry{ .JointId = 4, .InverseBindComponent = {} };
    SkinBinding binding;
    REQUIRE(TryPrepareSkinBinding(skeleton,
                                  { .SkeletonCompatibility = skeleton.GetCompatibility(), .Palette = {&entry, 1} },
                                  {},
                                  binding) == AnimationStatus::Success);

    AnimationStatus setStatus = AnimationStatus::InvalidArgument;
    AnimationStatus evaluateStatus = AnimationStatus::InvalidArgument;
    AnimationStatus paletteStatus = AnimationStatus::InvalidArgument;
    AnimationStatus inspectStatus = AnimationStatus::InvalidArgument;
    AnimationStatus resetStatus = AnimationStatus::InvalidArgument;
    math::Affine3 palette{};
    JointInspection inspection{};
    const ludus::foundation::int64 allocations = CountAllocations([&]() {
        setStatus = local.TrySetJoint(3, MakeTransform({1, 0, 0}, math::QuaternionIdentity(), {2, 1, 1}));
        evaluateStatus = TryEvaluateComponentPose(skeleton, local, component);
        paletteStatus = TryComputeSkinningPalette(binding, component, {&palette, 1});
        inspectStatus = TryInspectJoint(skeleton, local, &component, 3, inspection);
        resetStatus = local.TryResetToRest(skeleton);
    });
    REQUIRE(allocations == 0);
    REQUIRE(setStatus == AnimationStatus::Success);
    REQUIRE(evaluateStatus == AnimationStatus::Success);
    REQUIRE(paletteStatus == AnimationStatus::Success);
    REQUIRE(inspectStatus == AnimationStatus::Success);
    REQUIRE(resetStatus == AnimationStatus::Success);
}
