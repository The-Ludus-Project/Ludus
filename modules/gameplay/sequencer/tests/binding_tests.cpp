#include <ludus/gameplay/sequencer/binding.hpp>

#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::gameplay::sequencer;
using ludus::foundation::uint64;
using ludus::foundation::usize;
namespace foundation = ludus::foundation;
namespace identity = foundation::identity;
namespace math = foundation::math;
namespace camera = ludus::gameplay::camera;

namespace
{
constexpr uint64 CAMERA_A = 1;
constexpr uint64 CAMERA_B = 2;
constexpr uint64 CRATE = 3;
constexpr uint64 GUARD = 4;
constexpr uint64 CAMERA_POSITION_TRACK = 10;
constexpr uint64 CRATE_TRANSLATION_TRACK = 11;
constexpr uint64 GUARD_VISIBILITY_TRACK = 12;
constexpr uint64 SECOND_TRANSLATION_TRACK = 13;
constexpr foundation::uint32 TICKS_PER_SECOND = 24;
constexpr uint64 PLAYBACK_END = 96;
constexpr uint64 CUT_TICK = 48;
constexpr uint64 SCENE_REVISION = 7;
constexpr usize CATALOG_COUNT = 3;
constexpr usize CRATE_TARGET = 1;
constexpr usize GUARD_TARGET = 2;
constexpr foundation::uint8 DIGEST_BYTE = 0xA5;
constexpr foundation::float32 CRATE_BASE_Y = 2.0F;
constexpr foundation::float32 GUARD_BASE_X = -3.0F;

identity::Uuid MakeUuid(foundation::uint8 seed) noexcept
{
    identity::Uuid value{};
    for (usize index = 0; index < identity::UUID_BYTE_COUNT; ++index)
    {
        value.Bytes[index] = static_cast<foundation::uint8>(seed + index);
    }
    value.Bytes[identity::UUID_VERSION_BYTE_INDEX] = 0x40; // Version 4 in the high nibble.
    value.Bytes[identity::UUID_VARIANT_BYTE_INDEX] = identity::UUID_STANDARD_VARIANT_BITS;
    return value;
}

SequenceWorldDigest MakeDigest(foundation::uint8 value) noexcept
{
    SequenceWorldDigest digest{};
    for (usize index = 0; index < digest.GetSize(); ++index)
    {
        digest[index] = value;
    }
    return digest;
}

camera::CameraSample MakeCamera(foundation::float64 z) noexcept
{
    camera::CameraSample sample;
    sample.Pose.Position = {0, 1, z};
    sample.Lens.Projection = camera::CameraProjection::PerspectiveFinite;
    sample.Lens.NearPlane = 0.1F;
    sample.Lens.FarPlane = 100.0F;
    return sample;
}

/// A complete fixture: two cameras with a cut, one UUID object and one legacy object.
struct Fixture final
{
    identity::AssetId World{MakeUuid(1)};
    identity::SourceObjectId CrateObject{MakeUuid(2)};
    foundation::StaticArray<SequencePositionKey, 2> CameraPositions{{
        {0, {0, 1, 10}},
        {PLAYBACK_END, {0, 1, 2}},
    }};
    foundation::StaticArray<SequenceVectorKey, 2> CrateTranslations{{
        {0, {0, 0, 0}},
        {PLAYBACK_END, {4, 0, 0}},
    }};
    foundation::StaticArray<SequenceVisibilityKey, 2> GuardVisibility{{
        {0, true},
        {CUT_TICK, false},
    }};
    foundation::StaticArray<SequenceBinding, 4> Bindings{};
    foundation::StaticArray<SequenceTrack, 3> Tracks{};
    foundation::StaticArray<SequenceCameraInterval, 2> Selection{{
        {0, CUT_TICK, CAMERA_A},
        {CUT_TICK, PLAYBACK_END, CAMERA_B},
    }};
    foundation::StaticArray<SequenceSceneTarget, CATALOG_COUNT> Targets{};

    Fixture() noexcept
    {
        Bindings[0] = { .BindingId = CAMERA_A, .Kind = SequenceBindingKind::SequenceCamera, .Camera = MakeCamera(10) };
        Bindings[1] = { .BindingId = CAMERA_B, .Kind = SequenceBindingKind::SequenceCamera, .Camera = MakeCamera(5) };
        Bindings[2] = { .BindingId = CRATE, .Kind = SequenceBindingKind::WorldObject, .Object = CrateObject };
        Bindings[3] = { .BindingId = GUARD, .Kind = SequenceBindingKind::LegacyWorldObject, .LegacyKey = "guard-a" };
        Tracks[0] =
        {
            .TrackId = CAMERA_POSITION_TRACK,
            .BindingId = CAMERA_A,
            .Channel = SequenceChannel::CameraPosition,
            .PositionKeys = {CameraPositions.GetData(), CameraPositions.GetSize()},
        };
        Tracks[1] =
        {
            .TrackId = CRATE_TRANSLATION_TRACK,
            .BindingId = CRATE,
            .Channel = SequenceChannel::Translation,
            .VectorKeys = {CrateTranslations.GetData(), CrateTranslations.GetSize()},
        };
        Tracks[2] =
        {
            .TrackId = GUARD_VISIBILITY_TRACK,
            .BindingId = GUARD,
            .Channel = SequenceChannel::Visibility,
            .VisibilityKeys = {GuardVisibility.GetData(), GuardVisibility.GetSize()},
        };
        // Target 0 is an unrelated root; the crate is parented under it.
        Targets[0] = { .Object = identity::SourceObjectId{MakeUuid(3)}, .CanWriteLocalTransform = true };
        Targets[CRATE_TARGET] =
        {
            .Object = CrateObject,
            .CanWriteLocalTransform = true,
            .CanWriteVisibility = true,
            .BaseTransformLocal = { .Translation = {0, CRATE_BASE_Y, 0} },
            .ParentIndex = 0,
        };
        Targets[GUARD_TARGET] =
        {
            .LegacyKey = "guard-a",
            .CanWriteLocalTransform = true,
            .CanWriteVisibility = true,
            .BaseTransformLocal = { .Translation = {GUARD_BASE_X, 0, 0} },
        };
    }

    [[nodiscard]] SequenceDefinition GetDefinition() const noexcept
    {
        return
        {
            .World = { .Document = World, .ContentDigest = MakeDigest(DIGEST_BYTE), .HasContentDigest = true },
            .TicksPerSecond = TICKS_PER_SECOND,
            .PlaybackStartTick = 0,
            .PlaybackEndTick = PLAYBACK_END,
            .Bindings = {Bindings.GetData(), Bindings.GetSize()},
            .Tracks = {Tracks.GetData(), Tracks.GetSize()},
            .CameraSelection = {Selection.GetData(), Selection.GetSize()},
        };
    }

    [[nodiscard]] SequenceSceneCatalog GetCatalog() const noexcept
    {
        return
        {
            .WorldDocument = World,
            .ContentDigest = MakeDigest(DIGEST_BYTE),
            .HasContentDigest = true,
            .SceneRevision = SCENE_REVISION,
            .Targets = {Targets.GetData(), Targets.GetSize()},
        };
    }
};

SequenceStatus Prepare(const Fixture& fixture, PreparedSequencePlan& outPlan, SequenceDiagnostic& outDiagnostic)
{
    return TryPrepareSequencePlan(fixture.GetDefinition(), fixture.GetCatalog(), {}, outPlan, outDiagnostic);
}
} // namespace

TEST_CASE("A definition resolves UUID, legacy and camera bindings into one plan", "[sequencer][binding]")
{
    const Fixture fixture;
    SequenceDiagnostic diagnostic;
    REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) == SequenceStatus::Success);

    PreparedSequencePlan plan;
    REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::Success);
    REQUIRE(diagnostic.Status == SequenceStatus::Success);
    REQUIRE(plan.IsPrepared() == true);
    REQUIRE(plan.GetSceneRevision() == SCENE_REVISION);
    REQUIRE(plan.GetTicksPerSecond() == TICKS_PER_SECOND);
    REQUIRE(plan.GetSequence().GetObjectCount() == 2);
    REQUIRE(plan.GetSequence().GetCameraCount() == 2);

    const foundation::BorrowedView<const SequenceResolvedBinding> resolved = plan.GetResolvedBindings();
    REQUIRE(resolved.GetSize() == 4);
    REQUIRE(resolved[0].BindingId == CAMERA_A);
    REQUIRE(resolved[0].TargetIndex == SEQUENCE_NO_INDEX);
    REQUIRE(resolved[2].BindingId == CRATE);
    REQUIRE(resolved[2].TargetIndex == CRATE_TARGET);
    REQUIRE(resolved[3].BindingId == GUARD);
    REQUIRE(resolved[3].TargetIndex == GUARD_TARGET);

    // Unanimated channels come from the scene's frozen base values.
    SequenceObjectSample crate;
    REQUIRE(TryEvaluateSequenceObject(plan.GetSequence(), CRATE, {CUT_TICK, 0, 1}, crate) == SequenceStatus::Success);
    REQUIRE(crate.TransformLocal.Translation == math::Vector3{2, 0, 0});
    SequenceObjectSample guard;
    REQUIRE(TryEvaluateSequenceObject(plan.GetSequence(), GUARD, {CUT_TICK, 0, 1}, guard) == SequenceStatus::Success);
    REQUIRE(guard.TransformLocal.Translation == math::Vector3{GUARD_BASE_X, 0, 0});
    REQUIRE(guard.IsVisible == false);
}

TEST_CASE("Camera selection cuts exactly on an interval boundary", "[sequencer][binding]")
{
    const Fixture fixture;
    PreparedSequencePlan plan;
    SequenceDiagnostic diagnostic;
    REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::Success);

    SequenceCameraSelection selection;
    REQUIRE(TryEvaluateSequenceCameraSelection(plan, {0, 0, 1}, selection) == SequenceStatus::Success);
    REQUIRE(selection.CameraBindingId == CAMERA_A);
    REQUIRE(selection.IntervalIndex == 0);
    REQUIRE(TryEvaluateSequenceCameraSelection(plan, {CUT_TICK - 1, 999, 1000}, selection) == SequenceStatus::Success);
    REQUIRE(selection.CameraBindingId == CAMERA_A);
    REQUIRE(TryEvaluateSequenceCameraSelection(plan, {CUT_TICK, 0, 1}, selection) == SequenceStatus::Success);
    REQUIRE(selection.CameraBindingId == CAMERA_B);
    REQUIRE(selection.IntervalIndex == 1);

    // The exclusive end is outside playback; a failure preserves the output.
    const SequenceCameraSelection before = selection;
    REQUIRE(TryEvaluateSequenceCameraSelection(plan, {PLAYBACK_END, 0, 1}, selection) == SequenceStatus::OutOfRange);
    REQUIRE(TryEvaluateSequenceCameraSelection(plan, {1, 1, 1}, selection) == SequenceStatus::InvalidArgument);
    REQUIRE(selection.CameraBindingId == before.CameraBindingId);
    REQUIRE(selection.IntervalIndex == before.IntervalIndex);

    const PreparedSequencePlan unprepared;
    REQUIRE(TryEvaluateSequenceCameraSelection(unprepared, {0, 0, 1}, selection) == SequenceStatus::Unprepared);
}

TEST_CASE("Two enabled writers of one channel are rejected with both tracks", "[sequencer][binding]")
{
    Fixture fixture;
    foundation::StaticArray<SequenceTrack, 4> tracks{};
    for (usize index = 0; index < fixture.Tracks.GetSize(); ++index)
    {
        tracks[index] = fixture.Tracks[index];
    }
    tracks[3] = fixture.Tracks[1];
    tracks[3].TrackId = SECOND_TRANSLATION_TRACK;
    SequenceDefinition definition = fixture.GetDefinition();
    definition.Tracks = {tracks.GetData(), tracks.GetSize()};

    SequenceDiagnostic diagnostic;
    REQUIRE(TryValidateSequenceDefinition(definition, {}, diagnostic) == SequenceStatus::ConflictingWriter);
    REQUIRE(diagnostic.BindingId == CRATE);
    REQUIRE(diagnostic.TrackId == CRATE_TRANSLATION_TRACK);
    REQUIRE(diagnostic.OtherTrackId == SECOND_TRANSLATION_TRACK);

    // Reordering the rows reports the same pair.
    const SequenceTrack first = tracks[1];
    tracks[1] = tracks[3];
    tracks[3] = first;
    REQUIRE(TryValidateSequenceDefinition(definition, {}, diagnostic) == SequenceStatus::ConflictingWriter);
    REQUIRE(diagnostic.TrackId == CRATE_TRANSLATION_TRACK);
    REQUIRE(diagnostic.OtherTrackId == SECOND_TRANSLATION_TRACK);

    // A disabled writer is kept but does not conflict.
    tracks[3].IsEnabled = false;
    REQUIRE(TryValidateSequenceDefinition(definition, {}, diagnostic) == SequenceStatus::Success);
}

TEST_CASE("Track and binding shape errors name the offending identity", "[sequencer][binding]")
{
    SECTION("duplicate binding identity")
    {
        Fixture fixture;
        fixture.Bindings[1].BindingId = CAMERA_A;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::DuplicateBinding);
        REQUIRE(diagnostic.BindingId == CAMERA_A);
    }
    SECTION("duplicate track identity")
    {
        Fixture fixture;
        fixture.Tracks[2].TrackId = CAMERA_POSITION_TRACK;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::DuplicateBinding);
        REQUIRE(diagnostic.TrackId == CAMERA_POSITION_TRACK);
    }
    SECTION("track names a missing binding")
    {
        Fixture fixture;
        fixture.Tracks[1].BindingId = 99;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::MissingBinding);
        REQUIRE(diagnostic.TrackId == CRATE_TRANSLATION_TRACK);
    }
    SECTION("camera channel on an object binding")
    {
        Fixture fixture;
        fixture.Tracks[0].BindingId = CRATE;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::UnsupportedChannel);
        REQUIRE(diagnostic.TrackId == CAMERA_POSITION_TRACK);
    }
    SECTION("keys in a view that does not match the channel")
    {
        Fixture fixture;
        fixture.Tracks[1].VisibilityKeys = {fixture.GuardVisibility.GetData(), 1};
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::InvalidArgument);
        REQUIRE(diagnostic.TrackId == CRATE_TRANSLATION_TRACK);
    }
    SECTION("a track without keys")
    {
        Fixture fixture;
        fixture.Tracks[1].VectorKeys = {};
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) == SequenceStatus::InvalidKeys);
        REQUIRE(diagnostic.TrackId == CRATE_TRANSLATION_TRACK);
    }
    SECTION("duplicate key times")
    {
        Fixture fixture;
        fixture.CrateTranslations[1].Tick = 0;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) == SequenceStatus::InvalidKeys);
        REQUIRE(diagnostic.TrackId == CRATE_TRANSLATION_TRACK);
        REQUIRE(diagnostic.BindingId == CRATE);
    }
    SECTION("unknown channel kind")
    {
        Fixture fixture;
        fixture.Tracks[1].Channel = static_cast<SequenceChannel>(99);
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::InvalidArgument);
    }
    SECTION("World object binding without a World document")
    {
        Fixture fixture;
        SequenceDefinition definition = fixture.GetDefinition();
        definition.World.Document = {};
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(definition, {}, diagnostic) == SequenceStatus::InvalidArgument);
        REQUIRE(diagnostic.BindingId == CRATE);
    }
    SECTION("legacy binding without a pinned World digest")
    {
        Fixture fixture;
        SequenceDefinition definition = fixture.GetDefinition();
        definition.World.HasContentDigest = false;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(definition, {}, diagnostic) == SequenceStatus::InvalidArgument);
        REQUIRE(diagnostic.BindingId == GUARD);
    }
    SECTION("legacy key longer than the storage bound")
    {
        Fixture fixture;
        fixture.Bindings[3].LegacyKey = "0123456789012345678901234567890123456789012345678901234567890123x";
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::InvalidArgument);
        REQUIRE(diagnostic.BindingId == GUARD);
    }
}

TEST_CASE("Camera selection must cover playback without gaps or overlaps", "[sequencer][binding]")
{
    SECTION("gap between intervals")
    {
        Fixture fixture;
        fixture.Selection[1].StartTick = CUT_TICK + 1;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::InvalidSelection);
        REQUIRE(diagnostic.Index == 1);
    }
    SECTION("overlapping intervals")
    {
        Fixture fixture;
        fixture.Selection[1].StartTick = CUT_TICK - 1;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::InvalidSelection);
    }
    SECTION("selection ends before playback")
    {
        Fixture fixture;
        fixture.Selection[1].EndTick = PLAYBACK_END - 1;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::InvalidSelection);
    }
    SECTION("selection names an object binding")
    {
        Fixture fixture;
        fixture.Selection[0].CameraBindingId = CRATE;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(fixture.GetDefinition(), {}, diagnostic) ==
                SequenceStatus::InvalidSelection);
        REQUIRE(diagnostic.BindingId == CRATE);
        REQUIRE(diagnostic.Index == 0);
    }
    SECTION("cameras without a selection")
    {
        Fixture fixture;
        SequenceDefinition definition = fixture.GetDefinition();
        definition.CameraSelection = {};
        SequenceDiagnostic diagnostic;
        REQUIRE(TryValidateSequenceDefinition(definition, {}, diagnostic) == SequenceStatus::InvalidSelection);
    }
}

TEST_CASE("Resolution reports missing, ambiguous and unsupported targets", "[sequencer][binding]")
{
    SECTION("missing active target")
    {
        Fixture fixture;
        fixture.Targets[CRATE_TARGET].Object = identity::SourceObjectId{MakeUuid(9)};
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::MissingBinding);
        REQUIRE(diagnostic.BindingId == CRATE);
    }
    SECTION("missing target of an inactive binding")
    {
        Fixture fixture;
        fixture.Targets[CRATE_TARGET].Object = identity::SourceObjectId{MakeUuid(9)};
        fixture.Tracks[1].IsEnabled = false;
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::Success);
        REQUIRE(plan.GetResolvedBindings().GetSize() == 3);
    }
    SECTION("ambiguous expansion")
    {
        Fixture fixture;
        fixture.Targets[0].Object = fixture.CrateObject;
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::AmbiguousTarget);
        REQUIRE(diagnostic.BindingId == CRATE);
        REQUIRE(diagnostic.Index == CRATE_TARGET);
    }
    SECTION("a subobject key disambiguates expansion")
    {
        Fixture fixture;
        fixture.Targets[0].Object = fixture.CrateObject;
        fixture.Targets[0].SubobjectKey = 1;
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::Success);
    }
    SECTION("target without writable visibility")
    {
        Fixture fixture;
        fixture.Targets[GUARD_TARGET].CanWriteVisibility = false;
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::UnsupportedChannel);
        REQUIRE(diagnostic.BindingId == GUARD);
        REQUIRE(diagnostic.TrackId == GUARD_VISIBILITY_TRACK);
    }
    SECTION("parent cycle")
    {
        Fixture fixture;
        fixture.Targets[0].ParentIndex = CRATE_TARGET;
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::ParentCycle);
        REQUIRE(diagnostic.BindingId == CRATE);
    }
    SECTION("parent index outside the catalog")
    {
        Fixture fixture;
        fixture.Targets[0].ParentIndex = CATALOG_COUNT;
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::InvalidArgument);
        REQUIRE(diagnostic.Index == 0);
    }
    SECTION("two bindings resolve to one target")
    {
        Fixture fixture;
        fixture.Targets[CRATE_TARGET].LegacyKey = "guard-a";
        fixture.Targets[GUARD_TARGET].LegacyKey = {};
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::DuplicateBinding);
        REQUIRE(diagnostic.BindingId == GUARD);
        REQUIRE(diagnostic.Index == CRATE_TARGET);
    }
    SECTION("nonunit base rotation from the adapter")
    {
        Fixture fixture;
        fixture.Targets[CRATE_TARGET].BaseTransformLocal.Rotation = {0, 0, 0, 2};
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::InvalidArgument);
        REQUIRE(diagnostic.BindingId == CRATE);
    }
}

TEST_CASE("A different World or content revision is rejected", "[sequencer][binding]")
{
    SECTION("World document differs")
    {
        const Fixture fixture;
        SequenceSceneCatalog catalog = fixture.GetCatalog();
        catalog.WorldDocument = identity::AssetId{MakeUuid(8)};
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryPrepareSequencePlan(fixture.GetDefinition(), catalog, {}, plan, diagnostic) ==
                SequenceStatus::WorldMismatch);
    }
    SECTION("pinned digest differs")
    {
        const Fixture fixture;
        SequenceSceneCatalog catalog = fixture.GetCatalog();
        catalog.ContentDigest[0] = 0;
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryPrepareSequencePlan(fixture.GetDefinition(), catalog, {}, plan, diagnostic) ==
                SequenceStatus::WorldMismatch);
    }
    SECTION("scene reports no digest for a pinned sequence")
    {
        const Fixture fixture;
        SequenceSceneCatalog catalog = fixture.GetCatalog();
        catalog.HasContentDigest = false;
        PreparedSequencePlan plan;
        SequenceDiagnostic diagnostic;
        REQUIRE(TryPrepareSequencePlan(fixture.GetDefinition(), catalog, {}, plan, diagnostic) ==
                SequenceStatus::WorldMismatch);
    }
}

TEST_CASE("Failed plan preparation preserves the accepted plan", "[sequencer][binding]")
{
    Fixture fixture;
    PreparedSequencePlan plan;
    SequenceDiagnostic diagnostic;
    REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::Success);
    const PreparedSequence* acceptedSequence = &plan.GetSequence();

    fixture.Targets[GUARD_TARGET].LegacyKey = "renamed";
    REQUIRE(Prepare(fixture, plan, diagnostic) == SequenceStatus::MissingBinding);
    REQUIRE(diagnostic.BindingId == GUARD);
    REQUIRE(plan.IsPrepared() == true);
    REQUIRE(&plan.GetSequence() == acceptedSequence);
    REQUIRE(plan.GetResolvedBindings().GetSize() == 4);

    SequenceDefinitionLimits limits;
    limits.Sampler.MaxKeys = 1;
    fixture.Targets[GUARD_TARGET].LegacyKey = "guard-a";
    REQUIRE(TryPrepareSequencePlan(fixture.GetDefinition(), fixture.GetCatalog(), limits, plan, diagnostic) ==
            SequenceStatus::BudgetExceeded);
    REQUIRE(&plan.GetSequence() == acceptedSequence);

    PreparedSequencePlan moved = foundation::Move(plan);
    REQUIRE(plan.IsPrepared() == false);
    REQUIRE(plan.GetSequence().IsPrepared() == false);
    REQUIRE(moved.IsPrepared() == true);
}

TEST_CASE("Track order does not change evaluation", "[sequencer][binding]")
{
    Fixture fixture;
    PreparedSequencePlan forward;
    SequenceDiagnostic diagnostic;
    REQUIRE(Prepare(fixture, forward, diagnostic) == SequenceStatus::Success);

    const SequenceTrack first = fixture.Tracks[0];
    fixture.Tracks[0] = fixture.Tracks[2];
    fixture.Tracks[2] = first;
    PreparedSequencePlan reversed;
    REQUIRE(Prepare(fixture, reversed, diagnostic) == SequenceStatus::Success);

    for (uint64 tick = 0; tick < PLAYBACK_END; tick += CUT_TICK / 2)
    {
        const SequenceTime time{tick, 1, 3};
        SequenceObjectSample left;
        SequenceObjectSample right;
        REQUIRE(TryEvaluateSequenceObject(forward.GetSequence(), CRATE, time, left) == SequenceStatus::Success);
        REQUIRE(TryEvaluateSequenceObject(reversed.GetSequence(), CRATE, time, right) == SequenceStatus::Success);
        REQUIRE(left.TransformLocal.Translation == right.TransformLocal.Translation);
        camera::CameraSample leftCamera;
        camera::CameraSample rightCamera;
        REQUIRE(TryEvaluateSequenceCamera(forward.GetSequence(), CAMERA_A, time, leftCamera) ==
                SequenceStatus::Success);
        REQUIRE(TryEvaluateSequenceCamera(reversed.GetSequence(), CAMERA_A, time, rightCamera) ==
                SequenceStatus::Success);
        REQUIRE(leftCamera == rightCamera);
    }
}

TEST_CASE("An empty definition is valid content but has no plan", "[sequencer][binding]")
{
    const SequenceDefinition definition{ .TicksPerSecond = TICKS_PER_SECOND, .PlaybackEndTick = PLAYBACK_END };
    SequenceDiagnostic diagnostic;
    REQUIRE(TryValidateSequenceDefinition(definition, {}, diagnostic) == SequenceStatus::Success);
    PreparedSequencePlan plan;
    REQUIRE(TryPrepareSequencePlan(definition, {}, {}, plan, diagnostic) == SequenceStatus::InvalidArgument);
    REQUIRE(plan.IsPrepared() == false);

    SequenceDefinition reversedRange = definition;
    reversedRange.PlaybackStartTick = PLAYBACK_END;
    REQUIRE(TryValidateSequenceDefinition(reversedRange, {}, diagnostic) == SequenceStatus::InvalidArgument);
}
