#include <ludus/audio/content/music_director.h>
#include <ludus/foundation/base/checked_integer.hpp>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::audio::content
{
namespace
{
using LayerGains = ludus::foundation::StaticArray<float32, STREAM_CAPACITY>;
LayerGains ComputeGains(const MusicSection& section, float32 intensity, float32 gain) noexcept
{
    LayerGains gains{};
    for (usize i = 0; i < section.LayerCount; ++i)
    {
        const MusicLayer& layer = section.Layers[i];
        for (usize j = 1; j < layer.PointCount; ++j)
        {
            const LayerPoint& end = layer.Points[j];
            if (intensity <= end.Intensity)
            {
                const LayerPoint& begin = layer.Points[j - 1];
                gains[i] = gain * ludus::foundation::math::Lerp(begin.Gain,
                                                                end.Gain,
                                                                (intensity - begin.Intensity) /
                                                                    (end.Intensity - begin.Intensity));
                break;
            }
        }
    }
    return gains;
}
} // namespace
MusicDirector::MusicDirector(AudioSystem& inoutAudio, Loader& inoutLoader) noexcept
    : mAudio(inoutAudio), mLoader(inoutLoader)
{
}
MusicDirector::~MusicDirector() noexcept
{
    retirePrepared();
    if (mTransport.Active.IsValid() == true)
    {
        (void)mAudio.Stop(mTransport.Active);
    }
    if (mTransport.Incoming.IsValid() == true)
    {
        (void)mAudio.Stop(mTransport.Incoming);
    }
    if (mFading.IsValid() == true)
    {
        (void)mAudio.Stop(mFading);
    }
    for (VoiceHandle voice : mAccents)
    {
        if (voice.IsValid() == true)
        {
            (void)mAudio.Stop(voice);
        }
    }
}
MusicTransport MusicDirector::GetTransport() const noexcept
{
    MusicTransport result = mTransport;
    if (result.Active.IsValid() == true)
    {
        result.ActiveSection = mScore.Sections[mSection].Id;
    }
    if (result.HasPlan == true)
    {
        result.PendingSection = mScore.Sections[mCandidate].Id;
    }
    return result;
}
void MusicDirector::retirePrepared() noexcept
{
    for (usize i = 0; i < mPreparedCount; ++i)
    {
        (void)mAudio.RetireStream(mPrepared[i]);
    }
    mPreparedCount = 0;
}
Status MusicDirector::prepare(usize section) noexcept
{
    for (usize i = 0; i < mScore.Sections[section].LayerCount; ++i)
    {
        const Status result = mLoader.PrepareMusicLayer(mLease, section, i, mPrepared[i]);
        if (result != Status::Ok)
        {
            retirePrepared();
            return result;
        }
        ++mPreparedCount;
    }
    return Status::Ok;
}
Status MusicDirector::Start(Lease score, float32 intensity) noexcept
{
    if (mTransport.Active.IsValid() == true || foundation::math::IsFinite(intensity) == false || intensity < 0 ||
        intensity > 1)
    {
        return Status::InvalidArgument;
    }
    if (mLoader.GetMusicScore(score, mScore, mBus) != ContentStatus::Ok)
    {
        return Status::InvalidHandle;
    }
    mLease = score;
    mIntensity = intensity;
    for (usize i = 0; i < mScore.StateCount; ++i)
    {
        if (mScore.States[i].Id.View() != mScore.InitialState.View())
        {
            continue;
        }
        for (mSection = 0; mSection < mScore.SectionCount; ++mSection)
        {
            if (mScore.Sections[mSection].Id.View() == mScore.States[i].Section.View())
            {
                Status result = prepare(mSection);
                if (result == Status::Ok)
                {
                    const LayerGains gains = ComputeGains(mScore.Sections[mSection], intensity, mScore.Gain);
                    result = mAudio.PlayStreamGroup(std::span(mPrepared).first(mPreparedCount),
                                                    std::span(gains).first(mPreparedCount),
                                                    mBus,
                                                    mScore.Priority,
                                                    mTransport.Active);
                    retirePrepared();
                }
                return result;
            }
        }
    }
    return Status::InvalidArgument;
}
bool MusicDirector::quantize(MusicBoundary boundary, uint64 requested, uint64& outFrame) noexcept
{
    SystemSnapshot snapshot;
    mAudio.GetSystemSnapshot(snapshot);
    VoiceInfo current;
    if (mAudio.GetVoiceInfo(mTransport.Active, current) != Status::Ok || current.State != VoiceState::Mixed)
    {
        return false;
    }
    if (mHasOrigin == false)
    {
        mOrigin = current.StartFrame;
        mTransport.AppliedFrame = current.StartFrame;
        mHasOrigin = true;
    }
    uint64 next = 0;
    uint64 origin = 0;
    return TryAdd(mOrigin, current.HeldFrames, origin) == true &&
           TryAdd(snapshot.RenderFrame, uint64{1}, next) == true &&
           QuantizeMusicFrame(mScore,
                              snapshot.SampleRate,
                              boundary,
                              origin,
                              ludus::foundation::math::Max(requested, next),
                              outFrame) == true;
}
Status MusicDirector::SelectState(std::string_view state, MusicCueTime cue) noexcept
{
    if (mTransport.Active.IsValid() == false)
    {
        return Status::NotReady;
    }
    usize candidate = mScore.SectionCount;
    for (usize i = 0; i < mScore.StateCount; ++i)
    {
        if (mScore.States[i].Id.View() == state)
        {
            for (usize j = 0; j < mScore.SectionCount; ++j)
            {
                if (mScore.Sections[j].Id.View() == mScore.States[i].Section.View())
                {
                    candidate = j;
                    break;
                }
            }
            break;
        }
    }
    if (mFading.IsValid() == true)
    {
        VoiceInfo fading;
        if (mAudio.GetVoiceInfo(mFading, fading) == Status::Ok && fading.State != VoiceState::Terminal)
        {
            return Status::NotReady;
        }
        mFading = {};
    }
    if (candidate == mScore.SectionCount)
    {
        return Status::InvalidArgument;
    }
    if (candidate == mSection)
    {
        return mTransport.HasPlan == true ? CancelTransition() : Status::Ok;
    }
    if (mTransport.HasPlan == true && candidate != mCandidate)
    {
        return Status::NotReady;
    }
    usize edge = 0;
    while (edge < mScore.TransitionCount &&
           (mScore.Transitions[edge].From.View() != mScore.Sections[mSection].Id.View() ||
            mScore.Transitions[edge].To.View() != mScore.Sections[candidate].Id.View()))
    {
        ++edge;
    }
    if (edge == mScore.TransitionCount)
    {
        return Status::InvalidArgument;
    }
    uint64 frame = 0;
    if (quantize(mScore.Transitions[edge].Quantize, cue.RequestedFrame, frame) == false)
    {
        return Status::NotReady;
    }
    if (mTransport.Incoming.IsValid() == true)
    {
        const Status result = mAudio.ReviseStreamTransition(mTransport.Incoming, frame);
        if (result != Status::Ok)
        {
            return result;
        }
    }
    else if (mPreparedCount == 0)
    {
        const Status result = prepare(candidate);
        if (result != Status::Ok)
        {
            return result;
        }
    }
    VoiceInfo active;
    if (mAudio.GetVoiceInfo(mTransport.Active, active) == Status::Ok)
    {
        mPlanOrigin = mOrigin + active.HeldFrames;
    }
    mTransport.HasPlan = true;
    mCandidate = candidate;
    mEdge = edge;
    mTransport.RequestedFrame = cue.RequestedFrame;
    mTransport.BoundaryFrame = frame;
    return mPreparedCount > 0 ? submit() : Status::Ok;
}
Status MusicDirector::submit() noexcept
{
    SystemSnapshot snapshot;
    mAudio.GetSystemSnapshot(snapshot);
    if (quantize(mScore.Transitions[mEdge].Quantize, mTransport.RequestedFrame, mTransport.BoundaryFrame) == false)
    {
        return Status::NotReady;
    }
    if (mTransport.BoundaryFrame - snapshot.RenderFrame > uint64{snapshot.SampleRate} * MAX_SCHEDULE_HORIZON_SECONDS)
    {
        return Status::Pending;
    }
    VoiceInfo active;
    if (mAudio.GetVoiceInfo(mTransport.Active, active) != Status::Ok ||
        TryAdd(mOrigin, active.HeldFrames, mPlanOrigin) == false)
    {
        return Status::NotReady;
    }
    const LayerGains gains = ComputeGains(mScore.Sections[mCandidate], mIntensity, mScore.Gain);
    const Status result = mAudio.ScheduleStreamGroup(
        std::span(mPrepared).first(mPreparedCount),
        std::span(gains).first(mPreparedCount),
        mTransport.Active,
        { .StartFrame = mTransport.BoundaryFrame, .DurationFrames = mScore.Transitions[mEdge].FadeFrames },
        mTransport.Incoming);
    if (result == Status::Ok)
    {
        retirePrepared();
    }
    return result;
}
Status MusicDirector::CancelTransition() noexcept
{
    if (mTransport.HasPlan == false)
    {
        return Status::NotReady;
    }
    if (mTransport.Incoming.IsValid() == true)
    {
        const Status result = mAudio.ReviseStreamTransition(mTransport.Incoming, 0);
        if (result != Status::Ok)
        {
            return result;
        }
    }
    retirePrepared();
    mTransport.HasPlan = false;
    mTransport.Incoming = {};
    return Status::Ok;
}
Status MusicDirector::Service() noexcept
{
    mLoader.Service();
    if (mPreparedCount > 0)
    {
        return submit();
    }
    if (mTransport.Incoming.IsValid() == true)
    {
        VoiceInfo info;
        const Status result = mAudio.GetVoiceInfo(mTransport.Incoming, info);
        if (result != Status::Ok || info.State == VoiceState::Terminal)
        {
            mTransport.HasPlan = false;
            mTransport.Incoming = {};
            return Status::NotReady;
        }
        if (info.State == VoiceState::Mixed && info.CursorFrame > 0)
        {
            if (info.StartFrame >= mTransport.BoundaryFrame)
            {
                mOrigin = mPlanOrigin + (info.StartFrame - mTransport.BoundaryFrame);
            }
            mFading = mTransport.Active;
            mSection = mCandidate;
            mTransport.Active = mTransport.Incoming;
            mTransport.Incoming = {};
            mTransport.HasPlan = false;
            mTransport.AppliedFrame = info.StartFrame;
        }
    }
    VoiceInfo active;
    if (mAudio.GetVoiceInfo(mTransport.Active, active) != Status::Ok || active.State == VoiceState::Terminal)
    {
        return active.Terminal == TerminalReason::StreamError ? Status::DecodeError : Status::NotReady;
    }
    mTransport.SectionFrame = active.CursorFrame;
    mTransport.HeldFrames = active.HeldFrames;
    if (active.State == VoiceState::Mixed && mHasOrigin == false)
    {
        mOrigin = active.StartFrame;
        mHasOrigin = true;
        mTransport.AppliedFrame = active.StartFrame;
    }
    return Status::Ok;
}
Status MusicDirector::SetIntensity(float32 intensity, MusicCueTime cue) noexcept
{
    if (foundation::math::IsFinite(intensity) == false || intensity < 0 || intensity > 1)
    {
        return Status::InvalidArgument;
    }
    if (mTransport.HasPlan == true)
    {
        return Status::NotReady;
    }
    uint64 frame = 0;
    if (quantize(MusicBoundary::Beat, cue.RequestedFrame, frame) == false)
    {
        return Status::NotReady;
    }
    const MusicSection& section = mScore.Sections[mSection];
    const LayerGains gains = ComputeGains(section, intensity, mScore.Gain);
    const Status result = mAudio.SetStreamGroupGains(mTransport.Active,
                                                     std::span(gains).first(section.LayerCount),
                                                     { .StartFrame = frame, .DurationFrames = mScore.LayerRampFrames });
    if (result == Status::Ok)
    {
        mIntensity = intensity;
    }
    return result;
}
Status MusicDirector::PlayStinger(std::string_view id, Lease sound, uint32 variation, MusicCueTime cue) noexcept
{
    Sound definition;
    app::EventDescriptor event;
    if (mLoader.GetSound(sound, definition, event) != ContentStatus::Ok || variation >= event.VariationCount ||
        event.Looping == true)
    {
        return Status::InvalidArgument;
    }
    for (usize i = 0; i < mScore.StingerCount; ++i)
    {
        const MusicStinger& accent = mScore.Stingers[i];
        if (id != accent.Id.View() || definition.Id.View() != accent.Sound.View())
        {
            continue;
        }
        PlayParams play;
        play.Clip = event.Variations[variation];
        play.BusIndex = event.BusIndex;
        play.GroupIndex = event.GroupIndex;
        play.Rate = event.Rate;
        play.Priority = event.Priority;
        play.Gain = event.DefaultGain * mScore.Gain;
        if (quantize(accent.Quantize, cue.RequestedFrame, play.StartFrame) == false)
        {
            return Status::NotReady;
        }
        for (usize j = 0; j < mScore.MaxStingers; ++j)
        {
            VoiceInfo info;
            if (mAudio.GetVoiceInfo(mAccents[j], info) != Status::Ok || info.State == VoiceState::Terminal)
            {
                return mAudio.PlayClip(play, mAccents[j]);
            }
        }
        return Status::VoiceCapacity;
    }
    return Status::InvalidArgument;
}
} // namespace ludus::audio::content
