#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/gameplay/camera/camera.hpp>
#include <ludus/gameplay/sequencer/time.hpp>

namespace ludus::gameplay::sequencer
{
/// Default admission count for object/camera track groups, independent of World limits.
inline constexpr foundation::usize DEFAULT_SEQUENCE_MAX_TRACKS = 256;
/// Default total key admission count across every channel of one prepared sequence.
inline constexpr foundation::usize DEFAULT_SEQUENCE_MAX_KEYS = 65536;
/// Default storage payload allowance for one immutable prepared revision, in bytes.
inline constexpr foundation::usize DEFAULT_SEQUENCE_MAX_PREPARED_BYTES = foundation::usize{8} * 1024 * 1024;
/// Largest span duration admitted for exact integer-to-binary64 local timing conversion.
inline constexpr foundation::uint64 MAX_SEQUENCE_SPAN_TICKS = foundation::uint64{1} << 53;
/// Maximum absolute squared-norm error admitted before a quaternion is normalized at preparation.
inline constexpr foundation::float32 SEQUENCE_ROTATION_UNIT_TOLERANCE = 1e-4F;

/// Interpolation of the outgoing span. Exact keys select their own value; endpoints hold.
enum class SequenceInterpolation : foundation::uint8
{
    Step,   ///< Hold the left key throughout [left time, right time).
    Linear, ///< Linear scalar/vector interpolation; shortest-path spherical rotation interpolation.
};

/// One parent-local translation or scale key. Translation uses world units; scale is dimensionless.
struct SequenceVectorKey final
{
    foundation::uint64 Tick = 0;       ///< Nonnegative authored time, strictly increasing within its channel.
    foundation::math::Vector3 Value{}; ///< Finite translation or strictly positive componentwise scale.
    SequenceInterpolation Interpolation = SequenceInterpolation::Linear; ///< Outgoing span kind.
};

/// One rigid camera position in float64 world coordinates and world units.
struct SequencePositionKey final
{
    foundation::uint64 Tick = 0;                ///< Nonnegative authored time, strictly increasing within its channel.
    foundation::math::Vector3d PositionWorld{}; ///< Finite world position; never narrowed to float32 here.
    SequenceInterpolation Interpolation = SequenceInterpolation::Linear; ///< Outgoing span kind.
};

/// One local-to-parent object rotation or local-to-world camera orientation.
struct SequenceRotationKey final
{
    foundation::uint64 Tick = 0;          ///< Nonnegative authored time, strictly increasing within its channel.
    foundation::math::Quaternion Value{}; ///< Finite unit quaternion within SEQUENCE_ROTATION_UNIT_TOLERANCE.
    SequenceInterpolation Interpolation = SequenceInterpolation::Linear; ///< Step or shortest-path spherical blend.
};

/// A visibility change; always Step, without creating or destroying a scene object.
struct SequenceVisibilityKey final
{
    foundation::uint64 Tick = 0; ///< Nonnegative authored time, strictly increasing within its channel.
    bool IsVisible = true;       ///< Render inclusion from this key until the next key.
};

/// A complete camera lens key. Uses CameraLens units and active-field validation.
struct SequenceLensKey final
{
    foundation::uint64 Tick = 0; ///< Nonnegative authored time, strictly increasing within its channel.
    camera::CameraLens Value{};  ///< Valid lens; inactive fields are ignored during validation/interpolation.
    SequenceInterpolation Interpolation = SequenceInterpolation::Linear; ///< A projection-kind change requires Step.
};

/// Borrowed source channels for one resolved object binding. Empty channels keep the base value.
/// The adapter owns durable World/source identity resolution and parent composition;
/// BindingId is a nonzero sequence-local identity, never a persisted runtime handle.
struct SequenceObjectTrack final
{
    foundation::uint64 BindingId = 0; ///< Unique nonzero identity across both object and camera groups.
    foundation::math::TransformTRS BaseTransformLocal{}; ///< Finite parent-local TRS, unit rotation, positive scale.
    bool IsBaseVisible = true;                           ///< Visibility when the visibility channel is absent.
    /// Parent-local world-unit translation.
    foundation::BorrowedView<const SequenceVectorKey> TranslationKeys{nullptr, 0};
    /// Parent-local rotations.
    foundation::BorrowedView<const SequenceRotationKey> RotationKeys{nullptr, 0};
    /// Strictly positive componentwise scales.
    foundation::BorrowedView<const SequenceVectorKey> ScaleKeys{nullptr, 0};
    /// Render inclusion only.
    foundation::BorrowedView<const SequenceVisibilityKey> VisibilityKeys{nullptr, 0};
};

/// Borrowed source channels for one unparented sequence camera; absent channels keep BaseSample.
struct SequenceCameraTrack final
{
    foundation::uint64 BindingId = 0;  ///< Unique nonzero identity across both object and camera groups.
    camera::CameraSample BaseSample{}; ///< Valid camera, including output/revision metadata; metadata is retained.
    /// Float64 world positions.
    foundation::BorrowedView<const SequencePositionKey> PositionKeys{nullptr, 0};
    /// Local-to-world rigid orientations.
    foundation::BorrowedView<const SequenceRotationKey> RotationKeys{nullptr, 0};
    /// Complete lens values.
    foundation::BorrowedView<const SequenceLensKey> LensKeys{nullptr, 0};
};

/// Source tracks borrowed only until TryPrepareSequence returns. At least one group is required.
struct SequenceSource final
{
    /// Object track groups in source order.
    foundation::BorrowedView<const SequenceObjectTrack> Objects{nullptr, 0};
    /// Camera track groups in source order.
    foundation::BorrowedView<const SequenceCameraTrack> Cameras{nullptr, 0};
};

/// Per-revision preparation admission policy. Each allowance must be positive.
/// MaxPreparedBytes counts the storage object plus exact array capacities; allocator
/// bookkeeping and coexistence with an old accepted revision are additional peak memory.
struct SequenceLimits final
{
    foundation::usize MaxTracks = DEFAULT_SEQUENCE_MAX_TRACKS;                ///< Combined object/camera group count.
    foundation::usize MaxKeys = DEFAULT_SEQUENCE_MAX_KEYS;                    ///< Combined key count over all channels.
    foundation::usize MaxPreparedBytes = DEFAULT_SEQUENCE_MAX_PREPARED_BYTES; ///< Storage payload bytes.
};

/// One sampled object's parent-local values. The scene adapter composes parents before children.
struct SequenceObjectSample final
{
    foundation::uint64 BindingId = 0;                ///< Identity of the sampled binding.
    foundation::math::TransformTRS TransformLocal{}; ///< Local TRS; no world hierarchy or GPU state is retained.
    bool IsVisible = true;                           ///< Render inclusion; does not imply a gameplay lifetime change.
};

namespace detail
{
struct SequenceStorage;
}

/// Immutable move-only sequence revision owning copied keys, normalized rotations and base values.
/// Preparation may allocate; sampling never allocates or advances mutable playback state.
/// Const sampling is safe concurrently while this owner stays alive and unmoved;
/// replacement/move/destruction requires external synchronization with its readers.
class PreparedSequence final
{
public:
    PreparedSequence() noexcept;                         ///< Construct unprepared storage without allocation.
    ~PreparedSequence() noexcept;                        ///< Release the owned revision; no readers may remain.
    PreparedSequence(PreparedSequence&& other) noexcept; ///< Transfer ownership; other becomes unprepared.
    PreparedSequence& operator=(PreparedSequence&& other) noexcept; ///< Release this revision and take other's.
    PreparedSequence(const PreparedSequence&) = delete;             ///< Revisions are not implicitly copied.
    PreparedSequence& operator=(const PreparedSequence&) = delete;  ///< Revisions are not implicitly copied.

    [[nodiscard]] bool IsPrepared() const noexcept;                  ///< Whether an accepted revision is owned.
    [[nodiscard]] foundation::usize GetObjectCount() const noexcept; ///< Object groups, or zero when unprepared.
    [[nodiscard]] foundation::usize GetCameraCount() const noexcept; ///< Camera groups, or zero when unprepared.
    [[nodiscard]] foundation::usize
    GetPreparedBytes() const noexcept; ///< Owned payload bytes, excluding allocator metadata.

private:
    friend SequenceStatus TryPrepareSequence(const SequenceSource& source,
                                             const SequenceLimits& limits,
                                             PreparedSequence& outSequence) noexcept;
    friend SequenceStatus TryEvaluateSequenceObject(const PreparedSequence& sequence,
                                                    foundation::uint64 bindingId,
                                                    SequenceTime time,
                                                    SequenceObjectSample& outSample) noexcept;
    friend SequenceStatus TryEvaluateSequenceCamera(const PreparedSequence& sequence,
                                                    foundation::uint64 bindingId,
                                                    SequenceTime time,
                                                    camera::CameraSample& outSample) noexcept;
    foundation::UniquePtr<detail::SequenceStorage> mStorage;
};

/// Validate and copy source channels into a candidate immutable revision.
/// Keys must be strictly ordered with span widths <= MAX_SEQUENCE_SPAN_TICKS.
/// Duplicate groups are rejected instead of silently assigning writer priority.
/// Rotations within the unit tolerance are normalized in the owned copy; source data is unchanged.
/// @param source Borrowed groups/keys; storage need not survive a successful return.
/// @param limits Track/key/payload admission policy; preparation has no thread affinity.
/// @param outSequence Replaced only on Success, preserving any accepted revision on every failure.
/// @return Success, InvalidArgument, InvalidKeys, DuplicateBinding, BudgetExceeded or OutOfMemory.
[[nodiscard]] SequenceStatus
TryPrepareSequence(const SequenceSource& source, const SequenceLimits& limits, PreparedSequence& outSequence) noexcept;

/// Sample one object's complete local value at an absolute time, independently of earlier calls.
/// Empty channels retain the base; nonempty channels hold their first/last endpoints.
/// Exact interior keys select the outgoing span; a single key is a constant channel.
/// @param sequence Prepared immutable owner retained for the call.
/// @param bindingId Nonzero object binding identity; camera bindings are a different requested kind.
/// @param time Nonnegative proper-fraction authored ticks; no caller tick-grid rounding is needed.
/// @param outSample Replaced only on Success, including when time/binding validation fails.
/// @return Success, Unprepared, MissingBinding or InvalidArgument. No allocation or thread affinity.
[[nodiscard]] SequenceStatus TryEvaluateSequenceObject(const PreparedSequence& sequence,
                                                       foundation::uint64 bindingId,
                                                       SequenceTime time,
                                                       SequenceObjectSample& outSample) noexcept;

/// Sample one rigid camera's pose/lens at an absolute time; retain BaseSample's metadata.
/// Uses the same boundary/ownership rules as TryEvaluateSequenceObject. Position remains float64.
/// The caller owns shot selection, history/cut revisions, presentation counters and scene publication.
/// @param sequence Prepared immutable owner retained for the call.
/// @param bindingId Nonzero camera binding identity.
/// @param time Nonnegative proper-fraction authored ticks.
/// @param outSample Replaced only on Success; a numerically invalid interpolated lens preserves it.
/// @return Success, Unprepared, MissingBinding, InvalidArgument or OutOfRange. Allocation-free.
[[nodiscard]] SequenceStatus TryEvaluateSequenceCamera(const PreparedSequence& sequence,
                                                       foundation::uint64 bindingId,
                                                       SequenceTime time,
                                                       camera::CameraSample& outSample) noexcept;
} // namespace ludus::gameplay::sequencer
