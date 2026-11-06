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

#include "language_server.hpp"
#include "support.hpp"

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
using namespace ludus::tools::luslc;

/// Process exit codes.
constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;

/// Most `--define` values; the preprocessor admits no more.
constexpr usize MAX_DEFINES = language::MAX_CONFIGURATION_VALUES;

const std::string_view USAGE =
    "usage: luslc check  [--root DIR] [--include-directory DIR]... [--style strict|warn|off]\n"
    "                    [--define NAME=VALUE]... [--max-specializations N] SOURCE\n"
    "       luslc lower  [--root DIR] [--include-directory DIR]... [--style strict|warn|off]\n"
    "                    [--define NAME=VALUE]... [--max-specializations N] [--bounds-checks]\n"
    "                    [--output FILE] [--report FILE] SOURCE\n"
    "       luslc format [--root DIR] [--check | --write] FILE...\n"
    "       luslc lsp    [--root DIR] [--include-directory DIR]... [--style strict|warn|off]\n"
    "SOURCE, FILE and --include-directory are '/'-separated paths relative to --root (default: the\n"
    "current directory). --max-specializations is the specialization budget, 1 to 256 (default 256).\n"
    "--report writes the specializations, groupshared clears, bindings and vertex inputs of the\n"
    "lowered module. lsp serves the Language Server Protocol on standard input and output; without\n"
    "--root it uses the client's workspace root.\n";

struct Options final
{
    std::string_view Command;
    std::string_view Root = ".";
    bool HasRoot = false;
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
            outOptions.HasRoot = true;
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
    const bool hasPaths = outOptions.Command == "lsp" ? outOptions.PathCount == 0 : outOptions.PathCount > 0;
    return hasPaths == true && (outOptions.ShouldCheck == false || outOptions.ShouldWrite == false);
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

/// Spelling of a resource kind in the report.
[[nodiscard]] std::string_view GetResourceKindName(language::ResourceKind kind) noexcept
{
    switch (kind)
    {
        case language::ResourceKind::Texture2D:
            return "Texture2D";
        case language::ResourceKind::RWTexture2D:
            return "RWTexture2D";
        case language::ResourceKind::Sampler:
            return "SamplerState";
        case language::ResourceKind::ConstantBuffer:
            return "ConstantBuffer";
        case language::ResourceKind::StructuredBuffer:
            return "StructuredBuffer";
        case language::ResourceKind::RWStructuredBuffer:
            return "RWStructuredBuffer";
    }
    return {};
}

/// The build report of a lowered module, one tab-separated record per line:
/// `specialization <template location> <name> <generated name>`,
/// `groupshared-clear <entry location> <variable location> <unpadded bytes>`,
/// `binding <variable location> <name> <kind> <slot>` and
/// `vertex-input <entry location> <name> <location>`.
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
    for (const language::BindingRecord& record : result.Bindings)
    {
        Line slot;
        slot.AppendUnsigned(record.Slot);
        if (AppendText("binding\t", outText) == false || AppendLocation(snapshot, record.Variable, outText) == false ||
            AppendText("\t", outText) == false || AppendText(record.Name.GetView(), outText) == false ||
            AppendText("\t", outText) == false || AppendText(GetResourceKindName(record.Kind), outText) == false ||
            AppendText("\t", outText) == false || AppendText(slot.GetView(), outText) == false ||
            AppendText("\n", outText) == false)
        {
            return false;
        }
    }
    for (const language::VertexInputRecord& record : result.VertexInputs)
    {
        Line location;
        location.AppendUnsigned(record.Location);
        if (AppendText("vertex-input\t", outText) == false ||
            AppendLocation(snapshot, record.Entry, outText) == false || AppendText("\t", outText) == false ||
            AppendText(record.Name.GetView(), outText) == false || AppendText("\t", outText) == false ||
            AppendText(location.GetView(), outText) == false || AppendText("\n", outText) == false)
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
                         (options.Command == "format" || options.Command == "lsp" || options.PathCount == 1) &&
                         (options.Command == "check" || options.Command == "lower" || options.Command == "format" ||
                          options.Command == "lsp");
    if (isValid == false)
    {
        WriteError(USAGE);
        return EXIT_USAGE;
    }
    if (options.Command == "lsp")
    {
        LanguageServerOptions server;
        server.Root = options.HasRoot == true ? options.Root : std::string_view{};
        server.IncludeDirectories = BorrowedView<const BorrowedView<const char>>(options.IncludeDirectories.GetData(),
                                                                                 options.IncludeDirectoryCount);
        server.Style = options.Lowering.Check.Style;
        return RunLanguageServer(server);
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
