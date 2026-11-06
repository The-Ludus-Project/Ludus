// Shared parts of luslc: bounded output, a source snapshot read from disk or supplied by an
// editor, and diagnostic printing (docs/modules/tooling/shader-language.md#luslc).
#pragma once

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/tooling/shader_compiler/compiler.hpp>
#include <ludus/tooling/shader_language/diagnostic.hpp>
#include <ludus/tooling/shader_language/limits.hpp>

#include <cerrno>
#include <span>
#include <string_view>

#if defined(LUDUS_PLATFORM_WINDOWS)
#    include <io.h>
#else
#    include <unistd.h>
#endif

namespace ludus::tools::luslc
{
using namespace ludus::foundation;
namespace language = ludus::tooling::shader_language;
namespace compiler = ludus::tooling::shader_compiler;

inline constexpr int32 STANDARD_INPUT = 0;
inline constexpr int32 STANDARD_OUTPUT = 1;
inline constexpr int32 STANDARD_ERROR = 2;
#if defined(LUDUS_PLATFORM_WINDOWS)
/// The CRT transfers at most this many bytes per call.
inline constexpr usize MAX_TRANSFER_BYTES = 0x7FFFFFFF;
#endif
/// Digits of a displayed diagnostic code, as in `LUSL0504`.
inline constexpr usize DIAGNOSTIC_CODE_DIGITS = 4;
/// Decimal digits of the largest uint64.
inline constexpr usize MAX_DECIMAL_DIGITS = 20;
inline constexpr uint64 DECIMAL_BASE = 10;

using PathText = StaticString<filesystem::MAX_PATH_BYTES>;

/// Write part of `bytes` to a standard stream: the byte count, or negative on failure.
inline isize WriteDescriptor(int32 descriptor, const char* bytes, usize count) noexcept
{
#if defined(LUDUS_PLATFORM_WINDOWS)
    const usize bounded = count < MAX_TRANSFER_BYTES ? count : MAX_TRANSFER_BYTES;
    return ::_write(descriptor, bytes, static_cast<unsigned int>(bounded));
#else
    return ::write(descriptor, bytes, count);
#endif
}

inline bool WriteAll(int32 descriptor, std::string_view text) noexcept
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

inline void WriteError(std::string_view text) noexcept
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
[[nodiscard]] inline BorrowedView<const char> MakeView(std::string_view text) noexcept
{
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the view carries the size.
    return BorrowedView<const char>(text.data(), text.size());
}

[[nodiscard]] inline std::string_view ViewText(const BorrowedView<const char>& view) noexcept
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

    /// Add `text` as the file `path`, such as an editor's unsaved document, unless the path is
    /// already present. Returns false (and explains why) when the snapshot limits are exceeded.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a path, then the text stored under it.
    [[nodiscard]] bool TryAddText(std::string_view path, std::string_view text) noexcept
    {
        for (const PathText& existing : mPaths)
        {
            if (existing.GetView() == path)
            {
                return true;
            }
        }
        if (mFiles.GetSize() >= compiler::MAX_SOURCE_FILES ||
            text.size() > compiler::MAX_SOURCE_BYTES - mText.GetSize())
        {
            reportFile(path, "does not fit the snapshot limits of the shader compiler");
            return false;
        }
        PathText stored;
        if (stored.TryAssign(path) != StringStatus::Ok)
        {
            reportFile(path, "has a path longer than the snapshot admits");
            return false;
        }
        const usize offset = mText.GetSize();
        if (mText.TryAddRange(std::span<const char>(text.data(), text.size())) == false)
        {
            reportFile(path, "cannot be stored: out of memory");
            return false;
        }
        mPaths.Add(stored);
        compiler::SourceFile source;
        source.Text = BorrowedView<const char>(mText.GetData() + offset, text.size());
        mFiles.Add(source);
        for (usize i = 0; i < mFiles.GetSize(); ++i)
        {
            mFiles[i].Path = MakeView(mPaths[i].GetView());
        }
        return true;
    }

    /// Remove every file, keeping the reserved storage.
    void Clear() noexcept
    {
        mText.Clear();
        mPaths.Clear();
        mFiles.Clear();
    }

    /// The text of file `file`; empty when there is no such file.
    [[nodiscard]] std::string_view GetText(usize file) const noexcept
    {
        return file < mFiles.GetSize() ? ViewText(mFiles[file].Text) : std::string_view{};
    }

    /// The index of the file `path`, or the file count when it is not present.
    [[nodiscard]] usize Find(std::string_view path) const noexcept
    {
        for (usize i = 0; i < mPaths.GetSize(); ++i)
        {
            if (mPaths[i].GetView() == path)
            {
                return i;
            }
        }
        return mPaths.GetSize();
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
inline void PrintDiagnostic(const Snapshot& snapshot, const language::Diagnostic& diagnostic) noexcept
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

} // namespace ludus::tools::luslc
