#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/gameplay/camera/camera.hpp>
#include <ludus/gameplay/sequencer/sequence.hpp>
#include <ludus/gameplay/sequencer/time.hpp>

#include <string_view>

/// @file
/// Durable sequence definitions and their resolution against one scene revision.
///
/// A definition is the scene-independent content of a saved sequence: durable
/// bindings, channel tracks with stable identities and the camera selection.
/// TryValidateSequenceDefinition checks it without a scene, as a cooker or decoder
/// does. TryPrepareSequencePlan resolves its active bindings against a
/// caller-supplied SequenceSceneCatalog and publishes an immutable plan around a
/// PreparedSequence. GameplaySequencer never reads World records: the scene
/// adapter owns World access and builds the catalog. See
/// docs/architecture/sequencer-cinematics.md, "Sequence assets and binding resolution".
namespace ludus::gameplay::sequencer
{
/// Width of a pinned World content digest (SHA-256), in bytes.
inline constexpr foundation::usize SEQUENCE_WORLD_DIGEST_BYTES = 32;
/// Storage bound for one version-1 scoped World entity key, in bytes. The key's
/// syntax is owned by the content identity records; the sequence asset codec
/// checks that every key it admits fits this bound.
inline constexpr foundation::usize MAX_SEQUENCE_LEGACY_KEY_BYTES = 64;
/// Sentinel index meaning "no index": no parent target, no catalog target or no interval.
inline constexpr foundation::usize SEQUENCE_NO_INDEX = ~foundation::usize{0};
/// Default admission count for channel tracks of one definition, including disabled tracks.
inline constexpr foundation::usize DEFAULT_SEQUENCE_MAX_CHANNEL_TRACKS = 1024;
/// Default admission count for camera-selection intervals of one definition.
inline constexpr foundation::usize DEFAULT_SEQUENCE_MAX_CAMERA_INTERVALS = 256;

/// A pinned SHA-256 content digest of the saved World resource.
using SequenceWorldDigest = foundation::StaticArray<foundation::uint8, SEQUENCE_WORLD_DIGEST_BYTES>;

/// What a durable binding names. Values are persisted by the sequence asset codec.
enum class SequenceBindingKind : foundation::uint8
{
    WorldObject,       ///< A World document object, named by SourceObjectId plus an optional subobject key.
    SequenceCamera,    ///< A camera definition owned by the sequence; it needs no World entity.
    LegacyWorldObject, ///< A version-1 World entity named by its scoped entity key; requires a pinned World digest.
};

/// The one value a track writes. Object channels replace parent-local values;
/// camera channels replace a sequence camera's rigid world pose or lens.
enum class SequenceChannel : foundation::uint8
{
    Translation,    ///< Object parent-local translation; SequenceVectorKey values in world units.
    Rotation,       ///< Object local-to-parent rotation; SequenceRotationKey values.
    Scale,          ///< Object parent-local strictly positive scale; SequenceVectorKey values.
    Visibility,     ///< Object render inclusion; SequenceVisibilityKey values, always Step.
    CameraPosition, ///< Sequence camera float64 world position; SequencePositionKey values.
    CameraRotation, ///< Sequence camera local-to-world orientation; SequenceRotationKey values.
    CameraLens,     ///< Sequence camera lens; SequenceLensKey values.
};

/// The World a sequence was authored against. A nil document and an unpinned
/// digest accept any scene, which suits a camera-only traversal of a frozen import.
struct SequenceWorldReference final
{
    /// World document identity; required (nonnil version 4) when any WorldObject binding exists.
    foundation::identity::AssetId Document{};
    /// Pinned content digest of the saved World resource; meaningful only when HasContentDigest is true.
    SequenceWorldDigest ContentDigest{};
    /// Whether ContentDigest pins the World; required when any LegacyWorldObject binding exists.
    bool HasContentDigest = false;
};

/// One durable binding. Only the fields of its Kind may be set; the others keep
/// their defaults. Display names are not identities and do not appear here.
struct SequenceBinding final
{
    /// Nonzero sequence-local identity, unique in the definition and stable across renames.
    foundation::uint64 BindingId = 0;
    /// The kind of target named by this binding.
    SequenceBindingKind Kind = SequenceBindingKind::SequenceCamera;
    /// WorldObject only: the nonnil version-4 source-object identity.
    foundation::identity::SourceObjectId Object{};
    /// WorldObject only: adapter-owned stable key of one expanded draw object; zero names the whole object.
    foundation::uint64 SubobjectKey = 0;
    /// LegacyWorldObject only: the borrowed 1..MAX_SEQUENCE_LEGACY_KEY_BYTES-byte scoped entity key.
    std::string_view LegacyKey = "";
    /// SequenceCamera only: valid base camera, including output and revision metadata.
    camera::CameraSample Camera{};
};

/// One channel writer. Exactly the key view that matches Channel is nonempty;
/// the other views stay empty. A disabled track is validated and saved but
/// writes nothing, so it never conflicts with an enabled track.
struct SequenceTrack final
{
    /// Nonzero identity, unique in the definition and stable across rename and reordering.
    foundation::uint64 TrackId = 0;
    /// Identity of the binding this track writes.
    foundation::uint64 BindingId = 0;
    /// The written channel; it must suit the binding's kind.
    SequenceChannel Channel = SequenceChannel::Translation;
    /// Whether this track writes its channel. Only one enabled track may own a binding channel.
    bool IsEnabled = true;
    /// Translation or Scale keys.
    foundation::BorrowedView<const SequenceVectorKey> VectorKeys{nullptr, 0};
    /// Rotation or CameraRotation keys.
    foundation::BorrowedView<const SequenceRotationKey> RotationKeys{nullptr, 0};
    /// CameraPosition keys.
    foundation::BorrowedView<const SequencePositionKey> PositionKeys{nullptr, 0};
    /// Visibility keys.
    foundation::BorrowedView<const SequenceVisibilityKey> VisibilityKeys{nullptr, 0};
    /// CameraLens keys.
    foundation::BorrowedView<const SequenceLensKey> LensKeys{nullptr, 0};
};

/// One camera-selection interval [StartTick, EndTick) in authored ticks.
struct SequenceCameraInterval final
{
    foundation::uint64 StartTick = 0;       ///< Inclusive start; equals the previous interval's end.
    foundation::uint64 EndTick = 0;         ///< Exclusive end; greater than StartTick.
    foundation::uint64 CameraBindingId = 0; ///< A SequenceCamera binding selected throughout the interval.
};

/// Scene-independent content of one saved sequence, borrowed for the duration of a call.
///
/// Camera selection covers the playback range exactly when the definition has a
/// camera binding, and is empty otherwise.
struct SequenceDefinition final
{
    /// The World this sequence was authored against.
    SequenceWorldReference World{};
    /// Positive authored tick frequency.
    foundation::uint32 TicksPerSecond = 1;
    /// Inclusive playback start tick.
    foundation::uint64 PlaybackStartTick = 0;
    /// Exclusive playback end tick; greater than PlaybackStartTick.
    foundation::uint64 PlaybackEndTick = 1;
    /// Durable bindings in source order.
    foundation::BorrowedView<const SequenceBinding> Bindings{nullptr, 0};
    /// Channel tracks in source order; order never changes the result.
    foundation::BorrowedView<const SequenceTrack> Tracks{nullptr, 0};
    /// Camera selection intervals in increasing time order.
    foundation::BorrowedView<const SequenceCameraInterval> CameraSelection{nullptr, 0};
};

/// Admission limits for one definition, on top of the sampler's SequenceLimits.
struct SequenceDefinitionLimits final
{
    /// Largest track count, including disabled tracks.
    foundation::usize MaxTracks = DEFAULT_SEQUENCE_MAX_CHANNEL_TRACKS;
    /// Largest camera-selection interval count.
    foundation::usize MaxCameraIntervals = DEFAULT_SEQUENCE_MAX_CAMERA_INTERVALS;
    /// Sampler limits; MaxTracks counts bindings, which become sampler track groups.
    SequenceLimits Sampler{};
};

/// Where a validation or resolution failure was found. Unused fields keep their defaults.
struct SequenceDiagnostic final
{
    /// The failure, or Success.
    SequenceStatus Status = SequenceStatus::Success;
    /// The binding involved, or zero.
    foundation::uint64 BindingId = 0;
    /// The track involved, or zero.
    foundation::uint64 TrackId = 0;
    /// The second track of a ConflictingWriter or duplicate-identity failure, or zero.
    foundation::uint64 OtherTrackId = 0;
    /// The camera interval or scene target involved, or SEQUENCE_NO_INDEX.
    foundation::usize Index = SEQUENCE_NO_INDEX;
};

/// One object the scene adapter can bind, valid for one scene revision.
///
/// A target may carry both identities: a version-2 World entity has a nonnil
/// Object and may also report its version-1 label as LegacyKey.
struct SequenceSceneTarget final
{
    /// Durable source-object identity, or nil when the object has none.
    foundation::identity::SourceObjectId Object{};
    /// Adapter-owned subobject key; zero for the whole object.
    foundation::uint64 SubobjectKey = 0;
    /// Borrowed version-1 scoped entity key, or empty.
    std::string_view LegacyKey = "";
    /// Whether the extraction accepts a parent-local transform override.
    bool CanWriteLocalTransform = false;
    /// Whether the extraction accepts a render-inclusion override.
    bool CanWriteVisibility = false;
    /// Frozen parent-local base transform used for unanimated channels.
    foundation::math::TransformTRS BaseTransformLocal{};
    /// Frozen base render inclusion used when visibility is unanimated.
    bool IsBaseVisible = true;
    /// Index of the parent target in the same catalog, or SEQUENCE_NO_INDEX for a root.
    foundation::usize ParentIndex = SEQUENCE_NO_INDEX;
};

/// The scene targets available for binding, captured from one scene revision.
struct SequenceSceneCatalog final
{
    /// Identity of the loaded World document, or nil for a version-1 World.
    foundation::identity::AssetId WorldDocument{};
    /// Content digest of the loaded World resource; meaningful only when HasContentDigest is true.
    SequenceWorldDigest ContentDigest{};
    /// Whether ContentDigest describes the loaded World resource.
    bool HasContentDigest = false;
    /// Caller-defined revision of the scene these targets were captured from.
    foundation::uint64 SceneRevision = 0;
    /// Borrowed targets; indices are the TargetIndex values a plan reports.
    foundation::BorrowedView<const SequenceSceneTarget> Targets{nullptr, 0};
};

/// One active binding of a prepared plan.
struct SequenceResolvedBinding final
{
    foundation::uint64 BindingId = 0;                               ///< Binding identity in the definition.
    SequenceBindingKind Kind = SequenceBindingKind::SequenceCamera; ///< Binding kind.
    foundation::usize TargetIndex = SEQUENCE_NO_INDEX; ///< Catalog target index; SEQUENCE_NO_INDEX for a camera.
};

/// The camera selected at one time.
struct SequenceCameraSelection final
{
    foundation::uint64 CameraBindingId = 0;              ///< The selected SequenceCamera binding.
    foundation::usize IntervalIndex = SEQUENCE_NO_INDEX; ///< Index of the containing interval; a change is a cut.
};

namespace detail
{
struct SequencePlanStorage;
}

/// Immutable move-only result of resolving a definition against one scene revision.
///
/// Owns a PreparedSequence whose object groups carry the scene's base values,
/// the active binding table and the camera selection. It retains no pointer to
/// the definition, catalog or their storage. Const queries are safe concurrently
/// while the plan stays alive and unmoved; replacement requires external
/// synchronization with its readers.
class PreparedSequencePlan final
{
public:
    PreparedSequencePlan() noexcept;                             ///< Construct an unprepared plan without allocation.
    ~PreparedSequencePlan() noexcept;                            ///< Release the owned revision; no readers may remain.
    PreparedSequencePlan(PreparedSequencePlan&& other) noexcept; ///< Transfer ownership; other becomes unprepared.
    PreparedSequencePlan& operator=(PreparedSequencePlan&& other) noexcept; ///< Release this plan and take other's.
    PreparedSequencePlan(const PreparedSequencePlan&) = delete;             ///< Plans are not implicitly copied.
    PreparedSequencePlan& operator=(const PreparedSequencePlan&) = delete;  ///< Plans are not implicitly copied.

    /// Whether an accepted plan is owned.
    [[nodiscard]] bool IsPrepared() const noexcept;
    /// The sampler revision for TryEvaluateSequenceObject/Camera; unprepared when the plan is.
    [[nodiscard]] const PreparedSequence& GetSequence() const noexcept;
    /// Active bindings in definition order; empty when unprepared.
    [[nodiscard]] foundation::BorrowedView<const SequenceResolvedBinding> GetResolvedBindings() const noexcept;
    /// Camera selection intervals; empty when unprepared or object-only.
    [[nodiscard]] foundation::BorrowedView<const SequenceCameraInterval> GetCameraSelection() const noexcept;
    /// Inclusive playback start tick, or zero when unprepared.
    [[nodiscard]] foundation::uint64 GetPlaybackStartTick() const noexcept;
    /// Exclusive playback end tick, or zero when unprepared.
    [[nodiscard]] foundation::uint64 GetPlaybackEndTick() const noexcept;
    /// Authored tick frequency, or zero when unprepared.
    [[nodiscard]] foundation::uint32 GetTicksPerSecond() const noexcept;
    /// The catalog's SceneRevision this plan was resolved against, or zero when unprepared.
    [[nodiscard]] foundation::uint64 GetSceneRevision() const noexcept;

private:
    friend SequenceStatus TryPrepareSequencePlan(const SequenceDefinition& definition,
                                                 const SequenceSceneCatalog& catalog,
                                                 const SequenceDefinitionLimits& limits,
                                                 PreparedSequencePlan& outPlan,
                                                 SequenceDiagnostic& outDiagnostic) noexcept;
    foundation::UniquePtr<detail::SequencePlanStorage> mStorage;
};

/// Validate a definition without a scene, as a cooker, decoder or editor save does.
///
/// Checks identities, binding fields, channel/kind compatibility, conflicting
/// enabled writers, camera selection coverage and every track's keys with the
/// sampler's rules (object tracks against an identity base). Each track needs at
/// least one key.
/// @param definition Borrowed definition; nothing is retained.
/// @param limits Admission limits; allocation is bounded by them.
/// @param outDiagnostic Replaced on every call; names the failing binding, track(s) or interval.
/// @return Success, InvalidArgument, InvalidKeys, DuplicateBinding, ConflictingWriter,
/// InvalidSelection, BudgetExceeded or OutOfMemory. Cold path; no thread affinity.
[[nodiscard]] SequenceStatus TryValidateSequenceDefinition(const SequenceDefinition& definition,
                                                           const SequenceDefinitionLimits& limits,
                                                           SequenceDiagnostic& outDiagnostic) noexcept;

/// Validate a definition, resolve its active bindings against one scene revision
/// and publish an immutable plan.
///
/// Active bindings are those written by an enabled track and the cameras named
/// by the selection. Inactive bindings are not resolved, so an absent target only
/// fails when it is used. Every active World binding must match exactly one
/// target that supports its channels, whose parent chain reaches a root.
/// @param definition Borrowed definition; nothing is retained after return.
/// @param catalog Borrowed targets of one scene revision; nothing is retained after return.
/// @param limits Admission limits for the definition and the prepared sampler.
/// @param outPlan Replaced only on Success; any accepted plan is preserved on every failure.
/// @param outDiagnostic Replaced on every call; names the failing binding, track(s) or target.
/// @return Success, any TryValidateSequenceDefinition failure, WorldMismatch,
/// MissingBinding, AmbiguousTarget, UnsupportedChannel or ParentCycle.
/// Cold path; allocates; no thread affinity.
[[nodiscard]] SequenceStatus TryPrepareSequencePlan(const SequenceDefinition& definition,
                                                    const SequenceSceneCatalog& catalog,
                                                    const SequenceDefinitionLimits& limits,
                                                    PreparedSequencePlan& outPlan,
                                                    SequenceDiagnostic& outDiagnostic) noexcept;

/// Select the camera at an absolute time inside the playback range.
///
/// Intervals are half-open, so a time exactly on an interval boundary selects
/// the later interval: a cut on an exact frame boundary takes effect on that
/// frame. Independent of earlier calls.
/// @param plan Prepared plan retained for the call.
/// @param time Proper-fraction authored ticks.
/// @param outSelection Replaced only on Success.
/// @return Success; Unprepared; InvalidArgument for an improper time; OutOfRange
/// outside [PlaybackStartTick, PlaybackEndTick); MissingBinding for an object-only plan.
/// Allocation-free; no thread affinity.
[[nodiscard]] SequenceStatus TryEvaluateSequenceCameraSelection(const PreparedSequencePlan& plan,
                                                                SequenceTime time,
                                                                SequenceCameraSelection& outSelection) noexcept;
} // namespace ludus::gameplay::sequencer
