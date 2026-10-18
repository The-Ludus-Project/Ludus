#include "internal/diagnostic_format.hpp"
#include "internal/diagnostic_finish.hpp"

#include <charconv>

namespace ludus::foundation::diagnostics::detail
{
DiagnosticArg MakeCStringArg(const char* value, usize bound) noexcept
{
    usize size = 0;
    bool truncated = false;
    if (value != nullptr)
    {
        const usize limit = bound < 1024 ? bound : 1024;
        while (size < limit && value[size] != '\0')
        {
            ++size;
        }
        // Retain the actual readable extent, including short unterminated arrays.
        truncated = size == limit;
    }
    return {DiagnosticArgKind::Text, { .Text = {value, size} }, truncated};
}
} // namespace ludus::foundation::diagnostics::detail

namespace ludus::foundation::diagnostics::internal
{
namespace
{
void Argument(TextWriter& writer, const detail::DiagnosticArg& argument) noexcept
{
    using detail::DiagnosticArgKind;
    if (argument.Truncated)
    {
        writer.Truncate();
    }
    char number[64];
    std::to_chars_result result{};
    switch (argument.Kind)
    {
        case DiagnosticArgKind::Signed:
            result = std::to_chars(number, number + sizeof(number), argument.Value.Signed);
            break;
        case DiagnosticArgKind::Unsigned:
            result = std::to_chars(number, number + sizeof(number), argument.Value.Unsigned);
            break;
        case DiagnosticArgKind::Floating:
            result = std::to_chars(number, number + sizeof(number), argument.Value.Floating);
            break;
        case DiagnosticArgKind::Boolean:
            writer.Raw(argument.Value.Boolean ? "true" : "false");
            return;
        case DiagnosticArgKind::Text:
            writer.Counted(argument.Value.Text.Data, argument.Value.Text.Size);
            return;
        case DiagnosticArgKind::Address:
            static_assert(sizeof(usize) >= sizeof(void*));
            writer.Raw("0x");
            result =
                std::to_chars(number, number + sizeof(number), reinterpret_cast<usize>(argument.Value.Address), 16);
            break;
        default:
            writer.Raw("[invalid-argument]");
            return;
    }
    if (result.ec != std::errc{})
    {
        writer.Raw("<conversion-error>");
        return;
    }
    for (const char* current = number; current != result.ptr; ++current)
    {
        writer.Byte(*current);
    }
}

bool Valid(DiagnosticText format, usize count) noexcept
{
    if (format.Data == nullptr || format.Size == 0 || format.Size > 2048 || count > 8 ||
        format.Data[format.Size - 1] != '\0')
    {
        return false;
    }
    usize fields = 0;
    for (usize i = 0; i + 1 < format.Size; ++i)
    {
        const char byte = format.Data[i];
        if (byte == '{' || byte == '}')
        {
            if (i + 2 >= format.Size)
            {
                return false;
            }
            const char next = format.Data[++i];
            if (byte == '{' && next == '}')
            {
                ++fields;
            }
            else if (byte != next)
            {
                return false;
            }
        }
    }
    return fields == count;
}
} // namespace

void FormatDiagnostic(TextWriter& writer,
                      DiagnosticText format,
                      const detail::DiagnosticArg* args,
                      usize count) noexcept
{
    if (Valid(format, count) == false || (args == nullptr && count != 0))
    {
        writer.Raw("[format-error] format=");
        // Bounded fallback, reserving room for indexed arguments when possible.
        if (format.Data == nullptr)
        {
            writer.Raw("<null>");
        }
        else
        {
            usize extent = format.Size < 256 ? format.Size : 256;
            if (extent != 0 && extent == format.Size && format.Data[extent - 1] == '\0')
            {
                --extent;
            }
            writer.Counted(format.Data, extent);
            if (format.Size > 256)
            {
                writer.Truncate();
            }
        }
        if (args != nullptr)
        {
            const usize limited = count < 8 ? count : 8;
            for (usize i = 0; i < limited; ++i)
            {
                writer.Raw(" [");
                writer.Number(i);
                writer.Raw("]=");
                Argument(writer, args[i]);
            }
        }
        return;
    }
    usize argument = 0;
    for (usize i = 0; i + 1 < format.Size; ++i)
    {
        const char byte = format.Data[i];
        if (byte == '{' && format.Data[i + 1] == '}')
        {
            Argument(writer, args[argument++]);
            ++i;
        }
        else
        {
            writer.EscapedByte(byte);
            if (byte == '{' || byte == '}')
            {
                ++i;
            }
        }
    }
}
} // namespace ludus::foundation::diagnostics::internal

namespace ludus::foundation::diagnostics::detail
{
[[noreturn]] void FinishFatalArgs(DiagnosticText format, const DiagnosticArg* args, usize count) noexcept
{
    char message[2048];
    internal::TextWriter writer(message, sizeof(message));
    internal::FormatDiagnostic(writer, format, args, count);
    const usize size = writer.Finish();
    FinishFatalRendered({message, size - 1});
}
void FinishAssertArgs(DiagnosticText format, const DiagnosticArg* args, usize count) noexcept
{
    char message[2048];
    internal::TextWriter writer(message, sizeof(message));
    internal::FormatDiagnostic(writer, format, args, count);
    const usize size = writer.Finish();
    FinishAssertRendered({message, size - 1});
}
bool FinishCheckArgs(DiagnosticText format, const DiagnosticArg* args, usize count) noexcept
{
    char message[2048];
    internal::TextWriter writer(message, sizeof(message));
    internal::FormatDiagnostic(writer, format, args, count);
    const usize size = writer.Finish();
    return FinishCheckRendered({message, size - 1});
}
} // namespace ludus::foundation::diagnostics::detail
