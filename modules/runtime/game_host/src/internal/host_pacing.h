#pragma once

// Host frame pacing policy (docs/architecture/high-resolution-time.md, "Native
// frame pacing" and "Runtime timing telemetry"). Frame starts follow an absolute
// 60 Hz schedule while the host's window can be seen, or when it has none, and a
// 10 Hz schedule while the window is minimized or occluded: the same hidden rule
// as the world reference's LoopPacing, without that loop's swapchain pacing and
// frame-rate cap. The simulation is unaffected: the host still advances one
// fixed tick per running frame, so a hidden game advances at the hidden rate and
// the editor's Pause stays the only pause. The policy owns a FramePacer and
// nothing else; the play session and the static shipping loop share it.

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/time/time.hpp>
#include <ludus/platform/frame_pacing.h>

namespace ludus::runtime::game_host
{
/// Interval between frame starts while the window can be seen or the host has none: 60 Hz in whole
/// nanoseconds (16,666,667 ns, about 20 ns per second slower than exact).
inline constexpr ludus::foundation::uint64 HOST_FRAME_INTERVAL_NANOSECONDS = 16'666'667;
/// Interval between frame starts while the window is minimized or occluded: 10 Hz. The loop still pumps
/// window events and control commands at this rate, so it notices being shown or a Stop within 100 ms.
inline constexpr ludus::foundation::uint64 HOST_HIDDEN_FRAME_INTERVAL_NANOSECONDS = 100'000'000;
/// A frame later than this many intervals re-anchors the schedule instead of skipping to the next target.
inline constexpr ludus::foundation::uint64 HOST_MAXIMUM_LATE_INTERVALS = 4;

/// Owner-local pacing policy for the host frame loops.
///
/// Values are allocation-free and need caller synchronization if shared; the host thread owns its value.
class HostPacing final
{
public:
    /// Start with the visible interval and no schedule: the first Pace() is a Baseline.
    HostPacing() noexcept;

    /// Report whether the host's window can be seen, once per frame after pumping its events. A host
    /// without a window reports true. A change selects the other interval and drops the schedule, so
    /// the next frame starts at once and never waits out the old phase. Repeating the current
    /// visibility changes nothing.
    /// @param isVisible False while the window is minimized or occluded (platform::WindowVisibility).
    void SetVisible(bool isVisible) noexcept;

    /// Drop the schedule (startup, session replacement); the next Pace() starts at once. The interval is kept.
    void Reset() noexcept;

    /// Wait for the next frame start under the active interval; see platform::FramePacer::Pace().
    /// @param clock Monotonic time and wait primitive, normally platform::GetSystemPacingClock().
    [[nodiscard]] platform::PaceResult Pace(const platform::PacingClock& clock) noexcept;

    /// The active interval between frame starts, reported to the editor as `pacing_interval_ns`.
    [[nodiscard]] LUDUS_INLINE foundation::time::Duration GetInterval() const noexcept
    {
        return mPacer.GetPolicy().Interval;
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
    void applyPolicy(bool isVisible) noexcept;

    platform::FramePacer mPacer;
    bool mIsVisible = true;
};
} // namespace ludus::runtime::game_host
