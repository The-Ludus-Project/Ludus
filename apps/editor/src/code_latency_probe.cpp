#include "internal/code_latency_probe.h"

#include <algorithm>

namespace ludus::editor
{
using foundation::int32;
using foundation::int64;

void CodeLatencyProbe::Start() noexcept
{
    mIsRecording = true;
    mHasPendingKey = false;
    mWriteIndex = 0;
    mCount = 0;
    mSamples.clear();
}

void CodeLatencyProbe::Stop() noexcept
{
    mIsRecording = false;
    mHasPendingKey = false;
}

void CodeLatencyProbe::MarkKeyHandled() noexcept
{
    if (mIsRecording == false)
    {
        return;
    }
    mPendingKey.start();
    mHasPendingKey = true;
}

void CodeLatencyProbe::MarkPainted() noexcept
{
    if (mIsRecording == false || mHasPendingKey == false)
    {
        return;
    }
    const int64 elapsed = mPendingKey.nsecsElapsed();
    mHasPendingKey = false;
    if (mSamples.size() < CODE_LATENCY_SAMPLE_CAPACITY)
    {
        mSamples.append(elapsed);
    }
    else
    {
        mSamples[mWriteIndex] = elapsed;
    }
    mWriteIndex = (mWriteIndex + 1) % CODE_LATENCY_SAMPLE_CAPACITY;
    if (mCount < CODE_LATENCY_SAMPLE_CAPACITY)
    {
        mCount = mCount + 1;
    }
}

int32 CodeLatencyProbe::GetSampleCount() const noexcept
{
    return mCount;
}

int64 CodeLatencyProbe::GetPercentileNanoseconds(int32 percentile) const
{
    if (mCount == 0)
    {
        return 0;
    }
    QVector<int64> sorted = mSamples;
    std::sort(sorted.begin(), sorted.end());
    constexpr int32 PERCENT_MAX = 100;
    int32 rank = (percentile * (sorted.size() - 1)) / PERCENT_MAX;
    if (rank < 0)
    {
        rank = 0;
    }
    if (rank >= sorted.size())
    {
        rank = static_cast<int32>(sorted.size()) - 1;
    }
    return sorted.at(rank);
}
} // namespace ludus::editor
