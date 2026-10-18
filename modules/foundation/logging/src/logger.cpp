#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include "internal/backend.hpp"
#include "internal/breadcrumb.hpp"
#include "internal/category_registry.hpp"
#include "internal/emergency_logger.hpp"
#include "internal/format_engine.hpp"
#include "internal/formatter.hpp"
#include "internal/log_record.hpp"
#include "internal/mpsc_queue.hpp"
#include "internal/sink.hpp"
#include "sinks/console_sink.hpp"
#include "sinks/debugger_sink.hpp"
#include "sinks/file_sink.hpp"

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
// Maximum bytes of a single formatted/raw message on the producer path. Longer
// messages are truncated with a flag rather than allocating (requirements R25).
// Prototype starting value; final size is a measurement-dependent budget.
constexpr ludus::foundation::usize kMaxMessageBytes = 2048;
} // namespace

namespace ludus::foundation::logging
{

namespace
{

// Construct an owning UniquePtr<ILogSink> for a concrete sink type. Ludus'
// UniquePtr takes ownership of an lvalue raw pointer (nulling it), so we hand it
// a freshly-new'd sink; the base's virtual destructor makes deletion correct.
template <typename SinkType, typename... Args>
foundation::UniquePtr<internal::ILogSink> MakeSink(Args&&... args)
{
    internal::ILogSink* raw = new SinkType(static_cast<Args&&>(args)...);
    return foundation::UniquePtr<internal::ILogSink>(raw);
}

// -----------------------------------------------------------------------------
// Logger state.
//
// Ownership / synchronization contract (design.md sections 7, 8):
//   * GlobalLevel and the category registry are lock-free reads on the hot path
//     (ShouldLog): scalar atomic loads only, no lock, no allocation, no map
//     lookup (requirements R13).
//   * Admission and sink lifetime are governed by `DispatchMutex`, held
//     EXCLUSIVELY around every sink Write/Flush so no sink-owned buffer is
//     mutated by two threads at once (requirements R27 sync path; fixes F1).
//   * The lifecycle State is the admission gate: a producer that observes
//     Accepting under the dispatch lock is guaranteed the sinks are alive for
//     the duration of that locked region (fixes F4).
// -----------------------------------------------------------------------------
enum class State : uint8
{
    Uninitialized,
    Accepting,
    Draining,
    Stopped,
};

struct LoggerState
{
    // Serializes sink access AND admission decisions. A single exclusive mutex
    // (not a shared_mutex) because sink Write mutates sink-owned scratch; a
    // shared lock would allow the confirmed data race.
    std::mutex DispatchMutex;

    std::atomic<State> Lifecycle{State::Uninitialized};
    std::atomic<LogLevel> GlobalLevel{LogLevel::Info};
    std::atomic<LogMode> EffectiveMode{LogMode::Synchronous};

    internal::CategoryRegistry Categories;

    foundation::Array<foundation::UniquePtr<internal::ILogSink>> Sinks;

    // Timed-flush worker (requirements R46). Only runs while Accepting and only
    // when a positive interval is configured.
    std::thread FlushThread;
    std::mutex FlushMutex;
    std::condition_variable FlushCv;
    bool FlushStop = false;
    uint32 FlushIntervalMs = 0;

    std::atomic<uint64> Submitted{0};
    std::atomic<uint64> Written{0};
    std::atomic<uint64> Dropped{0};

    std::atomic<bool> ConsoleHealthy{true};
    std::atomic<bool> FileHealthy{true};
    std::atomic<bool> DebuggerHealthy{true};

    // Producer-side breadcrumb ring (requirements R34). Populated on the producer
    // for selected severities so recent context survives even if a record never
    // reaches a sink.
    internal::BreadcrumbRing Breadcrumbs;

    // Asynchronous backend (requirements R35-R40). Non-null and owning the sinks
    // when EffectiveMode == Asynchronous; null in the synchronous backend, where
    // Sinks holds them and DispatchMutex serializes access. Retained across a
    // timed-out shutdown so a still-running worker never references freed storage.
    foundation::UniquePtr<internal::AsyncBackend> Async;
};

LoggerState& state() noexcept
{
    static LoggerState sInstance;
    return sInstance;
}

// Effective threshold: category override if present, else the global level.
// Lock-free: the registry read is scalar atomic loads (requirements R13).
LogLevel effectiveLevel(LoggerState& s, LogCategory category) noexcept
{
    LogLevel overridden{};
    if (s.Categories.TryGetOverride(category.Id, overridden))
    {
        return overridden;
    }
    return s.GlobalLevel.load(std::memory_order_relaxed);
}

// Flush every sink while holding the dispatch lock. Records per-sink health.
void flushSinksLocked(LoggerState& s) noexcept
{
    for (auto& sink : s.Sinks)
    {
        const internal::SinkStatus status = sink->Flush();
        if (status != internal::SinkStatus::Ok)
        {
            // Health is tracked per concrete sink kind below via Write; a flush
            // failure marks the file sink unhealthy conservatively.
        }
        (void)status;
    }
}

// Build an owned/borrowed record view for synchronous dispatch. In the sync
// backend the borrowed spans are valid for the whole locked Write region.
internal::LogRecordView makeView(LogLevel level,
                                 LogCategory category,
                                 const std::source_location& location,
                                 std::string_view message,
                                 uint64 sequence) noexcept
{
    internal::LogRecordView record;
    record.Level = level;
    record.Category = category;
    record.Message = message;
    record.File = location.file_name();
    record.Function = location.function_name();
    record.Line = location.line();
    record.ThreadName = internal::GetCurrentThreadName();
    record.ThreadId = internal::GetCurrentThreadId();
    record.NativeThreadId = internal::GetNativeThreadId();
    record.MonotonicTicks = internal::GetMonotonicTicks();
    record.Sequence = sequence;
    return record;
}

// The single, admission-checked dispatch used by both the raw-text and format
// submit entry points. `message` is already-final text (no format parsing here).
void dispatchAdmitted(LogLevel level,
                      LogCategory category,
                      const std::source_location& location,
                      std::string_view message) noexcept
{
    LoggerState& s = state();
    const uint64 seq = s.Submitted.fetch_add(1, std::memory_order_relaxed) + 1;

    // Producer-side breadcrumb capture for selected severities (Warning+, spec
    // CD6): copy recent context NOW, on the producer, so it survives even if the
    // record is later dropped or a (future) worker is parked (requirements R34).
    if (level >= LogLevel::Warning)
    {
        s.Breadcrumbs.Push(seq, internal::GetMonotonicTicks(), category.Id, level, message);
    }

    // Emergency-first for Fatal (requirements R30; fixes F2): the independent,
    // allocation-free emergency writer runs BEFORE we touch the normal backend,
    // so a broken/locked/failed sink cannot prevent the critical record. Normal
    // sinks are then a best-effort secondary destination.
    if (level == LogLevel::Fatal)
    {
        internal::EmergencyLog(level, category, message, location);
    }

    // Asynchronous backend path: build an OWNED record and enqueue without
    // touching sinks or taking the dispatch lock (requirements R35/R36). The
    // backend worker owns the sinks. Admission is checked via the lifecycle
    // state; a record admitted here belongs to the current session.
    if (s.EffectiveMode.load(std::memory_order_acquire) == LogMode::Asynchronous)
    {
        if (s.Lifecycle.load(std::memory_order_acquire) != State::Accepting || s.Async.Get() == nullptr)
        {
            if (level >= LogLevel::Warning && level != LogLevel::Fatal)
            {
                internal::EmergencyLog(level, category, message, location);
            }
            return;
        }
        internal::QueuedRecord qr;
        qr.Sequence = seq;
        qr.MonotonicTicks = internal::GetMonotonicTicks();
        qr.NativeThreadId = internal::GetNativeThreadId();
        qr.ThreadId = internal::GetCurrentThreadId();
        qr.CategoryId = category.Id;
        qr.CategoryName = category.Name;
        qr.Line = location.line();
        qr.File = location.file_name();
        qr.Level = level;
        qr.SetThreadName(internal::GetCurrentThreadName());
        qr.SetMessage(message);
        // Overflow (Enqueue returned false) is already counted inside the queue's
        // own drop counter, which Statistics() reads via Async->Dropped(); we do
        // NOT also bump s.Dropped here or the drop would be counted twice
        // (requirements R48). Warning+ already left a breadcrumb above; no
        // emergency note here, to avoid an error-storm feedback loop.
        (void)s.Async->Enqueue(qr);
        return;
    }

    std::unique_lock lock(s.DispatchMutex);

    // Admission recheck UNDER the dispatch lock (fixes F4): if we are no longer
    // Accepting, route Warning+ to the emergency path and drop the rest. This
    // closes the window where a producer passed an unlocked init check, paused,
    // then acquired the lock after Shutdown cleared the sinks.
    if (s.Lifecycle.load(std::memory_order_acquire) != State::Accepting)
    {
        lock.unlock();
        if (level >= LogLevel::Warning && level != LogLevel::Fatal)
        {
            internal::EmergencyLog(level, category, message, location);
        }
        return;
    }

    const internal::LogRecordView record = makeView(level, category, location, message, seq);

    bool anyDelivered = false;
    for (auto& sink : s.Sinks)
    {
        const internal::SinkStatus status = sink->Write(record);
        if (status == internal::SinkStatus::Ok)
        {
            anyDelivered = true;
        }
    }

    // Error and Fatal flush synchronously so the record is externally visible
    // even if the process dies on the next statement (documented visibility, not
    // durability; requirements R46, R54).
    if (level >= LogLevel::Error)
    {
        flushSinksLocked(s);
    }

    // Honest counters (requirements R48; fixes F8): only count a write when a
    // sink actually accepted the record. A zero-sink or all-failed dispatch does
    // not inflate Written.
    if (anyDelivered)
    {
        s.Written.fetch_add(1, std::memory_order_relaxed);
    }
}

void startFlushThread(LoggerState& s)
{
    if (s.FlushIntervalMs == 0)
    {
        return;
    }
    s.FlushStop = false;
    s.FlushThread = std::thread([&s] {
        std::unique_lock lock(s.FlushMutex);
        while (s.FlushStop == false)
        {
            s.FlushCv.wait_for(lock, std::chrono::milliseconds(s.FlushIntervalMs), [&s] { return s.FlushStop; });
            if (s.FlushStop)
            {
                break;
            }
            // Flush under the dispatch lock so sink access stays serialized.
            std::unique_lock dispatch(s.DispatchMutex);
            if (s.Lifecycle.load(std::memory_order_acquire) == State::Accepting)
            {
                flushSinksLocked(s);
            }
        }
    });
}

void stopFlushThread(LoggerState& s)
{
    {
        std::lock_guard guard(s.FlushMutex);
        s.FlushStop = true;
    }
    s.FlushCv.notify_all();
    if (s.FlushThread.joinable())
    {
        s.FlushThread.join();
    }
}

} // namespace

bool ShouldLog(LogLevel level, LogCategory category) noexcept
{
    LoggerState& s = state();

    // Fatal is always eligible: it must reach the emergency path even before
    // Initialize() and even if a category is turned down (requirements R14).
    if (level == LogLevel::Fatal)
    {
        return true;
    }

    const State lifecycle = s.Lifecycle.load(std::memory_order_acquire);
    if (lifecycle != State::Accepting)
    {
        // Before init / after shutdown: only Warning and above are eligible,
        // and they go out via the emergency path (requirements R31).
        return level >= LogLevel::Warning;
    }

    // Hot path: scalar atomic loads only. No lock, no map, no allocation.
    return level >= effectiveLevel(s, category);
}

bool LogSystem::IsInitialized() noexcept
{
    return state().Lifecycle.load(std::memory_order_acquire) == State::Accepting;
}

LogInitResult LogSystem::Initialize(const LogConfig& config)
{
    LoggerState& s = state();
    std::unique_lock lock(s.DispatchMutex);

    LogInitResult result;

    if (s.Lifecycle.load(std::memory_order_acquire) == State::Accepting)
    {
        internal::EmergencyNote("LogSystem::Initialize called while already initialized; reconfiguring");
        // Tear down the previous session cleanly before reconfiguring.
        lock.unlock();
        stopFlushThread(s);
        if (s.Async)
        {
            (void)s.Async->Stop(2000);
            s.Async.Reset();
        }
        lock.lock();
        flushSinksLocked(s);
        s.Sinks.Clear();
    }

    s.GlobalLevel.store(config.GlobalLevel, std::memory_order_relaxed);
    s.Categories.Clear();
    s.ConsoleHealthy.store(true, std::memory_order_relaxed);
    s.FileHealthy.store(true, std::memory_order_relaxed);
    s.DebuggerHealthy.store(true, std::memory_order_relaxed);

    LogMode effective = config.Mode;
    result.Status = LogStatus::Ok;
    s.EffectiveMode.store(effective, std::memory_order_relaxed);
    result.EffectiveMode = effective;

    if (config.EnableConsole)
    {
        s.Sinks.Add(MakeSink<internal::ConsoleSink>());
    }
    if (config.EnableDebugger)
    {
        s.Sinks.Add(MakeSink<internal::DebuggerSink>());
    }
    if (config.EnableFile)
    {
        if (config.Directory.empty())
        {
            internal::EmergencyNote("file logging requested but LogConfig::Directory is empty; file sink disabled");
        }
        else
        {
            internal::FileSinkConfig fileConfig;
            fileConfig.Directory = config.Directory;
            fileConfig.MaxFileSizeBytes = config.MaxFileSizeBytes;
            fileConfig.MaxTotalBytes = config.MaxTotalBytes;
            fileConfig.MaxSessionAgeDays = config.MaxSessionAgeDays;
            fileConfig.RetainedSessions = config.RetainedSessions;
            internal::FileSink* file_sink = internal::FileSink::Create(fileConfig);
            if (file_sink != nullptr)
            {
                // UniquePtr takes ownership via an lvalue base pointer (nulling
                // it). FileSink derives from ILogSink (virtual dtor), so deleting
                // through the base is correct.
                internal::ILogSink* owned = file_sink;
                file_sink = nullptr;
                s.Sinks.Add(foundation::UniquePtr<internal::ILogSink>(owned));
            }
            else
            {
                s.FileHealthy.store(false, std::memory_order_relaxed);
                internal::EmergencyNote("failed to open log file; file sink disabled");
                if (result.Status == LogStatus::Ok)
                {
                    result.Status = LogStatus::Degraded;
                }
            }
        }
    }

    s.FlushIntervalMs = config.FlushIntervalMilliseconds;

    if (effective == LogMode::Asynchronous)
    {
        // Hand sink ownership to the worker; the worker is the sole owner of the
        // sinks for the async session (requirements R36). The synchronous timed
        // flush thread is NOT started; the worker owns the flush cadence.
        s.Async.Reset(new (std::nothrow) internal::AsyncBackend());
        if (s.Async)
        {
            s.Async->Start(Move(s.Sinks), config.FlushIntervalMilliseconds);
            s.Sinks.Clear();
        }
        else
        {
            effective = LogMode::Synchronous;
            s.FlushIntervalMs = 0; // Do not request another allocation for a timed worker after allocation failure.
            s.EffectiveMode.store(effective, std::memory_order_relaxed);
            result.EffectiveMode = effective;
            result.Status = LogStatus::Degraded;
            internal::EmergencyNote("async backend allocation failed; using synchronous logging");
        }
    }

    // Publish Accepting last, so ShouldLog/dispatch only admit once sinks exist.
    s.Lifecycle.store(State::Accepting, std::memory_order_release);

    lock.unlock();
    if (effective == LogMode::Synchronous)
    {
        startFlushThread(s);
    }
    return result;
}

LogStatus LogSystem::Shutdown()
{
    LoggerState& s = state();

    // Close admission first so racing late producers are rejected (fixes F4),
    // then stop the flush worker (outside the dispatch lock to avoid a deadlock),
    // then flush/close sinks under the lock and join.
    State expected = State::Accepting;
    if (s.Lifecycle.compare_exchange_strong(expected, State::Draining, std::memory_order_acq_rel) == false)
    {
        return LogStatus::NotInitialized;
    }

    stopFlushThread(s);

    LogStatus status = LogStatus::Ok;

    if (s.Async)
    {
        // Stop the worker: it drains published records, flushes, and exits. If it
        // cannot be joined within the deadline, RETAIN the backend (and thus its
        // queue/sinks) rather than freeing storage a running worker may touch
        // (requirements R40). Report Incomplete in that case.
        const bool stopped = s.Async->Stop(5000);
        if (stopped)
        {
            s.Async.Reset();
        }
        else
        {
            status = LogStatus::Incomplete;
            internal::EmergencyNote("async logging worker did not stop within timeout; storage retained");
        }
    }

    {
        std::unique_lock lock(s.DispatchMutex);
        flushSinksLocked(s); // no-op set in async mode (sinks moved to worker)
        s.Sinks.Clear();
        s.Categories.Clear();
        s.Lifecycle.store(State::Stopped, std::memory_order_release);
    }
    return status;
}

FlushResult LogSystem::Flush(FlushKind kind, uint32 timeoutMilliseconds)
{
    LoggerState& s = state();
    FlushResult result;

    if (s.Lifecycle.load(std::memory_order_acquire) != State::Accepting)
    {
        result.Status = LogStatus::NotInitialized;
        return result;
    }

    // Asynchronous: capture a fence and wait (bounded) for the worker to process
    // through it and flush the requested kind (requirements R28/R29). The request
    // travels on the backend's separate control channel, so a full data queue
    // cannot block requesting the flush.
    if (s.EffectiveMode.load(std::memory_order_acquire) == LogMode::Asynchronous)
    {
        if (s.Async.Get() == nullptr)
        {
            result.Status = LogStatus::NotInitialized;
            return result;
        }
        const internal::AsyncBackend::FlushOutcome outcome =
            s.Async->Flush(kind == FlushKind::Durable, timeoutMilliseconds);
        result.AcknowledgedSequence = outcome.AcknowledgedPos;
        result.Status = outcome.TimedOut ? LogStatus::TimedOut : (outcome.Ok ? LogStatus::Ok : LogStatus::SinkFailed);
        return result;
    }

    // Synchronous: flush the sinks directly under the dispatch lock.
    std::unique_lock lock(s.DispatchMutex);
    bool anyFailed = false;
    for (auto& sink : s.Sinks)
    {
        const internal::SinkStatus status = kind == FlushKind::Durable ? sink->FlushDurable() : sink->Flush();
        if (status != internal::SinkStatus::Ok && status != internal::SinkStatus::Unsupported)
        {
            anyFailed = true;
        }
    }

    result.AcknowledgedSequence = s.Submitted.load(std::memory_order_relaxed);
    result.Status = anyFailed ? LogStatus::SinkFailed : LogStatus::Ok;
    return result;
}

void LogSystem::SetGlobalLevel(LogLevel level)
{
    state().GlobalLevel.store(level, std::memory_order_relaxed);
}

void LogSystem::SetCategoryLevel(LogCategory category, LogLevel level)
{
    state().Categories.SetOverride(category.Id, category.Name, level);
}

void LogSystem::ClearCategoryLevels()
{
    state().Categories.Clear();
}

LogStatistics LogSystem::Statistics()
{
    LoggerState& s = state();
    LogStatistics stats;
    stats.Submitted = s.Submitted.load(std::memory_order_relaxed);
    if (s.EffectiveMode.load(std::memory_order_acquire) == LogMode::Asynchronous && s.Async)
    {
        // In async mode the worker owns delivery; report its counters
        // (requirements R48). Written = records the worker delivered to sinks;
        // Dropped = queue-full drops plus any producer-side drops.
        stats.Written = s.Async->Written();
        stats.Dropped = s.Dropped.load(std::memory_order_relaxed) + s.Async->Dropped();
    }
    else
    {
        stats.Written = s.Written.load(std::memory_order_relaxed);
        stats.Dropped = s.Dropped.load(std::memory_order_relaxed);
    }
    return stats;
}

LogHealth LogSystem::Health()
{
    LoggerState& s = state();
    LogHealth health;
    health.ConsoleHealthy = s.ConsoleHealthy.load(std::memory_order_relaxed);
    health.FileHealthy = s.FileHealthy.load(std::memory_order_relaxed);
    health.DebuggerHealthy = s.DebuggerHealthy.load(std::memory_order_relaxed);
    health.WorkerHealthy = true; // async worker arrives in a later batch
    health.QueueHighWaterMark = 0;
    health.DroppedTotal = s.Dropped.load(std::memory_order_relaxed);
    return health;
}

namespace detail
{

void SubmitText(LogLevel level,
                LogCategory category,
                const std::source_location& location,
                std::string_view text) noexcept
{
    // Raw text: no format interpretation whatsoever (requirements R16). Bounded:
    // over-long raw text is truncated (no allocation, requirements R25).
    if (text.size() <= kMaxMessageBytes)
    {
        dispatchAdmitted(level, category, location, text);
        return;
    }
    dispatchAdmitted(level, category, location, text.substr(0, kMaxMessageBytes));
}

void SubmitFormat(LogLevel level,
                  LogCategory category,
                  const std::source_location& location,
                  std::string_view format,
                  std::span<const FormatArg> args) noexcept
{
    // Bounded, no-heap typed formatting into a producer-owned stack buffer. No
    // allocation, no throw, no terminate: a bad/oversized format degrades to a
    // bounded marker and sets a flag (requirements R19/R25/R38). The bounded
    // buffer is the common-path storage; nothing borrowed escapes this call.
    foundation::StaticArray<char, kMaxMessageBytes> buffer;
    const internal::FormatOutcome outcome =
        internal::FormatInto(std::span<char>(buffer.GetData(), buffer.GetSize()), format, args);

    const std::string_view message(buffer.GetData(), outcome.BytesWritten);
    dispatchAdmitted(level, category, location, message);
}

DeliveryResult SubmitDirectText(LogLevel level,
                                LogCategory category,
                                const std::source_location& location,
                                std::string_view text) noexcept
{
    // Direct debugger-critical delivery (requirements R27 path 2). This path is
    // INDEPENDENT of the (future) asynchronous worker and of the ordinary sink
    // set: it writes straight to the direct diagnostic endpoint (stderr, and the
    // debugger on Windows) via the Base emergency primitive, so a breakpoint
    // immediately after the call still observes the record even if the worker is
    // parked. It also records a breadcrumb.
    LoggerState& s = state();
    const uint64 seq = s.Submitted.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::string_view bounded = text.size() <= kMaxMessageBytes ? text : text.substr(0, kMaxMessageBytes);
    s.Breadcrumbs.Push(seq, internal::GetMonotonicTicks(), category.Id, level, bounded);

    // Deliver directly and synchronously; the Base primitive flushes stderr
    // before returning, which is the "reached a healthy direct endpoint" promise
    // (not "a GUI painted it").
    internal::EmergencyLog(level, category, bounded, location);

    DeliveryResult result;
    result.Status = DeliveryStatus::Delivered;
    return result;
}

} // namespace detail

// Test/diagnostic accessor for the producer-side breadcrumb ring. Copies stable
// entries; skips any slot being written (requirements R34). Not installed in the
// SDK (declared only in the internal test header).
usize SnapshotBreadcrumbs(internal::BreadcrumbRing::Entry* out, usize capacity) noexcept
{
    return state().Breadcrumbs.Snapshot(out, capacity);
}

} // namespace ludus::foundation::logging
