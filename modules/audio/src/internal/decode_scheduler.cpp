#include "internal/decode_scheduler.hpp"

namespace ludus::audio::internal
{
uint32 DecodeScheduler::AddStream(DecodeSource* source, uint64 refillTarget) noexcept
{
    if (source == nullptr)
    {
        return SCHEDULER_STREAM_CAPACITY;
    }
    for (uint32 i = 0; i < SCHEDULER_STREAM_CAPACITY; ++i)
    {
        if (mStreams[i].Active == false)
        {
            mStreams[i] = ScheduledStream{};
            mStreams[i].Active = true;
            mStreams[i].Source = source;
            mStreams[i].RefillTarget = refillTarget;
            return i;
        }
    }
    return SCHEDULER_STREAM_CAPACITY;
}

void DecodeScheduler::CancelStream(uint32 slot) noexcept
{
    if (slot < SCHEDULER_STREAM_CAPACITY && mStreams[slot].Active)
    {
        mStreams[slot].Cancelled = true;
    }
}

ScheduledStream* DecodeScheduler::Stream(uint32 slot) noexcept
{
    return slot < SCHEDULER_STREAM_CAPACITY ? &mStreams[slot] : nullptr;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named slot/frames.
void DecodeScheduler::NotifyConsumed(uint32 slot, uint64 frames) noexcept
{
    if (slot >= SCHEDULER_STREAM_CAPACITY || mStreams[slot].Active == false)
    {
        return;
    }
    ScheduledStream& s = mStreams[slot];
    s.Buffered = frames >= s.Buffered ? 0 : s.Buffered - frames;
}

bool DecodeScheduler::BeginColdPrepare(DecodeSource* source, uint64 totalTargetFrames) noexcept
{
    if (source == nullptr || mCold.Active)
    {
        return false;
    }
    mCold = ScheduledStream{};
    mCold.Active = true;
    mCold.Source = source;
    mCold.RefillTarget = totalTargetFrames;
    return true;
}

uint32 DecodeScheduler::PickMostStarved() noexcept
{
    // Order by time-to-empty = Buffered / sessionRate. Since the rate is common,
    // the least Buffered is the most starved. Break equal deadlines round-robin
    // using LastServiceRank (serve the one not served most recently first). Only
    // streams below their refill target and not cancelled/eof are candidates.
    uint32 best = SCHEDULER_STREAM_CAPACITY;
    uint64 bestBuffered = 0;
    uint32 bestRank = 0;
    for (uint32 i = 0; i < SCHEDULER_STREAM_CAPACITY; ++i)
    {
        ScheduledStream& s = mStreams[i];
        if (s.Active == false || s.Cancelled || s.Eof || s.Buffered >= s.RefillTarget)
        {
            continue;
        }
        if (best == SCHEDULER_STREAM_CAPACITY || s.Buffered < bestBuffered ||
            (s.Buffered == bestBuffered && s.LastServiceRank < bestRank))
        {
            best = i;
            bestBuffered = s.Buffered;
            bestRank = s.LastServiceRank;
        }
    }
    return best;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): named frame counts.
SchedulerStats DecodeScheduler::RunPass(float32* scratch, uint32 scratchFrames, uint32 budgetUnits) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    SchedulerStats stats{};
    bool runwaySeen = false;

    const uint32 unitCap =
        scratchFrames < COLD_DECODE_UNIT_FRAMES ? scratchFrames : static_cast<uint32>(COLD_DECODE_UNIT_FRAMES);
    if (scratch == nullptr || unitCap == 0)
    {
        return stats;
    }

    for (uint32 unit = 0; unit < budgetUnits; ++unit)
    {
        // Between units: honour cancellations first (retire cancelled streams),
        // then recheck deadlines. This is the yield point.
        ++stats.YieldPoints;
        for (uint32 i = 0; i < SCHEDULER_STREAM_CAPACITY; ++i)
        {
            if (mStreams[i].Active && mStreams[i].Cancelled)
            {
                mStreams[i] = ScheduledStream{};
                ++stats.CancellationsHonored;
            }
        }

        // Deadline priority: an active refill below its target always preempts
        // the cold prepare, so a long cold job never starves an established
        // stream (design section 9).
        const uint32 starved = PickMostStarved();
        ScheduledStream* job = nullptr;
        bool isCold = false;
        if (starved != SCHEDULER_STREAM_CAPACITY)
        {
            job = &mStreams[starved];
            job->LastServiceRank = ++mRoundRobin;
        }
        else if (mCold.Active && mCold.Cancelled == false && mCold.Eof == false && mCold.Produced < mCold.RefillTarget)
        {
            job = &mCold;
            isCold = true;
        }

        if (job == nullptr)
        {
            break; // nothing needs work this pass
        }

        // Observe the minimum buffered runway across active refills before this
        // unit (diagnostic; the cold job has no runway constraint).
        for (uint32 i = 0; i < SCHEDULER_STREAM_CAPACITY; ++i)
        {
            if (mStreams[i].Active && mStreams[i].Cancelled == false && mStreams[i].Eof == false)
            {
                if (runwaySeen == false || mStreams[i].Buffered < stats.MinBufferedRunway)
                {
                    stats.MinBufferedRunway = mStreams[i].Buffered;
                    runwaySeen = true;
                }
            }
        }

        // Decode one cold unit of at most unitCap output frames.
        bool eof = false;
        const uint32 produced = job->Source->Decode(scratch, unitCap, eof);
        job->Buffered += produced;
        job->Produced += produced;
        job->Eof = eof;
        stats.FramesDecoded += produced;
        ++stats.UnitsDecoded;

        if (isCold && (eof || mCold.Produced >= mCold.RefillTarget))
        {
            mCold.Active = false; // cold prepare complete
        }
    }

    return stats;
}

} // namespace ludus::audio::internal
