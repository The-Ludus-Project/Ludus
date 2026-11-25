#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/animation/pose.hpp>
#include <ludus/animation/skeleton.hpp>
#include <ludus/animation/status.hpp>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::animation
{
/// Default admission count of joint tracks in one clip; equal to the default skeleton joint budget.
inline constexpr foundation::usize DEFAULT_CLIP_MAX_TRACKS = DEFAULT_SKELETON_MAX_JOINTS;
/// Default admission count of pose samples per track in one clip (about 18 minutes at 60 Hz).
inline constexpr foundation::usize DEFAULT_CLIP_MAX_SAMPLES = 65536;
/// Default admission count of authored timeline events in one clip.
inline constexpr foundation::usize DEFAULT_CLIP_MAX_EVENTS = 1024;
/// Largest admitted sample rate in samples per second.
inline constexpr foundation::float64 MAX_CLIP_SAMPLE_RATE_HERTZ = 1e4;
/// @brief Largest admitted clip duration in seconds (one year).
///
/// Bounds the clip timeline so cycle arithmetic in float64 keeps sub-microsecond resolution.
inline constexpr foundation::float64 MAX_CLIP_DURATION_SECONDS = 31'536'000.0;
/// @brief Seconds by which the last sample time may exceed the clip duration.
///
/// Absorbs the rounding of (SampleCount - 1) / SampleRateHertz for a cook whose last sample lies on
/// the endpoint. A later sample is rejected as InvalidArgument.
inline constexpr foundation::float64 CLIP_SAMPLE_TIME_TOLERANCE_SECONDS = 1e-9;
/// Version of the canonical byte layout hashed into a clip fingerprint.
inline constexpr foundation::uint32 CLIP_FINGERPRINT_VERSION = 1;

/// @brief Endpoint behavior of a clip timeline: an asset semantic, not a player choice.
enum class ClipWrapMode : foundation::uint8
{
    Clamp, ///< One-shot: time clamps to [0, duration] and completion latches once at the endpoint reached.
    Loop,  ///< Cyclic: time wraps in [0, duration); crossings are recorded as signed cycle counts.
};

/// @brief Policy of a clip's extracted root trajectory.
///
/// The cooker extracts a designated motion root into the trajectory and removes it from the visual
/// pose exactly once (docs/architecture/skeletal-animation.md, "Root motion and locomotion quality").
/// The sampled visual pose therefore never contains the trajectory again.
enum class RootMotionMode : foundation::uint8
{
    None,   ///< No trajectory; every root delta is the identity.
    Planar, ///< Trajectory translates in the character's XZ plane and rotates about +Y only.
    Full,   ///< Unrestricted rigid trajectory (translation and rotation).
};

/// @brief Scale-free rigid transform of a root trajectory: rotate by Rotation, then translate.
///
/// Applied to a point p as Rotate(Rotation, p) + Translation in the character-local frame. Rotation
/// is a unit Hamilton quaternion. Rotation and translation do not commute: compose with the
/// documented order, never by adding components.
struct RootTransform final
{
    foundation::math::Vector3 Translation = {0, 0, 0};                              ///< Translation in package units.
    foundation::math::Quaternion Rotation = foundation::math::QuaternionIdentity(); ///< Unit rotation.
};

/// @brief Compose rigid root transforms: apply right first, then left.
///
/// Evaluated in float64 and rounded once; the composed rotation is renormalized. Use it to apply
/// an accepted root delta to a character transform.
/// @param left Outer transform with a unit rotation.
/// @param right Inner transform with a unit rotation.
/// @param outComposed Replaced only on Success with left * right.
/// @return Success, InvalidTransform (nonfinite input) or OutOfRange (translation unrepresentable as
///         float32). Allocation-free.
[[nodiscard]] AnimationStatus
TryComposeRootTransforms(const RootTransform& left, const RootTransform& right, RootTransform& outComposed) noexcept;

/// @brief Inverse of a finite rigid root transform with a unit rotation.
///
/// Rotation preserves length, so the inverse of a finite transform is always representable.
/// @param value Transform to invert.
/// @return The transform t with value * t equal to the identity within float32 rounding.
[[nodiscard]] RootTransform InvertRootTransform(const RootTransform& value) noexcept;

/// @brief One authored timestamp event of a clip timeline.
///
/// Events are data delivered by the owner after a successful control commit, never callbacks. On a
/// looping clip an event at the duration is canonicalized to a loop-start event at zero, so the two
/// aliases cannot both fire. State-entry notifications are separate from a timestamp-zero event.
struct ClipEventRecord final
{
    foundation::uint64 EventId = 0;      ///< Nonzero identity, unique within the clip.
    foundation::float64 TimeSeconds = 0; ///< Timeline position in [0, duration].
    bool IsReversible = false;           ///< Whether reverse traversal also delivers this event.
};

/// @brief Stable authoring identity and revision of one cooked clip.
struct ClipIdentity final
{
    foundation::uint64 AuthoringId = 0; ///< Nonzero stable authoring identity.
    foundation::uint64 Revision = 0;    ///< Content revision of this cook; any value.

    /// Compare both identity fields.
    [[nodiscard]] friend constexpr bool operator==(const ClipIdentity&, const ClipIdentity&) noexcept = default;
};

/// @brief Borrowed uncompressed reference clip package, validated by TryPrepareClip.
///
/// Pose samples are uniform in time: sample i is at i / SampleRateHertz seconds and the last sample
/// may lie before the duration (a cook that dropped a duplicated loop endpoint). Between the last
/// sample and the duration, a looping clip interpolates towards sample 0 and a clamped clip holds the
/// last sample. A static pose may use one sample with a separate positive control duration.
/// The views need only remain valid until TryPrepareClip returns.
struct ClipPackage final
{
    ClipIdentity Identity{};                     ///< Authoring identity and revision recorded by the cook.
    foundation::float64 DurationSeconds = 0;     ///< Authored timeline length; zero only for clamped event-free clips.
    foundation::float64 SampleRateHertz = 0;     ///< Finite positive pose sample rate.
    foundation::uint32 SampleCount = 0;          ///< Positive number of pose samples per track.
    ClipWrapMode WrapMode = ClipWrapMode::Clamp; ///< Endpoint semantic of the timeline.
    /// Skeleton joint identity animated by each track; unique. Untracked joints sample their rest value.
    foundation::BorrowedView<const foundation::uint64> TrackJointIds{nullptr, 0};
    /// SampleCount * track count parent-local values, sample-major: Samples[sample * trackCount + track].
    foundation::BorrowedView<const foundation::math::TransformTRS> Samples{nullptr, 0};
    /// Timeline events ordered by nondecreasing time; may be empty.
    foundation::BorrowedView<const ClipEventRecord> Events{nullptr, 0};
    RootMotionMode RootMode = RootMotionMode::None; ///< Trajectory policy.
    /// SampleCount trajectory samples at the pose sample times when RootMode is not None, else empty.
    /// RootSamples[0] must be the identity: the cook stores that offset in RootReferenceOffset.
    foundation::BorrowedView<const RootTransform> RootSamples{nullptr, 0};
    /// Trajectory at the duration, R(duration). Kept even when the pose omits a duplicated loop
    /// endpoint; when the last sample lies on the duration it must equal that sample exactly.
    RootTransform RootEnd{};
    /// Character-local placement of the trajectory origin removed by the cook (R(0) before normalization).
    RootTransform RootReferenceOffset{};
};

/// Per-clip preparation admission policy.
struct ClipLimits final
{
    foundation::usize MaxTracks = DEFAULT_CLIP_MAX_TRACKS;   ///< Positive track count limit.
    foundation::usize MaxSamples = DEFAULT_CLIP_MAX_SAMPLES; ///< Positive per-track sample count limit.
    foundation::usize MaxEvents = DEFAULT_CLIP_MAX_EVENTS;   ///< Event count limit; zero forbids events.
};

namespace detail
{
struct ClipStorage;
}

/// @brief Immutable, move-only validated clip generation bound to one skeleton.
///
/// Owns normalized copies of the tracks, events (canonicalized, ordered by time then identity) and
/// root trajectory. Const reads are safe concurrently while the owner stays alive and unmoved;
/// replacement, move and destruction require external synchronization with readers.
class Clip final
{
public:
    Clip() noexcept;                        ///< Construct unprepared storage without allocation.
    ~Clip() noexcept;                       ///< Release the generation; no readers may remain.
    Clip(Clip&& other) noexcept;            ///< Transfer ownership; other becomes unprepared.
    Clip& operator=(Clip&& other) noexcept; ///< Release this generation and take other's.
    Clip(const Clip&) = delete;             ///< Generations are not implicitly copied.
    Clip& operator=(const Clip&) = delete;  ///< Generations are not implicitly copied.

    [[nodiscard]] bool IsPrepared() const noexcept;                        ///< Whether a generation is owned.
    [[nodiscard]] ClipIdentity GetIdentity() const noexcept;               ///< Package identity, or zeros.
    [[nodiscard]] foundation::float64 GetDurationSeconds() const noexcept; ///< Duration, or zero when unprepared.
    [[nodiscard]] foundation::float64 GetSampleRateHertz() const noexcept; ///< Sample rate, or zero.
    [[nodiscard]] foundation::uint32 GetSampleCount() const noexcept;      ///< Samples per track, or zero.
    [[nodiscard]] foundation::uint32 GetTrackCount() const noexcept;       ///< Track count, or zero.
    [[nodiscard]] ClipWrapMode GetWrapMode() const noexcept;               ///< Wrap mode; Clamp when unprepared.
    [[nodiscard]] RootMotionMode GetRootMode() const noexcept;             ///< Trajectory policy; None when unprepared.
    [[nodiscard]] RootTransform GetRootEnd() const noexcept;               ///< R(duration); identity without one.
    [[nodiscard]] RootTransform GetRootReferenceOffset() const noexcept;   ///< Removed trajectory origin.
    /// Compatibility identity of the skeleton the tracks were bound to; zero when unprepared.
    [[nodiscard]] foundation::Fingerprint128 GetSkeletonCompatibility() const noexcept;

    /// @brief Content identity used by players to reject a different generation.
    ///
    /// XXH3-128 over CLIP_FINGERPRINT_VERSION, the skeleton compatibility identity, timeline,
    /// tracks, samples, canonical events and trajectory. The authoring identity and revision are
    /// excluded, so an identical recook keeps its players valid. Zero when unprepared.
    [[nodiscard]] foundation::Fingerprint128 GetFingerprint() const noexcept;

    /// @brief Canonical events ordered by (time, identity); empty when unprepared.
    ///
    /// Looping clips report an authored endpoint event at time zero. Borrowed from this owner: valid
    /// until it is moved, replaced or destroyed.
    [[nodiscard]] foundation::BorrowedView<const ClipEventRecord> GetEvents() const noexcept;

private:
    friend AnimationStatus TryPrepareClip(const ClipPackage& package,
                                          const Skeleton& skeleton,
                                          const ClipLimits& limits,
                                          Clip& outClip) noexcept;
    friend AnimationStatus
    TrySampleClip(const Clip& clip, foundation::float64 sampleTimeSeconds, LocalPose& inoutPose) noexcept;
    friend AnimationStatus
    TrySampleClipRoot(const Clip& clip, foundation::float64 sampleTimeSeconds, RootTransform& outRoot) noexcept;
    foundation::UniquePtr<detail::ClipStorage> mStorage;
};

/// @brief Validate an uncompressed reference clip against a skeleton and publish a generation.
///
/// Rejects a zero authoring identity, a nonfinite, negative or oversized duration, a zero duration on
/// a looping or eventful clip, a nonpositive or oversized sample rate, a zero sample count, a last
/// sample later than the duration (beyond CLIP_SAMPLE_TIME_TOLERANCE_SECONDS), mismatched view
/// sizes, and counts beyond the limits (InvalidArgument or BudgetExceeded). Track identities absent
/// from the skeleton or duplicated return InvalidJointId; pose samples follow the skeleton's local
/// value rules (InvalidTransform, UnsupportedScale). Events need nonzero unique identities, finite
/// times in [0, duration] and nondecreasing order (InvalidArgument). Trajectory samples need finite
/// values and unit rotations, RootSamples[0] and the reference offset rotation must be valid, the
/// first sample must be the identity, and Planar trajectories must have zero Y translation and
/// zero X/Z rotation components exactly (InvalidTransform). Preparation may allocate.
/// @param package Borrowed package; its views need not survive a successful return.
/// @param skeleton Prepared skeleton whose joint identities the tracks name.
/// @param limits Admission policy.
/// @param outClip Replaced only on Success, preserving any previous generation on every failure.
/// @return Success or the failure listed above, or OutOfMemory. No thread affinity.
[[nodiscard]] AnimationStatus
TryPrepareClip(const ClipPackage& package, const Skeleton& skeleton, const ClipLimits& limits, Clip& outClip) noexcept;

/// @brief Reference sampler: write the clip's pose at a timeline position into a local pose.
///
/// Translation and scale interpolate linearly; rotation uses FoundationMath SlerpShortest between
/// the bracketing samples. Untracked joints receive their skeleton rest value. Every value is
/// computed and validated before the pose changes; the pose revision increases once on success.
/// Sampling reads only the clip and never advances any player or root motion, so skipping visual
/// evaluation cannot change control state.
/// @param clip Prepared clip.
/// @param sampleTimeSeconds Finite timeline position in [0, duration].
/// @param inoutPose Prepared local pose with the clip's skeleton compatibility identity; unchanged on failure.
/// @return Success, InvalidArgument (unprepared input or out-of-range time), IncompatibleSkeleton or
///         InvalidTransform. Allocation-free.
[[nodiscard]] AnimationStatus
TrySampleClip(const Clip& clip, foundation::float64 sampleTimeSeconds, LocalPose& inoutPose) noexcept;

/// @brief Sample the root trajectory R(t) at a timeline position.
///
/// Translation interpolates linearly and rotation with SlerpShortest; between the last sample and
/// the duration both interpolate towards RootEnd, and R(duration) is RootEnd exactly. Returns the
/// identity for RootMotionMode::None.
/// @param clip Prepared clip.
/// @param sampleTimeSeconds Finite timeline position in [0, duration].
/// @param outRoot Replaced only on Success.
/// @return Success or InvalidArgument. Allocation-free.
[[nodiscard]] AnimationStatus
TrySampleClipRoot(const Clip& clip, foundation::float64 sampleTimeSeconds, RootTransform& outRoot) noexcept;
} // namespace ludus::animation
