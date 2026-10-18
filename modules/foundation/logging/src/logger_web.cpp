#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/level.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/time/time.hpp>

#include "internal/breadcrumb.hpp"
#include "internal/category_registry.hpp"
#include "internal/emergency_logger.hpp"
#include "internal/format_engine.hpp"

#include <source_location>
#include <span>
#include <string_view>

#include <emscripten.h>

// Foreign console callbacks can throw or reenter wasm. Catch only at the JS
// boundary; C++ remains exception-free. Each borrowed string has an explicit
// byte limit, and no borrowed pointer escapes this synchronous call.
// clang-format off
EM_JS(ludus::foundation::int32,
      LudusWriteConsole,
      (ludus::foundation::int32 level,
       const char* category,
       ludus::foundation::int32 categoryBytes,
       const char* message,
       ludus::foundation::int32 messageBytes,
       const char* thread,
       ludus::foundation::int32 threadBytes,
       const char* file,
       ludus::foundation::int32 fileBytes,
       ludus::foundation::int32 line),
      {
          try
          {
              const methods = [ 'debug', 'debug', 'info', 'warn', 'error', 'error' ];
              const method = console[methods[level]];
              if (typeof method != 'function')
              {
                  return 0;
              }
              let text = '[' + UTF8ToString(thread, threadBytes) + '] [' + UTF8ToString(category, categoryBytes) +
                         '] ' + UTF8ToString(message, messageBytes);
              if (level >= 3 && fileBytes > 0)
              {
                  text += '\n    ' + UTF8ToString(file, fileBytes) + ':' + line;
              }
              method.call(console, text);
              return 1;
          }
          catch (_)
          {
              return 0;
          }
      });
// clang-format on

namespace ludus::foundation::logging
{
namespace
{
constexpr usize MAX_MESSAGE_BYTES = 2048;
constexpr usize MAX_METADATA_BYTES = 256;

// Single browser main thread only. No workers, timers, locks around foreign
// callbacks, or waits. CategoryRegistry's cold mutex is uncontended and never
// encloses JS. Counters and breadcrumbs survive reconfiguration/shutdown.
struct BrowserLogger
{
    bool Initialized = false;
    bool Dispatching = false;
    bool ConsoleEnabled = false;
    LogLevel GlobalLevel = LogLevel::Info;
    internal::CategoryRegistry Categories;
    internal::BreadcrumbRing Breadcrumbs;
    LogStatistics Statistics;
    LogHealth Health;
    foundation::StaticArray<char, 64> ThreadName{};
    usize ThreadNameBytes = 0;
};

BrowserLogger& State() noexcept
{
    static BrowserLogger instance;
    return instance;
}

uint64 NowTicks() noexcept
{
    return time::NowTicks();
}

void Dispatch(LogLevel level,
              LogCategory category,
              const std::source_location& location,
              std::string_view message) noexcept
{
    BrowserLogger& state = State();
    const uint64 sequence = ++state.Statistics.Submitted;
    message = message.substr(0, MAX_MESSAGE_BYTES);
    if (level >= LogLevel::Warning)
    {
        state.Breadcrumbs.Push(sequence, NowTicks(), category.Id, level, message);
    }
    if (state.Dispatching)
    {
        // A console override can log recursively. Do not recurse into it or
        // deadlock; preserve the breadcrumb and account for the lost delivery.
        ++state.Statistics.Dropped;
        return;
    }
    state.Dispatching = true;
    if (state.Initialized == false || level == LogLevel::Fatal)
    {
        internal::EmergencyLog(level, category, message, location);
    }
    if (state.Initialized && state.ConsoleEnabled)
    {
        const std::string_view thread = state.ThreadNameBytes == 0
                                            ? std::string_view{"Main"}
                                            : std::string_view{state.ThreadName.GetData(), state.ThreadNameBytes};
        const std::string_view categoryName = category.Name.substr(0, MAX_METADATA_BYTES);
        const std::string_view boundedThread = thread.substr(0, MAX_METADATA_BYTES);
        const std::string_view file = std::string_view{location.file_name()}.substr(0, MAX_METADATA_BYTES);
        const bool isDelivered = LudusWriteConsole(static_cast<int32>(level),
                                                   categoryName.data(),
                                                   static_cast<int32>(categoryName.size()),
                                                   message.data(),
                                                   static_cast<int32>(message.size()),
                                                   boundedThread.data(),
                                                   static_cast<int32>(boundedThread.size()),
                                                   file.data(),
                                                   static_cast<int32>(file.size()),
                                                   static_cast<int32>(location.line())) != 0;
        state.Health.ConsoleHealthy = isDelivered;
        if (isDelivered == true)
        {
            ++state.Statistics.Written;
        }
        else
        {
            ++state.Statistics.Dropped;
        }
    }
    state.Dispatching = false;
}
} // namespace

bool ShouldLog(LogLevel level, LogCategory category) noexcept
{
    const BrowserLogger& state = State();
    if (level == LogLevel::Fatal)
    {
        return true;
    }
    if (state.Initialized == false)
    {
        return level >= LogLevel::Warning;
    }
    LogLevel threshold = state.GlobalLevel;
    (void)state.Categories.TryGetOverride(category.Id, threshold);
    return level >= threshold;
}

LogInitResult LogSystem::Initialize(const LogConfig& config)
{
    BrowserLogger& state = State();
    if (state.Dispatching)
    {
        return {LogStatus::Incomplete, LogMode::Synchronous};
    }
    state.Categories.Clear();
    state.GlobalLevel = config.GlobalLevel;
    state.ConsoleEnabled = config.EnableConsole;
    state.Health = {};
    state.Health.FileHealthy = config.EnableFile == false;
    state.Health.DebuggerHealthy = config.EnableDebugger == false;
    state.Initialized = true;
    const bool degraded = config.Mode != LogMode::Synchronous || config.EnableFile || config.EnableDebugger;
    return {degraded ? LogStatus::Degraded : LogStatus::Ok, LogMode::Synchronous};
}

LogStatus LogSystem::Shutdown()
{
    BrowserLogger& state = State();
    if (state.Dispatching)
    {
        return LogStatus::Incomplete;
    }
    if (state.Initialized == false)
    {
        return LogStatus::NotInitialized;
    }
    state.Initialized = false;
    state.ConsoleEnabled = false;
    state.Categories.Clear();
    return LogStatus::Ok;
}

FlushResult LogSystem::Flush(FlushKind kind, uint32 /*timeoutMilliseconds*/)
{
    const BrowserLogger& state = State();
    if (state.Initialized == false)
    {
        return {LogStatus::NotInitialized, 0};
    }
    if (state.Dispatching)
    {
        return {LogStatus::Incomplete, 0};
    }
    const LogStatus status = kind == FlushKind::Durable
                                 ? LogStatus::Unsupported
                                 : (state.Health.ConsoleHealthy ? LogStatus::Ok : LogStatus::SinkFailed);
    return {status, state.Statistics.Submitted};
}

void LogSystem::SetGlobalLevel(LogLevel level)
{
    State().GlobalLevel = level;
}
void LogSystem::SetCategoryLevel(LogCategory category, LogLevel level)
{
    (void)State().Categories.SetOverride(category.Id, category.Name, level);
}
void LogSystem::ClearCategoryLevels()
{
    State().Categories.Clear();
}
LogStatistics LogSystem::Statistics()
{
    return State().Statistics;
}
LogHealth LogSystem::Health()
{
    LogHealth health = State().Health;
    health.DroppedTotal = State().Statistics.Dropped;
    return health;
}
bool LogSystem::IsInitialized() noexcept
{
    return State().Initialized;
}

void SetCurrentThreadName(std::string_view name)
{
    BrowserLogger& state = State();
    state.ThreadNameBytes = name.size() < state.ThreadName.GetSize() ? name.size() : state.ThreadName.GetSize();
    for (usize i = 0; i < state.ThreadNameBytes; ++i)
    {
        state.ThreadName[i] = name[i];
    }
}

namespace detail
{
void SubmitText(LogLevel level,
                LogCategory category,
                const std::source_location& location,
                std::string_view text) noexcept
{
    Dispatch(level, category, location, text);
}
void SubmitFormat(LogLevel level,
                  LogCategory category,
                  const std::source_location& location,
                  std::string_view format,
                  std::span<const FormatArg> args) noexcept
{
    foundation::StaticArray<char, MAX_MESSAGE_BYTES> buffer;
    const internal::FormatOutcome outcome =
        internal::FormatInto(std::span<char>(buffer.GetData(), buffer.GetSize()), format, args);
    Dispatch(level, category, location, {buffer.GetData(), outcome.BytesWritten});
}
DeliveryResult SubmitDirectText(LogLevel level,
                                LogCategory category,
                                const std::source_location& location,
                                std::string_view text) noexcept
{
    BrowserLogger& state = State();
    text = text.substr(0, MAX_MESSAGE_BYTES);
    state.Breadcrumbs.Push(++state.Statistics.Submitted, NowTicks(), category.Id, level, text);
    if (state.Dispatching)
    {
        ++state.Statistics.Dropped;
        return {DeliveryStatus::NoEndpoint};
    }
    state.Dispatching = true;
    internal::EmergencyLog(level, category, text, location);
    state.Dispatching = false;
    // Base's emergency API has no endpoint acknowledgement. Do not claim that
    // a browser console callback or developer tools displayed the diagnostic.
    return {DeliveryStatus::NoEndpoint};
}
} // namespace detail

usize SnapshotBreadcrumbs(internal::BreadcrumbRing::Entry* out, usize capacity) noexcept
{
    return State().Breadcrumbs.Snapshot(out, capacity);
}
} // namespace ludus::foundation::logging
