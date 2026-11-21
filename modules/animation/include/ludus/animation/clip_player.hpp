#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/animation/clip.hpp>
#include <ludus/animation/status.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/hash/hash.hpp>

namespace ludus::animation
{
/// @brief Largest cycle count one traversal may cross.
///
/// 2^52 keeps every crossed cycle count exactly representable in float64 cycle arithmetic.
inline constexpr foundation::int64 MAX_CLIP_TRAVERSAL_CYCLES = foundation::int64{1} << 52;

/// Direction in which a traversal crossed an event.
enum class ClipTraversalDirection : foundation::uint8
{
    Forward, ///< Increasing timeline position; delivers events in (old, new].
    Reverse, ///< Decreasing timeline position; delivers reversible events in [new, old).
};

/// @brief How a seek moves a player's cursor.
enum class ClipSeekPolicy : foundation::uint8
{
    Scrub,    ///< Jump without delivering events or root motion (the default).
    Traverse, ///< Traverse the interval to the target within the current cycle, like an advance.
};

/// @brief One delivered timeline event with the identities that make a crossing unique.
///
/// (InstanceId, ActivationId, Cycle, EventId) identifies a crossing in one direction, so replay
/// and filtering can recognize a duplicate.
struct ClipEvent final
{
    foundation::uint64 InstanceId = 0;   ///< Owner-supplied player instance identity.
    foundation::uint64 ActivationId = 0; ///< Owner-supplied activation identity.
    foundation::int64 Cycle = 0;         ///< Signed loop cycle containing the event; zero for clamped clips.
    foundation::uint64 EventId = 0;      ///< Authored event identity.
    foundation::float64 TimeSeconds = 0; ///< Canonical event time within its cycle.
    ClipTraversalDirection Direction = ClipTraversalDirection::Forward; ///< Direction of the crossing.
};

/// @brief Parameters of a clip activation.
struct ClipActivation final
{
    foundation::uint64 InstanceId = 0;        ///< Nonzero player instance identity, stable across activations.
    foundation::uint64 ActivationId = 0;      ///< Nonzero identity of this activation, chosen by the owner.
    foundation::float64 StartTimeSeconds = 0; ///< Start position in [0, duration]; a loop maps the duration to 0.
    foundation::float64 Rate = 1;             ///< Finite signed playback rate; negative plays in reverse.
};

/// @brief Committed outcome of one advance or seek.
struct ClipAdvanceResult final
{
    foundation::usize EventCount = 0; ///< Events written to the front of the caller's event buffer.
    /// @brief Requested root motion over the traversed interval, in the character-local frame at its start.
    ///
    /// inverse(R(old)) * cycleEnd^cycles * R(new): a request to the game motor, never a transform
    /// write. It depends only on the traversed timeline interval, so motion the motor rejected is
    /// never requested again by a later advance.
    RootTransform RootDelta{};
    foundation::int64 CyclesCrossed = 0; ///< Signed loop crossings; zero for clamped clips.
    bool HasCompleted = false;           ///< Whether this call latched completion of a clamped clip.
};

/// @brief Control state of one clip on the simulation tick, independent of visual pose evaluation.
///
/// Stores the clip fingerprint, instance and activation identities, the float64 cycle-local time,
/// the signed loop cycle, the playback rate, the previous interval boundary and the completion
/// latch. It holds no reference to the clip, owns no allocation and is cheap to copy. Advance it
/// every tick even when no pose is sampled: TrySampleClip reads GetTimeSeconds and changes nothing.
class ClipPlayer final
{
public:
    [[nodiscard]] bool IsActive() const noexcept;    ///< Whether an activation is current (not stopped).
    [[nodiscard]] bool IsPaused() const noexcept;    ///< Whether advances are suspended.
    [[nodiscard]] bool IsCompleted() const noexcept; ///< Whether a clamped clip latched completion.
    [[nodiscard]] foundation::uint64 GetInstanceId() const noexcept;   ///< Instance identity, or zero.
    [[nodiscard]] foundation::uint64 GetActivationId() const noexcept; ///< Activation identity, or zero.
    /// Fingerprint of the activated clip generation; zero when never activated.
    [[nodiscard]] foundation::Fingerprint128 GetClipFingerprint() const noexcept;
    /// Cycle-local position: [0, duration) for loops, [0, duration] when clamped.
    [[nodiscard]] foundation::float64 GetTimeSeconds() const noexcept;
    /// Cycle-local position at the start of the last committed interval.
    [[nodiscard]] foundation::float64 GetPreviousTimeSeconds() const noexcept;
    /// Signed loop cycle of the current position; zero for clamped clips.
    [[nodiscard]] foundation::int64 GetCycle() const noexcept;
    [[nodiscard]] foundation::float64 GetRate() const noexcept; ///< Signed playback rate.

    /// @brief Change the playback rate; the cursor, cycle and completion latch are unchanged.
    /// @param rate Finite signed rate; zero holds the cursor without pausing.
    /// @return Success, NotActive or InvalidArgument (nonfinite rate); unchanged on failure.
    [[nodiscard]] AnimationStatus TrySetRate(foundation::float64 rate) noexcept;
    void Pause() noexcept;  ///< Suspend advances; no effect when inactive.
    void Resume() noexcept; ///< Resume advances; no effect when inactive.
    void Stop() noexcept;   ///< End the activation; identities and position stay readable.

private:
    friend AnimationStatus
    TryActivateClipPlayer(const Clip& clip, const ClipActivation& activation, ClipPlayer& outPlayer) noexcept;
    friend AnimationStatus TryAdvanceClipPlayer(const Clip& clip,
                                                foundation::float64 deltaSeconds,
                                                foundation::BorrowedView<ClipEvent> outEvents,
                                                ClipPlayer& inoutPlayer,
                                                ClipAdvanceResult& outResult) noexcept;
    friend AnimationStatus TrySeekClipPlayer(const Clip& clip,
                                             foundation::float64 targetTimeSeconds,
                                             ClipSeekPolicy policy,
                                             foundation::BorrowedView<ClipEvent> outEvents,
                                             ClipPlayer& inoutPlayer,
                                             ClipAdvanceResult& outResult) noexcept;

    foundation::Fingerprint128 mClipFingerprint{};
    foundation::uint64 mInstanceId = 0;
    foundation::uint64 mActivationId = 0;
    foundation::float64 mTimeSeconds = 0;
    foundation::float64 mPreviousTimeSeconds = 0;
    foundation::int64 mCycle = 0;
    foundation::float64 mRate = 1;
    bool mIsActive = false;
    bool mIsPaused = false;
    bool mIsCompleted = false;
};

/// @brief Start a clip activation, replacing any previous player state.
/// @param clip Prepared clip.
/// @param activation Nonzero identities, a start time in [0, duration] and a finite rate.
/// @param outPlayer Replaced only on Success with an active, unpaused, uncompleted player at cycle zero.
///                  Activation delivers no events: state entry is separate from a time-zero event.
/// @return Success or InvalidArgument. Allocation-free.
[[nodiscard]] AnimationStatus
TryActivateClipPlayer(const Clip& clip, const ClipActivation& activation, ClipPlayer& outPlayer) noexcept;

/// @brief Advance a player by one simulation step and commit its control result.
///
/// The step covers rate * deltaSeconds of timeline. Forward traversal delivers events in
/// (old, new]; reverse traversal delivers reversible events in [new, old). Loop crossings are
/// split into ordered per-cycle intervals; events carry the cycle that contains them, so a
/// loop-start event uses the new cycle. A clamped clip stops at the endpoint it reaches, latching
/// completion and reporting HasCompleted once; later advances of a completed or paused player
/// succeed without motion or events. Events are counted before any is written: when they exceed
/// the buffer the step is rejected without advancing the cursor, so no crossing is lost.
/// @param clip The clip generation the player was activated with.
/// @param deltaSeconds Finite nonnegative simulation step in seconds.
/// @param outEvents Caller-owned event buffer; only the first EventCount entries are written, and
///                  none on failure.
/// @param inoutPlayer Active player; unchanged on failure.
/// @param outResult Replaced only on Success.
/// @return Success, NotActive, IncompatibleClip, InvalidArgument, EventCapacityExceeded or
///         OutOfRange (nonfinite step or more than MAX_CLIP_TRAVERSAL_CYCLES crossings).
///         Allocation-free; no thread affinity.
[[nodiscard]] AnimationStatus TryAdvanceClipPlayer(const Clip& clip,
                                                   foundation::float64 deltaSeconds,
                                                   foundation::BorrowedView<ClipEvent> outEvents,
                                                   ClipPlayer& inoutPlayer,
                                                   ClipAdvanceResult& outResult) noexcept;

/// @brief Move a player's cursor to a timeline position under an explicit policy.
///
/// Every seek clears the completion latch first. Scrub jumps with no events and an identity root
/// delta. Traverse moves within the current cycle, ignoring rate and pause, with the same event,
/// root-motion and completion rules as TryAdvanceClipPlayer; it lands exactly on the target and
/// latches completion only when the target is the endpoint in its direction. Under both policies
/// a loop target at the duration enters the next cycle at its start.
/// @param clip The clip generation the player was activated with.
/// @param targetTimeSeconds Finite position in [0, duration].
/// @param policy Scrub or Traverse.
/// @param outEvents Caller-owned event buffer, as for TryAdvanceClipPlayer.
/// @param inoutPlayer Active player; unchanged on failure.
/// @param outResult Replaced only on Success.
/// @return Success, NotActive, IncompatibleClip, InvalidArgument, EventCapacityExceeded or OutOfRange
///         (the cycle count would overflow). Allocation-free.
[[nodiscard]] AnimationStatus TrySeekClipPlayer(const Clip& clip,
                                                foundation::float64 targetTimeSeconds,
                                                ClipSeekPolicy policy,
                                                foundation::BorrowedView<ClipEvent> outEvents,
                                                ClipPlayer& inoutPlayer,
                                                ClipAdvanceResult& outResult) noexcept;
} // namespace ludus::animation
