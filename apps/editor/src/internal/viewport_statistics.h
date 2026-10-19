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

/// Overlay text: GPU and CPU frame times (two decimals, milliseconds), then resolution and draws.
/// Unavailable GPU timing reads "GPU: n/a"; a pending first sample reads "GPU: …".
[[nodiscard]] QString FormatViewportStatistics(const ViewportStatistics& statistics);
} // namespace ludus::editor
