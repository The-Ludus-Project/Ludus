#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/content/identity_records/migration_manifest.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/gameplay/sequencer/binding.hpp>

#include <span>
#include <string_view>

/// @file
/// Ludus::SequenceContent: the `.luseq.json` source document, its canonical
/// writer, the deterministic LUSEQ cooker and the validating LUSEQ decoder.
///
/// A SequenceDocument owns everything a saved sequence contains. Every reader,
/// writer and cooker validates it with GameplaySequencer's
/// TryValidateSequenceDefinition, so source, cooked and in-memory sequences obey
/// one set of rules. Binding resolution against a scene is not done here; pass
/// the definition of a SequenceDefinitionView to TryPrepareSequencePlan. See
/// docs/architecture/sequencer-cinematics.md, "Sequence assets and binding resolution".
///
/// This is an in-tree content library, like ContentIdentity, whose version-1
/// entity-key rule it reuses. It is not installed with the SDK.
namespace ludus::content::sequence_content
{
/// Value of the source document's `format` member.
inline constexpr std::string_view SOURCE_FORMAT = "ludus.sequence";
/// Source document version 1.
inline constexpr uint64 SOURCE_VERSION_1 = 1;
/// Largest admitted source document, in bytes.
inline constexpr usize MAX_SOURCE_BYTES = usize{16} * 1024 * 1024;
/// Largest binding or track label, in bytes of UTF-8.
inline constexpr usize MAX_LABEL_BYTES = 128;

/// An authoring label: a display name for diagnostics and editing, never an identity.
using SequenceLabel = Text<MAX_LABEL_BYTES>;
/// A version-1 scoped World entity key, using ContentIdentity's rule.
using SequenceLegacyKey = Text<identity_records::MAX_LEGACY_ID_BYTES>;

/// Result of a sequence content operation. Failures preserve every output.
enum class SequenceContentStatus : uint8
{
    Success,            ///< The operation completed.
    Malformed,          ///< Invalid JSON or LUSEQ bytes, a missing, unknown or mistyped member, or a bad tag.
    UnsupportedVersion, ///< An unknown format name, source version or LUSEQ version.
    InvalidIdentity,    ///< A UUID is not canonical version-4 text, or a required identity is nil.
    InvalidLegacyKey,   ///< A legacy entity key breaks the version-1 World ID rule.
    InvalidSequence,    ///< The content is well formed but fails sequence validation; see Sequence.
    Limit,              ///< A size, count or text capacity limit was exceeded.
    OutOfMemory,        ///< An allocation failed.
};

/// Where an operation failed.
struct SequenceContentError final
{
    /// The failure, or Success.
    SequenceContentStatus Status = SequenceContentStatus::Success;
    /// The member or record that failed, such as `tracks[2].keys[1].tick`; empty when unknown.
    MessageText Field{};
    /// Byte offset of a JSON syntax failure, or foundation::parsing::UNKNOWN_BYTE_OFFSET.
    usize Offset = ~usize{0};
    /// The sequence validation diagnostic when Status is InvalidSequence.
    gameplay::sequencer::SequenceDiagnostic Sequence{};
};

/// One authored binding. Only the fields of its Kind are meaningful; the others keep their defaults.
struct SequenceBindingRecord final
{
    /// Display name; may be empty and may repeat.
    SequenceLabel Label{};
    /// Nonzero durable binding identity.
    uint64 BindingId = 0;
    /// Binding kind.
    gameplay::sequencer::SequenceBindingKind Kind = gameplay::sequencer::SequenceBindingKind::SequenceCamera;
    /// WorldObject: source-object identity.
    foundation::identity::SourceObjectId Object{};
    /// WorldObject: adapter-owned subobject key; zero for the whole object.
    uint64 SubobjectKey = 0;
    /// LegacyWorldObject: version-1 scoped entity key.
    SequenceLegacyKey LegacyKey{};
    /// SequenceCamera: base camera. Only pose, lens, OutputId and DefinitionRevision are saved;
    /// the remaining CameraSample fields keep their defaults.
    gameplay::camera::CameraSample Camera{};
};

/// One authored track. Its keys are KeyCount consecutive entries, starting at
/// KeyOffset, of the document key array that matches Channel.
struct SequenceTrackRecord final
{
    /// Display name; may be empty and may repeat.
    SequenceLabel Label{};
    /// Nonzero durable track identity.
    uint64 TrackId = 0;
    /// Identity of the written binding.
    uint64 BindingId = 0;
    /// Written channel.
    gameplay::sequencer::SequenceChannel Channel = gameplay::sequencer::SequenceChannel::Translation;
    /// Whether the track writes its channel.
    bool IsEnabled = true;
    /// First key in the channel's key array.
    usize KeyOffset = 0;
    /// Number of keys.
    usize KeyCount = 0;
};

/// The owned content of one saved sequence.
struct SequenceDocument final
{
    /// Nonnil version-4 identity of this sequence asset.
    foundation::identity::AssetId Sequence{};
    /// The World the sequence was authored against.
    gameplay::sequencer::SequenceWorldReference World{};
    /// Content-relative source path of that World, a locator for editors; empty when unknown. Not cooked.
    ResourcePath WorldPath{};
    /// Positive authored tick frequency.
    uint32 TicksPerSecond = 1;
    /// Inclusive playback start tick.
    uint64 PlaybackStartTick = 0;
    /// Exclusive playback end tick.
    uint64 PlaybackEndTick = 1;
    /// Bindings in source order.
    foundation::Array<SequenceBindingRecord> Bindings;
    /// Tracks in source order.
    foundation::Array<SequenceTrackRecord> Tracks;
    /// Camera selection intervals.
    foundation::Array<gameplay::sequencer::SequenceCameraInterval> CameraSelection;
    /// Translation and scale keys.
    foundation::Array<gameplay::sequencer::SequenceVectorKey> VectorKeys;
    /// Object and camera rotation keys.
    foundation::Array<gameplay::sequencer::SequenceRotationKey> RotationKeys;
    /// Camera position keys.
    foundation::Array<gameplay::sequencer::SequencePositionKey> PositionKeys;
    /// Visibility keys.
    foundation::Array<gameplay::sequencer::SequenceVisibilityKey> VisibilityKeys;
    /// Lens keys.
    foundation::Array<gameplay::sequencer::SequenceLensKey> LensKeys;
};

/// Borrowed GameplaySequencer view of a document.
///
/// Owns the binding and track arrays the definition points at; the keys and
/// legacy key text stay in the document, which must outlive the view and stay
/// unmodified while the definition is used.
class SequenceDefinitionView final
{
public:
    SequenceDefinitionView() noexcept = default; ///< Construct an empty view without allocation.
    /// Views are not copied: the definition points into this view's own arrays.
    SequenceDefinitionView(const SequenceDefinitionView&) = delete;
    /// Views are not copied: the definition points into this view's own arrays.
    SequenceDefinitionView& operator=(const SequenceDefinitionView&) = delete;
    /// Transfer the arrays; the moved definition stays valid because array storage moves with it.
    SequenceDefinitionView(SequenceDefinitionView&& other) noexcept = default;
    /// Transfer the arrays; the moved definition stays valid because array storage moves with it.
    SequenceDefinitionView& operator=(SequenceDefinitionView&& other) noexcept = default;
    ~SequenceDefinitionView() noexcept = default; ///< Release the binding and track arrays.

    /// The borrowed definition; empty until TryMakeSequenceDefinition succeeds.
    [[nodiscard]] LUDUS_INLINE const gameplay::sequencer::SequenceDefinition& GetDefinition() const noexcept
    {
        return mDefinition;
    }

private:
    friend SequenceContentStatus TryMakeSequenceDefinition(const SequenceDocument& document,
                                                           SequenceDefinitionView& outView) noexcept;
    foundation::Array<gameplay::sequencer::SequenceBinding> mBindings;
    foundation::Array<gameplay::sequencer::SequenceTrack> mTracks;
    gameplay::sequencer::SequenceDefinition mDefinition{};
};

/// The definition limits implied by the LUSEQ format's admission limits.
/// Every operation here validates with these limits.
[[nodiscard]] gameplay::sequencer::SequenceDefinitionLimits GetSequenceContentLimits() noexcept;

/// Build a borrowed definition over a document without validating it.
/// @param document Source of all borrowed data; must outlive outView's use.
/// @param outView Replaced only on Success.
/// @return Success; Malformed when a track's key range lies outside its key array; Limit or OutOfMemory.
[[nodiscard]] SequenceContentStatus TryMakeSequenceDefinition(const SequenceDocument& document,
                                                              SequenceDefinitionView& outView) noexcept;

/// Validate a document's identities, legacy keys and sequence content.
/// @param document Document to check.
/// @param outError Replaced on every call.
/// @return Success, InvalidIdentity, InvalidLegacyKey, InvalidSequence, Malformed, Limit or OutOfMemory.
[[nodiscard]] SequenceContentStatus TryValidateSequenceDocument(const SequenceDocument& document,
                                                                SequenceContentError& outError) noexcept;

/// Read and validate a `.luseq.json` source document.
///
/// Strict: unknown or duplicate members, missing members, wrong types, unknown
/// tags, noncanonical UUIDs and values that do not fit their fields are errors.
/// Float32 fields accept any finite JSON number within float32 range and round
/// it to nearest.
/// @param json Complete UTF-8 document, at most MAX_SOURCE_BYTES.
/// @param outDocument Replaced only on Success.
/// @param outError Replaced on every call.
/// @return Success or any SequenceContentStatus failure. Cold path; allocates.
[[nodiscard]] SequenceContentStatus
TryReadSequenceSource(std::string_view json, SequenceDocument& outDocument, SequenceContentError& outError) noexcept;

/// Validate a document and write its canonical `.luseq.json` form.
///
/// Members are written in schema order with two-space indentation; equal
/// documents produce equal bytes, and reading the output reproduces the document.
/// @param document Document to write.
/// @param outBytes Replaced only on Success.
/// @param outError Replaced on every call.
/// @return Success or any SequenceContentStatus failure. Cold path; allocates.
[[nodiscard]] SequenceContentStatus
TryWriteSequenceSource(const SequenceDocument& document, Bytes& outBytes, SequenceContentError& outError) noexcept;

/// Validate a document and cook its deterministic LUSEQ bytes.
///
/// Labels and the World path are authoring data and are not cooked.
/// @param document Document to cook.
/// @param outBytes Replaced only on Success.
/// @param outError Replaced on every call.
/// @return Success or any SequenceContentStatus failure. Cold path; allocates.
[[nodiscard]] SequenceContentStatus
TryCookSequence(const SequenceDocument& document, Bytes& outBytes, SequenceContentError& outError) noexcept;

/// Decode and validate LUSEQ bytes.
///
/// The decoded document has empty labels and World path. Cooking it reproduces
/// the input bytes exactly.
/// @param bytes Complete cooked sequence, at most sequence_format::MAX_SEQUENCE_BYTES.
/// @param outDocument Replaced only on Success.
/// @param outError Replaced on every call.
/// @return Success, Malformed, UnsupportedVersion, InvalidIdentity, InvalidLegacyKey,
/// InvalidSequence, Limit or OutOfMemory. Cold path; allocates.
[[nodiscard]] SequenceContentStatus
TryDecodeSequence(std::span<const uint8> bytes, SequenceDocument& outDocument, SequenceContentError& outError) noexcept;

/// Stable lowercase name of a status, for diagnostics and command output.
[[nodiscard]] std::string_view GetSequenceContentStatusName(SequenceContentStatus status) noexcept;
} // namespace ludus::content::sequence_content
