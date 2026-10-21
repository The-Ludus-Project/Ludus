#include "internal/viewport_statistics.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>

namespace ludus::editor
{
namespace
{
using namespace foundation;
/// Decimal places shown for millisecond values.
constexpr int32 MILLISECOND_DECIMALS = 2;
constexpr int32 FPS_DECIMALS = 1;
QString FormatMilliseconds(const RollingAverage& average)
{
    return average.GetCount() == 0 ? QStringLiteral("…")
                                   : QStringLiteral("%1 ms").arg(average.GetAverage(), 0, 'f', MILLISECOND_DECIMALS);
}
} // namespace
void RollingAverage::Add(float64 value) noexcept
{
    if (math::IsFinite(value) == false || value < 0)
    {
        return;
    }
    mSamples[mNext] = value;
    mNext = (mNext + 1) % VIEWPORT_STATISTICS_AVERAGE_FRAMES;
    mCount = math::Min(mCount + 1, VIEWPORT_STATISTICS_AVERAGE_FRAMES);
}
void RollingAverage::Reset() noexcept
{
    mCount = 0;
    mNext = 0;
}
usize RollingAverage::GetCount() const noexcept
{
    return mCount;
}
float64 RollingAverage::GetAverage() const noexcept
{
    if (mCount == 0)
    {
        return 0;
    }
    // Retained samples are the mCount entries ending just before mNext.
    float64 sum = 0;
    for (usize i = 0; i < mCount; ++i)
    {
        sum += mSamples[(mNext + VIEWPORT_STATISTICS_AVERAGE_FRAMES - 1 - i) % VIEWPORT_STATISTICS_AVERAGE_FRAMES];
    }
    return sum / static_cast<float64>(mCount);
}
void GraphViewportStatistics::Reset() noexcept
{
    mCpuMilliseconds.Reset();
    mReadbackMilliseconds.Reset();
    mFrameNanoseconds.Reset();
    mPendingCpuMilliseconds = 0;
    mLastFrameNanoseconds = 0;
    mHasFrame = false;
}
void GraphViewportStatistics::AddCpuWork(float64 milliseconds) noexcept
{
    if (math::IsFinite(milliseconds) == true && milliseconds >= 0)
    {
        mPendingCpuMilliseconds += milliseconds;
    }
}
void GraphViewportStatistics::CompleteFrame(uint64 nowNanoseconds, float64 readbackMilliseconds) noexcept
{
    if (mHasFrame == true && nowNanoseconds > mLastFrameNanoseconds &&
        nowNanoseconds - mLastFrameNanoseconds < IDLE_NANOSECONDS)
    {
        mFrameNanoseconds.Add(static_cast<float64>(nowNanoseconds - mLastFrameNanoseconds));
    }
    else
    {
        mFrameNanoseconds.Reset();
    }
    mLastFrameNanoseconds = nowNanoseconds;
    mHasFrame = true;
    mCpuMilliseconds.Add(mPendingCpuMilliseconds);
    mPendingCpuMilliseconds = 0;
    mReadbackMilliseconds.Add(readbackMilliseconds);
}
float64 GraphViewportStatistics::GetFramesPerSecond(uint64 nowNanoseconds) const noexcept
{
    if (mHasFrame == false || nowNanoseconds < mLastFrameNanoseconds ||
        nowNanoseconds - mLastFrameNanoseconds >= IDLE_NANOSECONDS || mFrameNanoseconds.GetCount() == 0)
    {
        return 0;
    }
    return static_cast<float64>(time::NANOSECONDS_PER_SECOND) / mFrameNanoseconds.GetAverage();
}
const RollingAverage& GraphViewportStatistics::GetCpuMilliseconds() const noexcept
{
    return mCpuMilliseconds;
}
const RollingAverage& GraphViewportStatistics::GetReadbackMilliseconds() const noexcept
{
    return mReadbackMilliseconds;
}
QString FormatGraphViewportStatistics(const GraphViewportStatistics& statistics,
                                      const ViewportStatistics& deviceStatistics,
                                      uint64 nowNanoseconds)
{
    const QString gpu = deviceStatistics.GpuState == ViewportGpuTimeState::Unavailable
                            ? QStringLiteral("n/a")
                            : FormatMilliseconds(deviceStatistics.GpuMilliseconds);
    return QStringLiteral("FPS (updates): %1\nCPU: %2\nGPU: %3\nReadback: %4")
        .arg(statistics.GetFramesPerSecond(nowNanoseconds), 0, 'f', FPS_DECIMALS)
        .arg(FormatMilliseconds(statistics.GetCpuMilliseconds()),
             gpu,
             FormatMilliseconds(statistics.GetReadbackMilliseconds()));
}
QString FormatViewportStatistics(const ViewportStatistics& statistics)
{
    const QString gpu = statistics.GpuState == ViewportGpuTimeState::Unavailable
                            ? QStringLiteral("n/a")
                            : FormatMilliseconds(statistics.GpuMilliseconds);
    return QStringLiteral("GPU: %1\nCPU: %2\n%3×%4 · draws %5")
        .arg(gpu, FormatMilliseconds(statistics.CpuMilliseconds))
        .arg(statistics.Width)
        .arg(statistics.Height)
        .arg(statistics.Draws);
}
} // namespace ludus::editor
