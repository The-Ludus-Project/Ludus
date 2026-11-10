#pragma once

#include <ludus/foundation/base/types.h>

namespace ludus::foundation::time
{
/// Elapsed interval in integer nanoseconds, independent of any timestamp epoch.
struct Duration
{
    /// Interval length in nanoseconds; zero is a valid duration.
    uint64 Nanoseconds = 0;
    /// Compare exact interval lengths.
    bool operator==(const Duration&) const noexcept = default;
};

/// Reading of the process monotonic CPU clock; not UTC, a GPU counter or a game tick.
struct Timestamp
{
    /// Nanoseconds since the monotonic clock's unspecified epoch.
    uint64 Nanoseconds = 0;
    /// Compare exact readings; meaningful only within the same clock domain.
    bool operator==(const Timestamp&) const noexcept = default;
};

/// Result of checked time arithmetic or a timer transition.
enum class TimeStatus : uint8
{
    /// The operation succeeded and its output is available.
    Ok,
    /// The interval or timestamp sum exceeds the uint64 range.
    Overflow,
    /// The later sample precedes the relevant earlier sample.
    ClockRegression,
    /// The requested operation is unavailable in the timer's current state.
    InvalidState
};

/// Exact conversion factor from seconds to stored nanoseconds.
inline constexpr uint64 NANOSECONDS_PER_SECOND = 1'000'000'000;
/// Exact conversion factor from milliseconds to stored nanoseconds.
inline constexpr uint64 NANOSECONDS_PER_MILLISECOND = 1'000'000;
/// Exact conversion factor from microseconds to stored nanoseconds.
inline constexpr uint64 NANOSECONDS_PER_MICROSECOND = 1'000;

namespace detail
{
/// Multiply a unit count into nanoseconds; counts past the uint64 range saturate to the maximum.
[[nodiscard]] constexpr Duration ScaleToNanoseconds(uint64 count, uint64 nanosecondsPerUnit) noexcept
{
    constexpr uint64 MAXIMUM_NANOSECONDS = ~uint64{0};
    if (count > MAXIMUM_NANOSECONDS / nanosecondsPerUnit)
    {
        return {MAXIMUM_NANOSECONDS};
    }
    return {count * nanosecondsPerUnit};
}
} // namespace detail

/// Interval of whole seconds; counts past the uint64 nanosecond range saturate to the maximum interval.
[[nodiscard]] constexpr Duration Seconds(uint64 count) noexcept
{
    return detail::ScaleToNanoseconds(count, NANOSECONDS_PER_SECOND);
}
/// Interval of whole milliseconds; counts past the uint64 nanosecond range saturate to the maximum interval.
[[nodiscard]] constexpr Duration Milliseconds(uint64 count) noexcept
{
    return detail::ScaleToNanoseconds(count, NANOSECONDS_PER_MILLISECOND);
}
/// Interval of whole microseconds; counts past the uint64 nanosecond range saturate to the maximum interval.
[[nodiscard]] constexpr Duration Microseconds(uint64 count) noexcept
{
    return detail::ScaleToNanoseconds(count, NANOSECONDS_PER_MICROSECOND);
}

/// @brief Read the monotonic clock shared with profiling and logging, in nanoseconds.
///
/// No engine allocation, lock or initialization is required. Readings are
/// non-decreasing on one thread; equal readings are valid. Units do not guarantee
/// resolution. Timestamps do not establish a happens-before relation between threads.
[[nodiscard]] uint64 NowTicks() noexcept;
/// Read NowTicks() as a typed timestamp in the same monotonic clock domain.
[[nodiscard]] inline Timestamp Now() noexcept
{
    return {NowTicks()};
}

/// Add interval lengths without wrapping; Overflow leaves out unchanged.
[[nodiscard]] TimeStatus TryAdd(Duration left, Duration right, Duration& out) noexcept;
/// Add an interval to a timestamp; Overflow leaves out unchanged.
[[nodiscard]] TimeStatus TryAdd(Timestamp start, Duration interval, Timestamp& out) noexcept;
/// Compute end minus start; ClockRegression leaves out unchanged.
/// @pre Both samples share the same monotonic clock domain.
[[nodiscard]] TimeStatus TryElapsed(Timestamp start, Timestamp end, Duration& out) noexcept;
/// Convert an interval to seconds with float64 rounding; large intervals may lose precision.
[[nodiscard]] constexpr float64 ToSeconds(Duration interval) noexcept
{
    return static_cast<float64>(interval.Nanoseconds) / static_cast<float64>(NANOSECONDS_PER_SECOND);
}
} // namespace ludus::foundation::time
