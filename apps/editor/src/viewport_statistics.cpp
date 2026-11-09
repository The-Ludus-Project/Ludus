#include "internal/viewport_statistics.h"

#include <ludus/foundation/containers/sort.hpp>
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
constexpr usize PERCENTILE_NUMERATOR = 95;
constexpr usize PERCENTILE_DENOMINATOR = 100;
/// Characters of a pass name shown; longer names keep both ends so "Viewport/Scene" stays readable.
constexpr int32 PASS_NAME_COLUMNS = 22;
/// Characters of the right-aligned millisecond value.
constexpr int32 PASS_VALUE_COLUMNS = 6;
constexpr int32 PASS_NAME_HEAD_COLUMNS = (PASS_NAME_COLUMNS - 1) / 2;
constexpr int32 PASS_NAME_TAIL_COLUMNS = PASS_NAME_COLUMNS - 1 - PASS_NAME_HEAD_COLUMNS;
struct PassRow final
{
    usize Index = 0;
    float64 Milliseconds = 0;
};
/// Series with samples, slowest rolling average first; equal times keep first-appearance order.
usize RankPasses(const ViewportPassStatistics& passes, StaticArray<PassRow, MAX_VIEWPORT_PASS_SERIES>& outRows)
{
    usize count = 0;
    const Array<ViewportPassSeries>& series = passes.GetSeries();
    for (usize index = 0; index < series.GetSize() && count < outRows.GetSize(); ++index)
    {
        if (series[index].Milliseconds.GetCount() == 0)
        {
            continue;
        }
        outRows[count].Index = index;
        outRows[count].Milliseconds = series[index].Milliseconds.GetAverage();
        ++count;
    }
    Sort(outRows.GetData(), count, [](const PassRow& left, const PassRow& right) {
        return left.Milliseconds > right.Milliseconds ||
               (left.Milliseconds == right.Milliseconds && left.Index < right.Index);
    });
    return count;
}
QString FormatPassName(const QString& name)
{
    if (name.size() <= PASS_NAME_COLUMNS)
    {
        return name.leftJustified(PASS_NAME_COLUMNS);
    }
    return name.left(PASS_NAME_HEAD_COLUMNS) + QStringLiteral("…") + name.right(PASS_NAME_TAIL_COLUMNS);
}
QString FormatPassRow(const QString& label, float64 milliseconds)
{
    return QStringLiteral("%1 %2 ms")
        .arg(label, QString::number(milliseconds, 'f', MILLISECOND_DECIMALS).rightJustified(PASS_VALUE_COLUMNS));
}
QString FormatMilliseconds(const RollingAverage& average)
{
    return average.GetCount() == 0 ? QStringLiteral("…")
                                   : QStringLiteral("%1 ms").arg(average.GetAverage(), 0, 'f', MILLISECOND_DECIMALS);
}
QString FormatTimingSummary(const RollingAverage& average)
{
    if (average.GetCount() == 0)
    {
        return FormatMilliseconds(average);
    }
    return QStringLiteral("%1 (p95 %2, max %3 ms)")
        .arg(FormatMilliseconds(average))
        .arg(average.GetPercentile95(), 0, 'f', MILLISECOND_DECIMALS)
        .arg(average.GetMaximum(), 0, 'f', MILLISECOND_DECIMALS);
}
} // namespace
void ViewportPassStatistics::Reset() noexcept
{
    mSeries.Clear();
}
void ViewportPassStatistics::AddSample(uint64 identity, const QString& name, float64 milliseconds)
{
    if (identity == 0 || math::IsFinite(milliseconds) == false || milliseconds < 0)
    {
        return;
    }
    for (ViewportPassSeries& series : mSeries)
    {
        if (series.Identity == identity && series.Name == name)
        {
            series.BatchMilliseconds += milliseconds;
            series.IsInBatch = true;
            return;
        }
    }
    if (mSeries.GetSize() >= MAX_VIEWPORT_PASS_SERIES)
    {
        return;
    }
    ViewportPassSeries added;
    added.Identity = identity;
    added.Name = name;
    added.BatchMilliseconds = milliseconds;
    added.IsInBatch = true;
    mSeries.Add(Move(added));
}
void ViewportPassStatistics::CommitBatch()
{
    usize kept = 0;
    for (usize index = 0; index < mSeries.GetSize(); ++index)
    {
        ViewportPassSeries& series = mSeries[index];
        if (series.IsInBatch == true)
        {
            series.Milliseconds.Add(series.BatchMilliseconds);
            series.StaleBatches = 0;
        }
        else
        {
            ++series.StaleBatches;
        }
        series.BatchMilliseconds = 0;
        series.IsInBatch = false;
        if (series.StaleBatches >= VIEWPORT_PASS_STALE_BATCHES)
        {
            continue;
        }
        if (kept != index)
        {
            mSeries[kept] = Move(series);
        }
        ++kept;
    }
    while (mSeries.GetSize() > kept)
    {
        mSeries.RemoveAt(mSeries.GetSize() - 1);
    }
}
const Array<ViewportPassSeries>& ViewportPassStatistics::GetSeries() const noexcept
{
    return mSeries;
}
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
float64 RollingAverage::GetPercentile95() const noexcept
{
    if (mCount == 0)
    {
        return 0;
    }
    StaticArray<float64, VIEWPORT_STATISTICS_AVERAGE_FRAMES> ordered = mSamples;
    Sort(ordered.GetData(), mCount, [](float64 left, float64 right) noexcept { return left < right; });
    const usize rank = (mCount * PERCENTILE_NUMERATOR + PERCENTILE_DENOMINATOR - 1) / PERCENTILE_DENOMINATOR;
    return ordered[rank - 1];
}
float64 RollingAverage::GetMaximum() const noexcept
{
    float64 maximum = 0;
    for (usize index = 0; index < mCount; ++index)
    {
        maximum = math::Max(maximum, mSamples[index]);
    }
    return maximum;
}
float64 RollingAverage::GetLatest() const noexcept
{
    return mCount == 0
               ? 0
               : mSamples[(mNext + VIEWPORT_STATISTICS_AVERAGE_FRAMES - 1) % VIEWPORT_STATISTICS_AVERAGE_FRAMES];
}
bool ViewportSlowFrameMonitor::AddFrame(uint64 generation, float64 milliseconds) noexcept
{
    if (generation == 0 || generation != mGeneration)
    {
        // The first frame of a generation pays for its cold preparation; it does not show the drawing cost.
        mGeneration = generation;
        mStreak = 0;
        mStreakSumMilliseconds = 0;
        return false;
    }
    if (math::IsFinite(milliseconds) == false || milliseconds < mThresholdMilliseconds)
    {
        mStreak = 0;
        mStreakSumMilliseconds = 0;
        return false;
    }
    ++mStreak;
    mStreakSumMilliseconds += milliseconds;
    if (mStreak < SLOW_FRAME_STREAK)
    {
        return false;
    }
    mReportedMilliseconds = mStreakSumMilliseconds / static_cast<float64>(mStreak);
    mStreak = 0;
    mStreakSumMilliseconds = 0;
    return true;
}
void ViewportSlowFrameMonitor::Reset() noexcept
{
    mGeneration = 0;
    mStreak = 0;
    mStreakSumMilliseconds = 0;
    mReportedMilliseconds = 0;
}
void ViewportSlowFrameMonitor::SetThresholdMilliseconds(float64 milliseconds) noexcept
{
    if (math::IsFinite(milliseconds) == true && milliseconds >= 0)
    {
        mThresholdMilliseconds = milliseconds;
    }
}
float64 ViewportSlowFrameMonitor::GetThresholdMilliseconds() const noexcept
{
    return mThresholdMilliseconds;
}
float64 ViewportSlowFrameMonitor::GetStreakMilliseconds() const noexcept
{
    return mReportedMilliseconds;
}
void GraphViewportStatistics::Reset() noexcept
{
    mCpuMilliseconds.Reset();
    mReadbackMilliseconds.Reset();
    mFrameNanoseconds.Reset();
    mInspectionCpuMilliseconds.Reset();
    mInspectionReadbackMilliseconds.Reset();
    mConversionWorkMilliseconds.Reset();
    mConversionElapsedMilliseconds.Reset();
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
void GraphViewportStatistics::SetInspectionEnabled(bool isEnabled) noexcept
{
    mIsInspectionEnabled = isEnabled;
}
bool GraphViewportStatistics::IsInspectionEnabled() const noexcept
{
    return mIsInspectionEnabled;
}
void GraphViewportStatistics::AddInspection(float64 cpuMilliseconds, float64 elapsedMilliseconds) noexcept
{
    mInspectionCpuMilliseconds.Add(cpuMilliseconds);
    mInspectionReadbackMilliseconds.Add(elapsedMilliseconds);
}
const RollingAverage& GraphViewportStatistics::GetInspectionCpuMilliseconds() const noexcept
{
    return mInspectionCpuMilliseconds;
}
const RollingAverage& GraphViewportStatistics::GetInspectionReadbackMilliseconds() const noexcept
{
    return mInspectionReadbackMilliseconds;
}
void GraphViewportStatistics::AddInspectionConversion(float64 workMilliseconds, float64 elapsedMilliseconds) noexcept
{
    mConversionWorkMilliseconds.Add(workMilliseconds);
    mConversionElapsedMilliseconds.Add(elapsedMilliseconds);
}
const RollingAverage& GraphViewportStatistics::GetConversionWorkMilliseconds() const noexcept
{
    return mConversionWorkMilliseconds;
}
const RollingAverage& GraphViewportStatistics::GetConversionElapsedMilliseconds() const noexcept
{
    return mConversionElapsedMilliseconds;
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
                            : FormatTimingSummary(deviceStatistics.GpuMilliseconds);
    const QString frameText = QStringLiteral("FPS (updates): %1\nCPU: %2\nGPU: %3\nOutput readback: %4")
                                  .arg(statistics.GetFramesPerSecond(nowNanoseconds), 0, 'f', FPS_DECIMALS)
                                  .arg(FormatTimingSummary(statistics.GetCpuMilliseconds()),
                                       gpu,
                                       FormatMilliseconds(statistics.GetReadbackMilliseconds()));
    const QString passes = FormatViewportPassLines(deviceStatistics);
    const QString passText = passes.isEmpty() == true ? QString() : QLatin1Char('\n') + passes;
    if (statistics.IsInspectionEnabled() == false)
    {
        return frameText + QStringLiteral("\nInspection: paused") + passText;
    }
    return frameText +
           QStringLiteral(
               "\nInspection CPU: %1\nInspection readback: %2\nConversion worker: %3\nConversion elapsed: %4")
               .arg(FormatMilliseconds(statistics.GetInspectionCpuMilliseconds()),
                    FormatMilliseconds(statistics.GetInspectionReadbackMilliseconds()),
                    FormatMilliseconds(statistics.GetConversionWorkMilliseconds()),
                    FormatMilliseconds(statistics.GetConversionElapsedMilliseconds())) +
           passText;
}
QString FormatViewportPassLines(const ViewportStatistics& statistics)
{
    if (statistics.PassState == ViewportPassTimeState::Unavailable)
    {
        return QStringLiteral("Passes: n/a");
    }
    StaticArray<PassRow, MAX_VIEWPORT_PASS_SERIES> rows;
    const usize count = RankPasses(statistics.Passes, rows);
    if (statistics.PassState != ViewportPassTimeState::Available || count == 0)
    {
        return QString();
    }
    float64 total = 0;
    for (usize index = 0; index < count; ++index)
    {
        total += rows[index].Milliseconds;
    }
    QString text = QStringLiteral("Passes: %1 ms").arg(total, 0, 'f', MILLISECOND_DECIMALS);
    const usize shown = math::Min(count, MAX_VIEWPORT_PASS_ROWS);
    for (usize index = 0; index < shown; ++index)
    {
        const QString& name = statistics.Passes.GetSeries()[rows[index].Index].Name;
        text += QLatin1Char('\n') + FormatPassRow(FormatPassName(name), rows[index].Milliseconds);
    }
    if (count > shown)
    {
        float64 remainder = 0;
        for (usize index = shown; index < count; ++index)
        {
            remainder += rows[index].Milliseconds;
        }
        text +=
            QLatin1Char('\n') +
            FormatPassRow(QStringLiteral("+%1 more").arg(count - shown).leftJustified(PASS_NAME_COLUMNS), remainder);
    }
    return text;
}
ViewportPassBars MakeViewportPassBars(const ViewportStatistics& statistics)
{
    ViewportPassBars bars;
    StaticArray<PassRow, MAX_VIEWPORT_PASS_SERIES> rows;
    const usize count = RankPasses(statistics.Passes, rows);
    if (statistics.PassState != ViewportPassTimeState::Available || count == 0)
    {
        return bars;
    }
    bars.RowCount = math::Min(count, MAX_VIEWPORT_PASS_ROWS);
    bars.TrailingLines = count > bars.RowCount ? 1 : 0;
    const float64 slowest = rows[0].Milliseconds;
    for (usize index = 0; index < bars.RowCount; ++index)
    {
        bars.Fractions[index] = slowest > 0 ? static_cast<float32>(rows[index].Milliseconds / slowest) : 0.0F;
    }
    bars.SlowestRow = 0;
    return bars;
}
QString FormatViewportStatistics(const ViewportStatistics& statistics)
{
    const QString gpu = statistics.GpuState == ViewportGpuTimeState::Unavailable
                            ? QStringLiteral("n/a")
                            : FormatTimingSummary(statistics.GpuMilliseconds);
    const QString text = QStringLiteral("GPU: %1\nCPU: %2\n%3×%4 · draws %5")
                             .arg(gpu, FormatTimingSummary(statistics.CpuMilliseconds))
                             .arg(statistics.Width)
                             .arg(statistics.Height)
                             .arg(statistics.Draws);
    const QString passes = FormatViewportPassLines(statistics);
    const QString api =
        statistics.Api.isEmpty() == true ? QString() : QStringLiteral("API: ") + statistics.Api + QLatin1Char('\n');
    return api + (passes.isEmpty() == true ? text : text + QLatin1Char('\n') + passes);
}
} // namespace ludus::editor
