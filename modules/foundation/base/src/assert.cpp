#include <ludus/foundation/base/assert.hpp>

#include <ludus/foundation/base/build_metadata.hpp>
#include <ludus/foundation/base/diagnostic_output.hpp>

#include "internal/diagnostic_finish.hpp"
#include "internal/diagnostic_platform.hpp"
#include "internal/diagnostic_record.hpp"
#include "internal/diagnostic_text.hpp"

#include <atomic>
#include <string_view>

namespace ludus::foundation::diagnostics::internal
{
constinit FatalPacket gFatalPacket{};
} // namespace ludus::foundation::diagnostics::internal

namespace ludus::foundation::diagnostics
{
namespace
{
using internal::TextWriter;
struct Entry
{
    AssertionSite Site{};
    uint64 ThreadId = 0;
    FailureKind Kind = FailureKind::Fatal;
    bool Active = false;
    bool LastCheck = false;
};

constinit thread_local Entry gEntry{};
constinit std::atomic_flag gOwner = ATOMIC_FLAG_INIT;
constinit uint32 gCheckReports = 0; // Access only while owning gOwner.
constexpr uint32 CHECK_REPORT_LIMIT = 64;

const char* KindName(FailureKind kind) noexcept
{
    switch (kind)
    {
        case FailureKind::Assert:
            return "ASSERT";
        case FailureKind::Require:
            return "REQUIRE";
        case FailureKind::Check:
            return "CHECK";
        case FailureKind::Fatal:
            return "FATAL";
    }
    return "invalid-kind";
}

const char* FlavorName() noexcept
{
    switch (LUDUS_BUILD_FLAVOR_ID)
    {
        case 1:
            return "Debug";
        case 2:
            return "Development";
        case 3:
            return "Profile";
        case 4:
            return "Release";
        default:
            return "unknown";
    }
}

void SiteText(TextWriter& writer, const Entry& entry, bool minimal = false) noexcept
{
    writer.Raw("[LUDUS:");
    writer.Raw(KindName(entry.Kind));
    writer.Raw("] thread=");
    writer.Number(entry.ThreadId);
    writer.Raw(" site=");
    writer.Text(entry.Site.File, minimal ? 160 : 480);
    writer.Byte(':');
    writer.Number(entry.Site.Line);
    writer.Raw(" expression=");
    writer.Text(entry.Site.Expression == nullptr ? "<unconditional>" : entry.Site.Expression, minimal ? 160 : 480);
}

usize CompleteReport(char* buffer, usize capacity, const char* message, const DiagnosticText* rendered) noexcept
{
    TextWriter writer(buffer, capacity);
    writer.Raw("[LUDUS assertion report v1] revision=");
    // The revision is a string_view, so bound it by size rather than a terminator.
    constexpr usize REVISION_QUOTA = 64;
    const std::string_view revision = build_metadata::git_revision.substr(0, REVISION_QUOTA);
    writer.Counted(revision.data(), revision.size());
    writer.Raw(" flavor=");
    writer.Raw(FlavorName());
    writer.Raw(" asserts=");
    writer.Number(LUDUS_ENABLE_ASSERTS);
    writer.Raw(" policy-version=");
    writer.Number(LUDUS_ASSERT_POLICY_VERSION);
    writer.Raw(" check-break=");
    writer.Number(LUDUS_BREAK_ON_CHECK);
    writer.Byte('\n');
    SiteText(writer, gEntry);
    writer.Raw("\nfunction=");
    writer.Text(gEntry.Site.Function, 128);
    writer.Raw("\ncapture=stack-unavailable,artifact-id-unavailable; delivery=nonblocking-best-effort");
    if (gEntry.LastCheck)
    {
        writer.Raw("\n[check report limit reached; later detail suppressed]");
    }
    writer.Raw("\nmessage=");
    if (rendered != nullptr)
    {
        writer.Rendered(rendered->Data, rendered->Size);
    }
    else if (message != nullptr)
    {
        writer.Text(message);
    }
    return writer.Finish();
}

[[noreturn]] void Compromised(const char* marker, usize size) noexcept
{
    (void)TryWriteEmergencyBytes(marker, size);
    internal::TerminateImmediately();
}

void InspectIfAttached() noexcept
{
    if (internal::QueryDebugger() == internal::DebuggerState::Attached)
    {
        internal::BreakForDebugger();
    }
}

// Per-process incident id for the control decision exchange. Only ever touched
// while owning the reporting slot, so a plain counter is sufficient.
constinit uint32 gAssertIncident = 0;

// Resolve a failed enabled ASSERT to Continue-once or Terminate. This is the only
// place the resumable path decides to return. It never runs on success, never on
// a recursive/contended incident (those terminate before reaching here), and
// never for REQUIRE/FATAL.
//
//   * LUDUS_ASSERT_DIALOGS_AVAILABLE == 0 (Development/Profile/Release, or a
//     CI-built Debug binary)                      -> Terminate (report-only).
//   * Runtime CI veto: IsContinuousIntegration()  -> Terminate, even for a
//     locally built dialog-capable Debug binary run under CI.
//   * Debugger attached: break; a debugger Continue returns here             ->
//     ContinueOnce (no dialog when a debugger is present).
//   * No debugger, control endpoint Ready: ask the external helper. Anything but
//     an explicit ContinueOnce for this incident                            ->
//     Terminate. No control endpoint -> Terminate (no default-continue).
ControlDecision ResolveAssertDecision(uint32 incident, const char* report, usize size) noexcept
{
    // Runtime CI veto first: CI performs no interaction and no intentional
    // debugger stop, even for a locally built dialog-capable Debug binary.
    if (IsContinuousIntegration())
    {
        return ControlDecision::Terminate;
    }
    // With a debugger attached (Debug or Development, non-CI), the inspection
    // break IS the resume point: a debugger Continue returns here and resumes
    // past the ASSERT. This is not gated by dialog availability — only the
    // no-debugger dialog below is Debug-only.
    if (internal::QueryDebugger() == internal::DebuggerState::Attached)
    {
        internal::BreakForDebugger(); // Returns iff the developer continues.
        return ControlDecision::ContinueOnce;
    }
#if LUDUS_ASSERT_DIALOGS_AVAILABLE
    // No debugger: only an eligible non-CI Debug build with a live control
    // channel may ask the external helper for an explicit decision.
    if (ControlEndpointState() == ControlState::Ready)
    {
        return RequestAssertDecision(incident, report, size);
    }
#else
    (void)incident;
    (void)report;
    (void)size;
#endif
    // Development without a debugger, or Debug with no usable dialog channel:
    // report-only, terminate. Never a default continue.
    return ControlDecision::Terminate;
}
} // namespace

namespace detail
{
void BeginFatal(FailureKind kind, const AssertionSite& site) noexcept
{
    if (gEntry.Active)
    {
        constexpr char marker[] = "[LUDUS recursive fatal; immediate termination]\n";
        Compromised(marker, sizeof(marker) - 1);
    }
    gEntry.Active = true;
    gEntry.Kind = kind;
    gEntry.Site = site;
    gEntry.ThreadId = internal::NativeThreadId();
    if (gOwner.test_and_set(std::memory_order_acquire))
    {
        char secondary[512];
        TextWriter writer(secondary, sizeof(secondary));
        writer.Raw("[secondary fatal; immediate termination] ");
        SiteText(writer, gEntry, true);
        Compromised(secondary, writer.Finish());
    }
    auto& packet = internal::gFatalPacket;
    TextWriter writer(packet.Minimal, sizeof(packet.Minimal));
    SiteText(writer, gEntry, true);
    packet.MinimalSize = writer.Finish();
    packet.MinimalReady.store(true, std::memory_order_release);
    packet.MinimalDelivery = TryWriteEmergencyBytes(packet.Minimal, packet.MinimalSize);
}

bool BeginCheck(const AssertionSite& site) noexcept
{
    if (gEntry.Active)
    {
        return false; // Do not clear the enclosing failure's state.
    }
    gEntry.Active = true;
    if (gOwner.test_and_set(std::memory_order_acquire))
    {
        gEntry.Active = false;
        return false;
    }
    if (gCheckReports == CHECK_REPORT_LIMIT)
    {
        gEntry.Active = false;
        gOwner.clear(std::memory_order_release);
        return false;
    }
    gEntry.Site = site;
    gEntry.Kind = FailureKind::Check;
    gEntry.ThreadId = internal::NativeThreadId();
    gEntry.LastCheck = ++gCheckReports == CHECK_REPORT_LIMIT;
    return true;
}

// Resumable ASSERT entry. Recursion/contention are never resumable: they take
// the same immediate/secondary termination path as a fatal, preserving the first
// incident's evidence. On success it owns the reporting slot and records the site
// but, unlike BeginFatal, does NOT publish the terminal fatal packet: a resumable
// ASSERT report is stack-owned and only reaches gFatalPacket if it terminates.
void BeginAssert(const AssertionSite& site) noexcept
{
    if (gEntry.Active)
    {
        constexpr char marker[] = "[LUDUS recursive assert; immediate termination]\n";
        Compromised(marker, sizeof(marker) - 1);
    }
    gEntry.Active = true;
    gEntry.Kind = FailureKind::Assert;
    gEntry.Site = site;
    gEntry.ThreadId = internal::NativeThreadId();
    if (gOwner.test_and_set(std::memory_order_acquire))
    {
        char secondary[512];
        TextWriter writer(secondary, sizeof(secondary));
        writer.Raw("[secondary assert; immediate termination] ");
        SiteText(writer, gEntry, true);
        Compromised(secondary, writer.Finish());
    }
    gAssertIncident += 1;
}

[[noreturn]] static void FinishFatalImpl(const char* message, const DiagnosticText* rendered) noexcept
{
    if (!gEntry.Active || gEntry.Kind == FailureKind::Check)
    {
        constexpr char marker[] = "[LUDUS invalid fatal protocol]\n";
        Compromised(marker, sizeof(marker) - 1);
    }
    auto& packet = internal::gFatalPacket;
    packet.CompleteSize = CompleteReport(packet.Complete, sizeof(packet.Complete), message, rendered);
    packet.CompleteReady.store(true, std::memory_order_release);
    packet.CompleteDelivery = TryWriteEmergencyBytes(packet.Complete, packet.CompleteSize);
    packet.DeliveryReady.store(true, std::memory_order_release);
    InspectIfAttached();
    internal::TerminateForAssertion(); // Debugger continuation never escapes.
}

static bool FinishCheckImpl(const char* message, const DiagnosticText* rendered) noexcept
{
    if (!gEntry.Active || gEntry.Kind != FailureKind::Check)
    {
        constexpr char marker[] = "[LUDUS invalid Check protocol]\n";
        Compromised(marker, sizeof(marker) - 1);
    }
    char report[2048];
    const usize size = CompleteReport(report, sizeof(report), message, rendered);
    (void)TryWriteEmergencyBytes(report, size);
    // CHECK stays boolean and keeps its budget; the inspection break is suppressed
    // under CI (no intentional debugger stop in CI), matching the ASSERT/dialog
    // policy. CHECK never opens a dialog and never waits.
    if (LUDUS_BREAK_ON_CHECK != 0 && !IsContinuousIntegration())
    {
        InspectIfAttached();
    }
    gEntry = {};
    gOwner.clear(std::memory_order_release);
    return false;
}

// Resumable ASSERT finish. Builds a stack-owned report and delivers it (report
// visibility is independent of Logging). Then resolves the explicit developer
// decision. On ContinueOnce it releases the slot and clears TLS exactly once and
// RETURNS to the caller, so a later failure is independently reportable; the
// invariant is now known-broken. On Terminate it publishes terminal evidence into
// the fatal packet from the already-owned incident — without re-evaluating any
// diagnostic expression — and aborts. It never reuses the fatal packet on the
// continue path.
static void FinishAssertImpl(const char* message, const DiagnosticText* rendered) noexcept
{
    if (!gEntry.Active || gEntry.Kind != FailureKind::Assert)
    {
        constexpr char marker[] = "[LUDUS invalid assert protocol]\n";
        Compromised(marker, sizeof(marker) - 1);
    }
    char report[2048];
    const usize size = CompleteReport(report, sizeof(report), message, rendered);
    const DeliveryStatus delivery = TryWriteEmergencyBytes(report, size);

    // A resumed ASSERT is never budgeted: unlike a hot CHECK it cannot flood
    // autonomously, because each hit blocks on an explicit human/debugger action
    // (see the interactive-development plan). A per-site "ignore always" is
    // deliberately absent, and a budget could only silently continue (forbidden)
    // or terminate a legitimate later ASSERT.
    if (ResolveAssertDecision(gAssertIncident, report, size) == ControlDecision::ContinueOnce)
    {
        // Explicit developer continue: skip this assertion once. Release the slot
        // and clear TLS exactly once so the next failure owns cleanly.
        gEntry = {};
        gOwner.clear(std::memory_order_release);
        return;
    }

    // Terminate: publish the already-built report as terminal evidence, without
    // re-evaluating anything, then abort. The owned slot is intentionally never
    // released on this path. Delivery status reflects the real emergency write.
    auto& packet = internal::gFatalPacket;
    TextWriter minimal(packet.Minimal, sizeof(packet.Minimal));
    SiteText(minimal, gEntry, true);
    packet.MinimalSize = minimal.Finish();
    packet.MinimalDelivery = delivery;
    packet.MinimalReady.store(true, std::memory_order_release);
    for (usize i = 0; i < size && i < sizeof(packet.Complete) - 1; ++i)
    {
        packet.Complete[i] = report[i];
    }
    packet.CompleteSize = size < sizeof(packet.Complete) - 1 ? size : sizeof(packet.Complete) - 1;
    packet.Complete[packet.CompleteSize] = '\0';
    packet.CompleteDelivery = delivery;
    packet.CompleteReady.store(true, std::memory_order_release);
    packet.DeliveryReady.store(true, std::memory_order_release);
    internal::TerminateForAssertion();
}
[[noreturn]] void FinishFatal(const char* message) noexcept
{
    FinishFatalImpl(message, nullptr);
}
void FinishAssert(const char* message) noexcept
{
    FinishAssertImpl(message, nullptr);
}
bool FinishCheck(const char* message) noexcept
{
    return FinishCheckImpl(message, nullptr);
}
[[noreturn]] void FinishFatalRendered(DiagnosticText message) noexcept
{
    FinishFatalImpl(nullptr, &message);
}
void FinishAssertRendered(DiagnosticText message) noexcept
{
    FinishAssertImpl(nullptr, &message);
}
bool FinishCheckRendered(DiagnosticText message) noexcept
{
    return FinishCheckImpl(nullptr, &message);
}
} // namespace detail
} // namespace ludus::foundation::diagnostics
