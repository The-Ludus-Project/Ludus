#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/tooling/shader_language/diagnostic.hpp>

#include <string_view>

namespace ludus::tooling::shader_language::internal
{
/// Append as much of `text` as fits; diagnostic messages are clipped rather than rejected.
void AppendClipped(DiagnosticMessage& inoutMessage, std::string_view text) noexcept;
/// Append the decimal digits of `value`, clipped like AppendClipped.
void AppendUnsigned(DiagnosticMessage& inoutMessage, foundation::uint64 value) noexcept;

LUDUS_INLINE void AppendPart(DiagnosticMessage& inoutMessage, std::string_view text) noexcept
{
    AppendClipped(inoutMessage, text);
}

LUDUS_INLINE void AppendPart(DiagnosticMessage& inoutMessage, const char* text) noexcept
{
    AppendClipped(inoutMessage, std::string_view(text));
}

LUDUS_INLINE void AppendPart(DiagnosticMessage& inoutMessage, foundation::uint64 value) noexcept
{
    AppendUnsigned(inoutMessage, value);
}

/// @brief Build a message from string views and unsigned integers, in order.
template <typename... Parts>
[[nodiscard]] DiagnosticMessage ComposeMessage(const Parts&... parts) noexcept
{
    DiagnosticMessage message;
    (AppendPart(message, parts), ...);
    return message;
}

/// @brief Bounded collector shared by the stages of one request.
///
/// Keeps the first MAX_DIAGNOSTICS diagnostics and counts the rest. Errors are counted even when
/// dropped, so a request with only dropped errors still fails.
class DiagnosticSink final
{
public:
    /// Collect into `inoutDiagnostics` and `inoutDroppedCount`, which must outlive the sink.
    DiagnosticSink(foundation::Array<Diagnostic>& inoutDiagnostics, foundation::usize& inoutDroppedCount) noexcept;

    /// Record an error.
    void ReportError(DiagnosticCode code,
                     const SourceLocation& location,
                     const SourceLocation& expansionLocation,
                     const DiagnosticMessage& message) noexcept;

    /// Record a diagnostic of `level` with an optional fix; only errors fail the request.
    void Report(Severity level,
                DiagnosticCode code,
                const SourceLocation& location,
                const SourceLocation& expansionLocation,
                const DiagnosticMessage& message,
                const DiagnosticFix& fix) noexcept;

    /// True once any error was reported, including dropped ones.
    [[nodiscard]] bool HasErrors() const noexcept
    {
        return mErrorCount != 0;
    }

    /// True once storing a diagnostic failed for lack of memory.
    [[nodiscard]] bool HasOutOfMemory() const noexcept
    {
        return mHasOutOfMemory;
    }

    /// Record that an allocation outside the sink failed.
    void ReportOutOfMemory() noexcept
    {
        mHasOutOfMemory = true;
    }

private:
    foundation::Array<Diagnostic>& mDiagnostics;
    foundation::usize& mDroppedCount;
    foundation::usize mErrorCount = 0;
    bool mHasOutOfMemory = false;
};
} // namespace ludus::tooling::shader_language::internal
