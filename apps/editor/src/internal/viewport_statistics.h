#pragma once

#include <ludus/foundation/base/types.h>

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

private:
    foundation::StaticArray<foundation::float64, VIEWPORT_STATISTICS_AVERAGE_FRAMES> mSamples{};
    foundation::usize mCount = 0;
    foundation::usize mNext = 0;
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
    /// sum of active snapshot/readback/conversion calls; elapsed includes asynchronous gaps.
    /// Completed attempts include failures; cancelled attempts are omitted. GUI-thread only.
    void AddInspection(foundation::float64 cpuMilliseconds, foundation::float64 elapsedMilliseconds) noexcept;
    /// Active milliseconds per completed inspection attempt, including synchronous viewer conversion.
    [[nodiscard]] const RollingAverage& GetInspectionCpuMilliseconds() const noexcept;
    /// Elapsed milliseconds per completed inspection attempt, including time between service calls.
    [[nodiscard]] const RollingAverage& GetInspectionReadbackMilliseconds() const noexcept;
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
    foundation::float64 mPendingCpuMilliseconds = 0;
    foundation::uint64 mLastFrameNanoseconds = 0;
    bool mHasFrame = false;
    bool mIsInspectionEnabled = true;
};

/// Graph overlay: delivered-image FPS, active CPU work, device GPU time, and elapsed readback.
/// GPU timing comes from the existing device owner; this formatter never arms or polls queries.
[[nodiscard]] QString FormatGraphViewportStatistics(const GraphViewportStatistics& statistics,
                                                    const ViewportStatistics& deviceStatistics,
                                                    foundation::uint64 nowNanoseconds);

/// Overlay text: GPU and CPU frame times (two decimals, milliseconds), then resolution and draws.
/// Unavailable GPU timing reads "GPU: n/a"; a pending first sample reads "GPU: …".
[[nodiscard]] QString FormatViewportStatistics(const ViewportStatistics& statistics);
} // namespace ludus::editor
