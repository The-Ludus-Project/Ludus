#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/diagnostic.hpp>
#include <ludus/foundation/base/diagnostic_output.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent(void);
#endif

namespace ludus::foundation::base
{

namespace
{

// Per-thread reentry guard. Set for the duration of a report; a nested call sees
// it set, emits a minimal literal, and returns without recursing (requirements
// R32). thread_local of a trivial type has no throwing initialization.
thread_local bool tInEmergency = false;

std::atomic<uint64> gReentryCount{0};

[[nodiscard]] std::string_view SeverityLabel(DiagnosticSeverity severity) noexcept
{
    switch (severity)
    {
        case DiagnosticSeverity::Note:
            return "Note";
        case DiagnosticSeverity::Warning:
            return "Warning";
        case DiagnosticSeverity::Error:
            return "Error";
        case DiagnosticSeverity::Fatal:
            return "Fatal";
    }
    return "Unknown";
}

// Append into a fixed buffer, never overflowing. Updates `length`, always leaves
// the buffer NUL-terminated, and reports whether the text fully fit.
bool Append(std::array<char, 512>& buffer, usize& length, std::string_view text) noexcept
{
    const usize capacity = buffer.size() - 1; // reserve terminator
    const usize remaining = capacity - length;
    const usize count = text.size() < remaining ? text.size() : remaining;
    if (count > 0)
    {
        std::memcpy(buffer.data() + length, text.data(), count);
        length += count;
    }
    buffer[length] = '\0';
    return count == text.size();
}

} // namespace

// The two string_view parameters (category name, then message) mirror the
// logging call convention and are always passed as named/literal arguments at
// the two call sites; swapping them is not a realistic hazard here.
void EmergencyReport(DiagnosticSeverity severity,
                     std::string_view categoryName, // NOLINT(bugprone-easily-swappable-parameters)
                     std::string_view message,
                     const std::source_location& location) noexcept
{
    if (tInEmergency)
    {
        // Nested failure while already reporting: do not recurse. Emit a minimal,
        // fixed literal and count it (requirements R32).
        gReentryCount.fetch_add(1, std::memory_order_relaxed);
        static constexpr char kNested[] = "[LUDUS:Diagnostic] nested emergency suppressed\n";
        (void)diagnostics::WriteEmergencyBytes(kNested, sizeof(kNested) - 1);
        return;
    }
    tInEmergency = true;

    std::array<char, 512> buffer{};
    usize length = 0;
    bool fit = true;

    fit = Append(buffer, length, "[LUDUS:") && fit;
    fit = Append(buffer, length, SeverityLabel(severity)) && fit;
    fit = Append(buffer, length, "] [") && fit;
    fit = Append(buffer, length, categoryName.empty() ? std::string_view{"Diagnostic"} : categoryName) && fit;
    fit = Append(buffer, length, "] ") && fit;
    fit = Append(buffer, length, message) && fit;

    fit = Append(buffer, length, " (") && fit;
    fit = Append(buffer, length, location.file_name()) && fit;
    fit = Append(buffer, length, ":") && fit;
    std::array<char, 16> lineDigits{};
    const int written =
        std::snprintf(lineDigits.data(), lineDigits.size(), "%u", static_cast<unsigned>(location.line()));
    if (written > 0)
    {
        fit = Append(buffer, length, std::string_view(lineDigits.data(), static_cast<usize>(written))) && fit;
    }
    fit = Append(buffer, length, ")") && fit;

    // Mark truncation explicitly rather than silently losing content
    // (requirements R31; fixes the F8-adjacent silent-truncation gap).
    if (fit == false)
    {
        // Overwrite the tail with a truncation marker if there is room; otherwise
        // the reader still sees a well-formed (if clipped) line.
        static constexpr std::string_view kMark = "[...]";
        if (length + kMark.size() < buffer.size())
        {
            std::memcpy(buffer.data() + length, kMark.data(), kMark.size());
            length += kMark.size();
        }
    }
    if (length < buffer.size() - 1)
    {
        buffer[length++] = '\n';
    }

    // Share Base byte transport without entering normal Logging. Its stderr
    // fallback can block; assertions use only TryWriteEmergencyBytes instead.
    (void)diagnostics::WriteEmergencyBytes(buffer.data(), length);

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    if (IsDebuggerPresent())
    {
        OutputDebugStringA(buffer.data());
    }
#endif

    tInEmergency = false;
}

void EmergencyNote(std::string_view message, const std::source_location& location) noexcept
{
    EmergencyReport(DiagnosticSeverity::Warning, "Diagnostic", message, location);
}

uint64 EmergencyReentryCount() noexcept
{
    return gReentryCount.load(std::memory_order_relaxed);
}

} // namespace ludus::foundation::base
