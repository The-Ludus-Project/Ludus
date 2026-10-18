#include "internal/formatter.hpp"
#include <ludus/foundation/base/config.h>

#include <ludus/foundation/time/time.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>

namespace ludus::foundation::logging::internal
{

namespace
{

// ANSI SGR color codes keyed by level. Trace dim; Debug/Info default; Warning
// yellow; Error red; Fatal bright red.
[[nodiscard]] std::string_view GetColorForLevel(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Trace:
            return "\x1b[2m";
        case LogLevel::Debug:
        case LogLevel::Info:
            return "";
        case LogLevel::Warning:
            return "\x1b[33m";
        case LogLevel::Error:
            return "\x1b[31m";
        case LogLevel::Fatal:
            return "\x1b[91m";
    }
    return "";
}

constexpr std::string_view COLOR_RESET = "\x1b[0m";

// Monotonic readable thread-id allocator (not the OS tid): a small, stable,
// readable counter so log output is deterministic across runs for a given
// thread order.
std::atomic<uint32> gNextThreadId{0};

struct ThreadLocalIdentity
{
    uint32 Id;
    std::string Name;

    ThreadLocalIdentity() noexcept : Id(gNextThreadId.fetch_add(1, std::memory_order_relaxed))
    {
        if (Id == 0)
        {
            Name = "Main";
        }
        else
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "Thread-%u", Id);
            Name = buffer;
        }
    }
};

ThreadLocalIdentity& GetThreadLocalIdentity() noexcept
{
    thread_local ThreadLocalIdentity value;
    return value;
}

// Session clock anchor: captured once, maps monotonic ticks to wall-clock time
// for rendering. Monotonic ticks drive ordering; wall time is derived only for
// human-readable output (requirements R23; F11).
struct SessionClockAnchor
{
    uint64 SteadyEpoch;
    std::chrono::system_clock::time_point SystemEpoch;

    SessionClockAnchor() noexcept : SteadyEpoch(time::NowTicks()), SystemEpoch(std::chrono::system_clock::now()) {}
};

const SessionClockAnchor& GetSessionClockAnchor() noexcept
{
    static SessionClockAnchor anchor;
    return anchor;
}

// Convert producer monotonic ticks to a wall-clock "HH:MM:SS.mmm" via the
// session anchor. Allocation-free.
usize FormatTimestampFromTicks(uint64 monotonic_ticks_ns, char* out, usize capacity) noexcept
{
    if (capacity < 13)
    {
        if (capacity > 0)
        {
            out[0] = '\0';
        }
        return 0;
    }

    const SessionClockAnchor& anchor = GetSessionClockAnchor();
    const auto steady_epoch_ns = anchor.SteadyEpoch;
    const int64 delta_ns = static_cast<int64>(monotonic_ticks_ns) - static_cast<int64>(steady_epoch_ns);
    const auto wall = anchor.SystemEpoch + std::chrono::nanoseconds(delta_ns);
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall.time_since_epoch()).count();

    const uint64 total_seconds = static_cast<uint64>(wall_ns) / 1'000'000'000ull;
    const uint32 milliseconds = static_cast<uint32>((static_cast<uint64>(wall_ns) / 1'000'000ull) % 1000ull);

    const std::time_t seconds = static_cast<std::time_t>(total_seconds);
    std::tm broken{};
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    localtime_s(&broken, &seconds);
#else
    localtime_r(&seconds, &broken);
#endif

    const int written =
        std::snprintf(out, capacity, "%02d:%02d:%02d.%03u", broken.tm_hour, broken.tm_min, broken.tm_sec, milliseconds);
    return written > 0 ? static_cast<usize>(written) : 0;
}

} // namespace

bool IsSourceVisibleFor(LogLevel level) noexcept
{
    return level >= LogLevel::Warning;
}

void FormatConsoleLine(const LogRecordView& record, bool use_color, std::string& buffer)
{
    buffer.clear();

    char timestamp[16];
    const usize ts_len = FormatTimestampFromTicks(record.MonotonicTicks, timestamp, sizeof(timestamp));
    buffer.append(timestamp, ts_len);

    buffer.append(" [");
    buffer.append(record.ThreadName.empty() ? std::string_view{"?"} : record.ThreadName);
    buffer.append("] [");

    if (use_color)
    {
        buffer.append(GetColorForLevel(record.Level));
    }
    buffer.append(ToPaddedString(record.Level));
    if (use_color)
    {
        buffer.append(COLOR_RESET);
    }

    buffer.append("] [");
    buffer.append(record.Category.Name);
    buffer.append("] ");
    buffer.append(record.Message);

    if (IsSourceVisibleFor(record.Level) && record.File.empty() == false)
    {
        buffer.append("\n    ");
        buffer.append(record.File);
        buffer.push_back(':');
        char line_digits[16];
        const int n = std::snprintf(line_digits, sizeof(line_digits), "%u", static_cast<unsigned>(record.Line));
        if (n > 0)
        {
            buffer.append(line_digits, static_cast<usize>(n));
        }
    }
}

std::string_view GetCurrentThreadName() noexcept
{
    return GetThreadLocalIdentity().Name;
}

uint32 GetCurrentThreadId() noexcept
{
    return GetThreadLocalIdentity().Id;
}

uint64 GetNativeThreadId() noexcept
{
    // A stable hash of std::thread::id; portable and correlates across a run.
    const usize h = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return static_cast<uint64>(h);
}

uint64 GetMonotonicTicks() noexcept
{
    return time::NowTicks();
}

} // namespace ludus::foundation::logging::internal

namespace ludus::foundation::logging
{

void SetCurrentThreadName(std::string_view name)
{
    internal::GetThreadLocalIdentity().Name.assign(name);
}

} // namespace ludus::foundation::logging
