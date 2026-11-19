#pragma once

#include <ludus/foundation/base/types.h>

#include <string_view>

/// @file
/// LUSEQ cooked sequence format (Ludus::SequenceFormat).
///
/// This header is the single contract between the sequence cooker and the
/// validating decoder in Ludus::SequenceContent. It is owned by neither side and
/// depends on nothing but FoundationBase. See
/// docs/architecture/sequencer-cinematics.md, "Sequence assets and binding resolution".
///
/// A sequence is little-endian and tightly packed, in this order:
/// a HEADER_BYTES header; BindingCount binding records; TrackCount track
/// records; IntervalCount camera interval records; then each track's keys in
/// track order, KeyCount records of the width that matches its channel. The
/// total size is exact: trailing bytes are an error. Offsets below are derived
/// from the field widths, never from host structs, so host endianness and padding
/// never leak into files. Floating-point fields store IEEE-754 bit patterns.
///
/// Version 1 is canonical. Unused fields of a binding kind are zero, the legacy
/// key field is zero-filled after its length, and reserved words are zero, so
/// equal sequences encode to equal bytes. Authoring labels and the World source
/// path belong to the source document and are not cooked.
namespace ludus::content::sequence_format
{
using foundation::core::uint32;
using foundation::core::uint8;
using foundation::core::usize;

/// Eight-byte file magic, including the trailing NUL.
inline constexpr std::string_view MAGIC{"LUSEQ01\0", 8};
/// Version 1: durable bindings, channel tracks, camera selection and Step/Linear keys.
inline constexpr uint32 VERSION_1 = 1;
/// Version produced by the current cooker.
inline constexpr uint32 CURRENT_VERSION = VERSION_1;

/// Width of one canonical UUID field.
inline constexpr usize UUID_BYTES = 16;
/// Width of the pinned World content digest (SHA-256).
inline constexpr usize WORLD_DIGEST_BYTES = 32;
/// Width of the zero-filled legacy entity key field.
inline constexpr usize LEGACY_KEY_BYTES = 64;

/// Header World flags: the World content digest is pinned.
inline constexpr uint32 WORLD_FLAG_CONTENT_DIGEST = 1;
/// Every World flag defined by version 1.
inline constexpr uint32 WORLD_FLAGS_V1 = WORLD_FLAG_CONTENT_DIGEST;

/// Binding kind: a World object named by source-object UUID and subobject key.
inline constexpr uint32 BINDING_WORLD_OBJECT = 1;
/// Binding kind: a camera definition owned by the sequence.
inline constexpr uint32 BINDING_SEQUENCE_CAMERA = 2;
/// Binding kind: a version-1 World entity named by its scoped key.
inline constexpr uint32 BINDING_LEGACY_WORLD_OBJECT = 3;

/// Channel: object parent-local translation (vector keys).
inline constexpr uint32 CHANNEL_TRANSLATION = 1;
/// Channel: object local-to-parent rotation (rotation keys).
inline constexpr uint32 CHANNEL_ROTATION = 2;
/// Channel: object parent-local scale (vector keys).
inline constexpr uint32 CHANNEL_SCALE = 3;
/// Channel: object render inclusion (visibility keys).
inline constexpr uint32 CHANNEL_VISIBILITY = 4;
/// Channel: sequence camera float64 world position (position keys).
inline constexpr uint32 CHANNEL_CAMERA_POSITION = 5;
/// Channel: sequence camera local-to-world orientation (rotation keys).
inline constexpr uint32 CHANNEL_CAMERA_ROTATION = 6;
/// Channel: sequence camera lens (lens keys).
inline constexpr uint32 CHANNEL_CAMERA_LENS = 7;

/// Track flags: the track writes its channel.
inline constexpr uint32 TRACK_FLAG_ENABLED = 1;
/// Every track flag defined by version 1.
inline constexpr uint32 TRACK_FLAGS_V1 = TRACK_FLAG_ENABLED;

/// Interpolation: hold the left key until the next key.
inline constexpr uint32 INTERPOLATION_STEP = 1;
/// Interpolation: linear values and shortest-path spherical rotations.
inline constexpr uint32 INTERPOLATION_LINEAR = 2;

/// Lens projection: perspective with a finite far plane.
inline constexpr uint32 PROJECTION_PERSPECTIVE_FINITE = 1;
/// Lens projection: perspective without a far plane.
inline constexpr uint32 PROJECTION_PERSPECTIVE_INFINITE = 2;
/// Lens projection: orthographic with a finite far plane.
inline constexpr uint32 PROJECTION_ORTHOGRAPHIC_FINITE = 3;

/// Largest binding count.
inline constexpr uint32 MAX_BINDINGS = 256;
/// Largest track count, including disabled tracks.
inline constexpr uint32 MAX_TRACKS = 1024;
/// Largest camera interval count.
inline constexpr uint32 MAX_INTERVALS = 256;
/// Largest total key count over every track.
inline constexpr uint32 MAX_KEYS = 65536;

// Header fields.
/// Header field: MAGIC.size() bytes.
inline constexpr usize MAGIC_OFFSET = 0;
/// Header field: uint32 format version.
inline constexpr usize VERSION_OFFSET = MAGIC_OFFSET + MAGIC.size();
/// Header field: uint32 reserved word; must be zero.
inline constexpr usize RESERVED_OFFSET = VERSION_OFFSET + sizeof(uint32);
/// Header field: UUID_BYTES sequence asset identity.
inline constexpr usize SEQUENCE_ID_OFFSET = RESERVED_OFFSET + sizeof(uint32);
/// Header field: UUID_BYTES World document identity; all zero when absent.
inline constexpr usize WORLD_DOCUMENT_OFFSET = SEQUENCE_ID_OFFSET + UUID_BYTES;
/// Header field: WORLD_DIGEST_BYTES pinned World digest; all zero unless pinned.
inline constexpr usize WORLD_DIGEST_OFFSET = WORLD_DOCUMENT_OFFSET + UUID_BYTES;
/// Header field: uint32 World flags, a subset of WORLD_FLAGS_V1.
inline constexpr usize WORLD_FLAGS_OFFSET = WORLD_DIGEST_OFFSET + WORLD_DIGEST_BYTES;
/// Header field: uint32 positive ticks per second.
inline constexpr usize TICKS_PER_SECOND_OFFSET = WORLD_FLAGS_OFFSET + sizeof(uint32);
/// Header field: uint64 inclusive playback start tick.
inline constexpr usize PLAYBACK_START_OFFSET = TICKS_PER_SECOND_OFFSET + sizeof(uint32);
/// Header field: uint64 exclusive playback end tick.
inline constexpr usize PLAYBACK_END_OFFSET = PLAYBACK_START_OFFSET + sizeof(foundation::core::uint64);
/// Header field: uint32 binding count, at most MAX_BINDINGS.
inline constexpr usize BINDING_COUNT_OFFSET = PLAYBACK_END_OFFSET + sizeof(foundation::core::uint64);
/// Header field: uint32 track count, at most MAX_TRACKS.
inline constexpr usize TRACK_COUNT_OFFSET = BINDING_COUNT_OFFSET + sizeof(uint32);
/// Header field: uint32 camera interval count, at most MAX_INTERVALS.
inline constexpr usize INTERVAL_COUNT_OFFSET = TRACK_COUNT_OFFSET + sizeof(uint32);
/// Header field: uint32 total key count, at most MAX_KEYS; equals the sum of track key counts.
inline constexpr usize KEY_COUNT_OFFSET = INTERVAL_COUNT_OFFSET + sizeof(uint32);
/// Header size; binding records start here.
inline constexpr usize HEADER_BYTES = KEY_COUNT_OFFSET + sizeof(uint32);

// Shared value groups.
/// Width of a Vector3 of three float32 components.
inline constexpr usize VECTOR3_BYTES = 3 * sizeof(foundation::core::float32);
/// Width of a Vector3d of three float64 components.
inline constexpr usize VECTOR3D_BYTES = 3 * sizeof(foundation::core::float64);
/// Width of a quaternion of four float32 components in (x, y, z, w) order.
inline constexpr usize QUATERNION_BYTES = 4 * sizeof(foundation::core::float32);
/// Lens field: uint32 projection tag.
inline constexpr usize LENS_PROJECTION_FIELD = 0;
/// Lens field: float32 vertical field of view in radians.
inline constexpr usize LENS_VERTICAL_FOV_FIELD = LENS_PROJECTION_FIELD + sizeof(uint32);
/// Lens field: float32 orthographic vertical span in world units.
inline constexpr usize LENS_VERTICAL_SPAN_FIELD = LENS_VERTICAL_FOV_FIELD + sizeof(foundation::core::float32);
/// Lens field: float32 orthographic minimum visible width in world units.
inline constexpr usize LENS_MINIMUM_WIDTH_FIELD = LENS_VERTICAL_SPAN_FIELD + sizeof(foundation::core::float32);
/// Lens field: float32 near plane in world units.
inline constexpr usize LENS_NEAR_FIELD = LENS_MINIMUM_WIDTH_FIELD + sizeof(foundation::core::float32);
/// Lens field: float32 far plane in world units.
inline constexpr usize LENS_FAR_FIELD = LENS_NEAR_FIELD + sizeof(foundation::core::float32);
/// Width of one lens value.
inline constexpr usize LENS_BYTES = LENS_FAR_FIELD + sizeof(foundation::core::float32);

// Binding records.
/// Binding field: uint64 nonzero binding identity.
inline constexpr usize BINDING_ID_FIELD = 0;
/// Binding field: uint32 binding kind tag.
inline constexpr usize BINDING_KIND_FIELD = BINDING_ID_FIELD + sizeof(foundation::core::uint64);
/// Binding field: uint32 legacy key length; zero unless BINDING_LEGACY_WORLD_OBJECT.
inline constexpr usize BINDING_LEGACY_KEY_LENGTH_FIELD = BINDING_KIND_FIELD + sizeof(uint32);
/// Binding field: UUID_BYTES source-object identity; zero unless BINDING_WORLD_OBJECT.
inline constexpr usize BINDING_OBJECT_FIELD = BINDING_LEGACY_KEY_LENGTH_FIELD + sizeof(uint32);
/// Binding field: uint64 subobject key; zero unless BINDING_WORLD_OBJECT.
inline constexpr usize BINDING_SUBOBJECT_FIELD = BINDING_OBJECT_FIELD + UUID_BYTES;
/// Binding field: LEGACY_KEY_BYTES zero-filled legacy key bytes.
inline constexpr usize BINDING_LEGACY_KEY_FIELD = BINDING_SUBOBJECT_FIELD + sizeof(foundation::core::uint64);
/// Binding field: VECTOR3D_BYTES camera world position; zero unless BINDING_SEQUENCE_CAMERA.
inline constexpr usize BINDING_CAMERA_POSITION_FIELD = BINDING_LEGACY_KEY_FIELD + LEGACY_KEY_BYTES;
/// Binding field: QUATERNION_BYTES camera orientation; zero unless BINDING_SEQUENCE_CAMERA.
inline constexpr usize BINDING_CAMERA_ROTATION_FIELD = BINDING_CAMERA_POSITION_FIELD + VECTOR3D_BYTES;
/// Binding field: LENS_BYTES camera lens; zero unless BINDING_SEQUENCE_CAMERA.
inline constexpr usize BINDING_CAMERA_LENS_FIELD = BINDING_CAMERA_ROTATION_FIELD + QUATERNION_BYTES;
/// Binding field: uint64 camera output identity; zero unless BINDING_SEQUENCE_CAMERA.
inline constexpr usize BINDING_CAMERA_OUTPUT_FIELD = BINDING_CAMERA_LENS_FIELD + LENS_BYTES;
/// Binding field: uint64 camera definition revision; zero unless BINDING_SEQUENCE_CAMERA.
inline constexpr usize BINDING_CAMERA_REVISION_FIELD = BINDING_CAMERA_OUTPUT_FIELD + sizeof(foundation::core::uint64);
/// Width of one binding record.
inline constexpr usize BINDING_RECORD_BYTES = BINDING_CAMERA_REVISION_FIELD + sizeof(foundation::core::uint64);

// Track records.
/// Track field: uint64 nonzero track identity.
inline constexpr usize TRACK_ID_FIELD = 0;
/// Track field: uint64 identity of the written binding.
inline constexpr usize TRACK_BINDING_FIELD = TRACK_ID_FIELD + sizeof(foundation::core::uint64);
/// Track field: uint32 channel tag.
inline constexpr usize TRACK_CHANNEL_FIELD = TRACK_BINDING_FIELD + sizeof(foundation::core::uint64);
/// Track field: uint32 flags, a subset of TRACK_FLAGS_V1.
inline constexpr usize TRACK_FLAGS_FIELD = TRACK_CHANNEL_FIELD + sizeof(uint32);
/// Track field: uint32 positive key count.
inline constexpr usize TRACK_KEY_COUNT_FIELD = TRACK_FLAGS_FIELD + sizeof(uint32);
/// Track field: uint32 reserved word; must be zero.
inline constexpr usize TRACK_RESERVED_FIELD = TRACK_KEY_COUNT_FIELD + sizeof(uint32);
/// Width of one track record.
inline constexpr usize TRACK_RECORD_BYTES = TRACK_RESERVED_FIELD + sizeof(uint32);

// Camera interval records.
/// Interval field: uint64 inclusive start tick.
inline constexpr usize INTERVAL_START_FIELD = 0;
/// Interval field: uint64 exclusive end tick.
inline constexpr usize INTERVAL_END_FIELD = INTERVAL_START_FIELD + sizeof(foundation::core::uint64);
/// Interval field: uint64 selected camera binding identity.
inline constexpr usize INTERVAL_CAMERA_FIELD = INTERVAL_END_FIELD + sizeof(foundation::core::uint64);
/// Width of one camera interval record.
inline constexpr usize INTERVAL_RECORD_BYTES = INTERVAL_CAMERA_FIELD + sizeof(foundation::core::uint64);

// Key records; every key starts with its tick.
/// Key field: uint64 authored tick, strictly increasing within a track.
inline constexpr usize KEY_TICK_FIELD = 0;
/// Key field: uint32 interpolation tag, or the visibility value for visibility keys.
inline constexpr usize KEY_MODE_FIELD = KEY_TICK_FIELD + sizeof(foundation::core::uint64);
/// Key field: the channel's value, starting after the mode word.
inline constexpr usize KEY_VALUE_FIELD = KEY_MODE_FIELD + sizeof(uint32);
/// Width of a translation or scale key (Vector3 value).
inline constexpr usize VECTOR_KEY_BYTES = KEY_VALUE_FIELD + VECTOR3_BYTES;
/// Width of a rotation key (quaternion value).
inline constexpr usize ROTATION_KEY_BYTES = KEY_VALUE_FIELD + QUATERNION_BYTES;
/// Width of a camera position key (Vector3d value).
inline constexpr usize POSITION_KEY_BYTES = KEY_VALUE_FIELD + VECTOR3D_BYTES;
/// Width of a visibility key; its mode word is 1 for visible and 0 for hidden.
inline constexpr usize VISIBILITY_KEY_BYTES = KEY_VALUE_FIELD;
/// Width of a lens key (lens value).
inline constexpr usize LENS_KEY_BYTES = KEY_VALUE_FIELD + LENS_BYTES;

/// Largest key record width.
inline constexpr usize MAX_KEY_RECORD_BYTES = POSITION_KEY_BYTES > LENS_KEY_BYTES ? POSITION_KEY_BYTES : LENS_KEY_BYTES;
/// Largest admitted sequence in bytes.
inline constexpr usize MAX_SEQUENCE_BYTES =
    HEADER_BYTES + usize{MAX_BINDINGS} * BINDING_RECORD_BYTES + usize{MAX_TRACKS} * TRACK_RECORD_BYTES +
    usize{MAX_INTERVALS} * INTERVAL_RECORD_BYTES + usize{MAX_KEYS} * MAX_KEY_RECORD_BYTES;

// The record widths are pinned by ludus_sequence_format_tests; any change needs a new version.
static_assert(MAX_SEQUENCE_BYTES <= ~uint32{0}, "sequence offsets must fit the uint32 counts and 32-bit targets");
} // namespace ludus::content::sequence_format
