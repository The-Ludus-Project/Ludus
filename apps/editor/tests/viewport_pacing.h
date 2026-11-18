#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>

namespace ludus::editor::tests
{
/// Whether camera input runs during a sampled viewport window.
enum class ViewportPacingWindowKind : foundation::uint8
{
    Idle,
    Input,
};
/// One sampled window of a viewport presentation timeline, on the foundation::time::NowTicks() clock.
struct ViewportPacingWindow final
{
    ViewportPacingWindowKind Kind = ViewportPacingWindowKind::Idle;
    /// First tick inside the window.
    foundation::uint64 BeginTicks = 0;
    /// First tick after the window.
    foundation::uint64 EndTicks = 0;
    /// Camera updates delivered during the window; zero for idle windows.
    foundation::uint32 InputCount = 0;
};
/// New-image cadence inside one window. Intervals are in nanoseconds.
struct ViewportPacingWindowSummary final
{
    /// Distinct images presented inside [BeginTicks, EndTicks).
    foundation::usize Images = 0;
    /// Median interval between consecutive images inside the window; zero below two images.
    foundation::uint64 MedianInterval = 0;
    /// Nearest-rank 95th percentile of the same intervals; zero below two images.
    foundation::uint64 P95Interval = 0;
    /// Longest time without a new image, including the open gaps from the window start to its
    /// first image and from its last image to the window end. A gap spanning a window boundary is
    /// therefore visible as a lower bound. The whole window when no image arrived.
    foundation::uint64 LongestGap = 0;
};
/// Acceptance of sustained camera input against interleaved idle windows.
struct ViewportPacingVerdict final
{
    /// Median images per idle window and per input window; the upper median for an even count.
    foundation::uint64 IdleMedianImages = 0;
    foundation::uint64 InputMedianImages = 0;
    /// Median over windows of each window's longest gap, in nanoseconds.
    foundation::uint64 IdleMedianLongestGap = 0;
    foundation::uint64 InputMedianLongestGap = 0;
    /// Longest gap allowed for the input median: the larger of the absolute floor and a multiple of
    /// the idle median, in nanoseconds.
    foundation::uint64 InputLongestGapLimit = 0;
    /// Every input window delivered more than the minimum number of camera updates.
    bool IsInputSustained = false;
    /// Every input window presented more than the minimum number of new images.
    bool HasProgress = false;
    /// The idle median cleared the absolute frame floor, so the throughput ratio was judged.
    bool IsRatioJudged = false;
    /// The input median kept the required share of the idle median, or the ratio was not judged.
    bool IsThroughputKept = false;
    /// The input median longest gap stayed within InputLongestGapLimit.
    bool IsCadenceKept = false;
};
/// Camera updates each input window must deliver; fewer means the input itself was not sustained.
inline constexpr foundation::uint32 MIN_PACING_INPUT_EVENTS = 20;
/// New images each input window must present regardless of host throughput.
inline constexpr foundation::usize MIN_PACING_WINDOW_IMAGES = 2;
// The limits below come from the Ludus baseline in docs/development/viewport-pacing-evidence.md:
// on a 60 Hz macOS display under CPU load, sanitized input windows kept as little as 0.53 of the
// idle median images and their median longest gap reached 112 ms.
/// Idle median images per window below which the host is too slow for the throughput ratio.
inline constexpr foundation::uint64 PACING_IDLE_IMAGE_FLOOR = 20;
/// Required input/idle median image ratio, as a fraction, when the idle floor is cleared.
inline constexpr foundation::uint64 PACING_THROUGHPUT_NUMERATOR = 1;
/// Denominator of the required input/idle median image ratio.
inline constexpr foundation::uint64 PACING_THROUGHPUT_DENOMINATOR = 3;
/// Input median longest gap may grow to this multiple of the idle median longest gap.
inline constexpr foundation::uint64 PACING_GAP_IDLE_MULTIPLE = 4;
/// Input median longest gap that is always accepted, in milliseconds.
inline constexpr foundation::uint64 PACING_GAP_FLOOR_MILLISECONDS = 250;

/// Summarize the images of one window.
/// @param presentedTicks Whole timeline of presentation ticks in non-decreasing order.
/// @return False when the window is empty or reversed, or scratch storage could not be allocated;
/// outSummary is then unchanged.
[[nodiscard]] bool SummarizeViewportPacingWindow(foundation::BorrowedView<const foundation::uint64> presentedTicks,
                                                 const ViewportPacingWindow& window,
                                                 ViewportPacingWindowSummary& outSummary);
/// Judge interleaved idle and input windows; see docs/development/continuous-integration.md.
/// @return False when there is no idle or no input window or a window cannot be summarized;
/// outVerdict is then unchanged.
[[nodiscard]] bool EvaluateViewportPacing(foundation::BorrowedView<const foundation::uint64> presentedTicks,
                                          foundation::BorrowedView<const ViewportPacingWindow> windows,
                                          ViewportPacingVerdict& outVerdict);
/// True when every check of an evaluated verdict passed.
[[nodiscard]] bool IsViewportPacingAccepted(const ViewportPacingVerdict& verdict) noexcept;
} // namespace ludus::editor::tests
