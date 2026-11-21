#include <ludus/graphics/rhi/hosted_recorder.h>

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/strings/utf8.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/measurement.h>

#include <cstring>
#include <string_view>

// The editor-side contract (admission, coverage and export) is owned by
// docs/architecture/editor-gui-systems.md, "Hosted recording".
namespace ludus::graphics::rhi
{
namespace
{
using foundation::float64;
using foundation::uint64;
using foundation::usize;

/// Longest device interval accepted as a measurement; longer ones are invalid samples.
constexpr float64 MAX_SAMPLE_NANOSECONDS = 60'000'000'000.0;

[[nodiscard]] bool IsTerminal(GpuTimingState state) noexcept
{
    return state == GpuTimingState::Available || state == GpuTimingState::Unavailable ||
           state == GpuTimingState::Disjoint || state == GpuTimingState::Failed || state == GpuTimingState::Empty;
}

[[nodiscard]] bool IsValidDuration(float64 duration) noexcept
{
    return duration >= 0 && duration < MAX_SAMPLE_NANOSECONDS;
}

/// Round a valid device interval to the nearest nanosecond.
[[nodiscard]] uint64 RoundDuration(float64 duration) noexcept
{
    return static_cast<uint64>(foundation::math::Floor(duration + 0.5));
}

[[nodiscard]] platform::HostedPassKind GetPassKind(GpuPassKind kind) noexcept
{
    switch (kind)
    {
        case GpuPassKind::Raster:
            return platform::HostedPassKind::Raster;
        case GpuPassKind::Compute:
            return platform::HostedPassKind::Compute;
        case GpuPassKind::RayTracing:
            return platform::HostedPassKind::RayTracing;
    }
    return platform::HostedPassKind::Raster;
}

/// Copy a terminated scope name; false for an unnamed scope.
[[nodiscard]] bool TryCopyScopeName(const GpuScopeDescription& scope, platform::HostedPassName& outName) noexcept
{
    usize length = 0;
    while (length < GPU_SCOPE_NAME_BYTES && scope.Name[length] != '\0')
    {
        ++length;
    }
    if (scope.Identity == 0 || length == 0 || length >= GPU_SCOPE_NAME_BYTES)
    {
        return false;
    }
    platform::HostedPassName name{};
    std::memcpy(name.GetData(), scope.Name.GetData(), length);
    outName = name;
    return true;
}
} // namespace

HostedGpuRecorder::~HostedGpuRecorder() noexcept
{
    Detach();
}

bool HostedGpuRecorder::Attach(const platform::NativeWindowInfo& window,
                               foundation::BorrowedView<const char> producerName) noexcept
{
    if (mChannelOrNull != nullptr || window.HostedRecordingOrNull == nullptr ||
        producerName.GetSize() > platform::HOSTED_RECORDING_PRODUCER_NAME_BYTES ||
        (producerName.IsEmpty() == false && producerName.IsValid() == false))
    {
        return false;
    }
    const std::string_view text = producerName.IsEmpty() == true
                                      ? std::string_view{}
                                      : std::string_view(producerName.GetDataOrNull(), producerName.GetSize());
    foundation::Utf8View validated;
    usize errorOffset = 0;
    if (text.find('\0') != std::string_view::npos ||
        foundation::ValidateUtf8(text, validated, errorOffset) != foundation::StringStatus::Ok)
    {
        return false;
    }
    platform::HostedProducerName name{};
    if (text.empty() == false)
    {
        std::memcpy(name.GetData(), text.data(), text.size());
    }
    mProducerName = name;
    mChannelOrNull = window.HostedRecordingOrNull;
    mChannelOrNull->AttachRecorder();
    mPhase = Phase::Idle;
    mFrame = 0;
    mIsBatchArmed = false;
    mIsFrameTiming = false;
    mBatchFrame = 0;
    mBatchRecording = 0;
    mPending = {};
    return true;
}

void HostedGpuRecorder::Detach() noexcept
{
    if (mChannelOrNull == nullptr)
    {
        return;
    }
    mChannelOrNull->DetachRecorder();
    mChannelOrNull = nullptr;
    mPhase = Phase::Idle;
    mPending = {};
}

bool HostedGpuRecorder::IsRecording() const noexcept
{
    return mPhase != Phase::Idle;
}

bool HostedGpuRecorder::hasBatch() const noexcept
{
    return mIsBatchArmed == true || mBatchFrame != 0;
}

void HostedGpuRecorder::answer(const platform::HostedRecordingRequest& request,
                               platform::HostedRecordingResult result,
                               uint64 frame) noexcept
{
    platform::HostedRecordingAcknowledgement acknowledgement;
    acknowledgement.Kind = request.Kind;
    acknowledgement.Result = result;
    acknowledgement.Recording = request.Recording;
    acknowledgement.Request = request.Request;
    acknowledgement.Frame = frame;
    acknowledgement.Ticks = foundation::time::NowTicks();
    acknowledgement.ProducerName = mProducerName;
    if (request.Kind == platform::HostedRecordingRequestKind::Stop && result == platform::HostedRecordingResult::Ok)
    {
        acknowledgement.Coverage = mCoverage;
    }
    if (request.Kind == platform::HostedRecordingRequestKind::Start && result == platform::HostedRecordingResult::Ok)
    {
        acknowledgement.IsPassTimingSupported = mIsFrameTiming == false;
    }
    for (PendingAcknowledgement& pending : mPending)
    {
        if (pending.IsValid == false)
        {
            pending = { .IsValid = true, .Acknowledgement = acknowledgement };
            break;
        }
    }
    flushAcknowledgements();
}

void HostedGpuRecorder::flushAcknowledgements() noexcept
{
    // Acknowledgements keep their order; a full recording send room retries next frame.
    for (PendingAcknowledgement& pending : mPending)
    {
        if (pending.IsValid == false)
        {
            continue;
        }
        const platform::HostedSurfaceStatus status = mChannelOrNull->Acknowledge(pending.Acknowledgement);
        if (status == platform::HostedSurfaceStatus::Busy)
        {
            return;
        }
        // Ok, a lost channel or a refusal: either way the acknowledgement is finished.
        pending.IsValid = false;
    }
    // Compact so later acknowledgements follow earlier ones.
    usize next = 0;
    for (usize i = 0; i < mPending.GetSize(); ++i)
    {
        if (mPending[i].IsValid == true)
        {
            mPending[next] = mPending[i];
            if (next != i)
            {
                mPending[i] = {};
            }
            ++next;
        }
    }
}

void HostedGpuRecorder::startRecording(DeviceHandle device, const platform::HostedRecordingRequest& request) noexcept
{
    if (mPhase != Phase::Idle)
    {
        answer(request, platform::HostedRecordingResult::Busy, 0);
        return;
    }
    // A batch still in flight proves its timing works on this device, so the recording keeps
    // that batch's kind. One that measured an earlier frame is claimed by no recording and is
    // discarded when it resolves; one still waiting for a frame measures this recording's
    // first frame.
    if (mIsBatchArmed == true)
    {
        mBatchTicks = foundation::time::NowTicks();
        mBatchRecording = request.Recording;
    }
    else if (hasBatch() == false)
    {
        // A device without per-pass counters still measures whole frames, and the
        // acknowledgement says so; only a device that measures neither is Unsupported.
        const RasterStatus passes = ArmGpuPassTimings(device);
        const bool isFrameTiming = passes == RasterStatus::Unsupported && ArmGpuTiming(device) == RasterStatus::Ready;
        if (passes != RasterStatus::Ready && isFrameTiming == false)
        {
            answer(request, platform::HostedRecordingResult::Unsupported, 0);
            return;
        }
        mIsFrameTiming = isFrameTiming;
        mIsBatchArmed = true;
        mBatchFrame = 0;
        mBatchTicks = foundation::time::NowTicks();
        mBatchRecording = request.Recording;
    }
    mPhase = Phase::Recording;
    mRecording = request.Recording;
    mCoverage = {};
    answer(request, platform::HostedRecordingResult::Ok, mFrame + 1);
}

void HostedGpuRecorder::deliverBatch(const GpuPassTimings& batch) noexcept
{
    mCoverage.QueryDroppedPasses += batch.DroppedPasses;
    if (batch.State != GpuTimingState::Available)
    {
        // The whole frame's measurement failed or was unavailable.
        ++mCoverage.InvalidSamples;
        return;
    }
    const uint64 collected = foundation::time::NowTicks();
    for (usize index = 0; index < batch.Count; ++index)
    {
        const GpuPassTiming& pass = batch.Passes[index];
        const float64 duration = pass.Timing.DurationNanoseconds;
        platform::HostedRecordingSample sample;
        if (pass.Timing.State != GpuTimingState::Available || IsValidDuration(duration) == false ||
            TryCopyScopeName(pass.Scope, sample.Name) == false)
        {
            // Disjoint or failed intervals, and passes outside a named scope, carry no sample.
            ++mCoverage.InvalidSamples;
            continue;
        }
        sample.Recording = mRecording;
        sample.Frame = mBatchFrame;
        sample.SubmissionTicks = mBatchTicks;
        sample.CollectionTicks = collected >= mBatchTicks ? collected : mBatchTicks;
        sample.DurationNanoseconds = RoundDuration(duration);
        sample.ScopeIdentity = pass.Scope.Identity;
        sample.ScopePassIndex = pass.ScopePassIndex;
        sample.LogicalPassCount = pass.Scope.LogicalPassCount;
        sample.Kind = GetPassKind(pass.Kind);
        if (pass.HasCpuInterval == true && pass.EndTicks >= pass.StartTicks)
        {
            sample.HasCpuInterval = true;
            sample.StartTicks = pass.StartTicks;
            sample.EndTicks = pass.EndTicks;
            sample.ClockUncertaintyNanoseconds = batch.ClockUncertaintyNanoseconds;
        }
        sendSample(sample);
    }
}

void HostedGpuRecorder::deliverFrame(const GpuTiming& timing) noexcept
{
    if (timing.State != GpuTimingState::Available || IsValidDuration(timing.DurationNanoseconds) == false)
    {
        ++mCoverage.InvalidSamples;
        return;
    }
    const uint64 collected = foundation::time::NowTicks();
    platform::HostedRecordingSample sample;
    sample.Recording = mRecording;
    sample.Frame = mBatchFrame;
    sample.SubmissionTicks = mBatchTicks;
    sample.CollectionTicks = collected >= mBatchTicks ? collected : mBatchTicks;
    sample.DurationNanoseconds = RoundDuration(timing.DurationNanoseconds);
    sample.IsFrame = true;
    sendSample(sample);
}

void HostedGpuRecorder::sendSample(const platform::HostedRecordingSample& sample) noexcept
{
    const platform::HostedSurfaceStatus status = mChannelOrNull->SendSample(sample);
    if (status == platform::HostedSurfaceStatus::Ok)
    {
        ++mCoverage.DeliveredSamples;
    }
    else if (status == platform::HostedSurfaceStatus::InvalidArgument)
    {
        ++mCoverage.InvalidSamples;
    }
    else
    {
        ++mCoverage.DroppedSamples;
    }
}

RasterStatus HostedGpuRecorder::armBatch(DeviceHandle device) const noexcept
{
    return mIsFrameTiming == true ? ArmGpuTiming(device) : ArmGpuPassTimings(device);
}

void HostedGpuRecorder::collectBatch(DeviceHandle device) noexcept
{
    if (mBatchFrame == 0)
    {
        return;
    }
    // The device is gone or failed when a poll is not Ready: the batch can no longer resolve.
    GpuPassTimings batch;
    GpuTiming frame;
    if (mIsFrameTiming == true && GetGpuTiming(device, frame) != RasterStatus::Ready)
    {
        frame.State = GpuTimingState::Failed;
    }
    if (mIsFrameTiming == false && GetGpuPassTimings(device, batch) != RasterStatus::Ready)
    {
        batch.State = GpuTimingState::Failed;
    }
    if (IsTerminal(mIsFrameTiming == true ? frame.State : batch.State) == false)
    {
        return;
    }
    const bool isClaimed = mPhase != Phase::Idle && mBatchRecording == mRecording &&
                           (mPhase == Phase::Recording || mBatchFrame <= mStopFrame);
    if (isClaimed == true && mIsFrameTiming == true)
    {
        deliverFrame(frame);
    }
    else if (isClaimed == true)
    {
        deliverBatch(batch);
    }
    mBatchFrame = 0;
    mBatchRecording = 0;
}

void HostedGpuRecorder::finishStop() noexcept
{
    if (mBatchFrame != 0 && mBatchRecording == mRecording && mBatchFrame <= mStopFrame)
    {
        ++mCoverage.UnresolvedBatches;
    }
    // A batch still armed or in flight is no longer claimed; it resolves unseen.
    mBatchRecording = 0;
    platform::HostedRecordingRequest request;
    request.Kind = platform::HostedRecordingRequestKind::Stop;
    request.Recording = mRecording;
    request.Request = mStopRequest;
    mPhase = Phase::Idle;
    answer(request, platform::HostedRecordingResult::Ok, mStopFrame);
}

void HostedGpuRecorder::BeginFrame(DeviceHandle device) noexcept
{
    if (mChannelOrNull == nullptr)
    {
        return;
    }
    flushAcknowledgements();
    platform::HostedRecordingRequest request;
    while (mChannelOrNull->TryTakeRecordingRequest(request) == true)
    {
        if (request.Kind == platform::HostedRecordingRequestKind::Start)
        {
            startRecording(device, request);
        }
        else if (mPhase == Phase::Recording && request.Recording == mRecording)
        {
            mPhase = Phase::Stopping;
            mStopRequest = request.Request;
            mStopFrame = mFrame;
            mStopFramesLeft = HOSTED_RECORDING_STOP_FRAMES;
        }
        else
        {
            answer(request, platform::HostedRecordingResult::UnknownRecording, 0);
        }
    }
    collectBatch(device);
    if (mPhase == Phase::Stopping)
    {
        const bool isWaiting = mBatchFrame != 0 && mBatchRecording == mRecording && mBatchFrame <= mStopFrame;
        if (isWaiting == false || mStopFramesLeft == 0)
        {
            finishStop();
        }
        else
        {
            --mStopFramesLeft;
        }
    }
    if (mPhase == Phase::Recording && hasBatch() == false && armBatch(device) == RasterStatus::Ready)
    {
        mIsBatchArmed = true;
        mBatchFrame = 0;
        mBatchTicks = foundation::time::NowTicks();
        mBatchRecording = mRecording;
    }
}

void HostedGpuRecorder::EndFrame(bool isSubmitted) noexcept
{
    if (mChannelOrNull == nullptr || isSubmitted == false)
    {
        return;
    }
    ++mFrame;
    bool isMeasured = false;
    if (mIsBatchArmed == true)
    {
        mIsBatchArmed = false;
        mBatchFrame = mFrame;
        isMeasured = mPhase == Phase::Recording && mBatchRecording == mRecording;
    }
    if (mPhase == Phase::Recording)
    {
        ++mCoverage.SubmittedFrames;
        if (isMeasured == true)
        {
            ++mCoverage.MeasuredFrames;
        }
        else
        {
            ++mCoverage.SkippedFrames;
        }
    }
}
} // namespace ludus::graphics::rhi
