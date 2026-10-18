// Resumable-ASSERT decision test child. Fresh native process, production
// exception policy. It configures the report + control transports from inherited
// descriptors (as the startup integration would), then exercises the no-debugger
// dialog path: a failed enabled ASSERT sends a DecisionRequest and resumes only
// on an explicit ContinueOnce reply from the controlled test helper.
//
// There is no debugger here (native /proc TracerPid), so ResolveAssertDecision
// takes the control-endpoint branch. The driver scripts the helper's replies.

#include <ludus/foundation/base/assert_format.hpp>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/base/diagnostic_output.hpp>
#include <ludus/foundation/base/parse_number.hpp>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>

namespace fb = ludus::foundation;
namespace fbd = ludus::foundation::diagnostics;

#if defined(__cpp_exceptions)
#    error "Decision test child must use production exception policy"
#endif

namespace
{
void Mark(const char* text)
{
    (void)::write(STDOUT_FILENO, text, std::strlen(text));
}

int gConditions = 0;

bool FailingCondition()
{
    ++gConditions;
    Mark("CONDITION\n");
    return false;
}

fb::int32 ReadFileDescriptor(const char* name) noexcept
{
    const char* valueOrNull = std::getenv(name);
    if (valueOrNull == nullptr)
    {
        return -1;
    }
    fb::uint64 parsedDescriptor = 0;
    fb::int32 descriptor = -1;
    if (fb::ParseUint64(valueOrNull, std::strlen(valueOrNull), parsedDescriptor) != fb::NumberParseStatus::Success ||
        fb::TryIntegerCast(parsedDescriptor, descriptor) == false)
    {
        return -1;
    }
    return descriptor;
}
} // namespace

int main(int argc, char** argv)
{
    // Configure transports exactly like the startup integration would. Report is
    // optional here; the control endpoint is what drives the decision.
    const fb::int32 reportDescriptor = ReadFileDescriptor("LUDUS_DIAGNOSTIC_REPORT_FD");
    if (reportDescriptor >= 0 && fbd::ConfigureEmergencySocket(reportDescriptor) == false)
    {
        return 79;
    }
    const fb::int32 controlDescriptor = ReadFileDescriptor("LUDUS_DIAGNOSTIC_CONTROL_FD");
    if (controlDescriptor >= 0 && fbd::ConfigureControlEndpoint(controlDescriptor) != fbd::ControlState::Ready)
    {
        // A missing/failed handshake is a valid scenario the driver may assert on.
        Mark("CONTROL-NOT-READY\n");
    }

    const char* mode = argc > 1 ? argv[1] : "single";

    if (std::strcmp(mode, "single") == 0)
    {
        // One failing ASSERT. Resumes iff the helper replied ContinueOnce.
        LUDUS_ASSERT(FailingCondition(), "decision-probe");
        Mark("ASSERT-RETURNED\n");
        return gConditions == 1 ? 0 : 60;
    }
    if (std::strcmp(mode, "repeated") == 0)
    {
        // Each failing ASSERT is an independent incident: the slot is released
        // after a resume, so a later ASSERT is reportable again.
        LUDUS_ASSERT(FailingCondition(), "first");
        Mark("FIRST-RETURNED\n");
        LUDUS_ASSERT(FailingCondition(), "second");
        Mark("SECOND-RETURNED\n");
        return gConditions == 2 ? 0 : 61;
    }
    if (std::strcmp(mode, "require") == 0)
    {
        // REQUIRE must terminate even if the helper would say ContinueOnce.
        LUDUS_REQUIRE(FailingCondition(), "require-must-terminate");
        Mark("REQUIRE-RETURNED\n"); // Must never print.
        return 62;
    }
    if (std::strcmp(mode, "fatal") == 0)
    {
        // FATAL is unconditional and terminal, regardless of the helper reply.
        LUDUS_FATAL("fatal-must-terminate");
    }
    if (std::strcmp(mode, "formatted") == 0)
    {
        // Formatted ASSERT_F resumes on ContinueOnce; the condition and the
        // typed argument are evaluated exactly once.
        const int index = 7;
        LUDUS_ASSERT_F(FailingCondition(), "formatted index={}", index);
        Mark("ASSERT-RETURNED\n");
        return gConditions == 1 ? 0 : 64;
    }
    if (std::strcmp(mode, "app-lock") == 0)
    {
        // A resumed ASSERT while the caller holds an application lock must return
        // normally without deadlock or self-recursion: the runtime never touches
        // the application's lock.
        std::mutex application_mutex;
        const std::lock_guard lock(application_mutex);
        LUDUS_ASSERT(FailingCondition(), "under-app-lock");
        Mark("ASSERT-RETURNED\n");
        return gConditions == 1 ? 0 : 65;
    }
    if (std::strcmp(mode, "check") == 0)
    {
        // CHECK stays boolean and returns false with a visible report; it never
        // opens a dialog or sends a DecisionRequest.
        const bool ok = LUDUS_CHECK(FailingCondition(), "check-recovery-probe");
        Mark(ok ? "CHECK-TRUE\n" : "CHECK-FALSE\n");
        return ok == false && gConditions == 1 ? 0 : 66;
    }

    Mark("UNKNOWN-MODE\n");
    return 63;
}
