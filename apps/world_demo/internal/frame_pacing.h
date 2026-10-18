#pragma once
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/platform/frame_pacing.h>
namespace ludus::world_demo
{
using foundation::uint32;
using foundation::uint64;
using foundation::usize;
/// Pacing rate for native frames of a visible window that present nothing (startup, loading), so the
/// outer loop never polls at full speed. Presented frames are paced by the swapchain unless capped.
inline constexpr uint32 IDLE_FRAMES_PER_SECOND = 60;
/// Pacing rate while the window cannot be seen (minimized or occluded): every frame waits at this rate,
/// or at a lower cap, instead of being paced by the swapchain or IDLE_FRAMES_PER_SECOND. The hidden loop
/// still pumps window events and loading at this rate, so it notices being shown within 100 ms.
inline constexpr uint32 HIDDEN_FRAMES_PER_SECOND = 10;
/// Highest accepted --frame-rate cap.
inline constexpr uint32 MAXIMUM_FRAMES_PER_SECOND = 1000;
/// A frame later than this many intervals re-anchors the schedule instead of skipping to the next target.
inline constexpr uint64 MAXIMUM_LATE_INTERVALS = 4;
/// Largest --pacing-report run; each record is retained until the report is summarized.
inline constexpr usize MAXIMUM_PACING_RECORDS = 100'000;
/// Native outer-loop pacing policy for the world reference.
///
/// Without a cap, the FIFO swapchain paces presented frames and only frames that presented nothing
/// wait, at IDLE_FRAMES_PER_SECOND; a presented frame drops that idle schedule. With a cap, every frame
/// waits for the cap's schedule. While the window is hidden, every frame waits at
/// HIDDEN_FRAMES_PER_SECOND (or the cap, when lower), because the system pacing clock's wakes are not
/// coalesced and nobody sees the frames. Browsers never use this: they pace with presentation callbacks.
class LoopPacing final
{
public:
    /// Start uncapped: presented frames are swapchain-paced and idle frames wait at IDLE_FRAMES_PER_SECOND.
    LoopPacing() noexcept
    {
        (void)Configure(0);
    }
    /// Select a frame-rate cap. Zero keeps presentation pacing with idle waits; 1 to
    /// MAXIMUM_FRAMES_PER_SECOND caps every frame. Drops the current schedule; the visibility is kept.
    /// @return false for an out-of-range cap, preserving the previous policy and schedule.
    [[nodiscard]] bool Configure(uint32 framesPerSecondLimit) noexcept;
    /// Report whether the window can be seen, normally once per frame after pumping window events.
    /// A change switches between the visible policy and the hidden rate and drops the schedule, so
    /// the first frame after the change starts at once and the old phase never carries over.
    /// Repeating the current visibility changes nothing.
    /// @param isVisible False while the window is minimized or occluded (platform::WindowVisibility).
    void SetVisible(bool isVisible) noexcept;
    /// Drop the schedule (startup, suspension, session activation); the next paced frame starts at once.
    void Reset() noexcept;
    /// Pace after a frame. A presented frame without a cap, while visible, returns Disabled and drops the
    /// idle schedule; hidden frames always wait for the hidden schedule.
    /// @param hasPresented Whether the frame that just ended presented an image.
    /// @param clock Monotonic time and wait primitive, normally platform::GetSystemPacingClock().
    [[nodiscard]] platform::PaceResult EndFrame(bool hasPresented, const platform::PacingClock& clock) noexcept;
    /// Whether a frame-rate cap is configured.
    [[nodiscard]] LUDUS_INLINE bool HasLimit() const noexcept
    {
        return mFramesPerSecondLimit != 0;
    }
    /// Whether the latest SetVisible() reported a visible window; true until the first report.
    [[nodiscard]] LUDUS_INLINE bool IsVisible() const noexcept
    {
        return mIsVisible;
    }
    /// The underlying pacer and its active policy.
    [[nodiscard]] LUDUS_INLINE const platform::FramePacer& GetPacer() const noexcept
    {
        return mPacer;
    }

private:
    [[nodiscard]] bool applyPolicy(uint32 framesPerSecondLimit, bool isVisible) noexcept;
    platform::FramePacer mPacer;
    uint32 mFramesPerSecondLimit = 0;
    bool mIsVisible = true;
};
/// One outer-loop iteration of a --pacing-report run.
struct PacingRecord final
{
    platform::PaceResult Pace;
    /// Reading taken when EndFrame returned for this iteration's frame; meaningful when HasPresented.
    foundation::time::Timestamp PresentReturn;
    bool HasPresented = false;
};
/// Nearest-rank percentiles of one interval series, in nanoseconds.
struct IntervalDistribution final
{
    usize Count = 0;
    uint64 Minimum = 0;
    uint64 Median = 0;
    uint64 Percentile90 = 0;
    uint64 Percentile99 = 0;
    uint64 Maximum = 0;
    uint64 Mean = 0;
};
/// Summary of a --pacing-report run.
struct PacingSummary final
{
    usize Frames = 0;
    usize Waited = 0;
    usize Late = 0;
    usize Rescheduled = 0;
    usize PresentationPaced = 0;
    /// Baseline, wait failure, wait limit, regression and overflow outcomes.
    usize Other = 0;
    uint64 EarlyWakes = 0;
    /// Wake minus target for Waited frames: operating-system wake-up lateness.
    IntervalDistribution WakeLateness;
    /// Wake-to-wake intervals between consecutive paced frames.
    IntervalDistribution FrameStart;
    /// EndFrame-return intervals between consecutive presented frames (CPU side, not scanout).
    IntervalDistribution PresentReturn;
};
/// Bounded per-frame pacing measurements for the world reference --pacing-report option.
class PacingReport final
{
public:
    /// Reserve storage for `frames` records and drop earlier records.
    /// @return false when frames is zero, exceeds MAXIMUM_PACING_RECORDS or cannot be allocated.
    [[nodiscard]] bool Reserve(usize frames) noexcept;
    /// Append one record; records beyond the reserved capacity are counted as dropped.
    void Record(const PacingRecord& record) noexcept;
    /// Compute the summary. Allocates scratch storage proportional to the record count.
    /// @return false when scratch storage cannot be allocated; outSummary is then unchanged.
    [[nodiscard]] bool Summarize(PacingSummary& outSummary) const noexcept;
    [[nodiscard]] LUDUS_INLINE usize GetDroppedRecords() const noexcept
    {
        return mDropped;
    }

private:
    foundation::Array<PacingRecord> mRecords;
    usize mDropped = 0;
};
} // namespace ludus::world_demo
