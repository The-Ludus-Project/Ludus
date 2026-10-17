#include "internal/diagnostic_format.hpp"
#include <ludus/foundation/base/assert_format.hpp>

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string_view>

using namespace ludus::foundation;
using namespace ludus::foundation::diagnostics;

namespace
{
struct Report
{
    char Bytes[2048]{};
    template <usize N, typename... Args>
    std::string_view Format(const char (&format)[N], const Args&... args)
    {
        const detail::DiagnosticArg packed[] = {detail::MakeDiagnosticArg(args)..., detail::MakeDiagnosticArg(false)};
        internal::TextWriter writer(Bytes, sizeof(Bytes));
        internal::FormatDiagnostic(writer, {format, N}, packed, sizeof...(Args));
        return {Bytes, writer.Finish()};
    }
};
} // namespace

TEST_CASE("Diagnostic scalar set and exceptional floating values")
{
    Report report;
    CHECK(report.Format("{} {}", std::numeric_limits<int64>::min(), std::numeric_limits<uint64>::max()) ==
          "-9223372036854775808 18446744073709551615\n");
    CHECK(report.Format("{} {} {} {}",
                        std::numeric_limits<float64>::quiet_NaN(),
                        std::numeric_limits<float64>::infinity(),
                        -float64{0},
                        true) == "nan inf -0 true\n");
    CHECK(report.Format("{} {} {} {} {} {} {} {}",
                        int8{-1},
                        uint8{2},
                        int16{-3},
                        uint16{4},
                        int32{-5},
                        uint32{6},
                        int64{-7},
                        uint64{8}) == "-1 2 -3 4 -5 6 -7 8\n");
    CHECK(report.Format("{} {}", float32{1.5}, DiagnosticAddress(nullptr)) == "1.5 0x0\n");
}

TEST_CASE("Diagnostic text is explicit, bounded and escaped")
{
    Report report;
    CHECK(report.Format("{{{}}}", "text") == "{text}\n");
    CHECK(report.Format("{} {}", DiagnosticCString(nullptr), DiagnosticText{nullptr, 7}) == "<null> [invalid-text]\n");
    const char bytes[] = {'a', '\0', '\n', '\t', '\\', '\x1b'};
    CHECK(report.Format("{}", DiagnosticText{bytes, sizeof(bytes)}) == "a\\x00\\x0a\\x09\\x5c\\x1b\n");
    CHECK(report.Format("a\0b\n") == "a\\x00b\\x0a\n");
    char unterminated[3] = {'x', 'y', 'z'};
    CHECK(report.Format("{}", unterminated) == "xyz [truncated]\n");
    char text[1025]{};
    for (usize i = 0; i < 1024; ++i)
    {
        text[i] = 'x';
    }
    CHECK(report.Format("{}", DiagnosticText{text, 1024}).size() == 1025);
    CHECK(report.Format("{}", DiagnosticText{text, 1025}).ends_with(" [truncated]\n"));
    CHECK(report.Format("{}", DiagnosticCString(text)).ends_with(" [truncated]\n"));
}

TEST_CASE("Malformed formats never recurse and retain indexed arguments")
{
    Report report;
    CHECK(report.Format("{", 7).starts_with("[format-error]"));
    CHECK(report.Format("}", 7).starts_with("[format-error]"));
    CHECK(report.Format("{:x}", 7).starts_with("[format-error]"));
    CHECK(report.Format("{} {}", 7).starts_with("[format-error]"));
    CHECK(report.Format("", 7).starts_with("[format-error]"));
    CHECK(report.Format("bad {", 7).find("[0]=7") != std::string_view::npos);
    CHECK(report.Format("{}").starts_with("[format-error]"));
    const char no_terminator[] = {'{', '}'};
    CHECK(report.Format(no_terminator, 7).starts_with("[format-error]"));
    char oversized[2050]{};
    CHECK(report.Format(oversized).starts_with("[format-error]"));
}

TEST_CASE("Diagnostic output has exact bounds and atomic control escapes")
{
    char guarded[66]{};
    guarded[0] = 'L';
    guarded[65] = 'R';
    internal::TextWriter writer(guarded + 1, 64);
    for (int i = 0; i < 100; ++i)
    {
        writer.EscapedByte('\n');
    }
    const usize length = writer.Finish();
    CHECK(length < 64);
    CHECK(guarded[0] == 'L');
    CHECK(guarded[65] == 'R');
    CHECK(std::string_view(guarded + 1, length).ends_with("\\x0a [truncated]\n"));
}

TEST_CASE("Formatted macros retain plain evaluation semantics")
{
    int conditions = 0;
    int arguments = 0;
    struct Explicit
    {
        explicit operator bool() const
        {
            return true;
        }
    };
    LUDUS_REQUIRE_F(Explicit{}, "{}", ++arguments);
    LUDUS_ASSERT_F(true, "{}", ++arguments);
    CHECK(LUDUS_CHECK_F((++conditions, true), "{}", ++arguments));
    CHECK(conditions == 1);
    CHECK(arguments == 0);
    CHECK_FALSE(LUDUS_CHECK_F((++conditions, false), "{}", ++arguments));
    CHECK_FALSE(LUDUS_CHECK_F(false, "{} {}", 1));
    CHECK(conditions == 2);
    CHECK(arguments == 1);
    const bool choose_assert = conditions == 2;
    // Deliberately omit braces to exercise statement safety.
    // NOLINTBEGIN(readability-braces-around-statements)
    if (choose_assert)
        LUDUS_REQUIRE_F(true, "ok");
    else
        ++arguments;
    // NOLINTEND(readability-braces-around-statements)
#if !LUDUS_ENABLE_ASSERTS
    LUDUS_ASSERT_F(does_not_exist(), missing_format, missing_argument);
#endif
}

TEST_CASE("An invalid internal tag fails boundedly")
{
    char output[128];
    internal::TextWriter writer(output, sizeof(output));
    auto argument = detail::MakeDiagnosticArg(false);
    argument.Kind = static_cast<detail::DiagnosticArgKind>(255);
    internal::FormatDiagnostic(writer, {"{}", 3}, &argument, 1);
    CHECK(std::string_view(output, writer.Finish()) == "[invalid-argument]\n");
}
