#include "internal/viewport_statistics.h"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::editor
{
namespace
{
using namespace foundation;
/// Decimal places shown for millisecond values.
constexpr int32 MILLISECOND_DECIMALS = 2;
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
