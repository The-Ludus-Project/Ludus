// lusl-declarations: report a Slang module's declarations, spelled as LUSL, for generating the SDK's
// `.luslh` headers from Slang reflection (docs/modules/tooling/shader-language.md#sdk-declarations).
// cmake/shaders/generate_luslh.py expands the header templates from this output.

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/tooling/shader_compiler/compiler.hpp>

#include <cerrno>
#include <span>
#include <string_view>

#if defined(LUDUS_PLATFORM_WINDOWS)
#    include <io.h>
#else
#    include <unistd.h>
#endif

namespace
{
using namespace ludus::foundation;
namespace compiler = ludus::tooling::shader_compiler;

constexpr int EXIT_OK = 0;
constexpr int EXIT_FAILED = 1;
constexpr int EXIT_USAGE = 2;
constexpr int32 STANDARD_OUTPUT = 1;
constexpr int32 STANDARD_ERROR = 2;
/// Decimal digits of the largest uint64.
constexpr usize MAX_DECIMAL_DIGITS = 20;
constexpr uint64 DECIMAL_BASE = 10;

const std::string_view USAGE =
    "usage: lusl-declarations --root DIR --source FILE [--file FILE]...\n"
    "Reflects the Slang module FILE and prints one tab-separated record per line:\n"
    "  constant NAME TYPE [VALUE] | struct NAME | field NAME TYPE | enum NAME TYPE |\n"
    "  enumerator NAME [VALUE] | function NAME RETURN_TYPE | parameter NAME in|out|inout TYPE\n"
    "Types are spelled as LUSL (float32x3, uint32[4], StructName); '?' prefixes a type LUSL cannot\n"
    "spell. FILE paths are '/'-separated and relative to DIR; --file adds the files Slang may include.\n";

bool WriteAll(int32 descriptor, std::string_view text) noexcept
{
    usize offset = 0;
    while (offset < text.size())
    {
#if defined(LUDUS_PLATFORM_WINDOWS)
        const isize count = ::_write(descriptor, text.data() + offset, static_cast<unsigned int>(text.size() - offset));
#else
        const isize count = ::write(descriptor, text.data() + offset, text.size() - offset);
#endif
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

/// Output text built in memory, written once.
class Output final
{
public:
    void Append(std::string_view text) noexcept
    {
        mHasFailed =
            mHasFailed == true || mText.TryAddRange(BorrowedView<const char>(text.data(), text.size())) == false;
    }

    void AppendUnsigned(uint64 value) noexcept
    {
        StaticArray<char, MAX_DECIMAL_DIGITS> digits{};
        usize count = 0;
        do
        {
            digits[MAX_DECIMAL_DIGITS - 1 - count] = static_cast<char>('0' + static_cast<char>(value % DECIMAL_BASE));
            value /= DECIMAL_BASE;
            ++count;
        } while (value != 0 && count < MAX_DECIMAL_DIGITS);
        Append(std::string_view(digits.GetData() + (MAX_DECIMAL_DIGITS - count), count));
    }

    [[nodiscard]] bool HasFailed() const noexcept
    {
        return mHasFailed;
    }

    [[nodiscard]] std::string_view GetView() const noexcept
    {
        return std::string_view(mText.GetData(), mText.GetSize());
    }

private:
    Array<char> mText;
    bool mHasFailed = false;
};

[[nodiscard]] std::string_view GetScalarName(compiler::ReflectedScalar scalar) noexcept
{
    switch (scalar)
    {
        case compiler::ReflectedScalar::Bool:
            return "bool";
        case compiler::ReflectedScalar::Int16:
            return "int16";
        case compiler::ReflectedScalar::Uint16:
            return "uint16";
        case compiler::ReflectedScalar::Int32:
            return "int32";
        case compiler::ReflectedScalar::Uint32:
            return "uint32";
        case compiler::ReflectedScalar::Int64:
            return "int64";
        case compiler::ReflectedScalar::Uint64:
            return "uint64";
        case compiler::ReflectedScalar::Float16:
            return "float16";
        case compiler::ReflectedScalar::Float32:
            return "float32";
        case compiler::ReflectedScalar::Float64:
            return "float64";
    }
    return {};
}

[[nodiscard]] std::string_view GetText(const compiler::DeclarationResult& result, compiler::ReflectedText text) noexcept
{
    return std::string_view(result.Text.GetData() + text.Offset, text.Size);
}

/// Spell `type` as LUSL: `float32x3`, `float32x4x4`, a struct name, `void`, with `[N]` for an array.
void AppendType(const compiler::DeclarationResult& result, const compiler::ReflectedType& type, Output& inoutOutput)
{
    switch (type.Shape)
    {
        case compiler::ReflectedShape::Void:
            inoutOutput.Append("void");
            break;
        case compiler::ReflectedShape::Scalar:
            inoutOutput.Append(GetScalarName(type.Scalar));
            break;
        case compiler::ReflectedShape::Vector:
            inoutOutput.Append(GetScalarName(type.Scalar));
            inoutOutput.Append("x");
            inoutOutput.AppendUnsigned(type.Rows);
            break;
        case compiler::ReflectedShape::Matrix:
            inoutOutput.Append(GetScalarName(type.Scalar));
            inoutOutput.Append("x");
            inoutOutput.AppendUnsigned(type.Rows);
            inoutOutput.Append("x");
            inoutOutput.AppendUnsigned(type.Columns);
            break;
        case compiler::ReflectedShape::Struct:
        case compiler::ReflectedShape::Enum:
            inoutOutput.Append(GetText(result, type.Name));
            break;
        case compiler::ReflectedShape::Unsupported:
            inoutOutput.Append("?");
            inoutOutput.Append(GetText(result, type.Name));
            break;
    }
    if (type.ArrayCount != 0)
    {
        inoutOutput.Append("[");
        inoutOutput.AppendUnsigned(type.ArrayCount);
        inoutOutput.Append("]");
    }
}

/// The snapshot files, read once into storage that outlives the request.
class Snapshot final
{
public:
    [[nodiscard]] bool Add(const filesystem::Directory& root, std::string_view path) noexcept
    {
        filesystem::File file;
        if (filesystem::ValidPath(path) == false || root.OpenRead(path, file).Succeeded() == false ||
            mFiles.GetSize() >= compiler::MAX_SOURCE_FILES)
        {
            return false;
        }
        const uint64 size = file.Size();
        if (size > compiler::MAX_SOURCE_BYTES)
        {
            return false;
        }
        Array<char> text;
        if (text.TryResize(static_cast<usize>(size)) == false)
        {
            return false;
        }
        const filesystem::ReadResult read =
            file.ReadAt(0, std::span<uint8>(reinterpret_cast<uint8*>(text.GetData()), text.GetSize()));
        if (read.Outcome.Succeeded() == false || read.BytesRead != size)
        {
            return false;
        }
        Entry entry;
        entry.Path = path;
        entry.Text = Move(text);
        return mFiles.TryAdd(Move(entry)) == true;
    }

    /// Views of the files; valid until the next Add.
    [[nodiscard]] bool MakeViews(Array<compiler::SourceFile>& outViews) const noexcept
    {
        outViews.Clear();
        for (const Entry& entry : mFiles)
        {
            compiler::SourceFile file;
            // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the view carries the size.
            file.Path = BorrowedView<const char>(entry.Path.data(), entry.Path.size());
            file.Text = BorrowedView<const char>(entry.Text.GetData(), entry.Text.GetSize());
            if (outViews.TryAdd(file) == false)
            {
                return false;
            }
        }
        return true;
    }

private:
    struct Entry final
    {
        std::string_view Path;
        Array<char> Text;
    };

    Array<Entry> mFiles;
};

void AppendSigned(int64 value, Output& inoutOutput) noexcept
{
    if (value < 0)
    {
        inoutOutput.Append("-");
        // Negate in uint64 so the most negative value is exact.
        inoutOutput.AppendUnsigned(uint64{0} - static_cast<uint64>(value));
        return;
    }
    inoutOutput.AppendUnsigned(static_cast<uint64>(value));
}

void PrintRecords(const compiler::DeclarationResult& result, Output& inoutOutput) noexcept
{
    for (const compiler::ReflectedDeclaration& declaration : result.Declarations)
    {
        switch (declaration.Kind)
        {
            case compiler::DeclarationKind::Constant:
                inoutOutput.Append("constant\t");
                inoutOutput.Append(GetText(result, declaration.Name));
                inoutOutput.Append("\t");
                AppendType(result, declaration.Type, inoutOutput);
                if (declaration.HasValue == true)
                {
                    inoutOutput.Append("\t");
                    AppendSigned(declaration.Value, inoutOutput);
                }
                inoutOutput.Append("\n");
                break;
            case compiler::DeclarationKind::Enum:
                inoutOutput.Append("enum\t");
                inoutOutput.Append(GetText(result, declaration.Name));
                inoutOutput.Append("\t");
                AppendType(result, declaration.Type, inoutOutput);
                inoutOutput.Append("\n");
                break;
            case compiler::DeclarationKind::Struct:
                inoutOutput.Append("struct\t");
                inoutOutput.Append(GetText(result, declaration.Name));
                inoutOutput.Append("\n");
                break;
            case compiler::DeclarationKind::Function:
                inoutOutput.Append("function\t");
                inoutOutput.Append(GetText(result, declaration.Name));
                inoutOutput.Append("\t");
                AppendType(result, declaration.Type, inoutOutput);
                inoutOutput.Append("\n");
                break;
        }
        for (uint32 i = 0; i < declaration.MemberCount; ++i)
        {
            const compiler::ReflectedMember& member = result.Members[declaration.FirstMember + i];
            if (declaration.Kind == compiler::DeclarationKind::Enum)
            {
                inoutOutput.Append("enumerator\t");
                inoutOutput.Append(GetText(result, member.Name));
                inoutOutput.Append("\t");
                if (member.HasValue == true)
                {
                    AppendSigned(member.Value, inoutOutput);
                }
                inoutOutput.Append("\n");
                continue;
            }
            const bool isField = declaration.Kind == compiler::DeclarationKind::Struct;
            inoutOutput.Append(isField == true ? "field\t" : "parameter\t");
            inoutOutput.Append(GetText(result, member.Name));
            inoutOutput.Append("\t");
            if (isField == false)
            {
                inoutOutput.Append(member.Direction == compiler::ReflectedDirection::Inout ? "inout\t"
                                   : member.Direction == compiler::ReflectedDirection::Out ? "out\t"
                                                                                           : "in\t");
            }
            AppendType(result, member.Type, inoutOutput);
            inoutOutput.Append("\n");
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    std::string_view rootPath;
    std::string_view source;
    Array<std::string_view> files;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i];
        const bool hasValue = i + 1 < argc;
        if (argument == "--root" && hasValue == true)
        {
            rootPath = argv[++i];
        }
        else if (argument == "--source" && hasValue == true)
        {
            source = argv[++i];
        }
        else if (argument == "--file" && hasValue == true && files.TryAdd(std::string_view(argv[++i])) == true)
        {
        }
        else
        {
            (void)WriteAll(STANDARD_ERROR, USAGE);
            return EXIT_USAGE;
        }
    }
    if (rootPath.empty() == true || source.empty() == true)
    {
        (void)WriteAll(STANDARD_ERROR, USAGE);
        return EXIT_USAGE;
    }
    filesystem::Directory root;
    Snapshot snapshot;
    if (root.Open(rootPath).Succeeded() == false || snapshot.Add(root, source) == false)
    {
        (void)WriteAll(STANDARD_ERROR, "lusl-declarations: the source cannot be read\n");
        return EXIT_FAILED;
    }
    for (const std::string_view file : files)
    {
        if (snapshot.Add(root, file) == false)
        {
            (void)WriteAll(STANDARD_ERROR, "lusl-declarations: a --file cannot be read\n");
            return EXIT_FAILED;
        }
    }
    Array<compiler::SourceFile> views;
    compiler::Compiler slang;
    if (snapshot.MakeViews(views) == false || slang.Initialize() != compiler::Status::Ready)
    {
        (void)WriteAll(STANDARD_ERROR, "lusl-declarations: the pinned Slang compiler is unavailable\n");
        return EXIT_FAILED;
    }
    constexpr std::string_view UNUSED_ENTRY = "Unused";
    compiler::Request request;
    request.Files = BorrowedView<const compiler::SourceFile>(views.GetData(), views.GetSize());
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the view carries the size.
    request.Source = BorrowedView<const char>(source.data(), source.size());
    request.Entry = BorrowedView<const char>(UNUSED_ENTRY.data(), UNUSED_ENTRY.size());
    compiler::DeclarationResult result;
    const compiler::Status status = slang.ReflectDeclarations(request, result);
    const std::string_view diagnostics(reinterpret_cast<const char*>(result.Diagnostics.GetData()),
                                       result.Diagnostics.GetSize());
    (void)WriteAll(STANDARD_ERROR, diagnostics);
    if (status != compiler::Status::Ready)
    {
        (void)WriteAll(STANDARD_ERROR, "lusl-declarations: Slang could not reflect the module\n");
        return EXIT_FAILED;
    }
    Output output;
    PrintRecords(result, output);
    if (output.HasFailed() == true || WriteAll(STANDARD_OUTPUT, output.GetView()) == false)
    {
        return EXIT_FAILED;
    }
    return EXIT_OK;
}
