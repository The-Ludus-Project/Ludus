// Reader and canonical writer for the `.luseq.json` sequence source document.
// The schema is documented in docs/architecture/sequencer-cinematics.md#sequence-source-document.

#include <ludus/content/sequence_content/sequence_content.h>

#include <ludus/content/sequence_format/luseq.h>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/parsing/json.hpp>

#include <initializer_list>

#include "internal/sequence_content_common.h"

namespace ludus::content::sequence_content
{
namespace sequencer = gameplay::sequencer;
namespace camera = gameplay::camera;
namespace identity = foundation::identity;
namespace math = foundation::math;
namespace parsing = foundation::parsing;
using internal::Fail;
using internal::FieldPath;
using parsing::JsonValue;

namespace
{
/// Parser workspace for a maximal source: yyjson's worst-case estimate is about sixteen bytes per input byte.
constexpr usize MAX_SOURCE_WORKSPACE_BYTES = MAX_SOURCE_BYTES * 16; // ludus-style: magic-number yyjson estimate
/// Deepest nesting: root, bindings, binding, camera, lens.
constexpr usize MAX_SOURCE_DEPTH = 6;
/// Largest array: one track's keys.
constexpr usize MAX_SOURCE_ARRAY_ELEMENTS = sequence_format::MAX_KEYS;
/// Hexadecimal digits of a pinned World digest.
constexpr usize DIGEST_TEXT_LENGTH = sequencer::SEQUENCE_WORLD_DIGEST_BYTES * 2;
constexpr usize VECTOR3_COMPONENTS = 3;
constexpr usize QUATERNION_COMPONENTS = 4;
/// Component index of a quaternion's scalar part in (x, y, z, w) order.
constexpr usize QUATERNION_W_INDEX = 3;
constexpr usize BINDING_KIND_COUNT = 3;
constexpr usize CHANNEL_COUNT = 7;
constexpr usize INTERPOLATION_COUNT = 2;
constexpr usize PROJECTION_COUNT = 3;
constexpr usize NIBBLE_BITS = 4;
constexpr foundation::uint8 NIBBLE_MASK = 0x0F;
/// Deepest writer nesting; MAX_SOURCE_DEPTH plus inline value arrays.
constexpr usize MAX_WRITER_DEPTH = 8;
constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

struct NamedTag final
{
    std::string_view Name;
    foundation::uint8 Value;
};

constexpr foundation::StaticArray<NamedTag, BINDING_KIND_COUNT> BINDING_KINDS{{
    {"world-object", static_cast<foundation::uint8>(sequencer::SequenceBindingKind::WorldObject)},
    {"sequence-camera", static_cast<foundation::uint8>(sequencer::SequenceBindingKind::SequenceCamera)},
    {"legacy-world-object", static_cast<foundation::uint8>(sequencer::SequenceBindingKind::LegacyWorldObject)},
}};
constexpr foundation::StaticArray<NamedTag, CHANNEL_COUNT> CHANNELS{{
    {"translation", static_cast<foundation::uint8>(sequencer::SequenceChannel::Translation)},
    {"rotation", static_cast<foundation::uint8>(sequencer::SequenceChannel::Rotation)},
    {"scale", static_cast<foundation::uint8>(sequencer::SequenceChannel::Scale)},
    {"visibility", static_cast<foundation::uint8>(sequencer::SequenceChannel::Visibility)},
    {"camera-position", static_cast<foundation::uint8>(sequencer::SequenceChannel::CameraPosition)},
    {"camera-rotation", static_cast<foundation::uint8>(sequencer::SequenceChannel::CameraRotation)},
    {"camera-lens", static_cast<foundation::uint8>(sequencer::SequenceChannel::CameraLens)},
}};
constexpr foundation::StaticArray<NamedTag, INTERPOLATION_COUNT> INTERPOLATIONS{{
    {"step", static_cast<foundation::uint8>(sequencer::SequenceInterpolation::Step)},
    {"linear", static_cast<foundation::uint8>(sequencer::SequenceInterpolation::Linear)},
}};
constexpr foundation::StaticArray<NamedTag, PROJECTION_COUNT> PROJECTIONS{{
    {"perspective-finite", static_cast<foundation::uint8>(camera::CameraProjection::PerspectiveFinite)},
    {"perspective-infinite", static_cast<foundation::uint8>(camera::CameraProjection::PerspectiveInfinite)},
    {"orthographic-finite", static_cast<foundation::uint8>(camera::CameraProjection::OrthographicFinite)},
}};

template <usize Count>
bool TryFindTag(const foundation::StaticArray<NamedTag, Count>& tags,
                std::string_view name,
                foundation::uint8& outValue) noexcept
{
    for (const NamedTag& tag : tags)
    {
        if (tag.Name == name)
        {
            outValue = tag.Value;
            return true;
        }
    }
    return false;
}

template <usize Count>
std::string_view GetTagName(const foundation::StaticArray<NamedTag, Count>& tags, foundation::uint8 value) noexcept
{
    for (const NamedTag& tag : tags)
    {
        if (tag.Value == value)
        {
            return tag.Name;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

/// Reads one document, recording the first failure and its field.
class SourceReader final
{
public:
    SourceReader(SequenceDocument& outDocument, SequenceContentError& outError) noexcept
        : mDocument(outDocument), mError(outError)
    {
    }

    [[nodiscard]] SequenceContentStatus ReadRoot(JsonValue root) noexcept;

private:
    bool fail(SequenceContentStatus status, const FieldPath& path) noexcept
    {
        Fail(mError, status, path.GetView());
        return false;
    }
    bool malformed(const FieldPath& path) noexcept
    {
        return fail(SequenceContentStatus::Malformed, path);
    }

    bool
    hasOnlyMembers(JsonValue object, std::initializer_list<std::string_view> allowed, const FieldPath& path) noexcept;
    bool readMember(JsonValue object, std::string_view key, const FieldPath& path, JsonValue& outValue) noexcept;
    bool readUint64(JsonValue object, std::string_view key, const FieldPath& path, uint64& outValue) noexcept;
    bool readUint32(JsonValue object, std::string_view key, const FieldPath& path, uint32& outValue) noexcept;
    bool readBoolean(JsonValue object, std::string_view key, const FieldPath& path, bool& outValue) noexcept;
    bool readString(JsonValue object, std::string_view key, const FieldPath& path, std::string_view& outValue) noexcept;
    template <usize N>
    bool readText(JsonValue object, std::string_view key, const FieldPath& path, Text<N>& outValue) noexcept;
    template <usize Count>
    bool readTag(JsonValue object,
                 std::string_view key,
                 const FieldPath& path,
                 const foundation::StaticArray<NamedTag, Count>& tags,
                 foundation::uint8& outValue) noexcept;
    bool readUuid(JsonValue object,
                  std::string_view key,
                  const FieldPath& path,
                  identity::UuidPolicy policy,
                  identity::Uuid& outValue) noexcept;
    bool readFloat32(JsonValue value, const FieldPath& path, foundation::float32& outValue) noexcept;
    bool readFloat64(JsonValue value, const FieldPath& path, foundation::float64& outValue) noexcept;
    bool readComponents(JsonValue object,
                        std::string_view key,
                        const FieldPath& path,
                        usize count,
                        JsonValue& outArray) noexcept;
    bool readVector3(JsonValue object, std::string_view key, const FieldPath& path, math::Vector3& outValue) noexcept;
    bool readVector3d(JsonValue object, std::string_view key, const FieldPath& path, math::Vector3d& outValue) noexcept;
    bool
    readQuaternion(JsonValue object, std::string_view key, const FieldPath& path, math::Quaternion& outValue) noexcept;
    bool readLens(JsonValue object, std::string_view key, const FieldPath& path, camera::CameraLens& outLens) noexcept;
    bool readInterpolation(JsonValue key, const FieldPath& path, sequencer::SequenceInterpolation& outValue) noexcept;
    bool readWorld(JsonValue root) noexcept;
    bool readBinding(JsonValue value, const FieldPath& path) noexcept;
    bool readTrack(JsonValue value, const FieldPath& path) noexcept;
    bool readKeys(JsonValue keys, const FieldPath& path, SequenceTrackRecord& inoutTrack) noexcept;
    bool readKey(JsonValue key, const FieldPath& path, sequencer::SequenceChannel channel) noexcept;
    bool readSelection(JsonValue root) noexcept;

    SequenceDocument& mDocument;
    SequenceContentError& mError;
};

bool SourceReader::hasOnlyMembers(JsonValue object,
                                  std::initializer_list<std::string_view> allowed,
                                  const FieldPath& path) noexcept
{
    if (object.Object() == false)
    {
        return malformed(path);
    }
    for (usize index = 0; index < object.MemberCount(); ++index)
    {
        std::string_view key;
        JsonValue value;
        if (object.MemberAt(index, key, value) == false)
        {
            return malformed(path);
        }
        bool isKnown = false;
        for (const std::string_view name : allowed)
        {
            isKnown = isKnown == true || name == key;
        }
        if (isKnown == false)
        {
            return malformed(path.Member(key));
        }
    }
    return true;
}

bool SourceReader::readMember(JsonValue object,
                              std::string_view key,
                              const FieldPath& path,
                              JsonValue& outValue) noexcept
{
    const JsonValue value = object.Get(key);
    if (value.Valid() == false)
    {
        return malformed(path.Member(key));
    }
    outValue = value;
    return true;
}

bool SourceReader::readUint64(JsonValue object, std::string_view key, const FieldPath& path, uint64& outValue) noexcept
{
    JsonValue value;
    return readMember(object, key, path, value) == true &&
           (value.Integer(outValue) == true || malformed(path.Member(key)));
}

bool SourceReader::readUint32(JsonValue object, std::string_view key, const FieldPath& path, uint32& outValue) noexcept
{
    uint64 value = 0;
    if (readUint64(object, key, path, value) == false)
    {
        return false;
    }
    if (foundation::TryIntegerCast(value, outValue) == false)
    {
        return fail(SequenceContentStatus::Limit, path.Member(key));
    }
    return true;
}

bool SourceReader::readBoolean(JsonValue object, std::string_view key, const FieldPath& path, bool& outValue) noexcept
{
    JsonValue value;
    return readMember(object, key, path, value) == true &&
           (value.Boolean(outValue) == true || malformed(path.Member(key)));
}

bool SourceReader::readString(JsonValue object,
                              std::string_view key,
                              const FieldPath& path,
                              std::string_view& outValue) noexcept
{
    JsonValue value;
    return readMember(object, key, path, value) == true &&
           (value.String(outValue) == true || malformed(path.Member(key)));
}

template <usize N>
bool SourceReader::readText(JsonValue object, std::string_view key, const FieldPath& path, Text<N>& outValue) noexcept
{
    std::string_view text;
    if (readString(object, key, path, text) == false)
    {
        return false;
    }
    return outValue.Set(text) == true || fail(SequenceContentStatus::Limit, path.Member(key));
}

template <usize Count>
bool SourceReader::readTag(JsonValue object,
                           std::string_view key,
                           const FieldPath& path,
                           const foundation::StaticArray<NamedTag, Count>& tags,
                           foundation::uint8& outValue) noexcept
{
    std::string_view name;
    return readString(object, key, path, name) == true &&
           (TryFindTag(tags, name, outValue) == true || malformed(path.Member(key)));
}

bool SourceReader::readUuid(JsonValue object,
                            std::string_view key,
                            const FieldPath& path,
                            identity::UuidPolicy policy,
                            identity::Uuid& outValue) noexcept
{
    std::string_view text;
    if (readString(object, key, path, text) == false)
    {
        return false;
    }
    if (text.empty() == true && policy == identity::UuidPolicy::AllowNilOrVersion4)
    {
        outValue = {};
        return true;
    }
    identity::Uuid value{};
    if (identity::TryParseUuid(text.data(), text.size(), identity::UuidPolicy::RequireVersion4, value) !=
        identity::UuidStatus::Success)
    {
        return fail(SequenceContentStatus::InvalidIdentity, path.Member(key));
    }
    // Only canonical lowercase text round-trips byte for byte.
    foundation::StaticArray<char, identity::UUID_TEXT_BUFFER_BYTES> canonical{};
    if (identity::FormatUuid(value, canonical.GetData(), canonical.GetSize()) != identity::UuidStatus::Success ||
        std::string_view(canonical.GetData(), identity::UUID_TEXT_LENGTH) != text)
    {
        return fail(SequenceContentStatus::InvalidIdentity, path.Member(key));
    }
    outValue = value;
    return true;
}

bool SourceReader::readFloat64(JsonValue value, const FieldPath& path, foundation::float64& outValue) noexcept
{
    foundation::float64 number = 0;
    if (value.Number(number) == false || math::IsFinite(number) == false)
    {
        return malformed(path);
    }
    outValue = number;
    return true;
}

bool SourceReader::readFloat32(JsonValue value, const FieldPath& path, foundation::float32& outValue) noexcept
{
    foundation::float64 number = 0;
    if (readFloat64(value, path, number) == false)
    {
        return false;
    }
    const foundation::float32 narrowed = static_cast<foundation::float32>(number);
    if (math::IsFinite(narrowed) == false)
    {
        return fail(SequenceContentStatus::Limit, path);
    }
    outValue = narrowed;
    return true;
}

bool SourceReader::readComponents(JsonValue object,
                                  std::string_view key,
                                  const FieldPath& path,
                                  usize count,
                                  JsonValue& outArray) noexcept
{
    JsonValue value;
    if (readMember(object, key, path, value) == false)
    {
        return false;
    }
    if (value.Array() == false || value.Count() != count)
    {
        return malformed(path.Member(key));
    }
    outArray = value;
    return true;
}

bool SourceReader::readVector3(JsonValue object,
                               std::string_view key,
                               const FieldPath& path,
                               math::Vector3& outValue) noexcept
{
    JsonValue components;
    math::Vector3 value{};
    const FieldPath member = path.Member(key);
    if (readComponents(object, key, path, VECTOR3_COMPONENTS, components) == false ||
        readFloat32(components.At(0), member.Element(0), value.X) == false ||
        readFloat32(components.At(1), member.Element(1), value.Y) == false ||
        readFloat32(components.At(2), member.Element(2), value.Z) == false)
    {
        return false;
    }
    outValue = value;
    return true;
}

bool SourceReader::readVector3d(JsonValue object,
                                std::string_view key,
                                const FieldPath& path,
                                math::Vector3d& outValue) noexcept
{
    JsonValue components;
    math::Vector3d value{};
    const FieldPath member = path.Member(key);
    if (readComponents(object, key, path, VECTOR3_COMPONENTS, components) == false ||
        readFloat64(components.At(0), member.Element(0), value.X) == false ||
        readFloat64(components.At(1), member.Element(1), value.Y) == false ||
        readFloat64(components.At(2), member.Element(2), value.Z) == false)
    {
        return false;
    }
    outValue = value;
    return true;
}

bool SourceReader::readQuaternion(JsonValue object,
                                  std::string_view key,
                                  const FieldPath& path,
                                  math::Quaternion& outValue) noexcept
{
    JsonValue components;
    math::Quaternion value{};
    const FieldPath member = path.Member(key);
    if (readComponents(object, key, path, QUATERNION_COMPONENTS, components) == false ||
        readFloat32(components.At(0), member.Element(0), value.X) == false ||
        readFloat32(components.At(1), member.Element(1), value.Y) == false ||
        readFloat32(components.At(2), member.Element(2), value.Z) == false ||
        readFloat32(components.At(QUATERNION_W_INDEX), member.Element(QUATERNION_W_INDEX), value.W) == false)
    {
        return false;
    }
    outValue = value;
    return true;
}

bool SourceReader::readLens(JsonValue object,
                            std::string_view key,
                            const FieldPath& path,
                            camera::CameraLens& outLens) noexcept
{
    JsonValue lens;
    const FieldPath member = path.Member(key);
    if (readMember(object, key, path, lens) == false ||
        hasOnlyMembers(lens,
                       {"projection", "vertical_fov_radians", "vertical_span", "minimum_visible_width", "near", "far"},
                       member) == false)
    {
        return false;
    }
    camera::CameraLens value{};
    foundation::uint8 projection = 0;
    JsonValue field;
    const bool isRead = readTag(lens, "projection", member, PROJECTIONS, projection) == true &&
                        readMember(lens, "vertical_fov_radians", member, field) == true &&
                        readFloat32(field, member.Member("vertical_fov_radians"), value.VerticalFov) == true &&
                        readMember(lens, "vertical_span", member, field) == true &&
                        readFloat32(field, member.Member("vertical_span"), value.VerticalSpan) == true &&
                        readMember(lens, "minimum_visible_width", member, field) == true &&
                        readFloat32(field, member.Member("minimum_visible_width"), value.MinimumVisibleWidth) == true &&
                        readMember(lens, "near", member, field) == true &&
                        readFloat32(field, member.Member("near"), value.NearPlane) == true &&
                        readMember(lens, "far", member, field) == true &&
                        readFloat32(field, member.Member("far"), value.FarPlane) == true;
    if (isRead == false)
    {
        return false;
    }
    value.Projection = static_cast<camera::CameraProjection>(projection);
    outLens = value;
    return true;
}

bool SourceReader::readInterpolation(JsonValue key,
                                     const FieldPath& path,
                                     sequencer::SequenceInterpolation& outValue) noexcept
{
    foundation::uint8 value = 0;
    if (readTag(key, "interpolation", path, INTERPOLATIONS, value) == false)
    {
        return false;
    }
    outValue = static_cast<sequencer::SequenceInterpolation>(value);
    return true;
}

bool SourceReader::readWorld(JsonValue root) noexcept
{
    const FieldPath path("world");
    JsonValue world;
    if (readMember(root, "world", FieldPath("root"), world) == false ||
        hasOnlyMembers(world, {"document", "digest", "path"}, path) == false ||
        readUuid(world, "document", path, identity::UuidPolicy::AllowNilOrVersion4, mDocument.World.Document.Value) ==
            false ||
        readText(world, "path", path, mDocument.WorldPath) == false)
    {
        return false;
    }
    std::string_view digest;
    if (readString(world, "digest", path, digest) == false)
    {
        return false;
    }
    mDocument.World.HasContentDigest = digest.empty() == false;
    if (digest.empty() == true)
    {
        return true;
    }
    if (digest.size() != DIGEST_TEXT_LENGTH)
    {
        return malformed(path.Member("digest"));
    }
    for (usize index = 0; index < sequencer::SEQUENCE_WORLD_DIGEST_BYTES; ++index)
    {
        foundation::uint8 byte = 0;
        for (usize nibble = 0; nibble < 2; ++nibble)
        {
            const usize digit = HEX_DIGITS.find(digest[(index * 2) + nibble]);
            if (digit == std::string_view::npos)
            {
                // Only lowercase hexadecimal is canonical.
                return malformed(path.Member("digest"));
            }
            byte = static_cast<foundation::uint8>((byte << NIBBLE_BITS) | digit);
        }
        mDocument.World.ContentDigest[index] = byte;
    }
    return true;
}

bool SourceReader::readBinding(JsonValue value, const FieldPath& path) noexcept
{
    SequenceBindingRecord binding{};
    foundation::uint8 kind = 0;
    if (value.Object() == false)
    {
        return malformed(path);
    }
    if (readUint64(value, "id", path, binding.BindingId) == false ||
        readText(value, "label", path, binding.Label) == false ||
        readTag(value, "kind", path, BINDING_KINDS, kind) == false)
    {
        return false;
    }
    binding.Kind = static_cast<sequencer::SequenceBindingKind>(kind);
    switch (binding.Kind)
    {
        case sequencer::SequenceBindingKind::WorldObject:
            if (hasOnlyMembers(value, {"id", "label", "kind", "object", "subobject"}, path) == false ||
                readUuid(value, "object", path, identity::UuidPolicy::RequireVersion4, binding.Object.Value) == false ||
                readUint64(value, "subobject", path, binding.SubobjectKey) == false)
            {
                return false;
            }
            break;
        case sequencer::SequenceBindingKind::LegacyWorldObject: {
            std::string_view key;
            if (hasOnlyMembers(value, {"id", "label", "kind", "legacy_id"}, path) == false ||
                readString(value, "legacy_id", path, key) == false)
            {
                return false;
            }
            if (identity_records::IsLegacyId(key) == false || binding.LegacyKey.Set(key) == false)
            {
                return fail(SequenceContentStatus::InvalidLegacyKey, path.Member("legacy_id"));
            }
            break;
        }
        case sequencer::SequenceBindingKind::SequenceCamera: {
            JsonValue cameraValue;
            const FieldPath member = path.Member("camera");
            if (hasOnlyMembers(value, {"id", "label", "kind", "camera"}, path) == false ||
                readMember(value, "camera", path, cameraValue) == false ||
                hasOnlyMembers(cameraValue, {"position", "rotation", "lens", "output", "revision"}, member) == false ||
                readVector3d(cameraValue, "position", member, binding.Camera.Pose.Position) == false ||
                readQuaternion(cameraValue, "rotation", member, binding.Camera.Pose.Orientation) == false ||
                readLens(cameraValue, "lens", member, binding.Camera.Lens) == false ||
                readUint64(cameraValue, "output", member, binding.Camera.OutputId) == false ||
                readUint64(cameraValue, "revision", member, binding.Camera.DefinitionRevision) == false)
            {
                return false;
            }
            break;
        }
    }
    if (mDocument.Bindings.TryAdd(binding) == false)
    {
        return fail(SequenceContentStatus::OutOfMemory, path);
    }
    return true;
}

bool SourceReader::readKey(JsonValue key, const FieldPath& path, sequencer::SequenceChannel channel) noexcept
{
    uint64 tick = 0;
    if (readUint64(key, "tick", path, tick) == false)
    {
        return false;
    }
    bool isAdded = false;
    switch (channel)
    {
        case sequencer::SequenceChannel::Translation:
        case sequencer::SequenceChannel::Scale: {
            sequencer::SequenceVectorKey value{ .Tick = tick };
            if (hasOnlyMembers(key, {"tick", "interpolation", "value"}, path) == false ||
                readInterpolation(key, path, value.Interpolation) == false ||
                readVector3(key, "value", path, value.Value) == false)
            {
                return false;
            }
            isAdded = mDocument.VectorKeys.TryAdd(value);
            break;
        }
        case sequencer::SequenceChannel::Rotation:
        case sequencer::SequenceChannel::CameraRotation: {
            sequencer::SequenceRotationKey value{ .Tick = tick };
            if (hasOnlyMembers(key, {"tick", "interpolation", "value"}, path) == false ||
                readInterpolation(key, path, value.Interpolation) == false ||
                readQuaternion(key, "value", path, value.Value) == false)
            {
                return false;
            }
            isAdded = mDocument.RotationKeys.TryAdd(value);
            break;
        }
        case sequencer::SequenceChannel::CameraPosition: {
            sequencer::SequencePositionKey value{ .Tick = tick };
            if (hasOnlyMembers(key, {"tick", "interpolation", "value"}, path) == false ||
                readInterpolation(key, path, value.Interpolation) == false ||
                readVector3d(key, "value", path, value.PositionWorld) == false)
            {
                return false;
            }
            isAdded = mDocument.PositionKeys.TryAdd(value);
            break;
        }
        case sequencer::SequenceChannel::Visibility: {
            sequencer::SequenceVisibilityKey value{ .Tick = tick };
            if (hasOnlyMembers(key, {"tick", "visible"}, path) == false ||
                readBoolean(key, "visible", path, value.IsVisible) == false)
            {
                return false;
            }
            isAdded = mDocument.VisibilityKeys.TryAdd(value);
            break;
        }
        case sequencer::SequenceChannel::CameraLens: {
            sequencer::SequenceLensKey value{ .Tick = tick };
            if (hasOnlyMembers(key, {"tick", "interpolation", "value"}, path) == false ||
                readInterpolation(key, path, value.Interpolation) == false ||
                readLens(key, "value", path, value.Value) == false)
            {
                return false;
            }
            isAdded = mDocument.LensKeys.TryAdd(value);
            break;
        }
    }
    return isAdded == true || fail(SequenceContentStatus::OutOfMemory, path);
}

usize GetChannelKeyArraySize(const SequenceDocument& document, sequencer::SequenceChannel channel) noexcept
{
    switch (channel)
    {
        case sequencer::SequenceChannel::Translation:
        case sequencer::SequenceChannel::Scale:
            return document.VectorKeys.GetSize();
        case sequencer::SequenceChannel::Rotation:
        case sequencer::SequenceChannel::CameraRotation:
            return document.RotationKeys.GetSize();
        case sequencer::SequenceChannel::CameraPosition:
            return document.PositionKeys.GetSize();
        case sequencer::SequenceChannel::Visibility:
            return document.VisibilityKeys.GetSize();
        case sequencer::SequenceChannel::CameraLens:
            return document.LensKeys.GetSize();
    }
    return 0;
}

bool SourceReader::readKeys(JsonValue keys, const FieldPath& path, SequenceTrackRecord& inoutTrack) noexcept
{
    if (keys.Array() == false)
    {
        return malformed(path);
    }
    inoutTrack.KeyOffset = GetChannelKeyArraySize(mDocument, inoutTrack.Channel);
    inoutTrack.KeyCount = keys.Count();
    for (usize index = 0; index < keys.Count(); ++index)
    {
        if (readKey(keys.At(index), path.Element(index), inoutTrack.Channel) == false)
        {
            return false;
        }
    }
    return true;
}

bool SourceReader::readTrack(JsonValue value, const FieldPath& path) noexcept
{
    SequenceTrackRecord track{};
    foundation::uint8 channel = 0;
    JsonValue keys;
    if (hasOnlyMembers(value, {"id", "label", "binding", "channel", "enabled", "keys"}, path) == false ||
        readUint64(value, "id", path, track.TrackId) == false || readText(value, "label", path, track.Label) == false ||
        readUint64(value, "binding", path, track.BindingId) == false ||
        readTag(value, "channel", path, CHANNELS, channel) == false ||
        readBoolean(value, "enabled", path, track.IsEnabled) == false || readMember(value, "keys", path, keys) == false)
    {
        return false;
    }
    track.Channel = static_cast<sequencer::SequenceChannel>(channel);
    if (readKeys(keys, path.Member("keys"), track) == false)
    {
        return false;
    }
    if (mDocument.Tracks.TryAdd(track) == false)
    {
        return fail(SequenceContentStatus::OutOfMemory, path);
    }
    return true;
}

bool SourceReader::readSelection(JsonValue root) noexcept
{
    const FieldPath path("camera_selection");
    JsonValue intervals;
    if (readMember(root, "camera_selection", FieldPath("root"), intervals) == false)
    {
        return false;
    }
    if (intervals.Array() == false)
    {
        return malformed(path);
    }
    if (intervals.Count() > sequence_format::MAX_INTERVALS)
    {
        return fail(SequenceContentStatus::Limit, path);
    }
    if (mDocument.CameraSelection.TryEnsureCapacity(intervals.Count()) == false)
    {
        return fail(SequenceContentStatus::OutOfMemory, path);
    }
    for (usize index = 0; index < intervals.Count(); ++index)
    {
        const JsonValue value = intervals.At(index);
        const FieldPath element = path.Element(index);
        sequencer::SequenceCameraInterval interval{};
        if (hasOnlyMembers(value, {"start", "end", "camera"}, element) == false ||
            readUint64(value, "start", element, interval.StartTick) == false ||
            readUint64(value, "end", element, interval.EndTick) == false ||
            readUint64(value, "camera", element, interval.CameraBindingId) == false)
        {
            return false;
        }
        [[maybe_unused]] const bool isAdded = mDocument.CameraSelection.TryAdd(interval);
        LUDUS_ASSERT(isAdded == true);
    }
    return true;
}

SequenceContentStatus SourceReader::ReadRoot(JsonValue root) noexcept
{
    const FieldPath path("root");
    std::string_view formatName;
    uint64 version = 0;
    if (hasOnlyMembers(root,
                       {"format",
                        "version",
                        "sequence",
                        "world",
                        "ticks_per_second",
                        "playback",
                        "bindings",
                        "tracks",
                        "camera_selection"},
                       path) == false ||
        readString(root, "format", path, formatName) == false || readUint64(root, "version", path, version) == false)
    {
        return mError.Status;
    }
    if (formatName != SOURCE_FORMAT || version != SOURCE_VERSION_1)
    {
        return Fail(mError,
                    SequenceContentStatus::UnsupportedVersion,
                    formatName != SOURCE_FORMAT ? "format" : "version");
    }
    JsonValue playback;
    const FieldPath playbackPath("playback");
    if (readUuid(root, "sequence", path, identity::UuidPolicy::RequireVersion4, mDocument.Sequence.Value) == false ||
        readWorld(root) == false || readUint32(root, "ticks_per_second", path, mDocument.TicksPerSecond) == false ||
        readMember(root, "playback", path, playback) == false ||
        hasOnlyMembers(playback, {"start", "end"}, playbackPath) == false ||
        readUint64(playback, "start", playbackPath, mDocument.PlaybackStartTick) == false ||
        readUint64(playback, "end", playbackPath, mDocument.PlaybackEndTick) == false)
    {
        return mError.Status;
    }
    JsonValue bindings;
    JsonValue tracks;
    if (readMember(root, "bindings", path, bindings) == false || readMember(root, "tracks", path, tracks) == false)
    {
        return mError.Status;
    }
    if (bindings.Array() == false || tracks.Array() == false)
    {
        return Fail(mError, SequenceContentStatus::Malformed, bindings.Array() == false ? "bindings" : "tracks");
    }
    if (bindings.Count() > sequence_format::MAX_BINDINGS || tracks.Count() > sequence_format::MAX_TRACKS)
    {
        return Fail(mError,
                    SequenceContentStatus::Limit,
                    bindings.Count() > sequence_format::MAX_BINDINGS ? "bindings" : "tracks");
    }
    if (mDocument.Bindings.TryEnsureCapacity(bindings.Count()) == false ||
        mDocument.Tracks.TryEnsureCapacity(tracks.Count()) == false)
    {
        return Fail(mError, SequenceContentStatus::OutOfMemory, "bindings");
    }
    for (usize index = 0; index < bindings.Count(); ++index)
    {
        if (readBinding(bindings.At(index), FieldPath("bindings").Element(index)) == false)
        {
            return mError.Status;
        }
    }
    for (usize index = 0; index < tracks.Count(); ++index)
    {
        if (readTrack(tracks.At(index), FieldPath("tracks").Element(index)) == false)
        {
            return mError.Status;
        }
    }
    if (readSelection(root) == false)
    {
        return mError.Status;
    }
    return SequenceContentStatus::Success;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/// Canonical JSON: two-space indentation and one member or element per line,
/// except inline containers (numeric vectors and keys), which stay on one line.
class SourceWriter final
{
public:
    explicit SourceWriter(std::span<foundation::uint8> scratch) noexcept : mWriter(scratch) {}

    void Open(char bracket, bool isInline = false) noexcept
    {
        mWriter.Raw({&bracket, 1});
        ++mDepth;
        LUDUS_ASSERT(mDepth < MAX_WRITER_DEPTH);
        mIsFirst[mDepth] = true;
        mIsInline[mDepth] = isInline == true || mIsInline[mDepth - 1] == true;
    }
    void Close(char bracket) noexcept
    {
        const bool isInline = mIsInline[mDepth];
        const bool isEmpty = mIsFirst[mDepth];
        --mDepth;
        if (isInline == false && isEmpty == false)
        {
            newline();
        }
        mWriter.Raw({&bracket, 1});
    }
    void Member(std::string_view key) noexcept
    {
        separate();
        mWriter.String(key);
        mWriter.Raw(": ");
    }
    void Element() noexcept
    {
        separate();
    }
    void String(std::string_view value) noexcept
    {
        mWriter.String(value);
    }
    void Integer(uint64 value) noexcept
    {
        mWriter.Integer(value);
    }
    void Number(foundation::float64 value) noexcept
    {
        mWriter.Number(value);
    }
    void Boolean(bool isTrue) noexcept
    {
        mWriter.Boolean(isTrue);
    }
    [[nodiscard]] parsing::ParseStatus Finish(std::span<const foundation::uint8>& outText) noexcept
    {
        return mWriter.Finish(outText);
    }

private:
    void separate() noexcept
    {
        if (mIsFirst[mDepth] == false)
        {
            mWriter.Raw(",");
            if (mIsInline[mDepth] == true)
            {
                mWriter.Raw(" ");
            }
        }
        mIsFirst[mDepth] = false;
        if (mIsInline[mDepth] == false)
        {
            newline();
        }
    }
    void newline() noexcept
    {
        mWriter.Raw("\n");
        for (usize level = 0; level < mDepth; ++level)
        {
            mWriter.Raw("  ");
        }
    }

    parsing::JsonWriter mWriter;
    usize mDepth = 0;
    foundation::StaticArray<bool, MAX_WRITER_DEPTH> mIsFirst{};
    foundation::StaticArray<bool, MAX_WRITER_DEPTH> mIsInline{};
};

void WriteUuid(SourceWriter& inoutWriter, const identity::Uuid& value) noexcept
{
    if (value.IsNil() == true)
    {
        inoutWriter.String({});
        return;
    }
    foundation::StaticArray<char, identity::UUID_TEXT_BUFFER_BYTES> text{};
    [[maybe_unused]] const identity::UuidStatus status = identity::FormatUuid(value, text.GetData(), text.GetSize());
    LUDUS_ASSERT(status == identity::UuidStatus::Success);
    inoutWriter.String({text.GetData(), identity::UUID_TEXT_LENGTH});
}

void WriteVector3(SourceWriter& inoutWriter, math::Vector3 value) noexcept
{
    inoutWriter.Open('[', true);
    for (const foundation::float32 component : {value.X, value.Y, value.Z})
    {
        inoutWriter.Element();
        inoutWriter.Number(static_cast<foundation::float64>(component));
    }
    inoutWriter.Close(']');
}

void WriteVector3d(SourceWriter& inoutWriter, math::Vector3d value) noexcept
{
    inoutWriter.Open('[', true);
    for (const foundation::float64 component : {value.X, value.Y, value.Z})
    {
        inoutWriter.Element();
        inoutWriter.Number(static_cast<foundation::float64>(component));
    }
    inoutWriter.Close(']');
}

void WriteQuaternion(SourceWriter& inoutWriter, math::Quaternion value) noexcept
{
    inoutWriter.Open('[', true);
    for (const foundation::float32 component : {value.X, value.Y, value.Z, value.W})
    {
        inoutWriter.Element();
        inoutWriter.Number(static_cast<foundation::float64>(component));
    }
    inoutWriter.Close(']');
}

void WriteLens(SourceWriter& inoutWriter, const camera::CameraLens& lens, bool isInline) noexcept
{
    inoutWriter.Open('{', isInline);
    inoutWriter.Member("projection");
    inoutWriter.String(GetTagName(PROJECTIONS, static_cast<foundation::uint8>(lens.Projection)));
    inoutWriter.Member("vertical_fov_radians");
    inoutWriter.Number(static_cast<foundation::float64>(lens.VerticalFov));
    inoutWriter.Member("vertical_span");
    inoutWriter.Number(static_cast<foundation::float64>(lens.VerticalSpan));
    inoutWriter.Member("minimum_visible_width");
    inoutWriter.Number(static_cast<foundation::float64>(lens.MinimumVisibleWidth));
    inoutWriter.Member("near");
    inoutWriter.Number(static_cast<foundation::float64>(lens.NearPlane));
    inoutWriter.Member("far");
    inoutWriter.Number(static_cast<foundation::float64>(lens.FarPlane));
    inoutWriter.Close('}');
}

void WriteInterpolation(SourceWriter& inoutWriter, sequencer::SequenceInterpolation interpolation) noexcept
{
    inoutWriter.Member("interpolation");
    inoutWriter.String(GetTagName(INTERPOLATIONS, static_cast<foundation::uint8>(interpolation)));
}

void WriteDigest(SourceWriter& inoutWriter, const sequencer::SequenceWorldReference& world) noexcept
{
    if (world.HasContentDigest == false)
    {
        inoutWriter.String({});
        return;
    }
    foundation::StaticArray<char, DIGEST_TEXT_LENGTH> text{};
    for (usize index = 0; index < sequencer::SEQUENCE_WORLD_DIGEST_BYTES; ++index)
    {
        const foundation::uint8 byte = world.ContentDigest[index];
        text[index * 2] = HEX_DIGITS[byte >> NIBBLE_BITS];
        text[(index * 2) + 1] = HEX_DIGITS[byte & NIBBLE_MASK];
    }
    inoutWriter.String({text.GetData(), DIGEST_TEXT_LENGTH});
}

void WriteBinding(SourceWriter& inoutWriter, const SequenceBindingRecord& binding) noexcept
{
    inoutWriter.Element();
    inoutWriter.Open('{');
    inoutWriter.Member("id");
    inoutWriter.Integer(binding.BindingId);
    inoutWriter.Member("label");
    inoutWriter.String(binding.Label.View());
    inoutWriter.Member("kind");
    inoutWriter.String(GetTagName(BINDING_KINDS, static_cast<foundation::uint8>(binding.Kind)));
    switch (binding.Kind)
    {
        case sequencer::SequenceBindingKind::WorldObject:
            inoutWriter.Member("object");
            WriteUuid(inoutWriter, binding.Object.Value);
            inoutWriter.Member("subobject");
            inoutWriter.Integer(binding.SubobjectKey);
            break;
        case sequencer::SequenceBindingKind::LegacyWorldObject:
            inoutWriter.Member("legacy_id");
            inoutWriter.String(binding.LegacyKey.View());
            break;
        case sequencer::SequenceBindingKind::SequenceCamera:
            inoutWriter.Member("camera");
            inoutWriter.Open('{');
            inoutWriter.Member("position");
            WriteVector3d(inoutWriter, binding.Camera.Pose.Position);
            inoutWriter.Member("rotation");
            WriteQuaternion(inoutWriter, binding.Camera.Pose.Orientation);
            inoutWriter.Member("lens");
            WriteLens(inoutWriter, binding.Camera.Lens, false);
            inoutWriter.Member("output");
            inoutWriter.Integer(binding.Camera.OutputId);
            inoutWriter.Member("revision");
            inoutWriter.Integer(binding.Camera.DefinitionRevision);
            inoutWriter.Close('}');
            break;
    }
    inoutWriter.Close('}');
}

void WriteKeys(SourceWriter& inoutWriter, const SequenceDocument& document, const SequenceTrackRecord& track) noexcept
{
    for (usize index = track.KeyOffset; index < track.KeyOffset + track.KeyCount; ++index)
    {
        inoutWriter.Element();
        inoutWriter.Open('{', true);
        switch (track.Channel)
        {
            case sequencer::SequenceChannel::Translation:
            case sequencer::SequenceChannel::Scale:
                inoutWriter.Member("tick");
                inoutWriter.Integer(document.VectorKeys[index].Tick);
                WriteInterpolation(inoutWriter, document.VectorKeys[index].Interpolation);
                inoutWriter.Member("value");
                WriteVector3(inoutWriter, document.VectorKeys[index].Value);
                break;
            case sequencer::SequenceChannel::Rotation:
            case sequencer::SequenceChannel::CameraRotation:
                inoutWriter.Member("tick");
                inoutWriter.Integer(document.RotationKeys[index].Tick);
                WriteInterpolation(inoutWriter, document.RotationKeys[index].Interpolation);
                inoutWriter.Member("value");
                WriteQuaternion(inoutWriter, document.RotationKeys[index].Value);
                break;
            case sequencer::SequenceChannel::CameraPosition:
                inoutWriter.Member("tick");
                inoutWriter.Integer(document.PositionKeys[index].Tick);
                WriteInterpolation(inoutWriter, document.PositionKeys[index].Interpolation);
                inoutWriter.Member("value");
                WriteVector3d(inoutWriter, document.PositionKeys[index].PositionWorld);
                break;
            case sequencer::SequenceChannel::Visibility:
                inoutWriter.Member("tick");
                inoutWriter.Integer(document.VisibilityKeys[index].Tick);
                inoutWriter.Member("visible");
                inoutWriter.Boolean(document.VisibilityKeys[index].IsVisible);
                break;
            case sequencer::SequenceChannel::CameraLens:
                inoutWriter.Member("tick");
                inoutWriter.Integer(document.LensKeys[index].Tick);
                WriteInterpolation(inoutWriter, document.LensKeys[index].Interpolation);
                inoutWriter.Member("value");
                WriteLens(inoutWriter, document.LensKeys[index].Value, true);
                break;
        }
        inoutWriter.Close('}');
    }
}

void WriteDocument(SourceWriter& inoutWriter, const SequenceDocument& document) noexcept
{
    inoutWriter.Open('{');
    inoutWriter.Member("format");
    inoutWriter.String(SOURCE_FORMAT);
    inoutWriter.Member("version");
    inoutWriter.Integer(SOURCE_VERSION_1);
    inoutWriter.Member("sequence");
    WriteUuid(inoutWriter, document.Sequence.Value);
    inoutWriter.Member("world");
    inoutWriter.Open('{');
    inoutWriter.Member("document");
    WriteUuid(inoutWriter, document.World.Document.Value);
    inoutWriter.Member("digest");
    WriteDigest(inoutWriter, document.World);
    inoutWriter.Member("path");
    inoutWriter.String(document.WorldPath.View());
    inoutWriter.Close('}');
    inoutWriter.Member("ticks_per_second");
    inoutWriter.Integer(document.TicksPerSecond);
    inoutWriter.Member("playback");
    inoutWriter.Open('{', true);
    inoutWriter.Member("start");
    inoutWriter.Integer(document.PlaybackStartTick);
    inoutWriter.Member("end");
    inoutWriter.Integer(document.PlaybackEndTick);
    inoutWriter.Close('}');
    inoutWriter.Member("bindings");
    inoutWriter.Open('[');
    for (const SequenceBindingRecord& binding : document.Bindings)
    {
        WriteBinding(inoutWriter, binding);
    }
    inoutWriter.Close(']');
    inoutWriter.Member("tracks");
    inoutWriter.Open('[');
    for (const SequenceTrackRecord& track : document.Tracks)
    {
        inoutWriter.Element();
        inoutWriter.Open('{');
        inoutWriter.Member("id");
        inoutWriter.Integer(track.TrackId);
        inoutWriter.Member("label");
        inoutWriter.String(track.Label.View());
        inoutWriter.Member("binding");
        inoutWriter.Integer(track.BindingId);
        inoutWriter.Member("channel");
        inoutWriter.String(GetTagName(CHANNELS, static_cast<foundation::uint8>(track.Channel)));
        inoutWriter.Member("enabled");
        inoutWriter.Boolean(track.IsEnabled);
        inoutWriter.Member("keys");
        inoutWriter.Open('[');
        WriteKeys(inoutWriter, document, track);
        inoutWriter.Close(']');
        inoutWriter.Close('}');
    }
    inoutWriter.Close(']');
    inoutWriter.Member("camera_selection");
    inoutWriter.Open('[');
    for (const sequencer::SequenceCameraInterval& interval : document.CameraSelection)
    {
        inoutWriter.Element();
        inoutWriter.Open('{', true);
        inoutWriter.Member("start");
        inoutWriter.Integer(interval.StartTick);
        inoutWriter.Member("end");
        inoutWriter.Integer(interval.EndTick);
        inoutWriter.Member("camera");
        inoutWriter.Integer(interval.CameraBindingId);
        inoutWriter.Close('}');
    }
    inoutWriter.Close(']');
    inoutWriter.Close('}');
}
} // namespace

SequenceContentStatus
TryReadSequenceSource(std::string_view json, SequenceDocument& outDocument, SequenceContentError& outError) noexcept
{
    outError = {};
    if (json.size() > MAX_SOURCE_BYTES)
    {
        return Fail(outError, SequenceContentStatus::Limit, "document");
    }
    parsing::JsonLimits limits;
    limits.MaxInputBytes = MAX_SOURCE_BYTES;
    limits.MaxWorkspaceBytes = MAX_SOURCE_WORKSPACE_BYTES;
    limits.MaxDepth = MAX_SOURCE_DEPTH;
    limits.MaxObjectMembers = parsing::MAX_JSON_OBJECT_MEMBERS;
    limits.MaxArrayElements = MAX_SOURCE_ARRAY_ELEMENTS;
    limits.MaxStringBytes = MAX_RESOURCE_PATH_BYTES;
    parsing::JsonDocument parsed;
    parsing::ParseError parseError;
    const parsing::ParseStatus status = parsed.Read(json, parseError, limits);
    if (status != parsing::ParseStatus::Ok)
    {
        outError.Offset = parseError.Offset;
        const SequenceContentStatus failure =
            status == parsing::ParseStatus::OutOfMemory
                ? SequenceContentStatus::OutOfMemory
                : (status == parsing::ParseStatus::LimitExceeded ? SequenceContentStatus::Limit
                                                                 : SequenceContentStatus::Malformed);
        return Fail(outError, failure, "document");
    }
    SequenceDocument candidate;
    SourceReader reader(candidate, outError);
    const SequenceContentStatus read = reader.ReadRoot(parsed.Root());
    if (read != SequenceContentStatus::Success)
    {
        return read;
    }
    const SequenceContentStatus valid = internal::ValidateDocument(candidate, outError);
    if (valid != SequenceContentStatus::Success)
    {
        return valid;
    }
    outDocument = foundation::Move(candidate);
    return SequenceContentStatus::Success;
}

SequenceContentStatus
TryWriteSequenceSource(const SequenceDocument& document, Bytes& outBytes, SequenceContentError& outError) noexcept
{
    outError = {};
    const SequenceContentStatus valid = internal::ValidateDocument(document, outError);
    if (valid != SequenceContentStatus::Success)
    {
        return valid;
    }
    Bytes scratch;
    if (scratch.Resize(MAX_SOURCE_BYTES) == false)
    {
        return Fail(outError, SequenceContentStatus::OutOfMemory, "document");
    }
    SourceWriter writer(scratch.Data());
    WriteDocument(writer, document);
    std::span<const foundation::uint8> text;
    const parsing::ParseStatus status = writer.Finish(text);
    if (status != parsing::ParseStatus::Ok)
    {
        return Fail(outError,
                    status == parsing::ParseStatus::OutOfMemory ? SequenceContentStatus::OutOfMemory
                                                                : SequenceContentStatus::Limit,
                    "document");
    }
    Bytes result;
    if (result.Resize(text.size()) == false)
    {
        return Fail(outError, SequenceContentStatus::OutOfMemory, "document");
    }
    for (usize index = 0; index < text.size(); ++index)
    {
        result.Data()[index] = text[index];
    }
    outBytes = foundation::Move(result);
    return SequenceContentStatus::Success;
}
} // namespace ludus::content::sequence_content
