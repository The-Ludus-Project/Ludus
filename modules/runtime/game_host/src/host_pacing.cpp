#include "internal/host_pacing.h"

#include <ludus/foundation/base/core.h>

namespace ludus::runtime::game_host
{
using ludus::foundation::uint64;

HostPacing::HostPacing() noexcept
{
    applyPolicy(true);
}

void HostPacing::SetVisible(bool isVisible) noexcept
{
    if (isVisible == mIsVisible)
    {
        return;
    }
    // Configure() drops the schedule, so the hidden and visible phases never carry over.
    applyPolicy(isVisible);
    mIsVisible = isVisible;
}

void HostPacing::Reset() noexcept
{
    mPacer.Reset();
}

platform::PaceResult HostPacing::Pace(const platform::PacingClock& clock) noexcept
{
    return mPacer.Pace(clock);
}

void HostPacing::applyPolicy(bool isVisible) noexcept
{
    const uint64 interval =
        isVisible == true ? HOST_FRAME_INTERVAL_NANOSECONDS : HOST_HIDDEN_FRAME_INTERVAL_NANOSECONDS;
    // Both intervals are valid constants within the pacer's bounds, so this cannot fail.
    const platform::PacingStatus status = mPacer.Configure(
    {
        .Interval = {interval},
        .MaximumLateness = {interval * HOST_MAXIMUM_LATE_INTERVALS},
    });
    LUDUS_ASSERT(status == platform::PacingStatus::Ok, "The host pacing intervals must be valid");
    (void)status;
}
} // namespace ludus::runtime::game_host
