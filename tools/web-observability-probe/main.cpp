#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/diagnostic.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/category.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/profiling/clock.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>

#include "internal/breadcrumb.hpp"

#include <source_location>
#include <string_view>

#include <emscripten.h>

extern "C" EMSCRIPTEN_KEEPALIVE ludus::foundation::int32 W2Reenter()
{
    using namespace ludus::foundation::logging;
    LUDUS_LOG_WARN(LOG_TEMP, "nested console callback");
    return LogSystem::Shutdown() == LogStatus::Incomplete ? 1 : 0;
}

namespace
{
using namespace ludus::foundation;
using namespace ludus::foundation::logging;

bool Verify(bool condition, std::string_view message) noexcept
{
    if (condition == false)
    {
        base::EmergencyReport(base::DiagnosticSeverity::Error, "W2", message);
    }
    return condition;
}

// Fault injection belongs to the probe, not the engine. Restored before Base
// diagnostics so logger failure cannot hide a failed assertion check.
// clang-format off
EM_JS(void, BreakConsole, (), {
    Module.savedWarn = console.warn;
    console.warn = function()
    {
        throw new Error('W2 injected sink failure');
    };
});
EM_JS(void, ReenterConsole, (), {
    Module.savedWarn = console.warn;
    console.warn = function (text) { Module.reentryOkay = _W2Reenter(); Module.savedWarn.call(console, text); };
});
EM_JS(int32, CheckReentry, (), { return Module.reentryOkay; });
EM_JS(void, CaptureConsole, (), {
    Module.savedWarn = console.warn;
    console.warn = function (text) { Module.boundedText = text; };
});
EM_JS(int32, CheckBoundedConsole, (), {
    return Module.boundedText.split('X').length - 1 == 2048 ? 1 : 0;
});
EM_JS(void, NotifyResult, (int32 code), {
    if (typeof Module.probeFinished == 'function') Module.probeFinished(code);
});
EM_JS(void, RestoreConsole, (), { console.warn = Module.savedWarn; });
EM_JS(void, PrintTrace, (), { console.info('[W2:trace]' + FS.readFile('/trace.json', {encoding : 'utf8'})); });
// clang-format on

bool Run() noexcept
{
    const auto clockBefore = time::Now();
    const auto profile = profiling::NowTicks();
    const auto clockAfter = time::Now();
    time::Duration elapsed;
    time::FrameClock frame{time::Duration{10}};
    time::Stopwatch stopwatch;
    time::Deadline deadline;
    stopwatch.Start(time::Timestamp{0});
    if (Verify(profile >= clockBefore.Nanoseconds && profile <= clockAfter.Nanoseconds &&
                   time::TryElapsed(clockBefore, clockAfter, elapsed) == time::TimeStatus::Ok &&
                   frame.Sample(time::Timestamp{100}).IsBaseline &&
                   frame.Sample(time::Timestamp{120}).Discarded.Nanoseconds == 10 &&
                   stopwatch.Pause(time::Timestamp{5}) == time::TimeStatus::Ok &&
                   stopwatch.Read(time::Timestamp{100}, elapsed) == time::TimeStatus::Ok && elapsed.Nanoseconds == 5 &&
                   deadline.Arm(clockAfter, {}) == time::TimeStatus::Ok && deadline.IsExpired(clockAfter),
               "FoundationTime clock and helpers") == false)
    {
        return false;
    }

    LogConfig config;
    config.EnableFile = false;
    config.EnableDebugger = false;
    config.GlobalLevel = LogLevel::Warning;
    config.FlushIntervalMilliseconds = 1000; // no background timer on web
    const LogCategory category = LogCategory{"BrowserProbe"};
    if (Verify(LogSystem::Initialize(config).Status == LogStatus::Ok, "initialize") == false)
    {
        return false;
    }
    SetCurrentThreadName("BrowserMain");
    if (Verify(ShouldLog(LogLevel::Info, category) == false, "global filtering") == false)
    {
        return false;
    }
    LogSystem::SetCategoryLevel(category, LogLevel::Debug);
    if (Verify(ShouldLog(LogLevel::Info, category), "category override") == false)
    {
        return false;
    }
    LogSystem::ClearCategoryLevels();
    const auto before = LogSystem::Statistics();
    LUDUS_LOG_WARN(category, "formatted {} {}", 42, 1.25);
    const auto delivered = LogSystem::Statistics();
    if (Verify(delivered.Written == before.Written + 1, "console delivery") == false)
    {
        return false;
    }
    BreakConsole();
    LUDUS_LOG_WARN(category, "sink failure");
    RestoreConsole();
    const auto failed = LogSystem::Statistics();
    if (Verify(failed.Written == delivered.Written && failed.Dropped == delivered.Dropped + 1 &&
                   LogSystem::Health().ConsoleHealthy == false && LogSystem::Flush().Status == LogStatus::SinkFailed,
               "sink failure accounting") == false)
    {
        return false;
    }
    StaticArray<internal::BreadcrumbRing::Entry, 4> breadcrumbs;
    if (Verify(SnapshotBreadcrumbs(breadcrumbs.GetData(), breadcrumbs.GetSize()) >= 2,
               "breadcrumbs survive failed delivery") == false)
    {
        return false;
    }
    LUDUS_CHECK(false, "W2 logger-independent CHECK");
    LUDUS_LOG_WARN(category, "console recovered");
    if (Verify(LogSystem::Health().ConsoleHealthy && LogSystem::Flush().Status == LogStatus::Ok &&
                   LogSystem::Flush(FlushKind::Durable).Status == LogStatus::Unsupported,
               "flush semantics") == false)
    {
        return false;
    }
    const auto beforeNested = LogSystem::Statistics();
    ReenterConsole();
    LUDUS_LOG_WARN(category, "outer console callback");
    RestoreConsole();
    const auto afterNested = LogSystem::Statistics();
    if (Verify(afterNested.Submitted == beforeNested.Submitted + 2 && afterNested.Written == beforeNested.Written + 1 &&
                   afterNested.Dropped == beforeNested.Dropped + 1 && LogSystem::IsInitialized(),
               "bounded reentry") == false)
    {
        return false;
    }
    StaticArray<char, 4096> longMessage;
    for (usize i = 0; i < longMessage.GetSize(); ++i)
    {
        longMessage[i] = 'X';
    }
    CaptureConsole();
    LUDUS_LOG_TEXT(category, Warning, std::string_view(longMessage.GetData(), longMessage.GetSize()));
    RestoreConsole();
    if (Verify(CheckBoundedConsole() != 0, "bounded raw message") == false)
    {
        return false;
    }
    config.Mode = LogMode::Asynchronous;
    config.EnableFile = true;
    config.EnableDebugger = true;
    const auto degraded = LogSystem::Initialize(config);
    if (Verify(degraded.Status == LogStatus::Degraded && degraded.EffectiveMode == LogMode::Synchronous &&
                   LogSystem::Health().FileHealthy == false && LogSystem::Health().DebuggerHealthy == false,
               "unsupported sinks") == false)
    {
        return false;
    }
    if (Verify(LogSystem::Shutdown() == LogStatus::Ok && LogSystem::IsInitialized() == false &&
                   LogSystem::Shutdown() == LogStatus::NotInitialized,
               "shutdown") == false)
    {
        return false;
    }
    config = {};
    config.EnableFile = false;
    config.EnableDebugger = false;
    if (Verify(LogSystem::Initialize(config).Status == LogStatus::Ok && LogSystem::Shutdown() == LogStatus::Ok,
               "restart") == false)
    {
        return false;
    }

    using namespace ludus::foundation::profiling;
    const uint64 firstTick = NowTicks();
    if (Verify(NowTicks() >= firstTick && TicksPerNanosecondDenominator() == 1, "browser clock") == false)
    {
        return false;
    }
    const bool capture = BeginCapture(2);
    int32 evaluated = 0;
    {
        LUDUS_PROFILE_SCOPE(BrowserFrame);
        LUDUS_PROFILE_MARK(BrowserMarker);
        LUDUS_PROFILE_FRAME();
        LUDUS_PROFILE_FLOW_IN(++evaluated);
    }
    EndCapture();
#if LUDUS_PROFILING_ENABLED
    if (Verify(capture && evaluated == 1 && GetTraceHealth().EventsRecorded == 5 &&
                   GetTraceHealth().ThreadsRegistered == 1 && ExportPerfettoTrace("/trace.json"),
               "capture/export") == false)
    {
        return false;
    }
    PrintTrace();
    if (Verify(BeginCapture(1), "capture restart") == false)
    {
        return false;
    }
    LUDUS_PROFILE_MARK(SecondCapture);
    EndCapture();
    if (Verify(GetTraceHealth().EventsRecorded == 1 && ExportPerfettoTrace("/second.json"), "second capture") == false)
    {
        return false;
    }
#else
    if (Verify(capture == false && evaluated == 0 && GetTraceHealth().EventsRecorded == 0 &&
                   ExportPerfettoTrace("/trace.json") == false,
               "Release compile-out") == false)
    {
        return false;
    }
#endif
    base::EmergencyReport(base::DiagnosticSeverity::Note, "W2", "[W2:passed]");
    return true;
}
} // namespace

int main()
{
    const ludus::foundation::int32 code = Run() ? 0 : 1;
    NotifyResult(code);
    return code;
}
