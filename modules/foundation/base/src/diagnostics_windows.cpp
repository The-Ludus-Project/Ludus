#include "internal/diagnostic_platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>

// Thanks to Microsoft, "IsDebuggerPresent function (debugapi.h)" and
// "GetCurrentThreadId function (processthreadsapi.h)", Win32 API reference:
// https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-isdebuggerpresent
// https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getcurrentthreadid
// IsDebuggerPresent reports only a user-mode debugger of this process; any
// other state is Detached, matching the Linux/macOS process-local queries.
namespace ludus::foundation::diagnostics::internal
{
uint64 NativeThreadId() noexcept
{
    return static_cast<uint64>(::GetCurrentThreadId());
}
DebuggerState QueryDebugger() noexcept
{
    return ::IsDebuggerPresent() != 0 ? DebuggerState::Attached : DebuggerState::Detached;
}
void BreakForDebugger() noexcept
{
    __builtin_debugtrap();
}
[[noreturn]] void TerminateForAssertion() noexcept
{
    std::abort();
}
[[noreturn]] void TerminateImmediately() noexcept
{
    std::_Exit(134);
}
} // namespace ludus::foundation::diagnostics::internal
