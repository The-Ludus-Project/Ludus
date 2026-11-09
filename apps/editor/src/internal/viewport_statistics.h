#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <QString>

namespace ludus::editor
{
/// Frames averaged by each viewport statistics series (about half a second at 60 Hz).
inline constexpr foundation::usize VIEWPORT_STATISTICS_AVERAGE_FRAMES = 30;

/// Availability of the GPU frame-time series.
enum class ViewportGpuTimeState : foundation::uint8
{
    /// Timing is enabled but no sample has completed yet.
    Waiting,
    /// At least one GPU sample is in the rolling average.
    Available,
    /// The backend or device exposes no timestamp queries; shown as "GPU: n/a".
    Unavailable,
};

/// Availability of the per-pass GPU time series.
enum class ViewportPassTimeState : foundation::uint8
{
    /// Nothing has been measured yet; the overlay shows no pass rows.
    Waiting,
    /// At least one completed batch of per-pass timings is in the rolling averages.
    Available,
    /// The backend exposes no per-pass timestamp queries; shown as "Passes: n/a".
    Unavailable,
};

/// Most pass rows drawn in the overlay; the remainder is summed into one "+N more" row.
inline constexpr foundation::usize MAX_VIEWPORT_PASS_ROWS = 6;
/// Most distinct GPU scopes tracked at once (the RHI times at most 64 physical passes per frame).
inline constexpr foundation::usize MAX_VIEWPORT_PASS_SERIES = 64;
/// Completed batches a scope may be absent from before its series is dropped as no longer rendered.
inline constexpr foundation::usize VIEWPORT_PASS_STALE_BATCHES = 30;

/// Rolling average of the most recent VIEWPORT_STATISTICS_AVERAGE_FRAMES samples.
/// Owner-thread value type; adding never allocates.
class RollingAverage final
{
public:
    /// Add one finite nonnegative sample; other values are ignored.
    void Add(foundation::float64 value) noexcept;
    void Reset() noexcept;
    [[nodiscard]] foundation::usize GetCount() const noexcept;
    /// Mean of the retained samples, or zero when empty.
    [[nodiscard]] foundation::float64 GetAverage() const noexcept;
    /// Nearest-rank 95th percentile of retained samples, or zero when empty; never allocates.
    [[nodiscard]] foundation::float64 GetPercentile95() const noexcept;
    /// Largest retained sample, or zero when empty.
    [[nodiscard]] foundation::float64 GetMaximum() const noexcept;
    /// Most recently accepted sample, or zero when empty.
    [[nodiscard]] foundation::float64 GetLatest() const noexcept;

private:
    foundation::StaticArray<foundation::float64, VIEWPORT_STATISTICS_AVERAGE_FRAMES> mSamples{};
    foundation::usize mCount = 0;
    foundation::usize mNext = 0;
};

/// Rolling GPU time of one named scope, summed over its physical passes within a frame.
struct ViewportPassSeries final
{
    /// Producer-local scope identity; paired with the name so a renamed scope starts a new series.
    foundation::uint64 Identity = 0;
    QString Name;
    /// Milliseconds per completed batch.
    RollingAverage Milliseconds;
    /// Completed batches since this scope last appeared; zero while it is rendered.
    foundation::usize StaleBatches = 0;
    /// Accumulator for the batch being built; folded into Milliseconds by CommitBatch.
    foundation::float64 BatchMilliseconds = 0;
    bool IsInBatch = false;
};

/// Per-scope GPU time series in order of first appearance. Owner-thread value type; it allocates only
/// when a scope is first seen and never while the same scopes repeat frame after frame.
class ViewportPassStatistics final
{
public:
    /// Drop every series, as on a device replacement or when the overlay is hidden.
    void Reset() noexcept;
    /// Add one physical pass of a scope to the batch being built. Samples that are negative or not
    /// finite are ignored; several passes of one scope sum. Scopes beyond MAX_VIEWPORT_PASS_SERIES are ignored.
    void AddSample(foundation::uint64 identity, const QString& name, foundation::float64 milliseconds);
    /// Fold the batch into the rolling averages, age scopes absent from it and drop stale ones.
    void CommitBatch();
    [[nodiscard]] const foundation::Array<ViewportPassSeries>& GetSeries() const noexcept;

private:
    foundation::Array<ViewportPassSeries> mSeries;
};

/// Bars drawn behind the pass rows at the end of the overlay text. The barred rows are the last
/// RowCount + TrailingLines lines of the text, in order, less the TrailingLines final ones.
struct ViewportPassBars final
{
    /// Number of barred rows, at most MAX_VIEWPORT_PASS_ROWS; zero when there is nothing to draw.
    foundation::usize RowCount = 0;
    /// Unbarred lines after the last barred row (the "+N more" summary), zero or one.
    foundation::usize TrailingLines = 0;
    /// Bar length of each row, from zero to one, relative to the slowest pass.
    foundation::StaticArray<foundation::float32, MAX_VIEWPORT_PASS_ROWS> Fractions{};
    /// Row of the slowest pass; drawn in the warning token instead of the accent.
    foundation::usize SlowestRow = 0;
};

/// Smoothed per-frame viewport measurements shown by the statistics overlay.
struct ViewportStatistics final
{
    /// GPU frame duration in milliseconds, from the device timestamp probe read back with latency.
    RollingAverage GpuMilliseconds;
    ViewportGpuTimeState GpuState = ViewportGpuTimeState::Waiting;
    /// CPU time spent preparing, recording and submitting a frame on the owner thread, in milliseconds.
    RollingAverage CpuMilliseconds;
    /// Rendered image size in physical pixels.
    foundation::uint32 Width = 0;
    foundation::uint32 Height = 0;
    /// Draw packets submitted for the current view.
    foundation::uint32 Draws = 0;
    /// Per-scope GPU time, measured only while the overlay is visible.
    ViewportPassStatistics Passes;
    ViewportPassTimeState PassState = ViewportPassTimeState::Waiting;
};

/// Completed graph-output measurements, independent of the device timing owner.
/// GUI-thread only. CPU samples sum active service work, excluding asynchronous waits.
class GraphViewportStatistics final
{
public:
    /// A gap this long marks the on-demand producer idle and starts a new FPS window.
    static constexpr foundation::uint64 IDLE_NANOSECONDS = 1'000'000'000;
    /// Clear all series and any unfinished CPU work, as on device replacement.
    void Reset() noexcept;
    /// Accumulate finite nonnegative CPU work in milliseconds toward the next delivered image.
    void AddCpuWork(foundation::float64 milliseconds) noexcept;
    /// Reflect whether the viewer permits inspection; keeps completed samples while paused.
    void SetInspectionEnabled(bool isEnabled) noexcept;
    /// Whether inspection measurements describe an enabled viewer rather than retained history.
    [[nodiscard]] bool IsInspectionEnabled() const noexcept;
    /// Record one completed texture inspection independently of output delivery. CPU is the
    /// sum of active GUI snapshot/readback calls; elapsed includes asynchronous gaps, ending at raw bytes.
    /// Completed attempts include failures; cancelled attempts are omitted. GUI-thread only.
    void AddInspection(foundation::float64 cpuMilliseconds, foundation::float64 elapsedMilliseconds) noexcept;
    /// Active GUI milliseconds per completed inspection attempt; excludes worker conversion.
    [[nodiscard]] const RollingAverage& GetInspectionCpuMilliseconds() const noexcept;
    /// Elapsed milliseconds per completed inspection attempt, including time between service calls.
    [[nodiscard]] const RollingAverage& GetInspectionReadbackMilliseconds() const noexcept;
    /// Accepted conversions only: active worker call and request-to-GUI-delivery durations in ms.
    /// These samples are independent of readback and never contribute worker time to GUI CPU.
    void AddInspectionConversion(foundation::float64 workMilliseconds,
                                 foundation::float64 elapsedMilliseconds) noexcept;
    [[nodiscard]] const RollingAverage& GetConversionWorkMilliseconds() const noexcept;
    [[nodiscard]] const RollingAverage& GetConversionElapsedMilliseconds() const noexcept;
    /// Commit one successfully read and converted image. Timestamps use one monotonic CPU clock.
    /// Readback is elapsed milliseconds from copy request through display-image conversion.
    void CompleteFrame(foundation::uint64 nowNanoseconds, foundation::float64 readbackMilliseconds) noexcept;
    /// Average update rate; zero after an idle gap or before two completed images.
    [[nodiscard]] foundation::float64 GetFramesPerSecond(foundation::uint64 nowNanoseconds) const noexcept;
    [[nodiscard]] const RollingAverage& GetCpuMilliseconds() const noexcept;
    [[nodiscard]] const RollingAverage& GetReadbackMilliseconds() const noexcept;

private:
    RollingAverage mCpuMilliseconds;
    RollingAverage mReadbackMilliseconds;
    RollingAverage mFrameNanoseconds;
    RollingAverage mInspectionCpuMilliseconds;
    RollingAverage mInspectionReadbackMilliseconds;
    RollingAverage mConversionWorkMilliseconds;
    RollingAverage mConversionElapsedMilliseconds;
    foundation::float64 mPendingCpuMilliseconds = 0;
    foundation::uint64 mLastFrameNanoseconds = 0;
    bool mHasFrame = false;
    bool mIsInspectionEnabled = true;
};

/// Recognizes a viewport renderer too slow to navigate: SLOW_FRAME_STREAK consecutive presented frames, each taking
/// at least the threshold from the start of its submission to its presentation. The first frame of each generation
/// is excluded, because it includes cold preparation (pipelines, acceleration structures) rather than the cost of
/// drawing. Owner-thread value type; never allocates.
class ViewportSlowFrameMonitor final
{
public:
    /// Frame time at which navigation becomes unbearable (one frame per second), in milliseconds.
    static constexpr foundation::float64 DEFAULT_SLOW_FRAME_MILLISECONDS = 1000;
    /// Consecutive slow frames that confirm the renderer, rather than one stall, is slow.
    static constexpr foundation::usize SLOW_FRAME_STREAK = 3;
    /// Record one presented frame.
    /// @param generation Nonzero identity of the graph generation that drew the frame; a change starts over.
    /// @param milliseconds Submission-to-presentation time; negative or non-finite values end the streak.
    /// @return True exactly when this frame completes a streak; the streak then starts over, so a renderer that
    /// stays slow is reported again after another SLOW_FRAME_STREAK frames.
    [[nodiscard]] bool AddFrame(foundation::uint64 generation, foundation::float64 milliseconds) noexcept;
    /// Forget the generation and streak, as on device replacement.
    void Reset() noexcept;
    /// Replace the threshold; non-finite or negative values preserve it. Zero treats every frame as slow (tests).
    void SetThresholdMilliseconds(foundation::float64 milliseconds) noexcept;
    [[nodiscard]] foundation::float64 GetThresholdMilliseconds() const noexcept;
    /// Mean frame time of the most recently completed streak in milliseconds, or zero before one.
    [[nodiscard]] foundation::float64 GetStreakMilliseconds() const noexcept;

private:
    foundation::float64 mThresholdMilliseconds = DEFAULT_SLOW_FRAME_MILLISECONDS;
    foundation::uint64 mGeneration = 0;
    foundation::usize mStreak = 0;
    foundation::float64 mStreakSumMilliseconds = 0;
    foundation::float64 mReportedMilliseconds = 0;
};

/// Graph overlay: delivered-image FPS, active CPU work, device GPU time, and elapsed readback.
/// GPU timing comes from the existing device owner; this formatter never arms or polls queries.
[[nodiscard]] QString FormatGraphViewportStatistics(const GraphViewportStatistics& statistics,
                                                    const ViewportStatistics& deviceStatistics,
                                                    foundation::uint64 nowNanoseconds);

/// Pass block appended to the overlay text: a header with the summed time, then the slowest
/// MAX_VIEWPORT_PASS_ROWS scopes by rolling average with one aligned millisecond column, then one
/// "+N more" row. Empty while waiting for the first batch; "Passes: n/a" when per-pass timing is
/// unsupported, even where frame GPU time is shown. Lines carry no leading or trailing newline.
[[nodiscard]] QString FormatViewportPassLines(const ViewportStatistics& statistics);

/// Bar lengths for the rows FormatViewportPassLines emits; RowCount is zero when it emits none or only
/// a status line.
[[nodiscard]] ViewportPassBars MakeViewportPassBars(const ViewportStatistics& statistics);

/// Overlay text: GPU and CPU frame times (two decimals, milliseconds), then resolution and draws.
/// Unavailable GPU timing reads "GPU: n/a"; a pending first sample reads "GPU: …". Per-pass lines
/// from FormatViewportPassLines follow once measured.
[[nodiscard]] QString FormatViewportStatistics(const ViewportStatistics& statistics);
} // namespace ludus::editor
