#include "internal/diagnostic_record.hpp"

#include <ludus/foundation/base/assert_format.hpp>
#include <ludus/foundation/base/diagnostic_output.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>

#include <windows.h>

namespace
{
void CaptureTerminalReport(int) noexcept
{
    using namespace ludus::foundation::diagnostics;
    const internal::FatalPacket& packet = internal::gFatalPacket;
    if (packet.CompleteReady.load(std::memory_order_acquire) == true &&
        packet.DeliveryReady.load(std::memory_order_acquire) == true &&
        packet.CompleteDelivery == DeliveryStatus::Unavailable)
    {
        // Expose the debugger packet to the test driver. The production fatal
        // path deliberately avoids blocking stderr when transport is unavailable.
        (void)WriteEmergencyBytes(packet.Complete, packet.CompleteSize);
    }
}

void MarkAfterExit() noexcept
{
    constexpr char MARKER[] = "unexpected normal exit\n";
    (void)ludus::foundation::diagnostics::WriteEmergencyBytes(MARKER, sizeof(MARKER) - 1);
}
} // namespace

int main()
{
    // This subprocess checks the engine report and exit, independently of WER.
    (void)SetErrorMode(SEM_NOGPFAULTERRORBOX);
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (std::signal(SIGABRT, CaptureTerminalReport) == SIG_ERR || std::atexit(MarkAfterExit) != 0)
    {
        return 1;
    }
    LUDUS_REQUIRE_F(false, "Windows terminal diagnostic {}", 42);
}
