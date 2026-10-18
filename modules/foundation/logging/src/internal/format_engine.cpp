#include "internal/format_engine.hpp"

#include <charconv>
#include <cstring>

namespace ludus::foundation::logging::internal
{

namespace
{

// Bounded write cursor over the caller-owned output buffer. Every put() clamps
// to the remaining capacity and reports whether it fully fit, so the formatter
// can flag truncation without ever writing out of bounds.
struct Cursor
{
    char* p;
    char* end;

    bool Put(char c) noexcept
    {
        if (p < end)
        {
            *p++ = c;
            return true;
        }
        return false;
    }

    bool Put(std::string_view s) noexcept
    {
        const usize room = static_cast<usize>(end - p);
        const usize n = s.size() < room ? s.size() : room;
        if (n > 0)
        {
            std::memcpy(p, s.data(), n);
            p += n;
        }
        return n == s.size();
    }

    template <typename T>
    bool PutInt(T v, int base = 10) noexcept
    {
        char buffer[32];
        const std::to_chars_result r = std::to_chars(buffer, buffer + sizeof(buffer), v, base);
        if (r.ec != std::errc())
        {
            return false;
        }
        return Put(std::string_view(buffer, static_cast<usize>(r.ptr - buffer)));
    }

    bool PutFloat(float64 v) noexcept
    {
        char buffer[64];
        const std::to_chars_result r = std::to_chars(buffer, buffer + sizeof(buffer), v);
        if (r.ec != std::errc())
        {
            return false;
        }
        return Put(std::string_view(buffer, static_cast<usize>(r.ptr - buffer)));
    }
};

// Emit one argument, honoring the (small) parsed specifier. Returns false only
// on a would-truncate; unsupported specifier/arg combinations are reported as a
// FormatError by the caller before reaching here.
bool EmitArg(Cursor& cursor, const FormatArg& arg, bool hex) noexcept
{
    switch (arg.Tag)
    {
        case ArgTag::StrView:
            return cursor.Put(arg.S);
        case ArgTag::Bool:
            return cursor.Put(arg.B ? std::string_view{"true"} : std::string_view{"false"});
        case ArgTag::I64:
            return hex ? cursor.PutInt(static_cast<uint64>(arg.I), 16) : cursor.PutInt(arg.I);
        case ArgTag::U64:
            return hex ? cursor.PutInt(arg.U, 16) : cursor.PutInt(arg.U);
        case ArgTag::Ptr: {
            const bool ok = cursor.Put(std::string_view{"0x"});
            // usize (== size_t) has pointer width on all Ludus-supported 64-bit
            // targets; avoids pulling <cstdint> for uintptr_t (ADR 0003 keeps
            // <cstdint> in types.h only).
            return cursor.PutInt(static_cast<uint64>(reinterpret_cast<usize>(arg.P)), 16) && ok;
        }
        case ArgTag::F64:
            return cursor.PutFloat(arg.D);
    }
    return false;
}

} // namespace

FormatOutcome FormatInto(std::span<char> out, std::string_view fmt, std::span<const FormatArg> args) noexcept
{
    Cursor cursor{out.data(), out.data() + out.size()};
    bool truncated = false;
    bool error = false;
    usize argIndex = 0;

    for (usize i = 0; i < fmt.size(); ++i)
    {
        const char ch = fmt[i];
        if (ch == '{')
        {
            // "{{" is a literal '{'.
            if (i + 1 < fmt.size() && fmt[i + 1] == '{')
            {
                truncated = cursor.Put('{') == false || truncated;
                ++i;
                continue;
            }

            // Parse the replacement field: '{' [':' spec] '}'. Only ":x" (hex
            // for integer args) is supported; anything else is a FormatError.
            bool hex = false;
            usize j = i + 1;
            if (j < fmt.size() && fmt[j] == ':')
            {
                ++j;
                if (j < fmt.size() && fmt[j] == 'x')
                {
                    hex = true;
                    ++j;
                }
                else
                {
                    error = true;
                }
            }
            if (j >= fmt.size() || fmt[j] != '}')
            {
                error = true;
                truncated = cursor.Put('{') == false || truncated;
                continue;
            }
            if (argIndex >= args.size())
            {
                error = true; // more fields than arguments
            }
            else
            {
                truncated = EmitArg(cursor, args[argIndex], hex) == false || truncated;
                ++argIndex;
            }
            i = j;
        }
        else if (ch == '}')
        {
            // "}}" is a literal '}'.
            if (i + 1 < fmt.size() && fmt[i + 1] == '}')
            {
                truncated = cursor.Put('}') == false || truncated;
                ++i;
                continue;
            }
            error = true; // stray '}'
            truncated = cursor.Put('}') == false || truncated;
        }
        else
        {
            truncated = cursor.Put(ch) == false || truncated;
        }
    }

    // Fewer fields than arguments is also a (mild) format error.
    if (argIndex != args.size())
    {
        error = true;
    }

    return FormatOutcome{static_cast<usize>(cursor.p - out.data()), truncated, error};
}

} // namespace ludus::foundation::logging::internal
