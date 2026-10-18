#include <ludus/foundation/time/time.hpp>

#include <ludus/foundation/base/checked_integer.hpp>

namespace ludus::foundation::time
{
TimeStatus TryAdd(Duration left, Duration right, Duration& out) noexcept
{
    if (core::TryAdd(left.Nanoseconds, right.Nanoseconds, out.Nanoseconds) == false)
    {
        return TimeStatus::Overflow;
    }
    return TimeStatus::Ok;
}

TimeStatus TryAdd(Timestamp start, Duration interval, Timestamp& out) noexcept
{
    if (core::TryAdd(start.Nanoseconds, interval.Nanoseconds, out.Nanoseconds) == false)
    {
        return TimeStatus::Overflow;
    }
    return TimeStatus::Ok;
}

TimeStatus TryElapsed(Timestamp start, Timestamp end, Duration& out) noexcept
{
    if (core::TrySubtract(end.Nanoseconds, start.Nanoseconds, out.Nanoseconds) == false)
    {
        return TimeStatus::ClockRegression;
    }
    return TimeStatus::Ok;
}
} // namespace ludus::foundation::time
