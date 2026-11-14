#include <ludus/gameplay/camera/director.hpp>

#include <ludus/foundation/math/precision.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <limits>

namespace ludus::gameplay::camera
{
namespace math = foundation::math;
using foundation::float32;
using foundation::float64;
using foundation::int32;
using foundation::uint16;
using foundation::uint32;
using foundation::uint64;
using foundation::usize;

namespace
{
/// Largest eye-to-pivot component, in world units, that an AroundPivot blend narrows to a local offset.
constexpr float64 MAX_PIVOT_BLEND_OFFSET = 1.0e6;

[[nodiscard]] CameraStatus ValidateTransition(const CameraTransition& transition) noexcept
{
    if (math::IsFinite(transition.DurationSeconds) == false || transition.DurationSeconds < 0 ||
        (transition.Easing != CameraEasing::Linear && transition.Easing != CameraEasing::SmoothStep) ||
        (transition.Path != CameraBlendPath::Linear && transition.Path != CameraBlendPath::AroundPivot))
    {
        return CameraStatus::InvalidInput;
    }
    return CameraStatus::Success;
}

[[nodiscard]] CameraStatus ValidateRequest(const CameraRigRequest& request) noexcept
{
    if (ValidateCameraRigDefinition(request.Definition) != CameraStatus::Success ||
        ValidateTransition(request.Transition) != CameraStatus::Success ||
        (request.Definition.Recipe != CameraRecipe::Fixed && request.BindingId == 0))
    {
        return CameraStatus::InvalidInput;
    }
    return CameraStatus::Success;
}

/// First target with the binding, or null for binding zero or an absent target.
[[nodiscard]] const CameraTargetSample* FindTarget(foundation::BorrowedView<const CameraTargetSample> targets,
                                                   uint64 bindingId) noexcept
{
    if (bindingId == 0)
    {
        return nullptr;
    }
    for (const CameraTargetSample& target : targets)
    {
        if (target.BindingId == bindingId)
        {
            return &target;
        }
    }
    return nullptr;
}

/// Same projection kind and clip planes: only magnification may blend.
[[nodiscard]] bool AreLensesCompatible(const CameraLens& first, const CameraLens& second) noexcept
{
    return first.Projection == second.Projection && first.NearPlane == second.NearPlane &&
           (first.Projection == CameraProjection::PerspectiveInfinite || first.FarPlane == second.FarPlane);
}

[[nodiscard]] float64 Ease(CameraEasing easing, float64 time) noexcept
{
    return easing == CameraEasing::SmoothStep ? math::SmoothStep01(time) : time;
}

[[nodiscard]] math::Vector3d LerpPosition(math::Vector3d first, math::Vector3d second, float64 weight) noexcept
{
    return {math::Lerp(first.X, second.X, weight),
            math::Lerp(first.Y, second.Y, weight),
            math::Lerp(first.Z, second.Z, weight)};
}

[[nodiscard]] math::Vector3d Widen(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

/// Perspective magnification cot(verticalFov / 2).
[[nodiscard]] float64 GetPerspectiveScale(float32 verticalFov) noexcept
{
    const float64 half = 0.5 * static_cast<float64>(verticalFov);
    return math::Cos(half) / math::Sin(half);
}

/// Interpolate magnification geometrically (linear in log projection scale); clip planes are shared.
[[nodiscard]] CameraStatus
TryBlendLens(const CameraLens& from, const CameraLens& to, float64 weight, CameraLens& outLens) noexcept
{
    CameraLens lens = to;
    if (to.Projection == CameraProjection::OrthographicFinite)
    {
        const float64 logSpan = math::Lerp(math::Log(static_cast<float64>(from.VerticalSpan)),
                                           math::Log(static_cast<float64>(to.VerticalSpan)),
                                           weight);
        lens.VerticalSpan = static_cast<float32>(math::Exp(logSpan));
        lens.MinimumVisibleWidth =
            math::Lerp(from.MinimumVisibleWidth, to.MinimumVisibleWidth, static_cast<float32>(weight));
    }
    else
    {
        const float64 logScale = math::Lerp(math::Log(GetPerspectiveScale(from.VerticalFov)),
                                            math::Log(GetPerspectiveScale(to.VerticalFov)),
                                            weight);
        lens.VerticalFov = static_cast<float32>(2.0 * math::Atan2(1.0, math::Exp(logScale)));
    }
    if (math::IsFinite(lens.VerticalSpan) == false || math::IsFinite(lens.VerticalFov) == false)
    {
        return CameraStatus::OutOfRange;
    }
    outLens = lens;
    return CameraStatus::Success;
}

/// Blend two published endpoints along the transition path.
[[nodiscard]] CameraStatus TryBlendPose(const CameraPose& fromPose,
                                        const math::Vector3d& fromPivot,
                                        const CameraPose& toPose,
                                        const math::Vector3d& toPivot,
                                        float64 weight,
                                        CameraBlendPath path,
                                        CameraPose& outPose,
                                        math::Vector3d& outPivot) noexcept
{
    const math::Quaternion orientation =
        math::SlerpShortest(fromPose.Orientation, toPose.Orientation, static_cast<float32>(weight));
    const math::Vector3d pivot = LerpPosition(fromPivot, toPivot, weight);
    math::Vector3d eye = LerpPosition(fromPose.Position, toPose.Position, weight);
    if (path == CameraBlendPath::AroundPivot)
    {
        // Each endpoint's eye, expressed in its own camera frame relative to its pivot, keeps the shared pivot on
        // the same image location while the orientation turns.
        math::Vector3 fromOffset;
        math::Vector3 toOffset;
        if (math::TryMakeRelative(fromPose.Position, fromPivot, MAX_PIVOT_BLEND_OFFSET, fromOffset) !=
                math::MathStatus::Success ||
            math::TryMakeRelative(toPose.Position, toPivot, MAX_PIVOT_BLEND_OFFSET, toOffset) !=
                math::MathStatus::Success)
        {
            return CameraStatus::OutOfRange;
        }
        const math::Vector3 local = math::Lerp(math::Rotate(math::Conjugate(fromPose.Orientation), fromOffset),
                                               math::Rotate(math::Conjugate(toPose.Orientation), toOffset),
                                               static_cast<float32>(weight));
        eye = pivot + Widen(math::Rotate(orientation, local));
    }
    if (math::IsFinite(eye) == false || math::IsFinite(pivot) == false)
    {
        return CameraStatus::OutOfRange;
    }
    outPose = { .Position = eye, .Orientation = orientation };
    outPivot = pivot;
    return CameraStatus::Success;
}
} // namespace

CameraStatus CameraDirector::TryConfigure(const CameraDirectorSettings& settings) noexcept
{
    if (settings.OutputId == 0 || math::IsFinite(settings.MaximumDeltaSeconds) == false ||
        settings.MaximumDeltaSeconds <= 0)
    {
        return CameraStatus::InvalidInput;
    }
    mSettings = settings;
    Reset();
    return CameraStatus::Success;
}

CameraStatus CameraDirector::SetBaseRig(const CameraRigRequest& request) noexcept
{
    if (ValidateRequest(request) != CameraStatus::Success)
    {
        return CameraStatus::InvalidInput;
    }
    if (mHasBase == true && mBase == request)
    {
        return CameraStatus::Success;
    }
    if (mNextSerial == std::numeric_limits<uint64>::max())
    {
        return CameraStatus::CapacityExceeded;
    }
    mBase = request;
    mBaseSerial = mNextSerial;
    ++mNextSerial;
    mHasBase = true;
    return CameraStatus::Success;
}

CameraStatus CameraDirector::AcquireOverride(const CameraRigRequest& request,
                                             int32 priority,
                                             uint64 ownerId,
                                             CameraOverrideToken& outToken) noexcept
{
    if (ValidateRequest(request) != CameraStatus::Success || ownerId == 0)
    {
        return CameraStatus::InvalidInput;
    }
    if (mNextSerial == std::numeric_limits<uint64>::max())
    {
        return CameraStatus::CapacityExceeded;
    }
    for (usize slot = 0; slot < MAX_CAMERA_OVERRIDES; ++slot)
    {
        OverrideSlot& record = mOverrides[slot];
        // A slot whose generation is exhausted retires rather than wrapping into an old token.
        if (record.IsActive == true || record.Generation == MAX_CAMERA_OVERRIDE_GENERATION)
        {
            continue;
        }
        ++record.Generation;
        record.Request = request;
        record.Serial = mNextSerial;
        record.OwnerId = ownerId;
        record.Priority = priority;
        record.IsActive = true;
        ++mNextSerial;
        outToken =
        {
            .OutputId = mSettings.OutputId,
            .Slot = static_cast<uint32>(slot),
            .Generation = record.Generation,
        };
        return CameraStatus::Success;
    }
    return CameraStatus::CapacityExceeded;
}

CameraStatus CameraDirector::ReleaseOverride(const CameraOverrideToken& token,
                                             const CameraTransition& transition) noexcept
{
    if (token.OutputId != mSettings.OutputId || token.Slot >= MAX_CAMERA_OVERRIDES || token.Generation == 0)
    {
        return CameraStatus::StaleHandle;
    }
    OverrideSlot& record = mOverrides[token.Slot];
    if (record.IsActive == false || record.Generation != token.Generation)
    {
        return CameraStatus::StaleHandle;
    }
    if (ValidateTransition(transition) != CameraStatus::Success)
    {
        return CameraStatus::InvalidInput;
    }
    record.IsActive = false;
    mHistory.HasRevealTransition = true;
    mHistory.RevealTransition = transition;
    return CameraStatus::Success;
}

usize CameraDirector::RevokeOverridesOwnedBy(uint64 ownerId, const CameraTransition& transition) noexcept
{
    if (ownerId == 0 || ValidateTransition(transition) != CameraStatus::Success)
    {
        return 0;
    }
    usize count = 0;
    for (OverrideSlot& record : mOverrides)
    {
        if (record.IsActive == true && record.OwnerId == ownerId)
        {
            record.IsActive = false;
            ++count;
        }
    }
    if (count != 0)
    {
        mHistory.HasRevealTransition = true;
        mHistory.RevealTransition = transition;
    }
    return count;
}

void CameraDirector::RequestCut() noexcept
{
    mHistory.IsCutPending = true;
}

void CameraDirector::Reset() noexcept
{
    mHasBase = false;
    mBase = {};
    mBaseSerial = 0;
    for (OverrideSlot& record : mOverrides)
    {
        record.IsActive = false;
    }
    const uint64 revision = mHistory.DiscontinuityRevision;
    mHistory = {};
    mHistory.DiscontinuityRevision = revision;
}

bool CameraDirector::HasPublished() const noexcept
{
    return mHistory.HasPublished;
}

const CameraSample& CameraDirector::GetPublished() const noexcept
{
    return mHistory.Published;
}

CameraStatus CameraDirector::TryUpdate(const CameraFrameInput& input,
                                       CameraSample& outSample,
                                       CameraDirectorTrace* traceOrNull) noexcept
{
    const CameraStatus status = tryAdvance(input, outSample, traceOrNull);
    if (status != CameraStatus::Success)
    {
        // The next accepted update re-primes instead of treating this gap as continuous time.
        mHistory.HasFailedSinceCommit = true;
    }
    return status;
}

bool CameraDirector::selectRequest(Selection& outSelection) const noexcept
{
    const uint64 incumbent = mHistory.SelectedSerial;
    usize best = MAX_CAMERA_OVERRIDES;
    for (usize slot = 0; slot < MAX_CAMERA_OVERRIDES; ++slot)
    {
        const OverrideSlot& record = mOverrides[slot];
        if (record.IsActive == false)
        {
            continue;
        }
        if (best == MAX_CAMERA_OVERRIDES)
        {
            best = slot;
            continue;
        }
        const OverrideSlot& current = mOverrides[best];
        const bool isHigher = record.Priority > current.Priority;
        const bool isTied = record.Priority == current.Priority;
        // Equal priority keeps the incumbent; otherwise the most recent acquisition wins the tie.
        const bool isTieWon = isTied == true && current.Serial != incumbent &&
                              (record.Serial == incumbent || record.Serial > current.Serial);
        if (isHigher == true || isTieWon == true)
        {
            best = slot;
        }
    }
    if (best != MAX_CAMERA_OVERRIDES)
    {
        const OverrideSlot& record = mOverrides[best];
        outSelection =
        {
            .Serial = record.Serial,
            .RequestOrNull = &record.Request,
            .IsOverride = true,
            .Slot = static_cast<uint32>(best),
        };
        return true;
    }
    if (mHasBase == true)
    {
        outSelection = { .Serial = mBaseSerial, .RequestOrNull = &mBase };
        return true;
    }
    return false;
}

CameraStatus CameraDirector::tryAdvance(const CameraFrameInput& input,
                                        CameraSample& outSample,
                                        CameraDirectorTrace* traceOrNull) noexcept
{
    if (math::IsFinite(input.DeltaSeconds) == false || input.DeltaSeconds < 0 || input.Targets.IsValid() == false ||
        input.PreviousTick > input.CurrentTick || math::IsFinite(input.RenderAlpha) == false || input.RenderAlpha < 0 ||
        input.RenderAlpha > 1 || ValidateControlView(input.Control) != CameraStatus::Success)
    {
        return CameraStatus::InvalidInput;
    }
    for (const CameraTargetSample& target : input.Targets)
    {
        if (target.BindingId == 0 || math::IsFinite(target.Position) == false)
        {
            return CameraStatus::InvalidInput;
        }
    }
    Selection selection;
    if (selectRequest(selection) == false)
    {
        return CameraStatus::NoSelection;
    }
    const CameraRigRequest& request = *selection.RequestOrNull;
    const float64 applied = math::Min(input.DeltaSeconds, static_cast<float64>(mSettings.MaximumDeltaSeconds));

    // Every change happens on a copy, committed only after the sample validates.
    History history = mHistory;
    CameraCutReason cut = CameraCutReason::None;
    if (history.HasPublished == false)
    {
        cut = CameraCutReason::FirstPublication;
    }
    else if (history.HasFailedSinceCommit == true)
    {
        cut = CameraCutReason::Recovery;
    }
    else if (input.WorldRevision != history.WorldRevision)
    {
        cut = CameraCutReason::WorldDiscontinuity;
    }
    else if (history.IsCutPending == true)
    {
        cut = CameraCutReason::Requested;
    }
    if (selection.Serial != history.SelectedSerial)
    {
        // A request made since the last publication enters with its own transition; a rig revealed by a release
        // enters with the release's transition.
        const bool isNewRequest = selection.Serial > history.CommittedSerial;
        const CameraTransition transition = isNewRequest == true || history.HasRevealTransition == false
                                                ? request.Transition
                                                : history.RevealTransition;
        if (cut == CameraCutReason::None && transition.DurationSeconds == 0)
        {
            cut = CameraCutReason::ZeroDuration;
        }
        if (cut == CameraCutReason::None &&
            AreLensesCompatible(history.Published.Lens, request.Definition.Lens) == false)
        {
            cut = CameraCutReason::IncompatibleLens;
        }
        if (cut == CameraCutReason::None)
        {
            if (history.Outgoing == OutgoingKind::None)
            {
                history.Previous = history.Incoming;
                history.Outgoing = OutgoingKind::Live;
            }
            else
            {
                // Interrupted blend: start exactly from what was last shown.
                history.Previous.Sample = history.Published;
                history.Previous.Pivot = history.PublishedPivot;
                history.Outgoing = OutgoingKind::Frozen;
            }
            history.Blend = transition;
            history.BlendElapsedSeconds = 0;
        }
        history.Incoming = { .Request = request };
        history.SelectedSerial = selection.Serial;
    }
    if (cut != CameraCutReason::None)
    {
        history.Outgoing = OutgoingKind::None;
        history.Incoming.State = {};
    }

    CameraUpdateContext context
    {
        .OutputId = mSettings.OutputId,
        .PreviousTick = input.PreviousTick,
        .CurrentTick = input.CurrentTick,
        .RenderAlpha = input.RenderAlpha,
        .PresentationSequence = input.PresentationSequence,
        .DiscontinuityRevision = history.DiscontinuityRevision,
        .Control = input.Control,
    };
    Endpoint& incoming = history.Incoming;
    const bool wasPrimed = incoming.State.IsPrimed;
    CameraTrace incomingTrace;
    const CameraStatus status = TryAdvanceCameraRig(incoming.Request.Definition,
                                                    context,
                                                    FindTarget(input.Targets, incoming.Request.BindingId),
                                                    applied,
                                                    incoming.State,
                                                    incoming.Sample,
                                                    &incomingTrace);
    if (status != CameraStatus::Success)
    {
        return status;
    }
    incoming.Pivot = incomingTrace.Pivot;
    if (cut == CameraCutReason::None && wasPrimed == true && incomingTrace.IsReprimed == true)
    {
        cut = CameraCutReason::TargetDiscontinuity;
        history.Outgoing = OutgoingKind::None;
    }

    if (history.Outgoing == OutgoingKind::Live)
    {
        // The outgoing rig is optional: losing or teleporting its target freezes it instead of failing.
        Endpoint outgoing = history.Previous;
        CameraTrace outgoingTrace;
        const CameraStatus outgoingStatus = TryAdvanceCameraRig(outgoing.Request.Definition,
                                                                context,
                                                                FindTarget(input.Targets, outgoing.Request.BindingId),
                                                                applied,
                                                                outgoing.State,
                                                                outgoing.Sample,
                                                                &outgoingTrace);
        if (outgoingStatus == CameraStatus::Success && outgoingTrace.IsReprimed == false)
        {
            outgoing.Pivot = outgoingTrace.Pivot;
            history.Previous = outgoing;
        }
        else
        {
            history.Outgoing = OutgoingKind::Frozen;
        }
    }

    CameraSample result = incoming.Sample;
    math::Vector3d pivot = incoming.Pivot;
    float32 weight = 1.0F;
    const bool isOutgoingFrozen = history.Outgoing == OutgoingKind::Frozen;
    bool isBlending = false;
    if (history.Outgoing != OutgoingKind::None)
    {
        history.BlendElapsedSeconds += applied;
        const float64 duration = static_cast<float64>(history.Blend.DurationSeconds);
        const float64 time = duration > 0 ? math::Min(history.BlendElapsedSeconds / duration, 1.0) : 1.0;
        const float64 eased = Ease(history.Blend.Easing, time);
        weight = static_cast<float32>(eased);
        if (time >= 1.0)
        {
            history.Outgoing = OutgoingKind::None;
        }
        else
        {
            const Endpoint& outgoing = history.Previous;
            if (TryBlendPose(outgoing.Sample.Pose,
                             outgoing.Pivot,
                             incoming.Sample.Pose,
                             incoming.Pivot,
                             eased,
                             history.Blend.Path,
                             result.Pose,
                             pivot) != CameraStatus::Success ||
                TryBlendLens(outgoing.Sample.Lens, incoming.Sample.Lens, eased, result.Lens) != CameraStatus::Success)
            {
                return CameraStatus::OutOfRange;
            }
            isBlending = true;
        }
    }

    const uint64 revision = history.DiscontinuityRevision + (cut != CameraCutReason::None ? 1 : 0);
    result.OutputId = mSettings.OutputId;
    result.DefinitionRevision = incoming.Request.Definition.Revision;
    result.PreviousTick = input.PreviousTick;
    result.CurrentTick = input.CurrentTick;
    result.RenderAlpha = input.RenderAlpha;
    result.PresentationSequence = input.PresentationSequence;
    result.DiscontinuityRevision = revision;
    if (ValidateCameraSample(result) != CameraStatus::Success)
    {
        return CameraStatus::OutOfRange;
    }

    history.HasPublished = true;
    history.Published = result;
    history.PublishedPivot = pivot;
    history.DiscontinuityRevision = revision;
    history.WorldRevision = input.WorldRevision;
    history.CommittedSerial = mNextSerial - 1;
    history.IsCutPending = false;
    history.HasFailedSinceCommit = false;
    history.HasRevealTransition = false;
    const CameraDirectorTrace checkpoint
    {
        .SelectedRequest = selection.Serial,
        .IsOverrideSelected = selection.IsOverride,
        .OverrideSlot = selection.Slot,
        .CutReason = cut,
        .IsBlending = isBlending,
        .IsOutgoingFrozen = isBlending == true && isOutgoingFrozen == true,
        .BlendWeight = isBlending == true ? weight : 1.0F,
        .AppliedDeltaSeconds = applied,
        .DiscardedDeltaSeconds = input.DeltaSeconds - applied,
        .Incoming = incomingTrace,
        .Outgoing = isBlending == true ? history.Previous.Sample : CameraSample{},
        .Published = result,
    };
    mHistory = history;
    outSample = result;
    if (traceOrNull != nullptr)
    {
        *traceOrNull = checkpoint;
    }
    return CameraStatus::Success;
}
} // namespace ludus::gameplay::camera
