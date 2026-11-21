#include <ludus/content/sequence_content/sequence_content.h>

#include <ludus/content/sequence_format/luseq.h>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/gameplay/sequencer/time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace ludus::content::sequence_content;
using ludus::content::Bytes;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;
namespace foundation = ludus::foundation;
namespace format = ludus::content::sequence_format;
namespace sequencer = ludus::gameplay::sequencer;
namespace identity = foundation::identity;
namespace math = foundation::math;

namespace
{
constexpr uint32 TICKS_PER_SECOND = 48000;
constexpr uint32 FRAMES_PER_SECOND = 24;
constexpr uint64 TICKS_PER_FRAME = TICKS_PER_SECOND / FRAMES_PER_SECOND;
constexpr uint64 FRAME_COUNT = 96;
constexpr uint64 CUT_FRAME = 48;
constexpr uint64 CAMERA_A = 1;
constexpr uint64 CAMERA_B = 2;
constexpr uint64 CRATE = 3;
constexpr uint64 GUARD = 4;
constexpr uint64 CRATE_TRANSLATION_TRACK = 11;
constexpr uint64 GUARD_VISIBILITY_TRACK = 14;
constexpr std::string_view WORLD_DOCUMENT = "0b7e8c1d-2f43-4a96-9d51-6c2e4f8a1b3d";
constexpr std::string_view CRATE_OBJECT = "5d1f0e7a-93c2-4b8e-a4f6-1e2d3c4b5a69";

/// Traversal fixture: a 4-second, 24 fps entrance-to-target camera with a cut at
/// two seconds, a translating and rotating crate and a guard hidden at the cut.
/// Values are written loosely on purpose; the canonical writer reformats them.
constexpr std::string_view FIXTURE = R"json({
  "format": "ludus.sequence",
  "version": 1,
  "sequence": "8a2b4c6d-1e3f-4a5b-9c7d-2e4f6a8b0c1d",
  "world": {
    "document": "0b7e8c1d-2f43-4a96-9d51-6c2e4f8a1b3d",
    "digest": "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5",
    "path": "levels/corridor.world.json"
  },
  "ticks_per_second": 48000,
  "playback": { "start": 0, "end": 192000 },
  "bindings": [
    { "id": 1, "label": "Entrance camera", "kind": "sequence-camera",
      "camera": { "position": [0, 1.5, 10], "rotation": [0, 0, 0, 1],
        "lens": { "projection": "perspective-finite", "vertical_fov_radians": 1.0, "vertical_span": 14,
                  "minimum_visible_width": 0, "near": 0.1, "far": 200 },
        "output": 1, "revision": 1 } },
    { "id": 2, "label": "Target camera", "kind": "sequence-camera",
      "camera": { "position": [0, 1.5, -20], "rotation": [0, 1, 0, 0],
        "lens": { "projection": "perspective-finite", "vertical_fov_radians": 0.75, "vertical_span": 14,
                  "minimum_visible_width": 0, "near": 0.1, "far": 200 },
        "output": 1, "revision": 3 } },
    { "id": 3, "label": "Crate", "kind": "world-object", "object": "5d1f0e7a-93c2-4b8e-a4f6-1e2d3c4b5a69",
      "subobject": 0 },
    { "id": 4, "label": "guard-a", "kind": "legacy-world-object", "legacy_id": "guard-a" }
  ],
  "tracks": [
    { "id": 10, "label": "Approach", "binding": 1, "channel": "camera-position", "enabled": true,
      "keys": [ { "tick": 0, "interpolation": "linear", "value": [0, 1.5, 10] },
                { "tick": 192000, "interpolation": "linear", "value": [0, 1.5, -10] } ] },
    { "id": 11, "label": "Slide", "binding": 3, "channel": "translation", "enabled": true,
      "keys": [ { "tick": 0, "interpolation": "linear", "value": [0, 0, 0] },
                { "tick": 192000, "interpolation": "linear", "value": [4, 0, 0] } ] },
    { "id": 12, "label": "Spin", "binding": 3, "channel": "rotation", "enabled": true,
      "keys": [ { "tick": 0, "interpolation": "linear", "value": [0, 0, 0, 1] },
                { "tick": 192000, "interpolation": "linear", "value": [0, 0.7071067811865476, 0, 0.7071067811865476] } ] },
    { "id": 13, "label": "Old slide", "binding": 3, "channel": "translation", "enabled": false,
      "keys": [ { "tick": 0, "interpolation": "step", "value": [0.1, 0, 0] } ] },
    { "id": 14, "label": "Hide guard", "binding": 4, "channel": "visibility", "enabled": true,
      "keys": [ { "tick": 0, "visible": true }, { "tick": 96000, "visible": false } ] },
    { "id": 15, "label": "Zoom", "binding": 2, "channel": "camera-lens", "enabled": true,
      "keys": [ { "tick": 96000, "interpolation": "linear",
                  "value": { "projection": "perspective-finite", "vertical_fov_radians": 0.75, "vertical_span": 14,
                             "minimum_visible_width": 0, "near": 0.1, "far": 200 } },
                { "tick": 192000, "interpolation": "linear",
                  "value": { "projection": "perspective-finite", "vertical_fov_radians": 0.5, "vertical_span": 14,
                             "minimum_visible_width": 0, "near": 0.1, "far": 200 } } ] }
  ],
  "camera_selection": [
    { "start": 0, "end": 96000, "camera": 1 },
    { "start": 96000, "end": 192000, "camera": 2 }
  ]
}
)json";

std::string_view GetText(const Bytes& bytes)
{
    return bytes.String();
}

SequenceDocument ReadFixture()
{
    SequenceDocument document;
    SequenceContentError error;
    const SequenceContentStatus status = TryReadSequenceSource(FIXTURE, document, error);
    INFO(error.Field.View());
    REQUIRE(status == SequenceContentStatus::Success);
    return document;
}

/// One textual substitution applied to the fixture.
struct Substitution final
{
    std::string_view From;
    std::string_view To;
};

std::string Replace(std::string_view text, const Substitution& substitution)
{
    std::string result(text);
    const usize position = result.find(substitution.From);
    REQUIRE(position != std::string::npos);
    result.replace(position, substitution.From.size(), substitution.To);
    return result;
}

SequenceContentStatus ReadText(std::string_view text, SequenceContentError& outError)
{
    SequenceDocument document;
    return TryReadSequenceSource(text, document, outError);
}

identity::Uuid ParseUuid(std::string_view text)
{
    identity::Uuid value{};
    REQUIRE(identity::TryParseUuid(text.data(), text.size(), identity::UuidPolicy::RequireVersion4, value) ==
            identity::UuidStatus::Success);
    return value;
}

Bytes Cook(const SequenceDocument& document)
{
    Bytes bytes;
    SequenceContentError error;
    REQUIRE(TryCookSequence(document, bytes, error) == SequenceContentStatus::Success);
    return bytes;
}

std::string CopyBytes(const Bytes& bytes)
{
    return std::string(bytes.String());
}

SequenceContentStatus DecodeText(const std::string& bytes, SequenceContentError& outError)
{
    SequenceDocument document;
    return TryDecodeSequence({reinterpret_cast<const foundation::uint8*>(bytes.data()), bytes.size()},
                             document,
                             outError);
}

/// One little-endian word written into cooked bytes.
struct WordEdit final
{
    usize Offset = 0;
    uint32 Value = 0;
};

void PutUint32(std::string& inoutBytes, const WordEdit& edit)
{
    const usize offset = edit.Offset;
    const uint32 value = edit.Value;
    foundation::uint8 encoded[sizeof(uint32)]{};
    REQUIRE(foundation::TryWriteLittleEndian(value, encoded) == true);
    for (usize index = 0; index < sizeof(uint32); ++index)
    {
        inoutBytes[offset + index] = static_cast<char>(encoded[index]);
    }
}
} // namespace

TEST_CASE("A source document reads into durable bindings, tracks and selection", "[sequence_content]")
{
    const SequenceDocument document = ReadFixture();
    REQUIRE(document.TicksPerSecond == TICKS_PER_SECOND);
    REQUIRE(document.PlaybackEndTick == FRAME_COUNT * TICKS_PER_FRAME);
    REQUIRE(document.World.Document.Value == ParseUuid(WORLD_DOCUMENT));
    REQUIRE(document.World.HasContentDigest == true);
    REQUIRE(document.WorldPath.View() == "levels/corridor.world.json");
    REQUIRE(document.Bindings.GetSize() == 4);
    REQUIRE(document.Bindings[2].Kind == sequencer::SequenceBindingKind::WorldObject);
    REQUIRE(document.Bindings[2].Object.Value == ParseUuid(CRATE_OBJECT));
    REQUIRE(document.Bindings[3].LegacyKey.View() == "guard-a");
    REQUIRE(document.Bindings[1].Camera.DefinitionRevision == 3);
    REQUIRE(document.Tracks.GetSize() == 6);
    REQUIRE(document.Tracks[3].IsEnabled == false);
    REQUIRE(document.Tracks[1].Label.View() == "Slide");
    REQUIRE(document.VectorKeys.GetSize() == 3);
    REQUIRE(document.VectorKeys[2].Value.X == 0.1F);
    REQUIRE(document.CameraSelection.GetSize() == 2);
}

TEST_CASE("The canonical source writer round-trips byte for byte", "[sequence_content]")
{
    const SequenceDocument document = ReadFixture();
    Bytes first;
    SequenceContentError error;
    REQUIRE(TryWriteSequenceSource(document, first, error) == SequenceContentStatus::Success);
    REQUIRE(GetText(first).starts_with("{\n  \"format\": \"ludus.sequence\",\n  \"version\": 1,\n") == true);
    REQUIRE(GetText(first).ends_with("}\n") == true);

    SequenceDocument reread;
    REQUIRE(TryReadSequenceSource(GetText(first), reread, error) == SequenceContentStatus::Success);
    Bytes second;
    REQUIRE(TryWriteSequenceSource(reread, second, error) == SequenceContentStatus::Success);
    REQUIRE(GetText(first) == GetText(second));
    // Cooked bytes are equal too, so source round-trips lose no cooked content.
    REQUIRE(CopyBytes(Cook(document)) == CopyBytes(Cook(reread)));
}

TEST_CASE("Cooking is deterministic and decoding reproduces the cooked bytes", "[sequence_content]")
{
    const SequenceDocument document = ReadFixture();
    const Bytes cooked = Cook(document);
    REQUIRE(CopyBytes(cooked) == CopyBytes(Cook(document)));
    REQUIRE(cooked.Data().size() > format::HEADER_BYTES);

    SequenceDocument decoded;
    SequenceContentError error;
    REQUIRE(TryDecodeSequence(cooked.Data(), decoded, error) == SequenceContentStatus::Success);
    REQUIRE(CopyBytes(Cook(decoded)) == CopyBytes(cooked));
    // Authoring labels and the World locator are not cooked.
    REQUIRE(decoded.WorldPath.View().empty() == true);
    REQUIRE(decoded.Tracks[1].Label.View().empty() == true);
    REQUIRE(decoded.Tracks[1].TrackId == CRATE_TRANSLATION_TRACK);
    REQUIRE(decoded.Tracks[3].IsEnabled == false);
    REQUIRE(decoded.Bindings[3].LegacyKey.View() == "guard-a");
    REQUIRE(decoded.Bindings[0].Camera == document.Bindings[0].Camera);
    REQUIRE(decoded.World.ContentDigest == document.World.ContentDigest);
}

TEST_CASE("Strict source reading rejects malformed and conflicting content", "[sequence_content]")
{
    SequenceContentError error;
    SECTION("unknown member")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {"\"playback\":", "\"extra\": 1, \"playback\":"}), error) ==
                SequenceContentStatus::Malformed);
        REQUIRE(error.Field.View() == "root.extra");
    }
    SECTION("unknown version")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {"\"version\": 1", "\"version\": 2"}), error) ==
                SequenceContentStatus::UnsupportedVersion);
    }
    SECTION("unknown channel")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {"\"channel\": \"rotation\"", "\"channel\": \"shear\""}), error) ==
                SequenceContentStatus::Malformed);
        REQUIRE(error.Field.View() == "tracks[2].channel");
    }
    SECTION("missing member")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {"\"subobject\": 0", "\"object_extra\": 0"}), error) ==
                SequenceContentStatus::Malformed);
    }
    SECTION("noncanonical UUID text")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {CRATE_OBJECT, "5D1F0E7A-93C2-4B8E-A4F6-1E2D3C4B5A69"}), error) ==
                SequenceContentStatus::InvalidIdentity);
        REQUIRE(error.Field.View() == "bindings[2].object");
    }
    SECTION("invalid legacy key")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {"\"legacy_id\": \"guard-a\"", "\"legacy_id\": \"guard/a\""}), error) ==
                SequenceContentStatus::InvalidLegacyKey);
    }
    SECTION("uppercase digest")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {"\"digest\": \"a5", "\"digest\": \"A5"}), error) ==
                SequenceContentStatus::Malformed);
        REQUIRE(error.Field.View() == "world.digest");
    }
    SECTION("float32 overflow")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {"[4, 0, 0]", "[4e39, 0, 0]"}), error) == SequenceContentStatus::Limit);
    }
    SECTION("duplicate key times")
    {
        REQUIRE(ReadText(Replace(FIXTURE,
                                 {"{ \"tick\": 96000, \"visible\": false }", "{ \"tick\": 0, \"visible\": false }"}),
                         error) == SequenceContentStatus::InvalidSequence);
        REQUIRE(error.Sequence.Status == sequencer::SequenceStatus::InvalidKeys);
        REQUIRE(error.Sequence.TrackId == GUARD_VISIBILITY_TRACK);
    }
    SECTION("conflicting enabled writers")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {"\"enabled\": false", "\"enabled\": true"}), error) ==
                SequenceContentStatus::InvalidSequence);
        REQUIRE(error.Sequence.Status == sequencer::SequenceStatus::ConflictingWriter);
        REQUIRE(error.Sequence.TrackId == CRATE_TRANSLATION_TRACK);
        REQUIRE(error.Sequence.OtherTrackId == 13);
    }
    SECTION("track names a missing binding")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {"\"binding\": 4", "\"binding\": 9"}), error) ==
                SequenceContentStatus::InvalidSequence);
        REQUIRE(error.Sequence.Status == sequencer::SequenceStatus::MissingBinding);
        REQUIRE(error.Sequence.TrackId == GUARD_VISIBILITY_TRACK);
    }
    SECTION("camera selection gap")
    {
        REQUIRE(
            ReadText(Replace(FIXTURE, {"{ \"start\": 96000, \"end\": 192000", "{ \"start\": 96001, \"end\": 192000"}),
                     error) == SequenceContentStatus::InvalidSequence);
        REQUIRE(error.Sequence.Status == sequencer::SequenceStatus::InvalidSelection);
    }
    SECTION("duplicate member")
    {
        REQUIRE(ReadText(Replace(FIXTURE, {"\"version\": 1,", "\"version\": 1, \"version\": 1,"}), error) ==
                SequenceContentStatus::Malformed);
    }
}

TEST_CASE("Failed reads, writes and cooks preserve their outputs", "[sequence_content]")
{
    SequenceDocument document = ReadFixture();
    SequenceContentError error;
    REQUIRE(TryReadSequenceSource("{}", document, error) == SequenceContentStatus::Malformed);
    REQUIRE(document.Bindings.GetSize() == 4);

    Bytes bytes = Cook(document);
    const std::string accepted = CopyBytes(bytes);
    SequenceDocument broken = ReadFixture();
    broken.VectorKeys[1].Tick = 0;
    REQUIRE(TryCookSequence(broken, bytes, error) == SequenceContentStatus::InvalidSequence);
    REQUIRE(TryWriteSequenceSource(broken, bytes, error) == SequenceContentStatus::InvalidSequence);
    REQUIRE(CopyBytes(bytes) == accepted);

    REQUIRE(TryDecodeSequence({}, document, error) == SequenceContentStatus::Malformed);
    REQUIRE(document.Bindings.GetSize() == 4);

    broken = ReadFixture();
    broken.Tracks[0].KeyOffset = broken.PositionKeys.GetSize();
    REQUIRE(TryValidateSequenceDocument(broken, error) == SequenceContentStatus::Malformed);
}

TEST_CASE("The decoder rejects corrupt and noncanonical cooked bytes", "[sequence_content]")
{
    const std::string cooked = CopyBytes(Cook(ReadFixture()));
    SequenceContentError error;
    REQUIRE(DecodeText(cooked, error) == SequenceContentStatus::Success);

    SECTION("truncated")
    {
        REQUIRE(DecodeText(cooked.substr(0, cooked.size() - 1), error) == SequenceContentStatus::Malformed);
    }
    SECTION("trailing byte")
    {
        REQUIRE(DecodeText(cooked + std::string(1, '\0'), error) == SequenceContentStatus::Malformed);
    }
    SECTION("bad magic")
    {
        std::string bytes = cooked;
        bytes[0] = 'X';
        REQUIRE(DecodeText(bytes, error) == SequenceContentStatus::Malformed);
    }
    SECTION("future version")
    {
        std::string bytes = cooked;
        PutUint32(bytes, {format::VERSION_OFFSET, format::CURRENT_VERSION + 1});
        REQUIRE(DecodeText(bytes, error) == SequenceContentStatus::UnsupportedVersion);
    }
    SECTION("nonzero reserved word")
    {
        std::string bytes = cooked;
        PutUint32(bytes, {format::RESERVED_OFFSET, 1});
        REQUIRE(DecodeText(bytes, error) == SequenceContentStatus::Malformed);
    }
    SECTION("field unused by the binding kind")
    {
        std::string bytes = cooked;
        // The first binding is a camera; its legacy key field must stay zero.
        bytes[format::HEADER_BYTES + format::BINDING_LEGACY_KEY_FIELD] = 'g';
        REQUIRE(DecodeText(bytes, error) == SequenceContentStatus::Malformed);
    }
    SECTION("unknown channel tag")
    {
        std::string bytes = cooked;
        const usize track = format::HEADER_BYTES + (4 * format::BINDING_RECORD_BYTES);
        PutUint32(bytes, {track + format::TRACK_CHANNEL_FIELD, format::CHANNEL_CAMERA_LENS + 1});
        REQUIRE(DecodeText(bytes, error) == SequenceContentStatus::Malformed);
        REQUIRE(error.Field.View() == "tracks[0]");
    }
    SECTION("key count disagrees with the header")
    {
        std::string bytes = cooked;
        PutUint32(bytes, {format::KEY_COUNT_OFFSET, 1});
        REQUIRE(DecodeText(bytes, error) == SequenceContentStatus::Malformed);
    }
    SECTION("unsorted keys")
    {
        std::string bytes = cooked;
        const usize keys = format::HEADER_BYTES + (4 * format::BINDING_RECORD_BYTES) +
                           (6 * format::TRACK_RECORD_BYTES) + (2 * format::INTERVAL_RECORD_BYTES);
        // The second camera position key moves to tick zero.
        for (usize index = 0; index < sizeof(uint64); ++index)
        {
            bytes[keys + format::POSITION_KEY_BYTES + index] = '\0';
        }
        REQUIRE(DecodeText(bytes, error) == SequenceContentStatus::InvalidSequence);
        REQUIRE(error.Sequence.Status == sequencer::SequenceStatus::InvalidKeys);
    }
}

TEST_CASE("A decoded sequence resolves and samples exact frame times", "[sequence_content]")
{
    SequenceDocument document;
    SequenceContentError error;
    REQUIRE(TryDecodeSequence(Cook(ReadFixture()).Data(), document, error) == SequenceContentStatus::Success);
    SequenceDefinitionView view;
    REQUIRE(TryMakeSequenceDefinition(document, view) == SequenceContentStatus::Success);

    const foundation::StaticArray<sequencer::SequenceSceneTarget, 2> targets{{
        {
            .Object = identity::SourceObjectId{ParseUuid(CRATE_OBJECT)},
            .CanWriteLocalTransform = true,
            .CanWriteVisibility = true,
        },
        {
            .LegacyKey = "guard-a",
            .CanWriteLocalTransform = true,
            .CanWriteVisibility = true,
        },
    }};
    const sequencer::SequenceSceneCatalog catalog
    {
        .WorldDocument = document.World.Document,
        .ContentDigest = document.World.ContentDigest,
        .HasContentDigest = true,
        .Targets = {targets.GetData(), targets.GetSize()},
    };
    sequencer::PreparedSequencePlan plan;
    sequencer::SequenceDiagnostic diagnostic;
    REQUIRE(sequencer::TryPrepareSequencePlan(view.GetDefinition(),
                                              catalog,
                                              GetSequenceContentLimits(),
                                              plan,
                                              diagnostic) == sequencer::SequenceStatus::Success);

    const sequencer::SequenceFrameSchedule schedule
    {
        .Rate = {FRAMES_PER_SECOND, 1},
        .TicksPerSecond = TICKS_PER_SECOND,
    };
    usize frames = 0;
    for (uint64 frame = 0;; ++frame)
    {
        sequencer::SequenceTime time;
        REQUIRE(sequencer::TryGetSequenceFrameTime(schedule, frame, time) == sequencer::SequenceStatus::Success);
        sequencer::SequenceCameraSelection selection;
        const sequencer::SequenceStatus selected = sequencer::TryEvaluateSequenceCameraSelection(plan, time, selection);
        if (selected == sequencer::SequenceStatus::OutOfRange)
        {
            // The terminal key at four seconds shapes interpolation but is not an output frame.
            REQUIRE(frame == FRAME_COUNT);
            break;
        }
        REQUIRE(selected == sequencer::SequenceStatus::Success);
        REQUIRE(selection.CameraBindingId == (frame < CUT_FRAME ? CAMERA_A : CAMERA_B));
        sequencer::SequenceObjectSample guard;
        REQUIRE(sequencer::TryEvaluateSequenceObject(plan.GetSequence(), GUARD, time, guard) ==
                sequencer::SequenceStatus::Success);
        REQUIRE(guard.IsVisible == (frame < CUT_FRAME));
        ++frames;
    }
    REQUIRE(frames == FRAME_COUNT);

    // First, middle and last sampled poses, checked analytically.
    const sequencer::SequenceTime middleTime{ .Tick = CUT_FRAME * TICKS_PER_FRAME };
    const sequencer::SequenceTime lastTime{ .Tick = (FRAME_COUNT - 1) * TICKS_PER_FRAME };
    ludus::gameplay::camera::CameraSample camera;
    REQUIRE(sequencer::TryEvaluateSequenceCamera(plan.GetSequence(), CAMERA_A, {0, 0, 1}, camera) ==
            sequencer::SequenceStatus::Success);
    REQUIRE(camera.Pose.Position == math::Vector3d{0, 1.5, 10});
    REQUIRE(sequencer::TryEvaluateSequenceCamera(plan.GetSequence(), CAMERA_A, middleTime, camera) ==
            sequencer::SequenceStatus::Success);
    REQUIRE(camera.Pose.Position == math::Vector3d{0, 1.5, 0});
    REQUIRE(sequencer::TryEvaluateSequenceCamera(plan.GetSequence(), CAMERA_A, lastTime, camera) ==
            sequencer::SequenceStatus::Success);
    constexpr foundation::float64 LAST_FRAME_Z = -10.0 + (20.0 / FRAME_COUNT);
    constexpr foundation::float64 POSITION_TOLERANCE = 1e-12;
    REQUIRE(camera.Pose.Position.X == 0);
    REQUIRE(math::Abs(camera.Pose.Position.Z - LAST_FRAME_Z) < POSITION_TOLERANCE);
    sequencer::SequenceObjectSample crate;
    REQUIRE(sequencer::TryEvaluateSequenceObject(plan.GetSequence(), CRATE, middleTime, crate) ==
            sequencer::SequenceStatus::Success);
    REQUIRE(crate.TransformLocal.Translation == math::Vector3{2, 0, 0});
}
