// luslc: the LUSL command-line compiler (docs/modules/tooling/shader-language.md#luslc).
// It reads a bounded source snapshot from disk and runs the same Ludus::ShaderLanguage library
// that offline and live builds use; it adds no language behavior of its own.

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/tooling/shader_compiler/compiler.hpp>
#include <ludus/tooling/shader_language/formatter.hpp>
#include <ludus/tooling/shader_language/limits.hpp>
#include <ludus/tooling/shader_language/pipeline.hpp>

#include <cerrno>
#include <span>
#include <string_view>

#if defined(LUDUS_PLATFORM_WINDOWS)
#    include <fcntl.h>
#    include <io.h>
#else
#    include <unistd.h>
#endif

namespace
{
using namespace ludus::foundation;
namespace language = ludus::tooling::shader_language;
namespace compiler = ludus::tooling::shader_compiler;

/// Process exit codes.
constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;

constexpr int32 STANDARD_OUTPUT = 1;
constexpr int32 STANDARD_ERROR = 2;
#if defined(LUDUS_PLATFORM_WINDOWS)
/// The CRT transfers at most this many bytes per call.
constexpr usize MAX_TRANSFER_BYTES = 0x7FFFFFFF;
#endif
/// Digits of a displayed diagnostic code, as in `LUSL0504`.
constexpr usize DIAGNOSTIC_CODE_DIGITS = 4;
/// Decimal digits of the largest uint64.
constexpr usize MAX_DECIMAL_DIGITS = 20;
constexpr uint64 DECIMAL_BASE = 10;
/// Most `--define` values; the preprocessor admits no more.
constexpr usize MAX_DEFINES = language::MAX_CONFIGURATION_VALUES;

using PathText = StaticString<filesystem::MAX_PATH_BYTES>;

const std::string_view USAGE =
    "usage: luslc check  [--root DIR] [--include-directory DIR]... [--style strict|warn|off]\n"
    "                    [--define NAME=VALUE]... [--max-specializations N] SOURCE\n"
    "       luslc lower  [--root DIR] [--include-directory DIR]... [--style strict|warn|off]\n"
    "                    [--define NAME=VALUE]... [--max-specializations N] [--bounds-checks]\n"
    "                    [--output FILE] [--report FILE] SOURCE\n"
    "       luslc format [--root DIR] [--check | --write] FILE...\n"
    "SOURCE, FILE and --include-directory are '/'-separated paths relative to --root (default: the\n"
    "current directory). --max-specializations is the specialization budget, 1 to 256 (default 256).\n"
    "--report writes the specializations and groupshared clears of the lowered module.\n";

/// Write part of `bytes` to a standard stream: the byte count, or negative on failure.
isize WriteDescriptor(int32 descriptor, const char* bytes, usize count) noexcept
{
#if defined(LUDUS_PLATFORM_WINDOWS)
    const usize bounded = count < MAX_TRANSFER_BYTES ? count : MAX_TRANSFER_BYTES;
    return ::_write(descriptor, bytes, static_cast<unsigned int>(bounded));
#else
    return ::write(descriptor, bytes, count);
#endif
}

bool WriteAll(int32 descriptor, std::string_view text) noexcept
{
    usize offset = 0;
    while (offset < text.size())
    {
        const isize count = WriteDescriptor(descriptor, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return false;
        }
        offset += static_cast<usize>(count);
    }
    return true;
}

void WriteError(std::string_view text) noexcept
{
    (void)WriteAll(STANDARD_ERROR, text);
}

/// A bounded line of diagnostic output.
class Line final
{
public:
    void Append(std::string_view text) noexcept
    {
        (void)mText.TryAppend(text);
    }

    void AppendUnsigned(uint64 value, usize minimumDigits = 1) noexcept
    {
        StaticArray<char, MAX_DECIMAL_DIGITS> digits{};
        usize count = 0;
        do
        {
            digits[MAX_DECIMAL_DIGITS - 1 - count] = static_cast<char>('0' + static_cast<char>(value % DECIMAL_BASE));
            value /= DECIMAL_BASE;
            ++count;
        } while ((value != 0 || count < minimumDigits) && count < MAX_DECIMAL_DIGITS);
        Append(std::string_view(digits.GetData() + (MAX_DECIMAL_DIGITS - count), count));
    }

    [[nodiscard]] std::string_view GetView() const noexcept
    {
        return mText.GetView();
    }

private:
    StaticString<language::MAX_DIAGNOSTIC_MESSAGE_BYTES + filesystem::MAX_PATH_BYTES * 2> mText;
};

/// A view of `text` for request fields; BorrowedView carries the size, so no terminator is needed.
[[nodiscard]] BorrowedView<const char> MakeView(std::string_view text) noexcept
{
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the view carries the size.
    return BorrowedView<const char>(text.data(), text.size());
}

[[nodiscard]] std::string_view ViewText(const BorrowedView<const char>& view) noexcept
{
    return view.IsValid() == true ? std::string_view(view.GetDataOrNull(), view.GetSize()) : std::string_view{};
}

/// A source snapshot read from disk: file texts and paths in storage reserved once, so the views
/// in Files stay valid.
class Snapshot final
{
public:
    [[nodiscard]] bool Initialize() noexcept
    {
        return mText.TryEnsureCapacity(compiler::MAX_SOURCE_BYTES) == true &&
               mPaths.TryEnsureCapacity(compiler::MAX_SOURCE_FILES) == true &&
               mFiles.TryEnsureCapacity(compiler::MAX_SOURCE_FILES) == true;
    }

    /// Read `path` relative to `root` unless already present. Returns false and explains why when
    /// the file exists but cannot be admitted; a missing file is reported only when `isRequired`.
    [[nodiscard]] bool TryAdd(const filesystem::Directory& root, std::string_view path, bool isRequired) noexcept
    {
        for (const PathText& existing : mPaths)
        {
            if (existing.GetView() == path)
            {
                return true;
            }
        }
        if (filesystem::ValidPath(path) == false)
        {
            if (isRequired == true)
            {
                reportFile(path, "is not a valid relative '/'-separated path");
            }
            return isRequired == false;
        }
        filesystem::File file;
        const filesystem::Result opened = root.OpenRead(path, file);
        if (opened.Succeeded() == false)
        {
            if (isRequired == true || opened.Code != filesystem::Status::NotFound)
            {
                reportFile(path, "cannot be opened");
                return false;
            }
            return true;
        }
        const uint64 size = file.Size();
        if (mFiles.GetSize() >= compiler::MAX_SOURCE_FILES || size > compiler::MAX_SOURCE_BYTES - mText.GetSize())
        {
            reportFile(path, "does not fit the snapshot limits of the shader compiler");
            return false;
        }
        const usize offset = mText.GetSize();
        if (mText.TryResize(offset + static_cast<usize>(size)) == false)
        {
            reportFile(path, "cannot be read: out of memory");
            return false;
        }
        const filesystem::ReadResult read =
            file.ReadAt(0,
                        std::span<uint8>(reinterpret_cast<uint8*>(mText.GetData() + offset), static_cast<usize>(size)));
        if (read.Outcome.Succeeded() == false || read.BytesRead != size)
        {
            reportFile(path, "cannot be read");
            return false;
        }
        PathText stored;
        (void)stored.TryAssign(path);
        mPaths.Add(stored);
        compiler::SourceFile source;
        source.Text = BorrowedView<const char>(mText.GetData() + offset, static_cast<usize>(size));
        mFiles.Add(source);
        // Views into mPaths are refreshed after every addition; mPaths never reallocates.
        for (usize i = 0; i < mFiles.GetSize(); ++i)
        {
            mFiles[i].Path = MakeView(mPaths[i].GetView());
        }
        return true;
    }

    /// Add the files that `#include` and `extern slang` name in each file, transitively. The scan is
    /// lexical, so it may add files that conditional groups or comments never use; that only makes
    /// the snapshot larger. Resolution follows the preprocessor: beside the naming file, from the
    /// root, then below each include directory.
    [[nodiscard]] bool AddDependencies(const filesystem::Directory& root,
                                       BorrowedView<const BorrowedView<const char>> includeDirectories) noexcept
    {
        for (usize index = 0; index < mFiles.GetSize(); ++index)
        {
            const std::string_view text = ViewText(mFiles[index].Text);
            const std::string_view path = mPaths[index].GetView();
            const usize slash = path.rfind('/');
            const std::string_view directory =
                slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash + 1);
            usize position = 0;
            while (position < text.size())
            {
                const usize quote = findNamedPath(text, position);
                if (quote == std::string_view::npos)
                {
                    break;
                }
                const usize end = text.find('"', quote + 1);
                if (end == std::string_view::npos)
                {
                    break;
                }
                const std::string_view name = text.substr(quote + 1, end - quote - 1);
                position = end + 1;
                PathText beside;
                if (beside.TryAssign(directory) == StringStatus::Ok && beside.TryAppend(name) == StringStatus::Ok &&
                    TryAdd(root, beside.GetView(), false) == false)
                {
                    return false;
                }
                if (TryAdd(root, name, false) == false)
                {
                    return false;
                }
                for (usize i = 0; i < includeDirectories.GetSize(); ++i)
                {
                    PathText below;
                    if (below.TryAssign(ViewText(includeDirectories[i])) == StringStatus::Ok &&
                        below.TryAppend("/") == StringStatus::Ok && below.TryAppend(name) == StringStatus::Ok &&
                        TryAdd(root, below.GetView(), false) == false)
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    [[nodiscard]] BorrowedView<const compiler::SourceFile> GetFiles() const noexcept
    {
        return BorrowedView<const compiler::SourceFile>(mFiles.GetData(), mFiles.GetSize());
    }

    [[nodiscard]] std::string_view GetPath(usize file) const noexcept
    {
        return file < mPaths.GetSize() ? mPaths[file].GetView() : std::string_view{};
    }

private:
    /// The position of the opening quote after the next `#include` or `extern slang` at or after
    /// `position`, or npos.
    [[nodiscard]] static usize findNamedPath(std::string_view text, usize position) noexcept
    {
        constexpr std::string_view INCLUDE = "include";
        constexpr std::string_view EXTERN = "extern";
        constexpr std::string_view SLANG = "slang";
        while (position < text.size())
        {
            const usize include = text.find(INCLUDE, position);
            const usize external = text.find(EXTERN, position);
            const usize next = include < external ? include : external;
            if (next == std::string_view::npos)
            {
                return std::string_view::npos;
            }
            usize cursor = next + (next == include ? INCLUDE.size() : EXTERN.size());
            cursor = skipSpaces(text, cursor);
            if (next == external)
            {
                if (text.substr(cursor, SLANG.size()) != SLANG)
                {
                    position = next + 1;
                    continue;
                }
                cursor = skipSpaces(text, cursor + SLANG.size());
            }
            if (cursor < text.size() && text[cursor] == '"')
            {
                return cursor;
            }
            position = next + 1;
        }
        return std::string_view::npos;
    }

    [[nodiscard]] static usize skipSpaces(std::string_view text, usize position) noexcept
    {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t'))
        {
            ++position;
        }
        return position;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a path, then the sentence about it.
    static void reportFile(std::string_view path, std::string_view reason) noexcept
    {
        Line line;
        line.Append("luslc: '");
        line.Append(path);
        line.Append("' ");
        line.Append(reason);
        line.Append("\n");
        WriteError(line.GetView());
    }

    Array<char> mText;
    Array<PathText> mPaths;
    Array<compiler::SourceFile> mFiles;
};

/// Print one diagnostic as `path:line:column: error LUSL0504 type-mismatch: message`.
void PrintDiagnostic(const Snapshot& snapshot, const language::Diagnostic& diagnostic) noexcept
{
    Line line;
    const language::SourceLocation& location = diagnostic.Location;
    if (location.FileIndex == language::CONFIGURATION_SOURCE)
    {
        line.Append("<configuration>:");
        line.AppendUnsigned(location.Line);
        line.Append(": ");
    }
    else if (location.IsValid() == true)
    {
        line.Append(snapshot.GetPath(location.FileIndex));
        line.Append(":");
        line.AppendUnsigned(location.Line);
        line.Append(":");
        line.AppendUnsigned(location.Column);
        line.Append(": ");
    }
    else
    {
        line.Append("luslc: ");
    }
    line.Append(diagnostic.Level == language::Severity::Error ? "error" : "warning");
    line.Append(" LUSL");
    line.AppendUnsigned(static_cast<uint64>(diagnostic.Code), DIAGNOSTIC_CODE_DIGITS);
    line.Append(" ");
    line.Append(language::GetDiagnosticCodeName(diagnostic.Code));
    line.Append(": ");
    line.Append(diagnostic.Message.GetView());
    const language::SourceLocation& expansion = diagnostic.ExpansionLocation;
    if (expansion.IsValid() == true && expansion.FileIndex != language::CONFIGURATION_SOURCE)
    {
        line.Append(" (in a macro used at ");
        line.Append(snapshot.GetPath(expansion.FileIndex));
        line.Append(":");
        line.AppendUnsigned(expansion.Line);
        line.Append(")");
    }
    line.Append("\n");
    WriteError(line.GetView());
}

struct Options final
{
    std::string_view Command;
    std::string_view Root = ".";
    std::string_view Output;
    std::string_view Report;
    language::LoweringOptions Lowering;
    bool ShouldCheck = false;
    bool ShouldWrite = false;
    StaticArray<language::ConfigurationValue, MAX_DEFINES> Defines{};
    usize DefineCount = 0;
    StaticArray<std::string_view, compiler::MAX_SOURCE_FILES> Paths{};
    usize PathCount = 0;
    StaticArray<BorrowedView<const char>, language::MAX_INCLUDE_DIRECTORIES> IncludeDirectories{};
    usize IncludeDirectoryCount = 0;
};

[[nodiscard]] bool ParseStyle(std::string_view text, language::StyleSetting& outStyle) noexcept
{
    if (text == "strict")
    {
        outStyle = language::StyleSetting::Strict;
        return true;
    }
    if (text == "warn")
    {
        outStyle = language::StyleSetting::Warn;
        return true;
    }
    if (text == "off")
    {
        outStyle = language::StyleSetting::Off;
        return true;
    }
    return false;
}

/// A decimal budget from 1 to MAX_SPECIALIZATIONS.
[[nodiscard]] bool ParseBudget(std::string_view text, usize& outBudget) noexcept
{
    usize value = 0;
    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        value = value * DECIMAL_BASE + static_cast<usize>(character - '0');
        if (value > language::MAX_SPECIALIZATIONS)
        {
            return false;
        }
    }
    if (text.empty() == true || value == 0)
    {
        return false;
    }
    outBudget = value;
    return true;
}

[[nodiscard]] bool ParseArguments(int argumentCount, char** arguments, Options& outOptions) noexcept
{
    if (argumentCount < 2)
    {
        return false;
    }
    outOptions.Command = arguments[1];
    for (int i = 2; i < argumentCount; ++i)
    {
        const std::string_view argument = arguments[i];
        const bool hasValue = i + 1 < argumentCount;
        if (argument == "--root" && hasValue == true)
        {
            ++i;
            outOptions.Root = arguments[i];
        }
        else if (argument == "--output" && hasValue == true)
        {
            ++i;
            outOptions.Output = arguments[i];
        }
        else if (argument == "--report" && hasValue == true)
        {
            ++i;
            outOptions.Report = arguments[i];
        }
        else if (argument == "--max-specializations" && hasValue == true)
        {
            ++i;
            if (ParseBudget(arguments[i], outOptions.Lowering.Check.SpecializationBudget) == false)
            {
                Line line;
                line.Append("luslc: --max-specializations takes a whole number from 1 to ");
                line.AppendUnsigned(language::MAX_SPECIALIZATIONS);
                line.Append(", the engine limit\n");
                WriteError(line.GetView());
                return false;
            }
        }
        else if (argument == "--style" && hasValue == true)
        {
            ++i;
            if (ParseStyle(arguments[i], outOptions.Lowering.Check.Style) == false)
            {
                return false;
            }
        }
        else if (argument == "--define" && hasValue == true)
        {
            ++i;
            const std::string_view define = arguments[i];
            const usize equals = define.find('=');
            if (equals == std::string_view::npos || outOptions.DefineCount >= MAX_DEFINES)
            {
                return false;
            }
            language::ConfigurationValue& value = outOptions.Defines[outOptions.DefineCount];
            value.Name = MakeView(define.substr(0, equals));
            value.Value = MakeView(define.substr(equals + 1));
            ++outOptions.DefineCount;
        }
        else if (argument == "--include-directory" && hasValue == true)
        {
            ++i;
            if (outOptions.IncludeDirectoryCount >= language::MAX_INCLUDE_DIRECTORIES)
            {
                return false;
            }
            outOptions.IncludeDirectories[outOptions.IncludeDirectoryCount] = MakeView(arguments[i]);
            ++outOptions.IncludeDirectoryCount;
        }
        else if (argument == "--bounds-checks")
        {
            outOptions.Lowering.Emit.ShouldCheckBounds = true;
        }
        else if (argument == "--check")
        {
            outOptions.ShouldCheck = true;
        }
        else if (argument == "--write")
        {
            outOptions.ShouldWrite = true;
        }
        else if (argument.starts_with("--") == false && outOptions.PathCount < compiler::MAX_SOURCE_FILES)
        {
            outOptions.Paths[outOptions.PathCount] = argument;
            ++outOptions.PathCount;
        }
        else
        {
            return false;
        }
    }
    return outOptions.PathCount > 0 && (outOptions.ShouldCheck == false || outOptions.ShouldWrite == false);
}

/// Publish `text` at native path `path`, replacing it atomically.
[[nodiscard]] bool WriteFile(std::string_view path, const Array<char>& text) noexcept
{
    usize slash = path.rfind('/');
#if defined(LUDUS_PLATFORM_WINDOWS)
    const usize backslash = path.rfind('\\');
    if (backslash != std::string_view::npos && (slash == std::string_view::npos || backslash > slash))
    {
        slash = backslash;
    }
#endif
    const std::string_view directory = slash == std::string_view::npos ? std::string_view(".") : path.substr(0, slash);
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    filesystem::WriteDirectory output;
    if (output.Open(directory.empty() == true ? std::string_view("/") : directory).Succeeded() == false)
    {
        return false;
    }
    filesystem::WriteOptions options;
    options.MaxBytes = compiler::MAX_SOURCE_BYTES;
    options.Sync = filesystem::SyncPolicy::PublishOnly;
    const filesystem::PublicationResult published =
        output.Publish(name,
                       std::span<const uint8>(reinterpret_cast<const uint8*>(text.GetData()), text.GetSize()),
                       options);
    return published.Published == true;
}

[[nodiscard]] bool AppendText(std::string_view text, Array<char>& inoutText) noexcept
{
    return inoutText.TryAddRange(std::span<const char>(text.data(), text.size())) == true;
}

/// Append `path:line` of `location`, or `<configuration>:line`.
[[nodiscard]] bool
AppendLocation(const Snapshot& snapshot, const language::SourceLocation& location, Array<char>& inoutText) noexcept
{
    Line line;
    line.Append(location.FileIndex == language::CONFIGURATION_SOURCE ? std::string_view("<configuration>")
                                                                     : snapshot.GetPath(location.FileIndex));
    line.Append(":");
    line.AppendUnsigned(location.Line);
    return AppendText(line.GetView(), inoutText);
}

/// The build report of a lowered module, one tab-separated record per line:
/// `specialization <template location> <name> <generated name>` and
/// `groupshared-clear <entry location> <variable location> <unpadded bytes>`.
[[nodiscard]] bool
ComposeReport(const Snapshot& snapshot, const language::LoweringResult& result, Array<char>& outText) noexcept
{
    outText.Clear();
    for (const language::SpecializationRecord& record : result.Specializations)
    {
        if (AppendText("specialization\t", outText) == false ||
            AppendLocation(snapshot, record.Template, outText) == false || AppendText("\t", outText) == false ||
            AppendText(record.Name.GetView(), outText) == false || AppendText("\t", outText) == false ||
            AppendText(record.GeneratedName.GetView(), outText) == false || AppendText("\n", outText) == false)
        {
            return false;
        }
    }
    for (const language::GroupsharedClearRecord& record : result.GroupsharedClears)
    {
        Line bytes;
        bytes.AppendUnsigned(record.Bytes);
        if (AppendText("groupshared-clear\t", outText) == false ||
            AppendLocation(snapshot, record.Entry, outText) == false || AppendText("\t", outText) == false ||
            AppendLocation(snapshot, record.Variable, outText) == false || AppendText("\t", outText) == false ||
            AppendText(bytes.GetView(), outText) == false || AppendText("\n", outText) == false)
        {
            return false;
        }
    }
    return true;
}

int RunLowering(const Options& options, const filesystem::Directory& root) noexcept
{
    Snapshot snapshot;
    const BorrowedView<const BorrowedView<const char>> includeDirectories(options.IncludeDirectories.GetData(),
                                                                          options.IncludeDirectoryCount);
    if (snapshot.Initialize() == false || snapshot.TryAdd(root, options.Paths[0], true) == false ||
        snapshot.AddDependencies(root, includeDirectories) == false)
    {
        return EXIT_FAILED;
    }
    language::PreprocessRequest request;
    request.Files = snapshot.GetFiles();
    request.Source = MakeView(options.Paths[0]);
    request.Configuration =
        BorrowedView<const language::ConfigurationValue>(options.Defines.GetData(), options.DefineCount);
    request.IncludeDirectories = includeDirectories;
    language::LoweringResult result;
    const language::Status outcome = language::LowerToSlang(request, options.Lowering, result);
    for (const language::Diagnostic& diagnostic : result.Diagnostics)
    {
        PrintDiagnostic(snapshot, diagnostic);
    }
    if (result.DroppedDiagnosticCount > 0)
    {
        Line line;
        line.Append("luslc: ");
        line.AppendUnsigned(result.DroppedDiagnosticCount);
        line.Append(" more diagnostics were not shown\n");
        WriteError(line.GetView());
    }
    if (outcome != language::Status::Ready)
    {
        return EXIT_FAILED;
    }
    if (options.Command == "check")
    {
        return EXIT_OK;
    }
    if (options.Report.empty() == false)
    {
        Array<char> report;
        if (ComposeReport(snapshot, result, report) == false || WriteFile(options.Report, report) == false)
        {
            WriteError("luslc: the report file cannot be written\n");
            return EXIT_FAILED;
        }
    }
    const std::string_view text(result.Text.GetData(), result.Text.GetSize());
    if (options.Output.empty() == true)
    {
        return WriteAll(STANDARD_OUTPUT, text) == true ? EXIT_OK : EXIT_FAILED;
    }
    if (WriteFile(options.Output, result.Text) == false)
    {
        WriteError("luslc: the output file cannot be written\n");
        return EXIT_FAILED;
    }
    return EXIT_OK;
}

int RunFormat(const Options& options, const filesystem::Directory& root) noexcept
{
    int exitCode = EXIT_OK;
    for (usize i = 0; i < options.PathCount; ++i)
    {
        Snapshot snapshot;
        if (snapshot.Initialize() == false || snapshot.TryAdd(root, options.Paths[i], true) == false)
        {
            return EXIT_FAILED;
        }
        const std::string_view text = ViewText(snapshot.GetFiles()[0].Text);
        language::FormatResult result;
        if (language::FormatSource(text, result) != language::Status::Ready)
        {
            for (const language::Diagnostic& diagnostic : result.Diagnostics)
            {
                PrintDiagnostic(snapshot, diagnostic);
            }
            exitCode = EXIT_FAILED;
            continue;
        }
        const std::string_view formatted(result.Text.GetData(), result.Text.GetSize());
        if (options.ShouldCheck == true)
        {
            if (formatted != text)
            {
                Line line;
                line.Append(options.Paths[i]);
                line.Append(": not formatted; run luslc format --write\n");
                WriteError(line.GetView());
                exitCode = EXIT_FAILED;
            }
            continue;
        }
        if (options.ShouldWrite == true)
        {
            if (formatted == text)
            {
                continue;
            }
            PathText native;
            if (native.TryAssign(options.Root) != StringStatus::Ok || native.TryAppend("/") != StringStatus::Ok ||
                native.TryAppend(options.Paths[i]) != StringStatus::Ok ||
                WriteFile(native.GetView(), result.Text) == false)
            {
                WriteError("luslc: a formatted file cannot be written\n");
                exitCode = EXIT_FAILED;
            }
            continue;
        }
        if (WriteAll(STANDARD_OUTPUT, formatted) == false)
        {
            return EXIT_FAILED;
        }
    }
    return exitCode;
}
} // namespace

int main(int argc, char** argv)
{
#if defined(LUDUS_PLATFORM_WINDOWS)
    // Generated Slang is a byte stream; text mode would translate line endings.
    (void)::_setmode(STANDARD_OUTPUT, _O_BINARY);
#endif
    Options options;
    const bool isValid = ParseArguments(argc, argv, options) == true &&
                         (options.Command == "format" || options.PathCount == 1) &&
                         (options.Command == "check" || options.Command == "lower" || options.Command == "format");
    if (isValid == false)
    {
        WriteError(USAGE);
        return EXIT_USAGE;
    }
    filesystem::Directory root;
    if (root.Open(options.Root).Succeeded() == false)
    {
        WriteError("luslc: the --root directory cannot be opened\n");
        return EXIT_FAILED;
    }
    if (options.Command == "format")
    {
        return RunFormat(options, root);
    }
    return RunLowering(options, root);
}
