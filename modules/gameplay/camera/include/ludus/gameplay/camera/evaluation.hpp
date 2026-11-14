#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/gameplay/camera/camera.hpp>
#include <ludus/gameplay/camera/control.hpp>

namespace ludus::gameplay::camera
{
/// Placement recipes. Fixed and OrthographicFollow are the C0 recipes; Orbit is the C1 third-person recipe.
enum class CameraRecipe : foundation::uint8
{
    /// Publish the authored rigid pose and lens.
    Fixed,
    /// Follow an already interpolated target on world XY with fixed eye Z.
    OrthographicFollow,
    /// Third-person orbit: a pivot above the target, control yaw/pitch, a shoulder offset and an arm.
    Orbit,
};

/// Default orbit arm length in world units.
inline constexpr foundation::float32 DEFAULT_ORBIT_ARM_LENGTH = 4.0F;

/// Third-person placement. The eye is `pivot + Rotate(look, ShoulderOffset + (0, 0, ArmLength))`, so the arm
/// extends along camera local +Z (behind the view) and the pivot stays on the view axis when the shoulder offset
/// is zero. C1 has no collision: C2 shortens the arm against geometry.
struct CameraOrbitParameters final
{
    /// World-axis offset from the target position to the orbit pivot, in world units.
    foundation::math::Vector3 PivotOffset{};
    /// Camera-local offset of the arm root from the pivot (+X right, +Y up), in world units.
    foundation::math::Vector3 ShoulderOffset{};
    /// Finite nonnegative distance from the arm root to the eye, in world units.
    foundation::float32 ArmLength = DEFAULT_ORBIT_ARM_LENGTH;
    /// The control pitch is clamped to these bounds for this rig.
    CameraPitchLimits Pitch{};
    /// Exact memberwise comparison.
    [[nodiscard]] friend constexpr bool operator==(const CameraOrbitParameters&,
                                                   const CameraOrbitParameters&) = default;
};

/// Presentation lag of a stateful rig. Zero snaps. Fixed rigs ignore damping.
struct CameraDampingParameters final
{
    /// Finite nonnegative critical-spring response time of the target pivot, in seconds.
    foundation::float32 PivotSmoothTimeSeconds = 0.0F;
    /// Finite nonnegative half-life of the orbit look orientation, in seconds; keep zero or small for aiming.
    foundation::float32 LookHalfLifeSeconds = 0.0F;
    /// Exact memberwise comparison.
    [[nodiscard]] friend constexpr bool operator==(const CameraDampingParameters&,
                                                   const CameraDampingParameters&) = default;
};

/// Immutable definition borrowed only during evaluation; editing is game-owned.
struct CameraRigDefinition final
{
    /// Placement recipe.
    CameraRecipe Recipe = CameraRecipe::Fixed;
    /// Authored pose; follow uses its orientation and eye Z.
    CameraPose Pose{};
    /// Authored lens; follow requires OrthographicFinite.
    CameraLens Lens{};
    /// World XY offset from the resolved target, in world units, for follow.
    foundation::math::Vector2 FollowOffset{};
    /// Nonzero authored revision, copied into the publication and trace.
    foundation::uint64 Revision = 1;
    /// Orbit placement; used only by CameraRecipe::Orbit.
    CameraOrbitParameters Orbit{};
    /// Presentation lag for follow and orbit rigs advanced with TryAdvanceCameraRig.
    CameraDampingParameters Damping{};
    /// Exact memberwise comparison.
    [[nodiscard]] friend constexpr bool operator==(const CameraRigDefinition&, const CameraRigDefinition&) = default;
};

/// Game-resolved presentation target; never retained or interpolated again.
struct CameraTargetSample final
{
    /// Finite position in world units, drawn with the same tick pair and alpha.
    foundation::math::Vector3d Position{};
    /// Nonzero stable game binding key; contains no component pointer.
    foundation::uint64 BindingId = 1;
    /// Game-owned target reset revision; a change (teleport, respawn) re-primes stateful rigs.
    foundation::uint64 DiscontinuityRevision = 0;
};

/// Presentation timing and identity; C0 integrates no clock or mutable history.
struct CameraUpdateContext final
{
    /// Nonzero logical output identity.
    foundation::uint64 OutputId = 1;
    /// Previous tick used by the game's target extraction.
    foundation::uint64 PreviousTick = 0;
    /// Completed current tick, greater than or equal to PreviousTick.
    foundation::uint64 CurrentTick = 0;
    /// Target extraction alpha in [0,1], recorded without another interpolation.
    foundation::float32 RenderAlpha = 1.0F;
    /// Presentation sequence; zero is allowed for previews.
    foundation::uint64 PresentationSequence = 0;
    /// Explicit game-owned discontinuity revision.
    foundation::uint64 DiscontinuityRevision = 0;
    /// Authoritative aim from the completed ticks; read only by the orbit recipe.
    CameraControlView Control{};
};

/// Optional copied checkpoints; debugging owns its ring/capture storage.
struct CameraTrace final
{
    /// Evaluated recipe.
    CameraRecipe Recipe = CameraRecipe::Fixed;
    /// Zero for fixed, otherwise the resolved binding identity.
    foundation::uint64 BindingId = 0;
    /// Validated desired placement before damping (no constraints before C2).
    CameraPose Desired{};
    /// Complete accepted sample.
    CameraSample Published{};
    /// Published pivot: the followed target point (orbit pivot or follow target), or the eye for fixed rigs.
    foundation::math::Vector3d Pivot{};
    /// True when TryAdvanceCameraRig snapped its history to the desired placement in this update.
    bool IsReprimed = false;
};

/// Check a definition before it is stored or evaluated: a valid authored pose and lens, a nonzero revision,
/// finite nonnegative damping, and the recipe's own constraints (orthographic identity-rotation follow, finite
/// orbit offsets and a valid pitch range). No allocation.
[[nodiscard]] CameraStatus ValidateCameraRigDefinition(const CameraRigDefinition& definition) noexcept;

/// Evaluate the undamped desired shot of a fixed, XY orthographic follow or orbit rig, without allocations.
/// @param definition Immutable authored recipe and nonzero revision, borrowed for this call.
/// @param context Presentation metadata from the same target extraction.
/// @param target Required for follow and orbit; borrowed for this call, already interpolated.
/// @param output Replaced only on Success, including when trace is omitted.
/// @param trace Optional checkpoint, replaced only on Success; no diagnostic failure path.
/// @return Explicit validation/range/missing-target result; failure preserves both outputs.
/// @note Stateless and thread independent; damping is ignored. Follow requires identity rotation (either
/// sign) and has no dead zone, look-ahead or bounds. Orbit orients from context.Control with the rig's pitch
/// clamp. Never changes simulation state.
[[nodiscard]] CameraStatus TryEvaluateCamera(const CameraRigDefinition& definition,
                                             const CameraUpdateContext& context,
                                             const CameraTargetSample* target,
                                             CameraSample& output,
                                             CameraTrace* trace = nullptr) noexcept;

/// Damped history of one evaluated rig for one output. Plain values: copy it to checkpoint or preview.
struct CameraRigState final
{
    /// False until the first accepted advance; an unprimed state snaps to the desired placement.
    bool IsPrimed = false;
    /// Target binding the history was built against; a different binding re-primes.
    foundation::uint64 BindingId = 0;
    /// Target discontinuity revision the history was built against; a change re-primes.
    foundation::uint64 TargetDiscontinuityRevision = 0;
    /// Damped pivot in world units.
    foundation::math::Vector3d Pivot{};
    /// Pivot velocity in world units per second.
    foundation::math::Vector3d PivotVelocity{};
    /// Damped unit look orientation (orbit only).
    foundation::math::Quaternion Look{};
};

/// Advance one stateful rig by a presentation delta and publish its damped sample.
/// @param definition Valid definition, borrowed for this call.
/// @param context Presentation metadata and control from the same target extraction.
/// @param targetOrNull Required for follow and orbit, already interpolated; borrowed for this call.
/// @param deltaSeconds Finite nonnegative presentation seconds under the game's clock policy. Zero (pause)
/// advances no history.
/// @param inoutState Rig history. An unprimed state, a different target binding or a changed target
/// discontinuity revision snaps to the desired placement and reports CameraTrace::IsReprimed.
/// @param outSample Replaced only on Success.
/// @param traceOrNull Optional checkpoint, replaced only on Success.
/// @return Same failures as TryEvaluateCamera; every failure preserves state, sample and trace.
/// @note The pivot uses an exact critical spring; the orbit look uses exponential damping with SlerpShortest.
/// Advance each state at most once per presentation; evaluate previews with copied state. No allocation.
[[nodiscard]] CameraStatus TryAdvanceCameraRig(const CameraRigDefinition& definition,
                                               const CameraUpdateContext& context,
                                               const CameraTargetSample* targetOrNull,
                                               foundation::float64 deltaSeconds,
                                               CameraRigState& inoutState,
                                               CameraSample& outSample,
                                               CameraTrace* traceOrNull = nullptr) noexcept;
} // namespace ludus::gameplay::camera
