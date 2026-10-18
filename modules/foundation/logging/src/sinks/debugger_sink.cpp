#include "sinks/debugger_sink.hpp"
#include <ludus/foundation/base/config.h>

#include "internal/formatter.hpp"

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent(void);
#endif

namespace ludus::foundation::logging::internal
{

DebuggerSink::DebuggerSink()
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    mActive = IsDebuggerPresent() != 0;
#endif
    mScratch.reserve(256);
}

SinkStatus DebuggerSink::Write([[maybe_unused]] const LogRecordView& record) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    // Re-check attachment rather than trusting a stale ctor-time snapshot
    // (fixes F10): a debugger may attach after Initialize().
    mActive = IsDebuggerPresent() != 0;
    if (mActive == false)
    {
        return SinkStatus::Disabled;
    }
    // Debugger output never uses ANSI color.
    FormatConsoleLine(record, /*use_color=*/false, mScratch);
    mScratch.push_back('\n');
    OutputDebugStringA(mScratch.c_str());
    return SinkStatus::Ok;
#else
    // Non-Windows: no universal debugger output window. Report Disabled so the
    // backend does not count a delivery here.
    return SinkStatus::Disabled;
#endif
}

SinkStatus DebuggerSink::Flush() noexcept
{
    // Debugger output is unbuffered from our side; nothing to flush.
    return SinkStatus::Ok;
}

} // namespace ludus::foundation::logging::internal
