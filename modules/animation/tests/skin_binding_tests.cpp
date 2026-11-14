#include "skeleton_fixtures.hpp"

#include <ludus/animation/pose.hpp>
#include <ludus/animation/skin_binding.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::animation;
using namespace ludus::animation::test;
namespace foundation = ludus::foundation;

namespace
{
constexpr float32 IDENTITY_TOLERANCE = 1e-5F;
constexpr uint32 LEFT_ARM = 2;
constexpr uint32 LEFT_HAND = 3;
constexpr uint32 RIGHT_ARM = 4;
constexpr uint32 RIGHT_HAND = 5;

bool IsNearIdentity(const math::Affine3& value)
{
    const math::Affine3 identity = math::Affine3::Identity();
    for (usize column = 0; column < 3; ++column)
    {
        const math::Vector3 difference = value.Columns[column] - identity.Columns[column];
        if (math::Abs(difference.X) > IDENTITY_TOLERANCE || math::Abs(difference.Y) > IDENTITY_TOLERANCE ||
            math::Abs(difference.Z) > IDENTITY_TOLERANCE)
        {
            return false;
        }
    }
    return math::Abs(value.Translation.X) <= IDENTITY_TOLERANCE &&
           math::Abs(value.Translation.Y) <= IDENTITY_TOLERANCE && math::Abs(value.Translation.Z) <= IDENTITY_TOLERANCE;
}

/// The mesh was bound in a T pose: both arms horizontal, unlike the skeleton's A-pose rest.
void PoseAtMeshBind(LocalPose& inoutPose)
{
    for (const uint32 arm : {LEFT_ARM, RIGHT_ARM})
    {
        math::TransformTRS value = inoutPose.GetJoints()[arm];
        value.Rotation = math::QuaternionIdentity();
        REQUIRE(inoutPose.TrySetJoint(arm, value) == AnimationStatus::Success);
    }
}

struct BoundCharacter final
{
    Skeleton CharacterSkeleton;
    LocalPose Local;
    ComponentPose Component;
    foundation::StaticArray<SkinBindingJointRecord, BRANCH_JOINT_COUNT> BodyPalette{};
};

BoundCharacter MakeBoundCharacter()
{
    BoundCharacter character
    {
        .CharacterSkeleton = PrepareSkeleton(MakeBranchJoints(), BRANCH_AUTHORING_ID),
        .Local = {},
        .Component = {},
        .BodyPalette = {},
    };
    REQUIRE(TryPrepareLocalPose(character.CharacterSkeleton, character.Local) == AnimationStatus::Success);
    REQUIRE(TryPrepareComponentPose(character.CharacterSkeleton, character.Component) == AnimationStatus::Success);
    // Cook the mesh's inverse bind transforms from its T-pose component transforms.
    PoseAtMeshBind(character.Local);
    REQUIRE(TryEvaluateComponentPose(character.CharacterSkeleton, character.Local, character.Component) ==
            AnimationStatus::Success);
    for (usize index = 0; index < BRANCH_JOINT_COUNT; ++index)
    {
        character.BodyPalette[index].JointId = character.CharacterSkeleton.GetJoints()[index].JointId;
        REQUIRE(math::TryInverse(character.Component.GetJoints()[index],
                                 character.BodyPalette[index].InverseBindComponent) == math::MathStatus::Success);
    }
    REQUIRE(character.Local.TryResetToRest(character.CharacterSkeleton) == AnimationStatus::Success);
    return character;
}

SkinBindingPackage MakeBodyPackage(const BoundCharacter& character)
{
    return
    {
        .SkeletonCompatibility = character.CharacterSkeleton.GetCompatibility(),
        .Palette = {character.BodyPalette.GetData(), BRANCH_JOINT_COUNT},
    };
}
} // namespace

TEST_CASE("The mesh bind pose is distinct from the skeleton rest pose", "[animation][skin]")
{
    BoundCharacter character = MakeBoundCharacter();
    SkinBinding binding;
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, MakeBodyPackage(character), {}, binding) ==
            AnimationStatus::Success);
    REQUIRE(binding.GetPaletteSize() == BRANCH_JOINT_COUNT);
    REQUIRE(binding.GetCompatibility() == character.CharacterSkeleton.GetCompatibility());
    for (usize index = 0; index < BRANCH_JOINT_COUNT; ++index)
    {
        REQUIRE(binding.GetJointIndices()[index] == index);
    }
    foundation::StaticArray<math::Affine3, BRANCH_JOINT_COUNT> palette{};
    const foundation::BorrowedView<math::Affine3> paletteView{palette.GetData(), BRANCH_JOINT_COUNT};

    // At the skeleton rest pose the arms hang down, so their skinning transforms move vertices.
    REQUIRE(TryEvaluateComponentPose(character.CharacterSkeleton, character.Local, character.Component) ==
            AnimationStatus::Success);
    REQUIRE(TryComputeSkinningPalette(binding, character.Component, paletteView) == AnimationStatus::Success);
    for (usize index = 0; index < BRANCH_JOINT_COUNT; ++index)
    {
        const bool isArmChain = index == LEFT_ARM || index == LEFT_HAND || index == RIGHT_ARM || index == RIGHT_HAND;
        REQUIRE(IsNearIdentity(palette[index]) == (isArmChain == false));
    }

    // At the mesh bind pose every skinning transform is identity.
    PoseAtMeshBind(character.Local);
    REQUIRE(TryEvaluateComponentPose(character.CharacterSkeleton, character.Local, character.Component) ==
            AnimationStatus::Success);
    REQUIRE(TryComputeSkinningPalette(binding, character.Component, paletteView) == AnimationStatus::Success);
    for (usize index = 0; index < BRANCH_JOINT_COUNT; ++index)
    {
        REQUIRE(IsNearIdentity(palette[index]) == true);
    }

    // Binding never edits the skeleton's rest pose.
    const Skeleton fresh = PrepareSkeleton(MakeBranchJoints(), BRANCH_AUTHORING_ID);
    REQUIRE(fresh.GetCompatibility() == character.CharacterSkeleton.GetCompatibility());
}

TEST_CASE("Modular meshes share a skeleton without sharing inverse bind data", "[animation][skin]")
{
    BoundCharacter character = MakeBoundCharacter();
    SkinBinding body;
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, MakeBodyPackage(character), {}, body) ==
            AnimationStatus::Success);

    // A helmet authored in centimeters and bound only to the head: its inverse bind includes
    // the mesh-to-skeleton unit conversion.
    constexpr uint32 HEAD = 6;
    constexpr float32 CENTIMETERS_TO_METERS = 0.01F;
    math::Affine3 centimeters = math::Affine3::Identity();
    centimeters.Columns[0] = {CENTIMETERS_TO_METERS, 0, 0};
    centimeters.Columns[1] = {0, CENTIMETERS_TO_METERS, 0};
    centimeters.Columns[2] = {0, 0, CENTIMETERS_TO_METERS};
    const foundation::StaticArray<SkinBindingJointRecord, 1> helmetPalette{{
        {
            .JointId = character.BodyPalette[HEAD].JointId,
            .InverseBindComponent = math::Compose(character.BodyPalette[HEAD].InverseBindComponent, centimeters),
        },
    }};
    SkinBinding helmet;
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton,
                                  {
                                      .SkeletonCompatibility = character.CharacterSkeleton.GetCompatibility(),
                                      .Palette = {helmetPalette.GetData(), 1},
                                  },
                                  {},
                                  helmet) == AnimationStatus::Success);
    REQUIRE(helmet.GetPaletteSize() == 1);
    REQUIRE(helmet.GetJointIndices()[0] == HEAD);

    PoseAtMeshBind(character.Local);
    REQUIRE(TryEvaluateComponentPose(character.CharacterSkeleton, character.Local, character.Component) ==
            AnimationStatus::Success);
    math::Affine3 helmetSkin{};
    REQUIRE(TryComputeSkinningPalette(helmet, character.Component, {&helmetSkin, 1}) == AnimationStatus::Success);
    // At bind time the helmet's skinning transform is just its unit conversion.
    REQUIRE(math::Abs(helmetSkin.Columns[0].X - CENTIMETERS_TO_METERS) < IDENTITY_TOLERANCE);
    REQUIRE(math::Abs(helmetSkin.Translation.Y) < IDENTITY_TOLERANCE);
    REQUIRE((body.GetInverseBindTransforms()[HEAD] == helmet.GetInverseBindTransforms()[0]) == false);
}

TEST_CASE("Invalid skin bindings are rejected and preserve the published binding", "[animation][skin]")
{
    BoundCharacter character = MakeBoundCharacter();
    SkinBinding binding;
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, MakeBodyPackage(character), {}, binding) ==
            AnimationStatus::Success);
    const uint32* publishedIndices = binding.GetJointIndices().GetDataOrNull();

    SkinBindingPackage package = MakeBodyPackage(character);
    package.SkeletonCompatibility.Low ^= 1;
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, package, {}, binding) ==
            AnimationStatus::IncompatibleSkeleton);

    package = MakeBodyPackage(character);
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, package, { .MaxPaletteEntries = 2 }, binding) ==
            AnimationStatus::BudgetExceeded);
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, package, { .MaxPaletteEntries = 0 }, binding) ==
            AnimationStatus::InvalidArgument);
    package.Palette = {};
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, package, {}, binding) ==
            AnimationStatus::InvalidArgument);

    foundation::StaticArray<SkinBindingJointRecord, BRANCH_JOINT_COUNT> palette = character.BodyPalette;
    package =
    {
        .SkeletonCompatibility = character.CharacterSkeleton.GetCompatibility(),
        .Palette = {palette.GetData(), BRANCH_JOINT_COUNT},
    };
    palette[3].JointId = 999;
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, package, {}, binding) ==
            AnimationStatus::InvalidJointId);
    palette = character.BodyPalette;
    palette[3].JointId = palette[1].JointId;
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, package, {}, binding) ==
            AnimationStatus::InvalidJointId);
    palette = character.BodyPalette;
    palette[4].InverseBindComponent.Columns[1] = {0, 0, 0};
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, package, {}, binding) ==
            AnimationStatus::InvalidTransform);
    palette = character.BodyPalette;
    palette[4].InverseBindComponent.Translation.X = std::numeric_limits<float32>::infinity();
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, package, {}, binding) ==
            AnimationStatus::InvalidTransform);

    const Skeleton unprepared;
    REQUIRE(TryPrepareSkinBinding(unprepared, MakeBodyPackage(character), {}, binding) ==
            AnimationStatus::InvalidArgument);

    REQUIRE(binding.GetPaletteSize() == BRANCH_JOINT_COUNT);
    REQUIRE(binding.GetJointIndices().GetDataOrNull() == publishedIndices);
}

TEST_CASE("Failed palette computation preserves the caller's palette", "[animation][skin]")
{
    BoundCharacter character = MakeBoundCharacter();
    SkinBinding binding;
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton, MakeBodyPackage(character), {}, binding) ==
            AnimationStatus::Success);
    foundation::StaticArray<math::Affine3, BRANCH_JOINT_COUNT> palette{};
    const math::Affine3 sentinel{{math::Vector3{2, 0, 0}, math::Vector3{0, 2, 0}, math::Vector3{0, 0, 2}}, {7, 7, 7}};
    for (math::Affine3& entry : palette)
    {
        entry = sentinel;
    }
    const foundation::BorrowedView<math::Affine3> paletteView{palette.GetData(), BRANCH_JOINT_COUNT};
    const auto requireSentinel = [&]() {
        for (const math::Affine3& entry : palette)
        {
            REQUIRE(entry == sentinel);
        }
    };

    // The component pose has not been evaluated since preparation of a fresh pose.
    ComponentPose fresh;
    REQUIRE(TryPrepareComponentPose(character.CharacterSkeleton, fresh) == AnimationStatus::Success);
    REQUIRE(TryComputeSkinningPalette(binding, fresh, paletteView) == AnimationStatus::NotEvaluated);
    requireSentinel();

    REQUIRE(TryComputeSkinningPalette(binding, character.Component, {palette.GetData(), BRANCH_JOINT_COUNT - 1}) ==
            AnimationStatus::InvalidArgument);
    requireSentinel();

    ChainJoints chain = MakeChainJoints();
    const Skeleton otherSkeleton = PrepareSkeleton(chain, CHAIN_AUTHORING_ID);
    ComponentPose otherPose;
    REQUIRE(TryPrepareComponentPose(otherSkeleton, otherPose) == AnimationStatus::Success);
    REQUIRE(TryComputeSkinningPalette(binding, otherPose, paletteView) == AnimationStatus::IncompatibleSkeleton);
    requireSentinel();

    // The last palette entry overflows after the earlier, finite entries could have been
    // written: a finite component pose composed with a large but invertible inverse bind.
    constexpr float32 LARGE_INVERSE_BIND_SCALE = 1e30F;
    foundation::StaticArray<SkinBindingJointRecord, BRANCH_JOINT_COUNT> largePalette = character.BodyPalette;
    math::Affine3& head = largePalette[BRANCH_JOINT_COUNT - 1].InverseBindComponent;
    for (math::Vector3& column : head.Columns)
    {
        column = column * LARGE_INVERSE_BIND_SCALE;
    }
    SkinBinding largeBinding;
    REQUIRE(TryPrepareSkinBinding(character.CharacterSkeleton,
                                  {
                                      .SkeletonCompatibility = character.CharacterSkeleton.GetCompatibility(),
                                      .Palette = {largePalette.GetData(), BRANCH_JOINT_COUNT},
                                  },
                                  {},
                                  largeBinding) == AnimationStatus::Success);
    const math::Vector3 largeScale{MAX_JOINT_SCALE, MAX_JOINT_SCALE, MAX_JOINT_SCALE};
    for (const uint32 joint : {0U, 1U, 6U})
    {
        math::TransformTRS value = character.Local.GetJoints()[joint];
        value.Scale = largeScale;
        REQUIRE(character.Local.TrySetJoint(joint, value) == AnimationStatus::Success);
    }
    REQUIRE(TryEvaluateComponentPose(character.CharacterSkeleton, character.Local, character.Component) ==
            AnimationStatus::Success);
    REQUIRE(TryComputeSkinningPalette(largeBinding, character.Component, paletteView) == AnimationStatus::OutOfRange);
    requireSentinel();
}
