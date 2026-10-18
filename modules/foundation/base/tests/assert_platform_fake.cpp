// Linked only into an isolated test executable. The runtime is the real source;
// inspection returns like debugger continuation, but termination still aborts.
#include "internal/diagnostic_platform.hpp"
#include "internal/diagnostic_record.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

// Deterministic closed-pipe cleanup interruption. The --wrap ABI requires the
// reserved names, confined to this isolated test executable.
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" int __real_sigtimedwait(const sigset_t*, siginfo_t*, const timespec*);
extern "C" int __wrap_sigtimedwait(const sigset_t* set, siginfo_t* info, const timespec* timeout)
{
    static int interruptions = 0;
    if (interruptions++ < 8)
    {
        errno = EINTR;
        return -1;
    }
    return __real_sigtimedwait(set, info, timeout);
}
extern "C" ludus::foundation::isize __real_send(int, const void*, ludus::foundation::usize, int);
extern "C" ludus::foundation::isize __wrap_send(int fd, const void* data, ludus::foundation::usize size, int flags)
{
    const char* fault = std::getenv("LUDUS_TEST_SEND_FAULT");
    if (fault != nullptr)
    {
        if (std::strcmp(fault, "short") == 0)
        {
            return 1;
        }
        if (std::strcmp(fault, "eintr") == 0)
        {
            errno = EINTR;
            return -1;
        }
        if (std::strcmp(fault, "eagain") == 0)
        {
            errno = EAGAIN;
            return -1;
        }
        if (std::strcmp(fault, "epipe") == 0)
        {
            errno = EPIPE;
            return -1;
        }
    }
    return __real_send(fd, data, size, flags);
}
// NOLINTEND(bugprone-reserved-identifier)

namespace ludus::foundation::diagnostics::internal
{
uint64 NativeThreadId() noexcept
{
    return 17;
}
DebuggerState QueryDebugger() noexcept
{
    return DebuggerState::Attached;
}
void BreakForDebugger() noexcept
{
    constexpr char marker[] = "INSPECTION-CONTINUED\n";
    (void)::write(STDOUT_FILENO, marker, sizeof(marker) - 1);
}
[[noreturn]] void TerminateForAssertion() noexcept
{
    const auto& packet = gFatalPacket;
    if (packet.MinimalReady.load(std::memory_order_acquire) == false ||
        packet.CompleteReady.load(std::memory_order_acquire) == false ||
        packet.DeliveryReady.load(std::memory_order_acquire) == false || packet.Complete[packet.CompleteSize] != '\0')
    {
        std::_Exit(75);
    }
    if (std::getenv("LUDUS_TEST_SEND_FAULT") != nullptr &&
        (packet.MinimalDelivery != DeliveryStatus::Failed || packet.CompleteDelivery != DeliveryStatus::Failed))
    {
        std::_Exit(70);
    }
    std::abort();
}
[[noreturn]] void TerminateImmediately() noexcept
{
    const auto& packet = gFatalPacket;
    if (packet.MinimalReady.load(std::memory_order_acquire) &&
        (packet.CompleteReady.load(std::memory_order_acquire) ||
         std::strstr(packet.Minimal, "[LUDUS:FATAL]") == nullptr))
    {
        std::_Exit(68);
    }
    std::_Exit(134);
}
} // namespace ludus::foundation::diagnostics::internal
