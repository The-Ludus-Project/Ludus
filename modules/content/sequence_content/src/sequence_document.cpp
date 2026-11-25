#include <ludus/content/sequence_content/sequence_content.h>

#include <ludus/content/sequence_format/luseq.h>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include "internal/sequence_content_common.h"

namespace ludus::content::sequence_content
{
namespace format = sequence_format;
namespace sequencer = gameplay::sequencer;
namespace identity = foundation::identity;

static_assert(identity_records::MAX_LEGACY_ID_BYTES <= sequencer::MAX_SEQUENCE_LEGACY_KEY_BYTES,
              "every version-1 entity key must fit the sequencer's legacy key bound");
static_assert(identity_records::MAX_LEGACY_ID_BYTES == format::LEGACY_KEY_BYTES,
              "the LUSEQ legacy key field must hold exactly one version-1 entity key");
static_assert(format::WORLD_DIGEST_BYTES == sequencer::SEQUENCE_WORLD_DIGEST_BYTES,
              "the LUSEQ digest field must hold the sequencer's pinned World digest");
static_assert(format::UUID_BYTES == identity::UUID_BYTE_COUNT, "LUSEQ UUID fields hold canonical UUID bytes");

namespace internal
{
namespace
{
constexpr usize DECIMAL_DIGITS_MAX = 20;
constexpr usize DECIMAL_BASE = 10;
} // namespace

FieldPath::FieldPath(std::string_view name) noexcept
{
    append(name);
}

FieldPath FieldPath::Member(std::string_view name) const noexcept
{
    FieldPath path = *this;
    path.append(".");
    path.append(name);
    return path;
}

FieldPath FieldPath::Element(usize index) const noexcept
{
    // Collect digits least significant first, then append them in reading order.
    foundation::StaticArray<char, DECIMAL_DIGITS_MAX> digits{};
    usize count = 0;
    do
    {
        digits[count] = static_cast<char>('0' + index % DECIMAL_BASE);
        ++count;
        index /= DECIMAL_BASE;
    } while (index != 0 && count < DECIMAL_DIGITS_MAX);
    FieldPath path = *this;
    path.append("[");
    for (usize i = count; i > 0; --i)
    {
        path.append({&digits[i - 1], 1});
    }
    path.append("]");
    return path;
}

void FieldPath::append(std::string_view text) noexcept
{
    const usize room = MAX_MESSAGE_BYTES - mText.Length;
    const usize count = text.size() < room ? text.size() : room;
    for (usize i = 0; i < count; ++i)
    {
        mText.Data[mText.Length + i] = text[i];
    }
    mText.Length += count;
    mText.Data[mText.Length] = '\0';
}

SequenceContentStatus
Fail(SequenceContentError& outError, SequenceContentStatus status, std::string_view field) noexcept
{
    outError.Status = status;
    [[maybe_unused]] const bool isSet = outError.Field.Set(field.substr(0, MAX_MESSAGE_BYTES));
    return status;
}

namespace
{
bool IsLensFinite(const gameplay::camera::CameraLens& lens) noexcept
{
    return foundation::math::IsFinite(lens.VerticalFov) == true &&
           foundation::math::IsFinite(lens.VerticalSpan) == true &&
           foundation::math::IsFinite(lens.MinimumVisibleWidth) == true &&
           foundation::math::IsFinite(lens.NearPlane) == true && foundation::math::IsFinite(lens.FarPlane) == true;
}

/// Only pose, lens, OutputId and DefinitionRevision are saved; other metadata must stay at its default.
bool HasOnlySavedCameraFields(const gameplay::camera::CameraSample& camera) noexcept
{
    const gameplay::camera::CameraSample defaults{};
    return camera.PreviousTick == defaults.PreviousTick && camera.CurrentTick == defaults.CurrentTick &&
           camera.RenderAlpha == defaults.RenderAlpha && camera.PresentationSequence == defaults.PresentationSequence &&
           camera.DiscontinuityRevision == defaults.DiscontinuityRevision;
}

SequenceContentStatus
ValidateBindingRecord(const SequenceDocument& document, usize index, SequenceContentError& outError) noexcept
{
    const SequenceBindingRecord& binding = document.Bindings[index];
    const FieldPath path = FieldPath("bindings").Element(index);
    switch (binding.Kind)
    {
        case sequencer::SequenceBindingKind::WorldObject:
            if (identity::ValidateUuid(binding.Object.Value, identity::UuidPolicy::RequireVersion4) !=
                identity::UuidStatus::Success)
            {
                return Fail(outError, SequenceContentStatus::InvalidIdentity, path.Member("object").GetView());
            }
            break;
        case sequencer::SequenceBindingKind::LegacyWorldObject:
            if (identity_records::IsLegacyId(binding.LegacyKey.View()) == false)
            {
                return Fail(outError, SequenceContentStatus::InvalidLegacyKey, path.Member("legacy_id").GetView());
            }
            break;
        case sequencer::SequenceBindingKind::SequenceCamera:
            if (HasOnlySavedCameraFields(binding.Camera) == false || IsLensFinite(binding.Camera.Lens) == false)
            {
                return Fail(outError, SequenceContentStatus::Malformed, path.Member("camera").GetView());
            }
            break;
    }
    return SequenceContentStatus::Success;
}
} // namespace

SequenceContentStatus ValidateDocument(const SequenceDocument& document, SequenceContentError& outError) noexcept
{
    if (identity::ValidateUuid(document.Sequence.Value, identity::UuidPolicy::RequireVersion4) !=
        identity::UuidStatus::Success)
    {
        return Fail(outError, SequenceContentStatus::InvalidIdentity, "sequence");
    }
    if (identity::ValidateUuid(document.World.Document.Value, identity::UuidPolicy::AllowNilOrVersion4) !=
        identity::UuidStatus::Success)
    {
        return Fail(outError, SequenceContentStatus::InvalidIdentity, "world.document");
    }
    if (document.WorldPath.Length > 0 && ValidPath(document.WorldPath.View()) == false)
    {
        return Fail(outError, SequenceContentStatus::Malformed, "world.path");
    }
    if (document.World.HasContentDigest == false)
    {
        for (const foundation::uint8 byte : document.World.ContentDigest)
        {
            if (byte != 0)
            {
                // An unpinned digest is canonically zero so it cannot carry stale data.
                return Fail(outError, SequenceContentStatus::Malformed, "world.digest");
            }
        }
    }
    for (usize index = 0; index < document.Bindings.GetSize(); ++index)
    {
        const SequenceContentStatus status = ValidateBindingRecord(document, index, outError);
        if (status != SequenceContentStatus::Success)
        {
            return status;
        }
    }
    for (usize index = 0; index < document.LensKeys.GetSize(); ++index)
    {
        if (IsLensFinite(document.LensKeys[index].Value) == false)
        {
            return Fail(outError, SequenceContentStatus::Malformed, "lens keys");
        }
    }
    SequenceDefinitionView view;
    const SequenceContentStatus made = TryMakeSequenceDefinition(document, view);
    if (made != SequenceContentStatus::Success)
    {
        return Fail(outError, made, "tracks");
    }
    const sequencer::SequenceStatus valid =
        sequencer::TryValidateSequenceDefinition(view.GetDefinition(), GetSequenceContentLimits(), outError.Sequence);
    if (valid != sequencer::SequenceStatus::Success)
    {
        return Fail(outError, SequenceContentStatus::InvalidSequence, "sequence content");
    }
    return SequenceContentStatus::Success;
}
} // namespace internal

namespace
{
/// Return the keys of one track from the document array that matches its channel.
template <typename KeyType>
bool TryBorrowKeys(const foundation::Array<KeyType>& keys,
                   const SequenceTrackRecord& track,
                   foundation::BorrowedView<const KeyType>& outKeys) noexcept
{
    usize end = 0;
    if (foundation::TryAdd(track.KeyOffset, track.KeyCount, end) == false || end > keys.GetSize())
    {
        return false;
    }
    outKeys = {keys.GetData() + track.KeyOffset, track.KeyCount};
    return true;
}

bool TryBorrowTrackKeys(const SequenceDocument& document,
                        const SequenceTrackRecord& record,
                        sequencer::SequenceTrack& outTrack) noexcept
{
    switch (record.Channel)
    {
        case sequencer::SequenceChannel::Translation:
        case sequencer::SequenceChannel::Scale:
            return TryBorrowKeys(document.VectorKeys, record, outTrack.VectorKeys);
        case sequencer::SequenceChannel::Rotation:
        case sequencer::SequenceChannel::CameraRotation:
            return TryBorrowKeys(document.RotationKeys, record, outTrack.RotationKeys);
        case sequencer::SequenceChannel::CameraPosition:
            return TryBorrowKeys(document.PositionKeys, record, outTrack.PositionKeys);
        case sequencer::SequenceChannel::Visibility:
            return TryBorrowKeys(document.VisibilityKeys, record, outTrack.VisibilityKeys);
        case sequencer::SequenceChannel::CameraLens:
            return TryBorrowKeys(document.LensKeys, record, outTrack.LensKeys);
    }
    // An unknown channel keeps empty views; definition validation reports it.
    return true;
}
} // namespace

sequencer::SequenceDefinitionLimits GetSequenceContentLimits() noexcept
{
    sequencer::SequenceDefinitionLimits limits;
    limits.MaxTracks = format::MAX_TRACKS;
    limits.MaxCameraIntervals = format::MAX_INTERVALS;
    limits.Sampler.MaxTracks = format::MAX_BINDINGS;
    limits.Sampler.MaxKeys = format::MAX_KEYS;
    return limits;
}

SequenceContentStatus TryMakeSequenceDefinition(const SequenceDocument& document,
                                                SequenceDefinitionView& outView) noexcept
{
    if (document.Bindings.GetSize() > format::MAX_BINDINGS || document.Tracks.GetSize() > format::MAX_TRACKS)
    {
        return SequenceContentStatus::Limit;
    }
    SequenceDefinitionView candidate;
    if (candidate.mBindings.TryEnsureCapacity(document.Bindings.GetSize()) == false ||
        candidate.mTracks.TryEnsureCapacity(document.Tracks.GetSize()) == false)
    {
        return SequenceContentStatus::OutOfMemory;
    }
    for (const SequenceBindingRecord& record : document.Bindings)
    {
        const sequencer::SequenceBinding binding
        {
            .BindingId = record.BindingId,
            .Kind = record.Kind,
            .Object = record.Object,
            .SubobjectKey = record.SubobjectKey,
            .LegacyKey = record.LegacyKey.View(),
            .Camera = record.Camera,
        };
        [[maybe_unused]] const bool isAdded = candidate.mBindings.TryAdd(binding);
        LUDUS_ASSERT(isAdded == true);
    }
    for (const SequenceTrackRecord& record : document.Tracks)
    {
        sequencer::SequenceTrack track
        {
            .TrackId = record.TrackId,
            .BindingId = record.BindingId,
            .Channel = record.Channel,
            .IsEnabled = record.IsEnabled,
        };
        if (TryBorrowTrackKeys(document, record, track) == false)
        {
            return SequenceContentStatus::Malformed;
        }
        [[maybe_unused]] const bool isAdded = candidate.mTracks.TryAdd(track);
        LUDUS_ASSERT(isAdded == true);
    }
    outView.mBindings = foundation::Move(candidate.mBindings);
    outView.mTracks = foundation::Move(candidate.mTracks);
    outView.mDefinition =
    {
        .World = document.World,
        .TicksPerSecond = document.TicksPerSecond,
        .PlaybackStartTick = document.PlaybackStartTick,
        .PlaybackEndTick = document.PlaybackEndTick,
        .Bindings = outView.mBindings.AsView(),
        .Tracks = outView.mTracks.AsView(),
        .CameraSelection = document.CameraSelection.AsView(),
    };
    return SequenceContentStatus::Success;
}

SequenceContentStatus TryValidateSequenceDocument(const SequenceDocument& document,
                                                  SequenceContentError& outError) noexcept
{
    outError = {};
    return internal::ValidateDocument(document, outError);
}

std::string_view GetSequenceContentStatusName(SequenceContentStatus status) noexcept
{
    switch (status)
    {
        case SequenceContentStatus::Success:
            return "success";
        case SequenceContentStatus::Malformed:
            return "malformed";
        case SequenceContentStatus::UnsupportedVersion:
            return "unsupported-version";
        case SequenceContentStatus::InvalidIdentity:
            return "invalid-identity";
        case SequenceContentStatus::InvalidLegacyKey:
            return "invalid-legacy-key";
        case SequenceContentStatus::InvalidSequence:
            return "invalid-sequence";
        case SequenceContentStatus::Limit:
            return "limit";
        case SequenceContentStatus::OutOfMemory:
            return "out-of-memory";
    }
    return "unknown";
}
} // namespace ludus::content::sequence_content
