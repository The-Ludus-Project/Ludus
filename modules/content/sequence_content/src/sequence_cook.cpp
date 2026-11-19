// Deterministic LUSEQ cooker and validating decoder; the layout is owned by
// Ludus::SequenceFormat (ludus/content/sequence_format/luseq.h).

#include <ludus/content/sequence_content/sequence_content.h>

#include <ludus/content/sequence_format/luseq.h>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/checked_integer.hpp>

#include <bit>

#include "internal/sequence_content_common.h"

namespace ludus::content::sequence_content
{
namespace format = sequence_format;
namespace sequencer = gameplay::sequencer;
namespace camera = gameplay::camera;
namespace identity = foundation::identity;
namespace math = foundation::math;
using internal::Fail;
using internal::FieldPath;

namespace
{
constexpr uint32 VISIBLE_WORD = 1;
constexpr uint32 HIDDEN_WORD = 0;
/// Component index of a quaternion's scalar part in (x, y, z, w) order.
constexpr usize QUATERNION_W_INDEX = 3;

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

uint32 GetKindTag(sequencer::SequenceBindingKind kind) noexcept
{
    switch (kind)
    {
        case sequencer::SequenceBindingKind::WorldObject:
            return format::BINDING_WORLD_OBJECT;
        case sequencer::SequenceBindingKind::SequenceCamera:
            return format::BINDING_SEQUENCE_CAMERA;
        case sequencer::SequenceBindingKind::LegacyWorldObject:
            return format::BINDING_LEGACY_WORLD_OBJECT;
    }
    return 0;
}

bool TryGetKind(uint32 tag, sequencer::SequenceBindingKind& outKind) noexcept
{
    switch (tag)
    {
        case format::BINDING_WORLD_OBJECT:
            outKind = sequencer::SequenceBindingKind::WorldObject;
            return true;
        case format::BINDING_SEQUENCE_CAMERA:
            outKind = sequencer::SequenceBindingKind::SequenceCamera;
            return true;
        case format::BINDING_LEGACY_WORLD_OBJECT:
            outKind = sequencer::SequenceBindingKind::LegacyWorldObject;
            return true;
        default:
            return false;
    }
}

uint32 GetChannelTag(sequencer::SequenceChannel channel) noexcept
{
    switch (channel)
    {
        case sequencer::SequenceChannel::Translation:
            return format::CHANNEL_TRANSLATION;
        case sequencer::SequenceChannel::Rotation:
            return format::CHANNEL_ROTATION;
        case sequencer::SequenceChannel::Scale:
            return format::CHANNEL_SCALE;
        case sequencer::SequenceChannel::Visibility:
            return format::CHANNEL_VISIBILITY;
        case sequencer::SequenceChannel::CameraPosition:
            return format::CHANNEL_CAMERA_POSITION;
        case sequencer::SequenceChannel::CameraRotation:
            return format::CHANNEL_CAMERA_ROTATION;
        case sequencer::SequenceChannel::CameraLens:
            return format::CHANNEL_CAMERA_LENS;
    }
    return 0;
}

bool TryGetChannel(uint32 tag, sequencer::SequenceChannel& outChannel) noexcept
{
    switch (tag)
    {
        case format::CHANNEL_TRANSLATION:
            outChannel = sequencer::SequenceChannel::Translation;
            return true;
        case format::CHANNEL_ROTATION:
            outChannel = sequencer::SequenceChannel::Rotation;
            return true;
        case format::CHANNEL_SCALE:
            outChannel = sequencer::SequenceChannel::Scale;
            return true;
        case format::CHANNEL_VISIBILITY:
            outChannel = sequencer::SequenceChannel::Visibility;
            return true;
        case format::CHANNEL_CAMERA_POSITION:
            outChannel = sequencer::SequenceChannel::CameraPosition;
            return true;
        case format::CHANNEL_CAMERA_ROTATION:
            outChannel = sequencer::SequenceChannel::CameraRotation;
            return true;
        case format::CHANNEL_CAMERA_LENS:
            outChannel = sequencer::SequenceChannel::CameraLens;
            return true;
        default:
            return false;
    }
}

usize GetKeyRecordBytes(sequencer::SequenceChannel channel) noexcept
{
    switch (channel)
    {
        case sequencer::SequenceChannel::Translation:
        case sequencer::SequenceChannel::Scale:
            return format::VECTOR_KEY_BYTES;
        case sequencer::SequenceChannel::Rotation:
        case sequencer::SequenceChannel::CameraRotation:
            return format::ROTATION_KEY_BYTES;
        case sequencer::SequenceChannel::CameraPosition:
            return format::POSITION_KEY_BYTES;
        case sequencer::SequenceChannel::Visibility:
            return format::VISIBILITY_KEY_BYTES;
        case sequencer::SequenceChannel::CameraLens:
            return format::LENS_KEY_BYTES;
    }
    return 0;
}

uint32 GetInterpolationTag(sequencer::SequenceInterpolation interpolation) noexcept
{
    return interpolation == sequencer::SequenceInterpolation::Step ? format::INTERPOLATION_STEP
                                                                   : format::INTERPOLATION_LINEAR;
}

bool TryGetInterpolation(uint32 tag, sequencer::SequenceInterpolation& outInterpolation) noexcept
{
    if (tag == format::INTERPOLATION_STEP || tag == format::INTERPOLATION_LINEAR)
    {
        outInterpolation = tag == format::INTERPOLATION_STEP ? sequencer::SequenceInterpolation::Step
                                                             : sequencer::SequenceInterpolation::Linear;
        return true;
    }
    return false;
}

uint32 GetProjectionTag(camera::CameraProjection projection) noexcept
{
    switch (projection)
    {
        case camera::CameraProjection::PerspectiveFinite:
            return format::PROJECTION_PERSPECTIVE_FINITE;
        case camera::CameraProjection::PerspectiveInfinite:
            return format::PROJECTION_PERSPECTIVE_INFINITE;
        case camera::CameraProjection::OrthographicFinite:
            return format::PROJECTION_ORTHOGRAPHIC_FINITE;
    }
    return 0;
}

bool TryGetProjection(uint32 tag, camera::CameraProjection& outProjection) noexcept
{
    switch (tag)
    {
        case format::PROJECTION_PERSPECTIVE_FINITE:
            outProjection = camera::CameraProjection::PerspectiveFinite;
            return true;
        case format::PROJECTION_PERSPECTIVE_INFINITE:
            outProjection = camera::CameraProjection::PerspectiveInfinite;
            return true;
        case format::PROJECTION_ORTHOGRAPHIC_FINITE:
            outProjection = camera::CameraProjection::OrthographicFinite;
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Field codecs over one record. Callers have checked every record extent.
// ---------------------------------------------------------------------------

class RecordWriter final
{
public:
    explicit RecordWriter(std::span<foundation::uint8> record) noexcept : mRecord(record) {}

    void Uint32(usize offset, uint32 value) noexcept
    {
        [[maybe_unused]] const bool isWritten =
            foundation::TryWriteLittleEndian(value, mRecord.subspan(offset, sizeof(uint32)));
        LUDUS_ASSERT(isWritten == true);
    }
    void Uint64(usize offset, uint64 value) noexcept
    {
        [[maybe_unused]] const bool isWritten =
            foundation::TryWriteLittleEndian(value, mRecord.subspan(offset, sizeof(uint64)));
        LUDUS_ASSERT(isWritten == true);
    }
    void Float32(usize offset, foundation::float32 value) noexcept
    {
        Uint32(offset, std::bit_cast<uint32>(value));
    }
    void Float64(usize offset, foundation::float64 value) noexcept
    {
        Uint64(offset, std::bit_cast<uint64>(value));
    }
    void Bytes(usize offset, std::span<const foundation::uint8> bytes) noexcept
    {
        for (usize index = 0; index < bytes.size(); ++index)
        {
            mRecord[offset + index] = bytes[index];
        }
    }
    void Vector3(usize offset, math::Vector3 value) noexcept
    {
        Float32(offset, value.X);
        Float32(offset + sizeof(foundation::float32), value.Y);
        Float32(offset + (2 * sizeof(foundation::float32)), value.Z);
    }
    void Vector3d(usize offset, math::Vector3d value) noexcept
    {
        Float64(offset, value.X);
        Float64(offset + sizeof(foundation::float64), value.Y);
        Float64(offset + (2 * sizeof(foundation::float64)), value.Z);
    }
    void Quaternion(usize offset, math::Quaternion value) noexcept
    {
        Float32(offset, value.X);
        Float32(offset + sizeof(foundation::float32), value.Y);
        Float32(offset + (2 * sizeof(foundation::float32)), value.Z);
        Float32(offset + (QUATERNION_W_INDEX * sizeof(foundation::float32)), value.W);
    }
    void Lens(usize offset, const camera::CameraLens& lens) noexcept
    {
        Uint32(offset + format::LENS_PROJECTION_FIELD, GetProjectionTag(lens.Projection));
        Float32(offset + format::LENS_VERTICAL_FOV_FIELD, lens.VerticalFov);
        Float32(offset + format::LENS_VERTICAL_SPAN_FIELD, lens.VerticalSpan);
        Float32(offset + format::LENS_MINIMUM_WIDTH_FIELD, lens.MinimumVisibleWidth);
        Float32(offset + format::LENS_NEAR_FIELD, lens.NearPlane);
        Float32(offset + format::LENS_FAR_FIELD, lens.FarPlane);
    }

private:
    std::span<foundation::uint8> mRecord;
};

class RecordReader final
{
public:
    explicit RecordReader(std::span<const foundation::uint8> record) noexcept : mRecord(record) {}

    [[nodiscard]] uint32 Uint32(usize offset) const noexcept
    {
        uint32 value = 0;
        [[maybe_unused]] const bool isRead =
            foundation::TryReadLittleEndian(mRecord.subspan(offset, sizeof(uint32)), value);
        LUDUS_ASSERT(isRead == true);
        return value;
    }
    [[nodiscard]] uint64 Uint64(usize offset) const noexcept
    {
        uint64 value = 0;
        [[maybe_unused]] const bool isRead =
            foundation::TryReadLittleEndian(mRecord.subspan(offset, sizeof(uint64)), value);
        LUDUS_ASSERT(isRead == true);
        return value;
    }
    [[nodiscard]] foundation::float32 Float32(usize offset) const noexcept
    {
        return std::bit_cast<foundation::float32>(Uint32(offset));
    }
    [[nodiscard]] foundation::float64 Float64(usize offset) const noexcept
    {
        return std::bit_cast<foundation::float64>(Uint64(offset));
    }
    [[nodiscard]] std::span<const foundation::uint8> Bytes(usize offset, usize count) const noexcept
    {
        return mRecord.subspan(offset, count);
    }
    [[nodiscard]] bool IsZero(usize offset, usize count) const noexcept
    {
        for (const foundation::uint8 byte : Bytes(offset, count))
        {
            if (byte != 0)
            {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] math::Vector3 Vector3(usize offset) const noexcept
    {
        return {Float32(offset),
                Float32(offset + sizeof(foundation::float32)),
                Float32(offset + (2 * sizeof(foundation::float32)))};
    }
    [[nodiscard]] math::Vector3d Vector3d(usize offset) const noexcept
    {
        return {Float64(offset),
                Float64(offset + sizeof(foundation::float64)),
                Float64(offset + (2 * sizeof(foundation::float64)))};
    }
    [[nodiscard]] math::Quaternion Quaternion(usize offset) const noexcept
    {
        return {Float32(offset),
                Float32(offset + sizeof(foundation::float32)),
                Float32(offset + (2 * sizeof(foundation::float32))),
                Float32(offset + (QUATERNION_W_INDEX * sizeof(foundation::float32)))};
    }
    [[nodiscard]] bool TryReadLens(usize offset, camera::CameraLens& outLens) const noexcept
    {
        camera::CameraLens lens;
        if (TryGetProjection(Uint32(offset + format::LENS_PROJECTION_FIELD), lens.Projection) == false)
        {
            return false;
        }
        lens.VerticalFov = Float32(offset + format::LENS_VERTICAL_FOV_FIELD);
        lens.VerticalSpan = Float32(offset + format::LENS_VERTICAL_SPAN_FIELD);
        lens.MinimumVisibleWidth = Float32(offset + format::LENS_MINIMUM_WIDTH_FIELD);
        lens.NearPlane = Float32(offset + format::LENS_NEAR_FIELD);
        lens.FarPlane = Float32(offset + format::LENS_FAR_FIELD);
        outLens = lens;
        return true;
    }

private:
    std::span<const foundation::uint8> mRecord;
};

// ---------------------------------------------------------------------------
// Cooking
// ---------------------------------------------------------------------------

/// Total cooked size; the document has been validated, so every count is admitted.
usize GetCookedBytes(const SequenceDocument& document) noexcept
{
    usize bytes = format::HEADER_BYTES + (document.Bindings.GetSize() * format::BINDING_RECORD_BYTES) +
                  (document.Tracks.GetSize() * format::TRACK_RECORD_BYTES) +
                  (document.CameraSelection.GetSize() * format::INTERVAL_RECORD_BYTES);
    for (const SequenceTrackRecord& track : document.Tracks)
    {
        bytes += track.KeyCount * GetKeyRecordBytes(track.Channel);
    }
    return bytes;
}

void WriteBinding(RecordWriter writer, const SequenceBindingRecord& binding) noexcept
{
    writer.Uint64(format::BINDING_ID_FIELD, binding.BindingId);
    writer.Uint32(format::BINDING_KIND_FIELD, GetKindTag(binding.Kind));
    switch (binding.Kind)
    {
        case sequencer::SequenceBindingKind::WorldObject:
            writer.Bytes(format::BINDING_OBJECT_FIELD, binding.Object.Value.Bytes);
            writer.Uint64(format::BINDING_SUBOBJECT_FIELD, binding.SubobjectKey);
            break;
        case sequencer::SequenceBindingKind::LegacyWorldObject: {
            const std::string_view key = binding.LegacyKey.View();
            writer.Uint32(format::BINDING_LEGACY_KEY_LENGTH_FIELD, static_cast<uint32>(key.size()));
            writer.Bytes(format::BINDING_LEGACY_KEY_FIELD,
                         {reinterpret_cast<const foundation::uint8*>(key.data()), key.size()});
            break;
        }
        case sequencer::SequenceBindingKind::SequenceCamera:
            writer.Vector3d(format::BINDING_CAMERA_POSITION_FIELD, binding.Camera.Pose.Position);
            writer.Quaternion(format::BINDING_CAMERA_ROTATION_FIELD, binding.Camera.Pose.Orientation);
            writer.Lens(format::BINDING_CAMERA_LENS_FIELD, binding.Camera.Lens);
            writer.Uint64(format::BINDING_CAMERA_OUTPUT_FIELD, binding.Camera.OutputId);
            writer.Uint64(format::BINDING_CAMERA_REVISION_FIELD, binding.Camera.DefinitionRevision);
            break;
    }
}

void WriteKey(RecordWriter writer,
              const SequenceDocument& document,
              sequencer::SequenceChannel channel,
              usize index) noexcept
{
    switch (channel)
    {
        case sequencer::SequenceChannel::Translation:
        case sequencer::SequenceChannel::Scale:
            writer.Uint64(format::KEY_TICK_FIELD, document.VectorKeys[index].Tick);
            writer.Uint32(format::KEY_MODE_FIELD, GetInterpolationTag(document.VectorKeys[index].Interpolation));
            writer.Vector3(format::KEY_VALUE_FIELD, document.VectorKeys[index].Value);
            break;
        case sequencer::SequenceChannel::Rotation:
        case sequencer::SequenceChannel::CameraRotation:
            writer.Uint64(format::KEY_TICK_FIELD, document.RotationKeys[index].Tick);
            writer.Uint32(format::KEY_MODE_FIELD, GetInterpolationTag(document.RotationKeys[index].Interpolation));
            writer.Quaternion(format::KEY_VALUE_FIELD, document.RotationKeys[index].Value);
            break;
        case sequencer::SequenceChannel::CameraPosition:
            writer.Uint64(format::KEY_TICK_FIELD, document.PositionKeys[index].Tick);
            writer.Uint32(format::KEY_MODE_FIELD, GetInterpolationTag(document.PositionKeys[index].Interpolation));
            writer.Vector3d(format::KEY_VALUE_FIELD, document.PositionKeys[index].PositionWorld);
            break;
        case sequencer::SequenceChannel::Visibility:
            writer.Uint64(format::KEY_TICK_FIELD, document.VisibilityKeys[index].Tick);
            writer.Uint32(format::KEY_MODE_FIELD,
                          document.VisibilityKeys[index].IsVisible == true ? VISIBLE_WORD : HIDDEN_WORD);
            break;
        case sequencer::SequenceChannel::CameraLens:
            writer.Uint64(format::KEY_TICK_FIELD, document.LensKeys[index].Tick);
            writer.Uint32(format::KEY_MODE_FIELD, GetInterpolationTag(document.LensKeys[index].Interpolation));
            writer.Lens(format::KEY_VALUE_FIELD, document.LensKeys[index].Value);
            break;
    }
}

void WriteSequence(const SequenceDocument& document, std::span<foundation::uint8> bytes) noexcept
{
    RecordWriter header(bytes.first(format::HEADER_BYTES));
    header.Bytes(format::MAGIC_OFFSET,
                 {reinterpret_cast<const foundation::uint8*>(format::MAGIC.data()), format::MAGIC.size()});
    header.Uint32(format::VERSION_OFFSET, format::CURRENT_VERSION);
    header.Bytes(format::SEQUENCE_ID_OFFSET, document.Sequence.Value.Bytes);
    header.Bytes(format::WORLD_DOCUMENT_OFFSET, document.World.Document.Value.Bytes);
    if (document.World.HasContentDigest == true)
    {
        header.Bytes(format::WORLD_DIGEST_OFFSET,
                     {document.World.ContentDigest.GetData(), document.World.ContentDigest.GetSize()});
        header.Uint32(format::WORLD_FLAGS_OFFSET, format::WORLD_FLAG_CONTENT_DIGEST);
    }
    header.Uint32(format::TICKS_PER_SECOND_OFFSET, document.TicksPerSecond);
    header.Uint64(format::PLAYBACK_START_OFFSET, document.PlaybackStartTick);
    header.Uint64(format::PLAYBACK_END_OFFSET, document.PlaybackEndTick);
    header.Uint32(format::BINDING_COUNT_OFFSET, static_cast<uint32>(document.Bindings.GetSize()));
    header.Uint32(format::TRACK_COUNT_OFFSET, static_cast<uint32>(document.Tracks.GetSize()));
    header.Uint32(format::INTERVAL_COUNT_OFFSET, static_cast<uint32>(document.CameraSelection.GetSize()));
    usize keyCount = 0;
    for (const SequenceTrackRecord& track : document.Tracks)
    {
        keyCount += track.KeyCount;
    }
    header.Uint32(format::KEY_COUNT_OFFSET, static_cast<uint32>(keyCount));

    usize offset = format::HEADER_BYTES;
    for (const SequenceBindingRecord& binding : document.Bindings)
    {
        RecordWriter record(bytes.subspan(offset, format::BINDING_RECORD_BYTES));
        WriteBinding(record, binding);
        offset += format::BINDING_RECORD_BYTES;
    }
    for (const SequenceTrackRecord& track : document.Tracks)
    {
        RecordWriter record(bytes.subspan(offset, format::TRACK_RECORD_BYTES));
        record.Uint64(format::TRACK_ID_FIELD, track.TrackId);
        record.Uint64(format::TRACK_BINDING_FIELD, track.BindingId);
        record.Uint32(format::TRACK_CHANNEL_FIELD, GetChannelTag(track.Channel));
        record.Uint32(format::TRACK_FLAGS_FIELD, track.IsEnabled == true ? format::TRACK_FLAG_ENABLED : 0);
        record.Uint32(format::TRACK_KEY_COUNT_FIELD, static_cast<uint32>(track.KeyCount));
        offset += format::TRACK_RECORD_BYTES;
    }
    for (const sequencer::SequenceCameraInterval& interval : document.CameraSelection)
    {
        RecordWriter record(bytes.subspan(offset, format::INTERVAL_RECORD_BYTES));
        record.Uint64(format::INTERVAL_START_FIELD, interval.StartTick);
        record.Uint64(format::INTERVAL_END_FIELD, interval.EndTick);
        record.Uint64(format::INTERVAL_CAMERA_FIELD, interval.CameraBindingId);
        offset += format::INTERVAL_RECORD_BYTES;
    }
    for (const SequenceTrackRecord& track : document.Tracks)
    {
        const usize width = GetKeyRecordBytes(track.Channel);
        for (usize index = track.KeyOffset; index < track.KeyOffset + track.KeyCount; ++index)
        {
            RecordWriter record(bytes.subspan(offset, width));
            WriteKey(record, document, track.Channel, index);
            offset += width;
        }
    }
    LUDUS_ASSERT(offset == bytes.size());
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

/// Decodes into a candidate document, recording the first failure.
class SequenceDecoder final
{
public:
    SequenceDecoder(std::span<const foundation::uint8> bytes,
                    SequenceDocument& outDocument,
                    SequenceContentError& outError) noexcept
        : mBytes(bytes), mDocument(outDocument), mError(outError)
    {
    }

    [[nodiscard]] SequenceContentStatus Decode() noexcept;

private:
    bool malformed(const FieldPath& path) noexcept
    {
        Fail(mError, SequenceContentStatus::Malformed, path.GetView());
        return false;
    }
    bool decodeHeader(uint32& outBindings, uint32& outTracks, uint32& outIntervals, uint32& outKeys) noexcept;
    bool decodeBinding(RecordReader record, usize index) noexcept;
    bool decodeTrack(RecordReader record, usize index, usize& inoutKeyBytes) noexcept;
    bool decodeKey(RecordReader record, const SequenceTrackRecord& track, const FieldPath& path) noexcept;
    bool reserveKeys(const FieldPath& path) noexcept;

    std::span<const foundation::uint8> mBytes;
    SequenceDocument& mDocument;
    SequenceContentError& mError;
};

bool SequenceDecoder::decodeHeader(uint32& outBindings,
                                   uint32& outTracks,
                                   uint32& outIntervals,
                                   uint32& outKeys) noexcept
{
    const FieldPath path("header");
    if (mBytes.size() < format::HEADER_BYTES)
    {
        return malformed(path);
    }
    const RecordReader header(mBytes.first(format::HEADER_BYTES));
    const std::span<const foundation::uint8> magic = header.Bytes(format::MAGIC_OFFSET, format::MAGIC.size());
    for (usize index = 0; index < magic.size(); ++index)
    {
        if (magic[index] != static_cast<foundation::uint8>(format::MAGIC[index]))
        {
            return malformed(path.Member("magic"));
        }
    }
    if (header.Uint32(format::VERSION_OFFSET) != format::CURRENT_VERSION)
    {
        Fail(mError, SequenceContentStatus::UnsupportedVersion, "header.version");
        return false;
    }
    const uint32 flags = header.Uint32(format::WORLD_FLAGS_OFFSET);
    if (header.Uint32(format::RESERVED_OFFSET) != 0 || (flags & ~format::WORLD_FLAGS_V1) != 0)
    {
        return malformed(path.Member("flags"));
    }
    for (usize index = 0; index < identity::UUID_BYTE_COUNT; ++index)
    {
        mDocument.Sequence.Value.Bytes[index] = header.Bytes(format::SEQUENCE_ID_OFFSET, format::UUID_BYTES)[index];
        mDocument.World.Document.Value.Bytes[index] =
            header.Bytes(format::WORLD_DOCUMENT_OFFSET, format::UUID_BYTES)[index];
    }
    mDocument.World.HasContentDigest = (flags & format::WORLD_FLAG_CONTENT_DIGEST) != 0;
    for (usize index = 0; index < format::WORLD_DIGEST_BYTES; ++index)
    {
        mDocument.World.ContentDigest[index] =
            header.Bytes(format::WORLD_DIGEST_OFFSET, format::WORLD_DIGEST_BYTES)[index];
    }
    mDocument.TicksPerSecond = header.Uint32(format::TICKS_PER_SECOND_OFFSET);
    mDocument.PlaybackStartTick = header.Uint64(format::PLAYBACK_START_OFFSET);
    mDocument.PlaybackEndTick = header.Uint64(format::PLAYBACK_END_OFFSET);
    outBindings = header.Uint32(format::BINDING_COUNT_OFFSET);
    outTracks = header.Uint32(format::TRACK_COUNT_OFFSET);
    outIntervals = header.Uint32(format::INTERVAL_COUNT_OFFSET);
    outKeys = header.Uint32(format::KEY_COUNT_OFFSET);
    if (outBindings > format::MAX_BINDINGS || outTracks > format::MAX_TRACKS || outIntervals > format::MAX_INTERVALS ||
        outKeys > format::MAX_KEYS)
    {
        Fail(mError, SequenceContentStatus::Limit, "header.counts");
        return false;
    }
    return true;
}

bool SequenceDecoder::decodeBinding(RecordReader record, usize index) noexcept
{
    const FieldPath path = FieldPath("bindings").Element(index);
    SequenceBindingRecord binding{};
    binding.BindingId = record.Uint64(format::BINDING_ID_FIELD);
    if (TryGetKind(record.Uint32(format::BINDING_KIND_FIELD), binding.Kind) == false)
    {
        return malformed(path.Member("kind"));
    }
    const uint32 keyLength = record.Uint32(format::BINDING_LEGACY_KEY_LENGTH_FIELD);
    const bool isObject = binding.Kind == sequencer::SequenceBindingKind::WorldObject;
    const bool isLegacy = binding.Kind == sequencer::SequenceBindingKind::LegacyWorldObject;
    const bool isCamera = binding.Kind == sequencer::SequenceBindingKind::SequenceCamera;
    const usize cameraOffset = format::BINDING_CAMERA_POSITION_FIELD;
    const usize cameraBytes = format::BINDING_RECORD_BYTES - cameraOffset;
    // Canonical records zero every field their kind does not use.
    if ((isObject == false &&
         record.IsZero(format::BINDING_OBJECT_FIELD, format::UUID_BYTES + sizeof(uint64)) == false) ||
        (isLegacy == false &&
         (keyLength != 0 || record.IsZero(format::BINDING_LEGACY_KEY_FIELD, format::LEGACY_KEY_BYTES) == false)) ||
        (isCamera == false && record.IsZero(cameraOffset, cameraBytes) == false) ||
        keyLength > format::LEGACY_KEY_BYTES ||
        record.IsZero(format::BINDING_LEGACY_KEY_FIELD + keyLength, format::LEGACY_KEY_BYTES - keyLength) == false)
    {
        return malformed(path);
    }
    if (isObject == true)
    {
        for (usize byte = 0; byte < format::UUID_BYTES; ++byte)
        {
            binding.Object.Value.Bytes[byte] = record.Bytes(format::BINDING_OBJECT_FIELD, format::UUID_BYTES)[byte];
        }
        binding.SubobjectKey = record.Uint64(format::BINDING_SUBOBJECT_FIELD);
    }
    if (isLegacy == true)
    {
        const std::span<const foundation::uint8> key = record.Bytes(format::BINDING_LEGACY_KEY_FIELD, keyLength);
        if (binding.LegacyKey.Set({reinterpret_cast<const char*>(key.data()), key.size()}) == false)
        {
            Fail(mError, SequenceContentStatus::InvalidLegacyKey, path.Member("legacy_id").GetView());
            return false;
        }
    }
    if (isCamera == true)
    {
        binding.Camera.Pose.Position = record.Vector3d(format::BINDING_CAMERA_POSITION_FIELD);
        binding.Camera.Pose.Orientation = record.Quaternion(format::BINDING_CAMERA_ROTATION_FIELD);
        if (record.TryReadLens(format::BINDING_CAMERA_LENS_FIELD, binding.Camera.Lens) == false)
        {
            return malformed(path.Member("camera.lens"));
        }
        binding.Camera.OutputId = record.Uint64(format::BINDING_CAMERA_OUTPUT_FIELD);
        binding.Camera.DefinitionRevision = record.Uint64(format::BINDING_CAMERA_REVISION_FIELD);
    }
    [[maybe_unused]] const bool isAdded = mDocument.Bindings.TryAdd(binding);
    LUDUS_ASSERT(isAdded == true);
    return true;
}

bool SequenceDecoder::decodeTrack(RecordReader record, usize index, usize& inoutKeyBytes) noexcept
{
    const FieldPath path = FieldPath("tracks").Element(index);
    SequenceTrackRecord track{};
    track.TrackId = record.Uint64(format::TRACK_ID_FIELD);
    track.BindingId = record.Uint64(format::TRACK_BINDING_FIELD);
    const uint32 flags = record.Uint32(format::TRACK_FLAGS_FIELD);
    if (TryGetChannel(record.Uint32(format::TRACK_CHANNEL_FIELD), track.Channel) == false ||
        (flags & ~format::TRACK_FLAGS_V1) != 0 || record.Uint32(format::TRACK_RESERVED_FIELD) != 0)
    {
        return malformed(path);
    }
    track.IsEnabled = (flags & format::TRACK_FLAG_ENABLED) != 0;
    track.KeyCount = record.Uint32(format::TRACK_KEY_COUNT_FIELD);
    usize keyBytes = 0;
    if (track.KeyCount > format::MAX_KEYS ||
        foundation::TryMultiply(track.KeyCount, GetKeyRecordBytes(track.Channel), keyBytes) == false ||
        foundation::TryAdd(inoutKeyBytes, keyBytes, inoutKeyBytes) == false)
    {
        Fail(mError, SequenceContentStatus::Limit, path.Member("keys").GetView());
        return false;
    }
    [[maybe_unused]] const bool isAdded = mDocument.Tracks.TryAdd(track);
    LUDUS_ASSERT(isAdded == true);
    return true;
}

bool SequenceDecoder::reserveKeys(const FieldPath& path) noexcept
{
    usize vectors = 0;
    usize rotations = 0;
    usize positions = 0;
    usize visibility = 0;
    usize lenses = 0;
    for (const SequenceTrackRecord& track : mDocument.Tracks)
    {
        switch (track.Channel)
        {
            case sequencer::SequenceChannel::Translation:
            case sequencer::SequenceChannel::Scale:
                vectors += track.KeyCount;
                break;
            case sequencer::SequenceChannel::Rotation:
            case sequencer::SequenceChannel::CameraRotation:
                rotations += track.KeyCount;
                break;
            case sequencer::SequenceChannel::CameraPosition:
                positions += track.KeyCount;
                break;
            case sequencer::SequenceChannel::Visibility:
                visibility += track.KeyCount;
                break;
            case sequencer::SequenceChannel::CameraLens:
                lenses += track.KeyCount;
                break;
        }
    }
    if (mDocument.VectorKeys.TryEnsureCapacity(vectors) == false ||
        mDocument.RotationKeys.TryEnsureCapacity(rotations) == false ||
        mDocument.PositionKeys.TryEnsureCapacity(positions) == false ||
        mDocument.VisibilityKeys.TryEnsureCapacity(visibility) == false ||
        mDocument.LensKeys.TryEnsureCapacity(lenses) == false)
    {
        Fail(mError, SequenceContentStatus::OutOfMemory, path.GetView());
        return false;
    }
    return true;
}

bool SequenceDecoder::decodeKey(RecordReader record, const SequenceTrackRecord& track, const FieldPath& path) noexcept
{
    const uint64 tick = record.Uint64(format::KEY_TICK_FIELD);
    const uint32 mode = record.Uint32(format::KEY_MODE_FIELD);
    if (track.Channel == sequencer::SequenceChannel::Visibility)
    {
        if (mode != VISIBLE_WORD && mode != HIDDEN_WORD)
        {
            return malformed(path);
        }
        [[maybe_unused]] const bool isAdded =
            mDocument.VisibilityKeys.TryAdd({ .Tick = tick, .IsVisible = mode == VISIBLE_WORD });
        LUDUS_ASSERT(isAdded == true);
        return true;
    }
    sequencer::SequenceInterpolation interpolation = sequencer::SequenceInterpolation::Step;
    if (TryGetInterpolation(mode, interpolation) == false)
    {
        return malformed(path);
    }
    bool isAdded = false;
    switch (track.Channel)
    {
        case sequencer::SequenceChannel::Translation:
        case sequencer::SequenceChannel::Scale:
            isAdded = mDocument.VectorKeys.TryAdd(
                { .Tick = tick, .Value = record.Vector3(format::KEY_VALUE_FIELD), .Interpolation = interpolation });
            break;
        case sequencer::SequenceChannel::Rotation:
        case sequencer::SequenceChannel::CameraRotation:
            isAdded = mDocument.RotationKeys.TryAdd(
                { .Tick = tick, .Value = record.Quaternion(format::KEY_VALUE_FIELD), .Interpolation = interpolation });
            break;
        case sequencer::SequenceChannel::CameraPosition:
            isAdded = mDocument.PositionKeys.TryAdd(
            {
                .Tick = tick,
                .PositionWorld = record.Vector3d(format::KEY_VALUE_FIELD),
                .Interpolation = interpolation,
            });
            break;
        case sequencer::SequenceChannel::CameraLens: {
            sequencer::SequenceLensKey key{ .Tick = tick, .Interpolation = interpolation };
            if (record.TryReadLens(format::KEY_VALUE_FIELD, key.Value) == false)
            {
                return malformed(path);
            }
            isAdded = mDocument.LensKeys.TryAdd(key);
            break;
        }
        case sequencer::SequenceChannel::Visibility:
            break;
    }
    LUDUS_ASSERT(isAdded == true);
    return isAdded;
}

SequenceContentStatus SequenceDecoder::Decode() noexcept
{
    uint32 bindingCount = 0;
    uint32 trackCount = 0;
    uint32 intervalCount = 0;
    uint32 keyCount = 0;
    if (decodeHeader(bindingCount, trackCount, intervalCount, keyCount) == false)
    {
        return mError.Status;
    }
    // Every count is bounded by the format limits, so the record extents cannot overflow.
    const usize recordBytes = format::HEADER_BYTES + (usize{bindingCount} * format::BINDING_RECORD_BYTES) +
                              (usize{trackCount} * format::TRACK_RECORD_BYTES) +
                              (usize{intervalCount} * format::INTERVAL_RECORD_BYTES);
    if (mBytes.size() < recordBytes)
    {
        return Fail(mError, SequenceContentStatus::Malformed, "records");
    }
    if (mDocument.Bindings.TryEnsureCapacity(bindingCount) == false ||
        mDocument.Tracks.TryEnsureCapacity(trackCount) == false ||
        mDocument.CameraSelection.TryEnsureCapacity(intervalCount) == false)
    {
        return Fail(mError, SequenceContentStatus::OutOfMemory, "records");
    }
    usize offset = format::HEADER_BYTES;
    for (usize index = 0; index < bindingCount; ++index)
    {
        if (decodeBinding(RecordReader(mBytes.subspan(offset, format::BINDING_RECORD_BYTES)), index) == false)
        {
            return mError.Status;
        }
        offset += format::BINDING_RECORD_BYTES;
    }
    usize keyBytes = 0;
    usize declaredKeys = 0;
    for (usize index = 0; index < trackCount; ++index)
    {
        if (decodeTrack(RecordReader(mBytes.subspan(offset, format::TRACK_RECORD_BYTES)), index, keyBytes) == false)
        {
            return mError.Status;
        }
        declaredKeys += mDocument.Tracks.GetLast().KeyCount;
        offset += format::TRACK_RECORD_BYTES;
    }
    if (declaredKeys != keyCount)
    {
        return Fail(mError, SequenceContentStatus::Malformed, "header.key_count");
    }
    for (usize index = 0; index < intervalCount; ++index)
    {
        const RecordReader record(mBytes.subspan(offset, format::INTERVAL_RECORD_BYTES));
        const sequencer::SequenceCameraInterval interval
        {
            .StartTick = record.Uint64(format::INTERVAL_START_FIELD),
            .EndTick = record.Uint64(format::INTERVAL_END_FIELD),
            .CameraBindingId = record.Uint64(format::INTERVAL_CAMERA_FIELD),
        };
        [[maybe_unused]] const bool isAdded = mDocument.CameraSelection.TryAdd(interval);
        LUDUS_ASSERT(isAdded == true);
        offset += format::INTERVAL_RECORD_BYTES;
    }
    usize expectedBytes = 0;
    if (foundation::TryAdd(recordBytes, keyBytes, expectedBytes) == false || mBytes.size() != expectedBytes)
    {
        // Short key payloads and trailing bytes are both corruption.
        return Fail(mError, SequenceContentStatus::Malformed, "keys");
    }
    if (reserveKeys(FieldPath("keys")) == false)
    {
        return mError.Status;
    }
    for (usize index = 0; index < mDocument.Tracks.GetSize(); ++index)
    {
        SequenceTrackRecord& track = mDocument.Tracks[index];
        const FieldPath path = FieldPath("tracks").Element(index).Member("keys");
        const usize width = GetKeyRecordBytes(track.Channel);
        track.KeyOffset = 0;
        switch (track.Channel)
        {
            case sequencer::SequenceChannel::Translation:
            case sequencer::SequenceChannel::Scale:
                track.KeyOffset = mDocument.VectorKeys.GetSize();
                break;
            case sequencer::SequenceChannel::Rotation:
            case sequencer::SequenceChannel::CameraRotation:
                track.KeyOffset = mDocument.RotationKeys.GetSize();
                break;
            case sequencer::SequenceChannel::CameraPosition:
                track.KeyOffset = mDocument.PositionKeys.GetSize();
                break;
            case sequencer::SequenceChannel::Visibility:
                track.KeyOffset = mDocument.VisibilityKeys.GetSize();
                break;
            case sequencer::SequenceChannel::CameraLens:
                track.KeyOffset = mDocument.LensKeys.GetSize();
                break;
        }
        for (usize key = 0; key < track.KeyCount; ++key)
        {
            if (decodeKey(RecordReader(mBytes.subspan(offset, width)), track, path.Element(key)) == false)
            {
                return mError.Status;
            }
            offset += width;
        }
    }
    return SequenceContentStatus::Success;
}
} // namespace

SequenceContentStatus
TryCookSequence(const SequenceDocument& document, Bytes& outBytes, SequenceContentError& outError) noexcept
{
    outError = {};
    const SequenceContentStatus valid = internal::ValidateDocument(document, outError);
    if (valid != SequenceContentStatus::Success)
    {
        return valid;
    }
    Bytes bytes;
    if (bytes.Resize(GetCookedBytes(document)) == false)
    {
        return Fail(outError, SequenceContentStatus::OutOfMemory, "document");
    }
    for (foundation::uint8& byte : bytes.Data())
    {
        byte = 0;
    }
    WriteSequence(document, bytes.Data());
    outBytes = foundation::Move(bytes);
    return SequenceContentStatus::Success;
}

SequenceContentStatus TryDecodeSequence(std::span<const foundation::uint8> bytes,
                                        SequenceDocument& outDocument,
                                        SequenceContentError& outError) noexcept
{
    outError = {};
    if (bytes.size() > format::MAX_SEQUENCE_BYTES)
    {
        return Fail(outError, SequenceContentStatus::Limit, "document");
    }
    SequenceDocument candidate;
    SequenceDecoder decoder(bytes, candidate, outError);
    const SequenceContentStatus decoded = decoder.Decode();
    if (decoded != SequenceContentStatus::Success)
    {
        return decoded;
    }
    const SequenceContentStatus valid = internal::ValidateDocument(candidate, outError);
    if (valid != SequenceContentStatus::Success)
    {
        return valid;
    }
    outDocument = foundation::Move(candidate);
    return SequenceContentStatus::Success;
}
} // namespace ludus::content::sequence_content
