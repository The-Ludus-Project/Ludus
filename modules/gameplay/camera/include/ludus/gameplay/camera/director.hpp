#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/camera/camera.hpp>
#include <ludus/gameplay/camera/control.hpp>
#include <ludus/gameplay/camera/evaluation.hpp>

namespace ludus::gameplay::camera
{
/// Override slots reserved by each director; acquisition beyond them returns CapacityExceeded.
inline constexpr foundation::usize MAX_CAMERA_OVERRIDES = 8;
/// Last generation an override slot can issue; the slot then retires instead of wrapping.
inline constexpr foundation::uint16 MAX_CAMERA_OVERRIDE_GENERATION = 0xFFFF;
/// Default bound on presentation seconds consumed by one update; a longer hitch discards the excess.
inline constexpr foundation::float32 DEFAULT_CAMERA_MAXIMUM_DELTA_SECONDS = 0.1F;

/// Monotonic easing of normalized blend time.
enum class CameraEasing : foundation::uint8
{
    /// Weight equals normalized time.
    Linear,
    /// Hermite smoothstep: zero slope at both ends.
    SmoothStep,
};

/// Interpolation path of a pose blend.
enum class CameraBlendPath : foundation::uint8
{
    /// Linear eye position and shortest-arc orientation.
    Linear,
    /// Interpolate the pivot linearly and the eye's camera-local offset from it, then rotate that offset by the
    /// shortest-arc orientation: the eye swings around a shared pivot instead of cutting across it. Endpoints
    /// without a pivot (fixed rigs) use their eye, which reduces to the linear path.
    AroundPivot,
};

/// How the director moves to a newly selected rig. A zero duration is an explicit cut.
struct CameraTransition final
{
    /// Finite nonnegative blend duration in presentation seconds.
    foundation::float32 DurationSeconds = 0.0F;
    /// Easing of the blend weight.
    CameraEasing Easing = CameraEasing::SmoothStep;
    /// Pose interpolation path.
    CameraBlendPath Path = CameraBlendPath::Linear;
    /// Exact memberwise comparison.
    [[nodiscard]] friend constexpr bool operator==(const CameraTransition&, const CameraTransition&) = default;
};

/// A rig the game asks a director to show. The director copies it; nothing is borrowed after the call.
struct CameraRigRequest final
{
    /// Definition validated with ValidateCameraRigDefinition.
    CameraRigDefinition Definition{};
    /// Target binding resolved from CameraFrameInput::Targets; nonzero for follow and orbit, ignored for fixed.
    foundation::uint64 BindingId = 0;
    /// Transition used when this request becomes the selection.
    CameraTransition Transition{};
    /// Exact memberwise comparison.
    [[nodiscard]] friend constexpr bool operator==(const CameraRigRequest&, const CameraRigRequest&) = default;
};

/// Handle to one acquired override: director output, slot and generation. Never reused after release.
struct CameraOverrideToken final
{
    /// Output identity of the issuing director.
    foundation::uint64 OutputId = 0;
    /// Slot index below MAX_CAMERA_OVERRIDES.
    foundation::uint32 Slot = 0;
    /// Nonzero generation of that slot.
    foundation::uint16 Generation = 0;
};

/// Why a publication is a cut. Every cut increments the published discontinuity revision, so renderers reject
/// view history.
enum class CameraCutReason : foundation::uint8
{
    /// Continuous: either steady or blending.
    None,
    /// First publication after construction, configuration or Reset.
    FirstPublication,
    /// RequestCut was called.
    Requested,
    /// The new selection's transition has zero duration.
    ZeroDuration,
    /// The new selection changes projection kind or clip planes, which cannot blend.
    IncompatibleLens,
    /// CameraFrameInput::WorldRevision changed.
    WorldDiscontinuity,
    /// The selected rig's target reported a new discontinuity revision (teleport or respawn).
    TargetDiscontinuity,
    /// The previous update failed; the history is re-primed instead of treating the gap as continuous.
    Recovery,
};

/// Fixed director configuration.
struct CameraDirectorSettings final
{
    /// Nonzero logical output identity copied into samples and override tokens.
    foundation::uint64 OutputId = 1;
    /// Finite positive bound on the seconds one update may consume.
    foundation::float32 MaximumDeltaSeconds = DEFAULT_CAMERA_MAXIMUM_DELTA_SECONDS;
};

/// One presentation update, extracted after the completed ticks with the same tick pair and alpha as the scene.
struct CameraFrameInput final
{
    /// Previous tick used by target extraction.
    foundation::uint64 PreviousTick = 0;
    /// Completed current tick, greater than or equal to PreviousTick.
    foundation::uint64 CurrentTick = 0;
    /// Target extraction alpha in [0, 1]; targets are not interpolated again.
    foundation::float32 RenderAlpha = 1.0F;
    /// Game presentation sequence; zero is allowed for previews.
    foundation::uint64 PresentationSequence = 0;
    /// Game-owned world revision; a change (world replacement, restored replay) cuts.
    foundation::uint64 WorldRevision = 0;
    /// Finite nonnegative presentation seconds under the game's clock policy. Zero (pause) freezes blends and
    /// damping. Values above the director's maximum are clamped and the excess reported as discarded.
    foundation::float64 DeltaSeconds = 0.0;
    /// Authoritative aim from the completed ticks.
    CameraControlView Control{};
    /// Interpolated targets with unique binding identities, borrowed for this call.
    foundation::BorrowedView<const CameraTargetSample> Targets{nullptr, 0};
};

/// Copied checkpoint of one director update.
struct CameraDirectorTrace final
{
    /// Identity of the selected request (changes whenever a different request is selected).
    foundation::uint64 SelectedRequest = 0;
    /// True when an override, rather than the base rig, is selected.
    bool IsOverrideSelected = false;
    /// Selected override slot; meaningful only when IsOverrideSelected.
    foundation::uint32 OverrideSlot = 0;
    /// Cut cause, or None for a continuous publication.
    CameraCutReason CutReason = CameraCutReason::None;
    /// True while an outgoing endpoint is still blended in.
    bool IsBlending = false;
    /// True when the outgoing endpoint is a frozen sample (an interrupted blend or a lost outgoing target).
    bool IsOutgoingFrozen = false;
    /// Eased incoming weight in [0, 1]; one when not blending.
    foundation::float32 BlendWeight = 1.0F;
    /// Seconds consumed by this update.
    foundation::float64 AppliedDeltaSeconds = 0.0;
    /// Seconds dropped by the hitch clamp.
    foundation::float64 DiscardedDeltaSeconds = 0.0;
    /// Checkpoint of the incoming (selected) rig.
    CameraTrace Incoming{};
    /// Outgoing endpoint sample; meaningful only when IsBlending.
    CameraSample Outgoing{};
    /// Complete accepted publication.
    CameraSample Published{};
};

/// Per-output camera owner: selects a base rig or the winning override, advances at most two rig endpoints and
/// blends them, and publishes one CameraSample per update.
///
/// Selection: an active override beats the base; a higher priority wins; on equal priority the incumbent stays,
/// otherwise the most recently acquired request wins. Re-sending an identical request does not restart its blend.
/// A selection change blends from the previous rig, which keeps updating; a change during a blend instead freezes
/// the last published sample as the outgoing endpoint, so the new blend starts exactly from what was shown.
///
/// Lenses blend in log projection scale (cot(fov/2) for perspective, 1/span for orthographic). A change of
/// projection kind or clip planes cannot blend and cuts. Cuts re-prime histories and increment the published
/// DiscontinuityRevision (the director owns it; game resets enter through WorldRevision).
///
/// Updates are transactional: a failure publishes nothing and keeps requests pending, and the next accepted
/// update cuts with CameraCutReason::Recovery. C1 has no collision or effects: the published sample is the
/// pre-effects base later phases constrain.
///
/// Fixed storage, no allocation, no locks; one owner thread. Request calls take effect at the next update.
class CameraDirector final
{
public:
    /// Construct with default settings and no requests.
    CameraDirector() noexcept = default;

    /// Replace the settings and clear requests, selection and history; the next publication cuts. Override
    /// generations persist, so tokens issued before stay stale.
    /// @return InvalidInput for a zero output or a nonfinite/nonpositive maximum delta; failure changes nothing.
    [[nodiscard]] CameraStatus TryConfigure(const CameraDirectorSettings& settings) noexcept;

    /// Set the base rig shown when no override is active. An identical request keeps the current selection.
    /// @return InvalidInput for an invalid definition, binding or transition; failure changes nothing.
    [[nodiscard]] CameraStatus SetBaseRig(const CameraRigRequest& request) noexcept;

    /// Acquire an override that competes by priority with the other active overrides.
    /// @param ownerId Nonzero game identity of the requester, for RevokeOverridesOwnedBy.
    /// @param request Rig to show while this override wins; copied.
    /// @param priority Higher values win; ties keep the incumbent, then prefer the most recent acquisition.
    /// @param outToken Replaced only on Success.
    /// @return InvalidInput or CapacityExceeded (every slot active or retired); failure changes nothing.
    [[nodiscard]] CameraStatus AcquireOverride(foundation::uint64 ownerId,
                                               const CameraRigRequest& request,
                                               foundation::int32 priority,
                                               CameraOverrideToken& outToken) noexcept;

    /// Release an override. If it was selected, the revealed rig is entered with `transition`.
    /// @return StaleHandle for a released, retired or foreign token, InvalidInput for an invalid transition;
    /// failure changes nothing.
    [[nodiscard]] CameraStatus ReleaseOverride(const CameraOverrideToken& token,
                                               const CameraTransition& transition) noexcept;

    /// Release every active override of an owner (for example when it is destroyed).
    /// @return Number of released overrides; an invalid transition releases nothing and returns zero.
    [[nodiscard]] foundation::usize RevokeOverridesOwnedBy(foundation::uint64 ownerId,
                                                           const CameraTransition& transition) noexcept;

    /// Make the next accepted update a cut. Stays pending across failed updates.
    void RequestCut() noexcept;

    /// World replacement: release every override, clear the base and history. The next publication is a cut;
    /// the discontinuity revision keeps increasing.
    void Reset() noexcept;

    /// Select, advance, blend and publish one sample.
    /// @param input Timing, control and borrowed targets of this presentation.
    /// @param outSample Replaced only on Success.
    /// @param traceOrNull Optional checkpoint, replaced only on Success.
    /// @return InvalidInput (malformed input, invalid targets), NoSelection, MissingTarget (the selected rig's
    /// binding is absent) or OutOfRange. Every failure preserves outputs, requests and history.
    /// @note An outgoing endpoint whose target is lost freezes at its last sample instead of failing.
    [[nodiscard]] CameraStatus TryUpdate(const CameraFrameInput& input,
                                         CameraSample& outSample,
                                         CameraDirectorTrace* traceOrNull = nullptr) noexcept;

    /// Whether any sample has been published since construction, configuration or Reset.
    [[nodiscard]] bool HasPublished() const noexcept;

    /// Last accepted publication; the default sample before the first one.
    [[nodiscard]] const CameraSample& GetPublished() const noexcept;

private:
    enum class OutgoingKind : foundation::uint8
    {
        None,
        Live,
        Frozen,
    };

    /// Private record of one reserved override slot.
    struct OverrideSlot final
    {
        /// Copied request shown while this override wins.
        CameraRigRequest Request{};
        /// Request identity; increases with every new request.
        foundation::uint64 Serial = 0;
        /// Game owner identity for RevokeOverridesOwnedBy.
        foundation::uint64 OwnerId = 0;
        /// Selection priority.
        foundation::int32 Priority = 0;
        /// Generation issued with the current or last token; MAX_CAMERA_OVERRIDE_GENERATION retires the slot.
        foundation::uint16 Generation = 0;
        /// Whether the slot currently competes for selection.
        bool IsActive = false;
    };

    /// Private record of one evaluated rig endpoint.
    struct Endpoint final
    {
        /// Request the endpoint evaluates.
        CameraRigRequest Request{};
        /// Rig damping history.
        CameraRigState State{};
        /// Last sample the endpoint produced.
        CameraSample Sample{};
        /// Pivot of that sample.
        foundation::math::Vector3d Pivot{};
    };

    /// Private selection, blend and publication state, copied and committed as one transaction.
    struct History final
    {
        /// Serial of the selected request.
        foundation::uint64 SelectedSerial = 0;
        /// Selected (incoming) endpoint.
        Endpoint Incoming{};
        /// Whether the previous endpoint is blended out, and how.
        OutgoingKind Outgoing = OutgoingKind::None;
        /// Outgoing endpoint, live or frozen.
        Endpoint Previous{};
        /// Transition of the current blend.
        CameraTransition Blend{};
        /// Presentation seconds consumed by the current blend.
        foundation::float64 BlendElapsedSeconds = 0.0;
        /// Whether Published holds an accepted sample.
        bool HasPublished = false;
        /// Last accepted publication.
        CameraSample Published{};
        /// Pivot of the last accepted publication.
        foundation::math::Vector3d PublishedPivot{};
        /// Director-owned revision, incremented by every cut.
        foundation::uint64 DiscontinuityRevision = 0;
        /// Game world revision of the last accepted publication.
        foundation::uint64 WorldRevision = 0;
        /// Highest request serial issued before the last accepted publication.
        foundation::uint64 CommittedSerial = 0;
        /// RequestCut is waiting for an accepted update.
        bool IsCutPending = false;
        /// An update failed after the last accepted publication.
        bool HasFailedSinceCommit = false;
        /// A release recorded RevealTransition since the last accepted publication.
        bool HasRevealTransition = false;
        /// Transition for a rig revealed by a release.
        CameraTransition RevealTransition{};
    };

    /// Private result of request selection.
    struct Selection final
    {
        /// Serial of the selected request.
        foundation::uint64 Serial = 0;
        /// Selected request inside the director, or null before selection.
        const CameraRigRequest* RequestOrNull = nullptr;
        /// Whether an override, rather than the base, was selected.
        bool IsOverride = false;
        /// Selected override slot.
        foundation::uint32 Slot = 0;
    };

    [[nodiscard]] bool selectRequest(Selection& outSelection) const noexcept;
    [[nodiscard]] CameraStatus
    tryAdvance(const CameraFrameInput& input, CameraSample& outSample, CameraDirectorTrace* traceOrNull) noexcept;

    CameraDirectorSettings mSettings{};
    bool mHasBase = false;
    CameraRigRequest mBase{};
    foundation::uint64 mBaseSerial = 0;
    foundation::StaticArray<OverrideSlot, MAX_CAMERA_OVERRIDES> mOverrides{};
    foundation::uint64 mNextSerial = 1;
    History mHistory{};
};
} // namespace ludus::gameplay::camera
