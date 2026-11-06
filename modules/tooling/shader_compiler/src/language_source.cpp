// LUSL sources in a compiler request (docs/architecture/shading-language.md#compiler-architecture):
// the request's `.lusl` Source is lowered with Ludus::ShaderLanguage, and Slang compiles the
// generated module in its place. Both compilers see the same immutable snapshot.

#include "language_source.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/tooling/shader_language/diagnostic.hpp>
#include <ludus/tooling/shader_language/emitter.hpp>

#include <cstring>

namespace ludus::tooling::shader_compiler::internal
{
namespace
{
using foundation::Array;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;
namespace language = shader_language;

constexpr std::string_view LANGUAGE_EXTENSION = ".lusl";
/// mFileIndices entry of a request file that the generated module replaces.
constexpr usize REPLACED_FILE = ~usize{0};
/// Digits of a displayed LUSL diagnostic code, as in `LUSL0504`.
constexpr usize DIAGNOSTIC_CODE_DIGITS = 4;
/// Decimal digits of the largest uint64.
constexpr usize MAX_DECIMAL_DIGITS = 20;
constexpr uint64 DECIMAL_BASE = 10;

[[nodiscard]] std::string_view GetText(const foundation::BorrowedView<const char>& view) noexcept
{
    return view.IsValid() == true ? std::string_view(view.GetDataOrNull(), view.GetSize()) : std::string_view{};
}

/// Bounded diagnostic text, clipped at MAX_DIAGNOSTIC_BYTES.
class DiagnosticText final
{
public:
    explicit DiagnosticText(Array<uint8>& outBytes) noexcept : mBytes(outBytes) {}

    void Append(std::string_view text) noexcept
    {
        for (const char character : text)
        {
            if (mBytes.GetSize() >= MAX_DIAGNOSTIC_BYTES || mBytes.TryAdd(static_cast<uint8>(character)) == false)
            {
                return;
            }
        }
    }

    void AppendUnsigned(uint64 value, usize minimumDigits = 1) noexcept
    {
        foundation::StaticArray<char, MAX_DECIMAL_DIGITS> digits{};
        usize count = 0;
        do
        {
            digits[MAX_DECIMAL_DIGITS - 1 - count] = static_cast<char>('0' + static_cast<char>(value % DECIMAL_BASE));
            value /= DECIMAL_BASE;
            ++count;
        } while ((value != 0 || count < minimumDigits) && count < MAX_DECIMAL_DIGITS);
        Append(std::string_view(digits.GetData() + (MAX_DECIMAL_DIGITS - count), count));
    }

private:
    Array<uint8>& mBytes;
};

/// Write `path(line,column): error LUSL0504: message` lines, the form Slang uses, so existing
/// consumers of compiler diagnostics read LUSL ones too. Errors only, or warnings only.
void WriteDiagnostics(const Request& request,
                      const Array<language::Diagnostic>& diagnostics,
                      language::Severity level,
                      Array<uint8>& outBytes) noexcept
{
    DiagnosticText text(outBytes);
    for (const language::Diagnostic& diagnostic : diagnostics)
    {
        if (diagnostic.Level != level)
        {
            continue;
        }
        const language::SourceLocation& location = diagnostic.Location;
        if (location.FileIndex < request.Files.GetSize())
        {
            text.Append(GetText(request.Files[location.FileIndex].Path));
            text.Append("(");
            text.AppendUnsigned(location.Line);
            text.Append(",");
            text.AppendUnsigned(location.Column);
            text.Append("): ");
        }
        else
        {
            text.Append(GetText(request.Source));
            text.Append(": ");
        }
        text.Append(level == language::Severity::Error ? "error LUSL" : "warning LUSL");
        text.AppendUnsigned(static_cast<uint64>(diagnostic.Code), DIAGNOSTIC_CODE_DIGITS);
        text.Append(": ");
        text.Append(diagnostic.Message.GetView());
        const language::SourceLocation& expansion = diagnostic.ExpansionLocation;
        if (expansion.FileIndex < request.Files.GetSize())
        {
            text.Append(" (in a macro used at ");
            text.Append(GetText(request.Files[expansion.FileIndex].Path));
            text.Append("(");
            text.AppendUnsigned(expansion.Line);
            text.Append("))");
        }
        text.Append("\n");
    }
}
} // namespace

bool IsLanguageSource(std::string_view source) noexcept
{
    return source.ends_with(LANGUAGE_EXTENSION) == true;
}

bool LanguageLowering::Lower(const Request& request, Result& outResult) noexcept
{
    language::PreprocessRequest source;
    source.Files = request.Files;
    source.Source = request.Source;
    source.Configuration = request.Configuration;
    language::LoweringOptions options;
    options.Check.Style = request.Style == LanguageStyle::Warn  ? language::StyleSetting::Warn
                          : request.Style == LanguageStyle::Off ? language::StyleSetting::Off
                                                                : language::StyleSetting::Strict;
    options.Emit.ShouldCheckBounds = request.ShouldCheckBounds;
    const language::Status outcome = language::LowerToSlang(source, options, mLowered);
    if (outcome != language::Status::Ready)
    {
        WriteDiagnostics(request, mLowered.Diagnostics, language::Severity::Error, outResult.Diagnostics);
        outResult.Outcome = outcome == language::Status::OutOfMemory      ? Status::OutOfMemory
                            : outcome == language::Status::InvalidRequest ? Status::InvalidRequest
                                                                          : Status::CompileFailed;
        return false;
    }
    if (mGeneratedPath.TryAssign(GetText(request.Source)) != foundation::StringStatus::Ok ||
        mGeneratedPath.TryAppend(language::GENERATED_SLANG_SUFFIX) != foundation::StringStatus::Ok ||
        foundation::filesystem::ValidPath(mGeneratedPath.GetView()) == false)
    {
        outResult.Outcome = Status::InvalidRequest;
        return false;
    }
    if (mFiles.TryEnsureCapacity(request.Files.GetSize() + 1) == false ||
        mFileIndices.TryEnsureCapacity(request.Files.GetSize()) == false)
    {
        outResult.Outcome = Status::OutOfMemory;
        return false;
    }
    for (usize i = 0; i < request.Files.GetSize(); ++i)
    {
        if (GetText(request.Files[i].Path) == mGeneratedPath.GetView())
        {
            mFileIndices.Add(REPLACED_FILE);
            continue;
        }
        mFileIndices.Add(mFiles.GetSize());
        mFiles.Add(request.Files[i]);
    }
    SourceFile generated;
    generated.Path = foundation::BorrowedView<const char>(mGeneratedPath.GetData(), mGeneratedPath.GetSize());
    generated.Text = foundation::BorrowedView<const char>(mLowered.Text.GetData(), mLowered.Text.GetSize());
    mFiles.Add(generated);
    mRequest = request;
    mRequest.Files = foundation::BorrowedView<const SourceFile>(mFiles.GetData(), mFiles.GetSize());
    mRequest.Source = generated.Path;
    mRequest.Configuration = {};
    return true;
}

void LanguageLowering::Finish(const Request& request, Result& inoutResult) noexcept
{
    Array<uint8> diagnostics;
    WriteDiagnostics(request, mLowered.Diagnostics, language::Severity::Warning, diagnostics);
    DiagnosticText text(diagnostics);
    text.Append(std::string_view(reinterpret_cast<const char*>(inoutResult.Diagnostics.GetData()),
                                 inoutResult.Diagnostics.GetSize()));
    inoutResult.Diagnostics = foundation::Move(diagnostics);
    if (inoutResult.Outcome != Status::Ready)
    {
        return;
    }
    Array<uint8> readMask;
    if (readMask.TryResize(request.Files.GetSize(), 0) == false ||
        inoutResult.GeneratedSource.TryResize(mLowered.Text.GetSize()) == false)
    {
        inoutResult = Result{};
        inoutResult.Identity = request.Identity;
        inoutResult.Outcome = Status::OutOfMemory;
        return;
    }
    for (usize i = 0; i < request.Files.GetSize(); ++i)
    {
        const usize compiled = mFileIndices[i];
        const bool isReadBySlang = compiled != REPLACED_FILE && compiled < inoutResult.SourceReadMask.GetSize() &&
                                   inoutResult.SourceReadMask[compiled] != 0;
        const bool isReadByLanguage = i < mLowered.SourceReadMask.GetSize() && mLowered.SourceReadMask[i] != 0;
        readMask[i] = isReadBySlang == true || isReadByLanguage == true ? 1 : 0;
    }
    inoutResult.SourceReadMask = foundation::Move(readMask);
    inoutResult.ConfigurationReadMask = foundation::Move(mLowered.ConfigurationReadMask);
    if (mLowered.Text.IsEmpty() == false)
    {
        std::memcpy(inoutResult.GeneratedSource.GetData(), mLowered.Text.GetData(), mLowered.Text.GetSize());
    }
}
} // namespace ludus::tooling::shader_compiler::internal
