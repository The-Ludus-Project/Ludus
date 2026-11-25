#pragma once

// -----------------------------------------------------------------------------
// Logging lifecycle, configuration, flush and health control surface.
//
// This header carries NO public <filesystem>: the log directory is a borrowed
// UTF-8 path view copied during Initialize() (requirements R21, R47). It is a
// separate, rarely-included header so ordinary call sites (log.hpp /
// log_format.hpp) never pay for configuration types.
//
// See .kiro/specs/logging-redesign/design.md sections 1, 6, 8.
// -----------------------------------------------------------------------------

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/level.hpp>

#include <string_view>

namespace ludus::foundation::logging
{

/// @brief Execution strategy for the logging backend.
///
/// Synchronous is the corrected, serialized backend used by tools/tests; Asynchronous selects the bounded MPSC
/// backend. The EFFECTIVE mode is reported by Initialize(); there is no live SetMode (a live transition would
/// require draining and transferring sink ownership). See requirements R41.
enum class LogMode : uint8
{
    Synchronous,
    Asynchronous,
};

/// Runtime configuration passed to LogSystem::Initialize.
struct LogConfig
{
    LogLevel GlobalLevel = LogLevel::Info;
    LogMode Mode = LogMode::Synchronous;

    bool EnableConsole = true;
    bool EnableFile = true;
    bool EnableDebugger = true;

    /// @brief Writable directory for the session file, resolved by the application / platform bootstrap and
    /// injected here as a borrowed UTF-8 path.
    ///
    /// Copied during Initialize(); the caller need not keep it alive afterward. When empty, the file sink is
    /// disabled and a diagnostic is emitted on the emergency path rather than guessing a directory
    /// (requirements R47).
    std::string_view Directory;

    /// @brief Timed flush cadence for the file sink.
    ///
    /// Honored by the backend (requirements R46). 0 disables timed flushing (records still flush on shutdown,
    /// on urgent events, and on explicit Flush()).
    uint32 FlushIntervalMilliseconds = 1000;

    /// @brief Per-session storage bounds.
    ///
    /// A record-boundary rotation triggers when a segment would exceed MaxFileSizeBytes; MaxTotalBytes /
    /// MaxSessionAgeDays bound retained storage across sessions (requirements R44). 0 means "no limit for this
    /// dimension" and is documented, not implicit.
    uint64 MaxFileSizeBytes = 32ull * 1024ull * 1024ull;
    uint64 MaxTotalBytes = 0;     ///< 0 = unbounded total (bounded by RetainedSessions/age)
    uint32 MaxSessionAgeDays = 0; ///< 0 = no age cap
    uint32 RetainedSessions = 10; ///< 0 = keep all
};

/// @brief Result of Initialize(): status plus the mode actually in effect.
///
/// A caller can observe whether a requested Asynchronous mode was honored or fell back to Synchronous / was
/// rejected as Unsupported (requirements R41, R28).
enum class LogStatus : uint8
{
    Ok,
    NotInitialized,
    AlreadyInitialized,
    Unsupported,
    Degraded,
    TimedOut,
    Incomplete,
    SinkFailed,
    Filtered,
};

struct LogInitResult
{
    LogStatus Status = LogStatus::NotInitialized;
    LogMode EffectiveMode = LogMode::Synchronous;
};

/// @brief Flush strength.
///
/// Visible pushes selected local sink buffers to the OS; Durable additionally requests a platform durable-file
/// operation (fsync/FlushFileBuffers) for selected files. Neither guarantees remote reception or hardware-failure
/// survival (requirements R28, R46).
enum class FlushKind : uint8
{
    Visible,
    Durable,
};

struct FlushResult
{
    LogStatus Status = LogStatus::NotInitialized;
    uint64 AcknowledgedSequence = 0; ///< highest sequence known processed at return
};

/// @brief Counters describing logging throughput and back-pressure.
///
/// Meanings are distinct (requirements R48): Submitted counts admitted attempts; Written counts records actually
/// handed to at least one sink and reported OK; Dropped counts records lost to overflow/failure. A failed delivery
/// never inflates Written.
struct LogStatistics
{
    uint64 Submitted = 0;
    uint64 Written = 0;
    uint64 Dropped = 0;
};

/// Per-sink and backend health snapshot (requirements R49).
struct LogHealth
{
    bool ConsoleHealthy = true;
    bool FileHealthy = true;
    bool DebuggerHealthy = true;
    bool WorkerHealthy = true;
    uint64 QueueHighWaterMark = 0;
    uint64 DroppedTotal = 0;
};

/// @brief Lifecycle and control surface.
///
/// Normal engine code uses the LUDUS_LOG_* macros almost exclusively; this class is for startup, diagnostics, and
/// tools.
class LogSystem
{
public:
    /// @brief Web builds run on the browser main thread, always synchronously.
    ///
    /// Requests for async, file, or debugger sinks return Degraded with Synchronous mode; unsupported requested
    /// sinks report unhealthy. No timed flush worker runs.
    /// Reentrant console callbacks cannot change lifecycle (Incomplete).
    /// Native async-owner allocation failure returns Degraded with Synchronous mode, retaining initialized sinks
    /// and disabling the timed flush worker; explicit and shutdown flushes remain available.
    static LogInitResult Initialize(const LogConfig& config);

    /// @brief Ends the session: closes admission, drains queued records, flushes and closes the sinks, and joins
    /// the native timed-flush and asynchronous workers. Later records take the emergency path.
    ///
    /// Call it before returning from `main`. If the process exits without it (for example through `exit()`),
    /// native process-lifetime teardown performs the same shutdown, so a running worker never terminates the
    /// process; a worker that cannot be joined is left running rather than destroyed.
    /// @return Ok; NotInitialized when no session is active; Incomplete when the asynchronous worker did not stop
    ///         within its deadline (its queue and sinks stay allocated), or on web when called from a console
    ///         callback.
    static LogStatus Shutdown();

    /// @brief Acknowledged flush: waits (up to timeoutMilliseconds) for all records accepted before the call to
    /// be processed and the selected local sinks to complete the requested flush kind, then reports status and
    /// the acknowledged sequence (requirements R28, R29).
    ///
    /// A timeout is honest: it cannot cancel an in-progress blocking OS write.
    /// Web Visible acknowledges completed synchronous console calls; Durable
    /// returns Unsupported. Neither promises developer-tools display or disk
    /// persistence. A failed console call returns SinkFailed until recovery.
    static FlushResult Flush(FlushKind kind = FlushKind::Visible, uint32 timeoutMilliseconds = 1000);

    static void SetGlobalLevel(LogLevel level);
    static void SetCategoryLevel(LogCategory category, LogLevel level);
    static void ClearCategoryLevels();

    [[nodiscard]] static LogStatistics Statistics();
    [[nodiscard]] static LogHealth Health();

    /// @brief True between a successful Initialize() and Shutdown().
    ///
    /// Normal code never needs to check this before logging; it exists for tests and diagnostics.
    [[nodiscard]] static bool IsInitialized() noexcept;
};

} // namespace ludus::foundation::logging
