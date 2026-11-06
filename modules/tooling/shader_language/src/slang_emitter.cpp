// Slang spelling of the LUSL IR (docs/architecture/shading-language.md#compiler-architecture).
// The IR already resolved LUSL's semantics; this file chooses Slang names, types, literals and
// helper bodies, writes `#line` directives and records the source map.

#include <ludus/tooling/shader_compiler/compiler.hpp>
#include <ludus/tooling/shader_language/limits.hpp>

#include "internal/array_growth.hpp"
#include "internal/intrinsics.hpp"
#include "internal/ir.hpp"
#include "internal/type_table.hpp"

namespace ludus::tooling::shader_language::internal
{
namespace
{
using foundation::Array;
using foundation::int64;
using foundation::uint16;
using foundation::uint32;
using foundation::uint64;
using foundation::usize;

/// Spaces per indentation level.
constexpr uint32 INDENT_WIDTH = 4;
/// Decimal digits of the largest uint64.
constexpr usize MAX_DECIMAL_DIGITS = 20;
/// Base of decimal numbers.
constexpr uint64 DECIMAL_BASE = 10;
/// Magnitudes of INT32_MIN and INT64_MIN, which have no literal of their own type.
constexpr uint64 INT32_MAGNITUDE_LIMIT = uint64{1} << 31U;
constexpr uint64 INT64_MAGNITUDE_LIMIT = uint64{1} << 63U;
/// Bit widths of the integer scalars, for shift masks.
constexpr uint32 INT16_BITS = 16;
constexpr uint32 INT32_BITS = 32;
constexpr uint32 INT64_BITS = 64;

/// Bounded output text that counts lines.
class TextWriter final
{
public:
    explicit TextWriter(Array<char>& outText) noexcept : mText(outText) {}

    void Append(std::string_view text) noexcept
    {
        if (mHasFailed == true)
        {
            return;
        }
        if (text.size() > shader_compiler::MAX_SOURCE_BYTES - mText.GetSize())
        {
            mIsTooLarge = true;
            mHasFailed = true;
            return;
        }
        if (TryReserveGrowing(mText, mText.GetSize() + text.size()) == false)
        {
            mIsOutOfMemory = true;
            mHasFailed = true;
            return;
        }
        for (const char character : text)
        {
            mText.Add(character);
            if (character == '\n')
            {
                ++mLine;
            }
        }
    }

    void AppendUnsigned(uint64 value) noexcept
    {
        foundation::StaticArray<char, MAX_DECIMAL_DIGITS> digits{};
        usize count = 0;
        do
        {
            digits[MAX_DECIMAL_DIGITS - 1 - count] = static_cast<char>('0' + static_cast<char>(value % DECIMAL_BASE));
            value /= DECIMAL_BASE;
            ++count;
        } while (value != 0 && count < MAX_DECIMAL_DIGITS);
        Append(std::string_view(digits.GetData() + (MAX_DECIMAL_DIGITS - count), count));
    }

    /// One-based number of the line being written.
    [[nodiscard]] uint32 GetLine() const noexcept
    {
        return mLine;
    }

    [[nodiscard]] bool HasFailed() const noexcept
    {
        return mHasFailed;
    }

    [[nodiscard]] bool IsTooLarge() const noexcept
    {
        return mIsTooLarge;
    }

    [[nodiscard]] bool IsOutOfMemory() const noexcept
    {
        return mIsOutOfMemory;
    }

private:
    Array<char>& mText;
    uint32 mLine = 1;
    bool mHasFailed = false;
    bool mIsTooLarge = false;
    bool mIsOutOfMemory = false;
};

// The emitter passes IR-node, type, symbol and helper indices, separate uint32 index spaces whose
// parameter names say which is which.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)

class SlangEmitter final
{
public:
    SlangEmitter(const AnalysisInputs& inputs,
                 const IrModule& module,
                 DiagnosticSink& inoutSink,
                 EmitResult& outResult) noexcept
        : mInputs(inputs), mModule(module), mSink(inoutSink), mResult(outResult), mWriter(outResult.Text)
    {
    }

    void Run() noexcept
    {
        if (prepare() == false)
        {
            return;
        }
        writeHeader();
        writeArrayAliases();
        if (mModule.BoundsChecks.IsEmpty() == false)
        {
            mWriter.Append("static uint ");
            mWriter.Append(FAILURE_CODE_VARIABLE);
            mWriter.Append(" = 0u;\n");
        }
        writeHelpers();
        writeMethodAdapters();
        for (uint32 item = mModule.Nodes[0].FirstChild; item != NO_IR; item = mModule.Nodes[item].NextSibling)
        {
            writeItem(item);
        }
        finish();
    }

private:
    // ----- Model access --------------------------------------------------------------------------

    [[nodiscard]] const IrNode& at(uint32 node) const noexcept
    {
        return mModule.Nodes[node];
    }

    [[nodiscard]] const SemanticType& getType(uint32 type) const noexcept
    {
        return mInputs.Model.Types[type];
    }

    [[nodiscard]] const Symbol& getSymbol(uint32 symbol) const noexcept
    {
        return mInputs.Model.Symbols[symbol];
    }

    [[nodiscard]] std::string_view spellToken(uint32 token) const noexcept
    {
        if (token == NO_TOKEN || token >= mInputs.Preprocessed.Tokens.GetSize())
        {
            return {};
        }
        return GetTokenSpelling(mInputs.Request, mInputs.Preprocessed, mInputs.Preprocessed.Tokens[token]);
    }

    [[nodiscard]] std::string_view getName(uint32 symbol) const noexcept
    {
        if (symbol < INTRINSIC_COUNT)
        {
            return INTRINSICS[symbol].Name;
        }
        return spellToken(getSymbol(symbol).NameToken);
    }

    [[nodiscard]] std::string_view getFilePath(uint16 file) const noexcept
    {
        return ViewText(mInputs.Request.Files[file].Path);
    }

    /// Check that every path can be spelled in a directive.
    [[nodiscard]] bool prepare() noexcept
    {
        mResult.Text.Clear();
        for (usize file = 0; file < mInputs.Request.Files.GetSize(); ++file)
        {
            const std::string_view path = getFilePath(static_cast<uint16>(file));
            if (path.find_first_of("\"\\\n") != std::string_view::npos)
            {
                mSink.ReportError(DiagnosticCode::LoweringLimit,
                                  SourceLocation{},
                                  SourceLocation{},
                                  ComposeMessage("the path '", path, "' cannot be spelled in a Slang directive"));
                return false;
            }
        }
        return true;
    }

    void finish() noexcept
    {
        if (mWriter.IsOutOfMemory() == true)
        {
            mSink.ReportOutOfMemory();
        }
        else if (mWriter.IsTooLarge() == true)
        {
            mSink.ReportError(
                DiagnosticCode::LoweringLimit,
                SourceLocation{},
                SourceLocation{},
                ComposeMessage("the generated Slang exceeds ", uint64{shader_compiler::MAX_SOURCE_BYTES}, " bytes"));
        }
        for (usize i = 0; i < mModule.BoundsChecks.GetSize(); ++i)
        {
            BoundsCheckSite site;
            site.FailureCode = static_cast<uint32>(i + 1);
            site.Location = mModule.BoundsChecks[i];
            if (mResult.BoundsChecks.TryAdd(site) == false)
            {
                mSink.ReportOutOfMemory();
                return;
            }
        }
    }

    // ----- Text and lines ------------------------------------------------------------------------

    void write(std::string_view text) noexcept
    {
        mWriter.Append(text);
    }

    void writeUnsigned(uint64 value) noexcept
    {
        mWriter.AppendUnsigned(value);
    }

    void writeIndent() noexcept
    {
        for (uint32 i = 0; i < mIndent * INDENT_WIDTH; ++i)
        {
            write(" ");
        }
    }

    void newline() noexcept
    {
        write("\n");
        if (mIsMapped == true)
        {
            ++mMappedLine;
        }
    }

    /// Point the following line at `location`, writing `#line` only when the mapping changes, and
    /// record the source map entry.
    void mapLine(const SourceLocation& location) noexcept
    {
        if (location.FileIndex >= mInputs.Request.Files.GetSize())
        {
            return;
        }
        if (mIsMapped == false || location.FileIndex != mMappedFile || location.Line != mMappedLine)
        {
            write("#line ");
            writeUnsigned(location.Line);
            write(" \"");
            write(getFilePath(location.FileIndex));
            write("\"\n");
            mIsMapped = true;
            mMappedFile = location.FileIndex;
            mMappedLine = location.Line;
        }
        SourceMapEntry entry;
        entry.GeneratedLine = mWriter.GetLine();
        entry.Location = location;
        if (mResult.SourceMap.TryAdd(entry) == false)
        {
            mSink.ReportOutOfMemory();
        }
    }

    // ----- Header, aliases and helpers -----------------------------------------------------------

    void writeHeader() noexcept
    {
        const std::string_view root = ViewText(mInputs.Request.Source);
        write("// Generated from ");
        write(root);
        write(" by the Ludus LUSL compiler; do not edit.\n");
        const usize slash = root.rfind('/');
        const std::string_view rootDirectory =
            slash == std::string_view::npos ? std::string_view{} : root.substr(0, slash);
        for (const uint16 include : mModule.Includes)
        {
            write("#include \"");
            writeRelativePath(rootDirectory, getFilePath(include));
            write("\"\n");
        }
    }

    /// Write `path` relative to `directory`, both snapshot-relative.
    void writeRelativePath(std::string_view directory, std::string_view path) noexcept
    {
        // Strip the common leading components, then climb out of the rest of `directory`.
        while (directory.empty() == false)
        {
            const usize directorySlash = directory.find('/');
            const std::string_view head = directory.substr(0, directorySlash);
            const usize pathSlash = path.find('/');
            if (pathSlash == std::string_view::npos || path.substr(0, pathSlash) != head)
            {
                break;
            }
            path = path.substr(pathSlash + 1);
            directory =
                directorySlash == std::string_view::npos ? std::string_view{} : directory.substr(directorySlash + 1);
        }
        while (directory.empty() == false)
        {
            write("../");
            const usize directorySlash = directory.find('/');
            directory =
                directorySlash == std::string_view::npos ? std::string_view{} : directory.substr(directorySlash + 1);
        }
        write(path);
    }

    /// Mark array types that helper signatures spell, with the arrays nested in them.
    void markArray(uint32 type) noexcept
    {
        while (type != NO_TYPE && getType(type).Kind == TypeKind::Array)
        {
            mIsAliasNeeded[type] = 1;
            type = getType(type).Element;
        }
    }

    void writeArrayAliases() noexcept
    {
        if (mIsAliasNeeded.TryResize(mInputs.Model.Types.GetSize(), 0) == false)
        {
            mSink.ReportOutOfMemory();
            return;
        }
        for (const IrHelper& helper : mModule.Helpers)
        {
            markArray(helper.Type);
            if (helper.Kind == IrHelperKind::Initializer)
            {
                for (uint32 k = 0; k < helper.FieldCount; ++k)
                {
                    markArray(getSymbol(mModule.HelperFields[helper.FirstField + k]).Type);
                }
            }
            else if (helper.Kind == IrHelperKind::CallAdapter)
            {
                const Symbol& function = getSymbol(helper.Symbol);
                for (uint32 k = 0; k < helper.FieldCount; ++k)
                {
                    const uint32 parameter =
                        mInputs.Model.Members[function.FirstMember + mModule.HelperFields[helper.FirstField + k]];
                    markArray(getSymbol(parameter).Type);
                }
            }
        }
        for (uint32 type = 0; type < mIsAliasNeeded.GetSize(); ++type)
        {
            if (mIsAliasNeeded[type] == 0)
            {
                continue;
            }
            write("typealias _luslArray");
            writeUnsigned(type);
            write(" = ");
            writeType(getType(type).Element);
            write("[");
            writeUnsigned(getType(type).Count);
            write("];\n");
        }
    }

    void writeHelperName(uint32 helper) noexcept
    {
        switch (mModule.Helpers[helper].Kind)
        {
            case IrHelperKind::Zero:
                write("_luslZero");
                break;
            case IrHelperKind::Initializer:
                write("_luslInit");
                break;
            case IrHelperKind::CheckIndex:
                write("_luslCheckIndex");
                break;
            case IrHelperKind::CallAdapter:
                write("_luslCall");
                break;
            default:
                write("_luslWrap");
                break;
        }
        writeUnsigned(helper);
    }

    void writeHelpers() noexcept
    {
        for (uint32 helper = 0; helper < mModule.Helpers.GetSize(); ++helper)
        {
            const IrHelper& entry = mModule.Helpers[helper];
            switch (entry.Kind)
            {
                case IrHelperKind::Zero:
                    writeZeroHelper(helper);
                    break;
                case IrHelperKind::Initializer:
                    writeInitializerHelper(helper);
                    break;
                case IrHelperKind::WrapBinary:
                case IrHelperKind::WrapNegate:
                case IrHelperKind::WrapAssign:
                case IrHelperKind::WrapIncrement:
                    writeWrapHelper(helper);
                    break;
                case IrHelperKind::CheckIndex:
                    writeCheckIndexHelper(helper);
                    break;
                case IrHelperKind::CallAdapter:
                    if (getSymbol(entry.Symbol).Parent == NO_SYMBOL)
                    {
                        writeAdapter(helper);
                    }
                    break;
            }
        }
    }

    void writeZeroHelper(uint32 helper) noexcept
    {
        const uint32 type = mModule.Helpers[helper].Type;
        const SemanticType& shape = getType(type);
        writeType(type);
        write(" ");
        writeHelperName(helper);
        write("()\n{\n    ");
        writeType(type);
        write(" value;\n");
        if (shape.Kind == TypeKind::Array)
        {
            write("    for (uint i = 0u; i < ");
            writeUnsigned(shape.Count);
            write("u; ++i)\n    {\n        value[i] = ");
            writeZero(shape.Element);
            write(";\n    }\n");
        }
        else
        {
            const Symbol& declared = getSymbol(shape.Symbol);
            for (uint32 i = 0; i < declared.MemberCount; ++i)
            {
                const uint32 member = mInputs.Model.Members[declared.FirstMember + i];
                if (getSymbol(member).Kind != SymbolKind::Field)
                {
                    continue;
                }
                write("    value.");
                write(getName(member));
                write(" = ");
                writeZero(getSymbol(member).Type);
                write(";\n");
            }
        }
        write("    return value;\n}\n");
    }

    void writeInitializerHelper(uint32 helper) noexcept
    {
        const IrHelper& entry = mModule.Helpers[helper];
        writeType(entry.Type);
        write(" ");
        writeHelperName(helper);
        write("(");
        for (uint32 k = 0; k < entry.FieldCount; ++k)
        {
            write(k == 0 ? "" : ", ");
            writeType(getSymbol(mModule.HelperFields[entry.FirstField + k]).Type);
            write(" _luslParameter");
            writeUnsigned(k);
        }
        write(")\n{\n    ");
        writeType(entry.Type);
        write(" value;\n");
        const Symbol& declared = getSymbol(getType(entry.Type).Symbol);
        uint32 value = entry.Body == NO_IR ? NO_IR : at(entry.Body).FirstChild;
        for (uint32 i = 0; i < declared.MemberCount && value != NO_IR; ++i)
        {
            const uint32 member = mInputs.Model.Members[declared.FirstMember + i];
            if (getSymbol(member).Kind != SymbolKind::Field)
            {
                continue;
            }
            write("    value.");
            write(getName(member));
            write(" = ");
            writeExpression(value, true);
            write(";\n");
            value = at(value).NextSibling;
        }
        write("    return value;\n}\n");
    }

    /// The scalar spelling of an unsigned type with `scalar`'s width.
    [[nodiscard]] static ScalarKind getUnsigned(ScalarKind scalar) noexcept
    {
        switch (scalar)
        {
            case ScalarKind::Int16:
                return ScalarKind::Uint16;
            case ScalarKind::Int64:
                return ScalarKind::Uint64;
            default:
                return ScalarKind::Uint32;
        }
    }

    /// Write `name` reinterpreted as the unsigned type of `type`'s shape.
    void writeAsUnsigned(uint32 type, std::string_view name) noexcept
    {
        const SemanticType& shape = getType(type);
        if (shape.Scalar == ScalarKind::Int32)
        {
            write("asuint(");
        }
        else
        {
            writeShape(getUnsigned(shape.Scalar), shape.Kind == TypeKind::Vector ? shape.Rows : 1);
            write("(");
        }
        write(name);
        write(")");
    }

    /// Write the signed result of unsigned arithmetic `expression` as `type`.
    void writeOpenSigned(uint32 type) noexcept
    {
        const SemanticType& shape = getType(type);
        if (shape.Scalar == ScalarKind::Int32)
        {
            write("asint(");
        }
        else
        {
            writeType(type);
            write("(");
        }
    }

    [[nodiscard]] static std::string_view spellOperator(Punctuator operation) noexcept
    {
        switch (operation)
        {
            case Punctuator::Plus:
                return "+";
            case Punctuator::Minus:
                return "-";
            case Punctuator::Star:
                return "*";
            case Punctuator::Slash:
                return "/";
            case Punctuator::Percent:
                return "%";
            case Punctuator::Ampersand:
                return "&";
            case Punctuator::Pipe:
                return "|";
            case Punctuator::Caret:
                return "^";
            case Punctuator::Tilde:
                return "~";
            case Punctuator::LessLess:
                return "<<";
            case Punctuator::GreaterGreater:
                return ">>";
            case Punctuator::AmpersandAmpersand:
                return "&&";
            case Punctuator::PipePipe:
                return "||";
            case Punctuator::EqualEqual:
                return "==";
            case Punctuator::ExclamationEqual:
                return "!=";
            case Punctuator::Less:
                return "<";
            case Punctuator::LessEqual:
                return "<=";
            case Punctuator::Greater:
                return ">";
            case Punctuator::GreaterEqual:
                return ">=";
            case Punctuator::Equal:
                return "=";
            case Punctuator::PlusEqual:
                return "+=";
            case Punctuator::MinusEqual:
                return "-=";
            case Punctuator::StarEqual:
                return "*=";
            case Punctuator::SlashEqual:
                return "/=";
            case Punctuator::PercentEqual:
                return "%=";
            case Punctuator::AmpersandEqual:
                return "&=";
            case Punctuator::PipeEqual:
                return "|=";
            case Punctuator::CaretEqual:
                return "^=";
            case Punctuator::LessLessEqual:
                return "<<=";
            case Punctuator::GreaterGreaterEqual:
                return ">>=";
            case Punctuator::PlusPlus:
                return "++";
            case Punctuator::MinusMinus:
                return "--";
            default:
                return {};
        }
    }

    /// The vector type among two operand types, or the first when both are scalars.
    [[nodiscard]] uint32 getResultShape(uint32 first, uint32 second) const noexcept
    {
        return getType(first).Kind == TypeKind::Scalar && second != NO_TYPE && getType(second).Kind != TypeKind::Scalar
                   ? second
                   : first;
    }

    /// Signed wrapping: the operation directly where the target defines it as wrapping (SPIR-V,
    /// DXIL, WGSL), and through unsigned arithmetic on Metal, whose C++ base leaves signed overflow
    /// undefined (docs/architecture/shading-language.md#signed-overflow).
    void writeWrapHelper(uint32 helper) noexcept
    {
        const IrHelper& entry = mModule.Helpers[helper];
        const uint32 result = getResultShape(entry.Type, entry.SecondType);
        const std::string_view operation = spellOperator(entry.Operator);
        const bool isUpdate = entry.Kind == IrHelperKind::WrapAssign || entry.Kind == IrHelperKind::WrapIncrement;
        if (isUpdate == true)
        {
            write("void ");
        }
        else
        {
            writeType(result);
            write(" ");
        }
        writeHelperName(helper);
        write("(");
        write(isUpdate == true ? "inout " : "");
        writeType(entry.Type);
        write(entry.Kind == IrHelperKind::WrapAssign || entry.Kind == IrHelperKind::WrapIncrement ? " target" : " a");
        if (entry.Kind == IrHelperKind::WrapBinary || entry.Kind == IrHelperKind::WrapAssign)
        {
            write(", ");
            writeType(entry.SecondType);
            write(entry.Kind == IrHelperKind::WrapAssign ? " value" : " b");
        }
        write(")\n{\n    __target_switch\n    {\n    case metal:\n        ");
        switch (entry.Kind)
        {
            case IrHelperKind::WrapBinary:
                write("return ");
                writeOpenSigned(result);
                writeAsUnsigned(entry.Type, "a");
                write(" ");
                write(operation);
                write(" ");
                writeAsUnsigned(entry.SecondType, "b");
                write(");\n    default:\n        return a ");
                write(operation);
                write(" b;\n");
                break;
            case IrHelperKind::WrapNegate:
                write("return ");
                writeOpenSigned(result);
                writeLiteral(getUnsignedType(entry.Type), 0);
                write(" - ");
                writeAsUnsigned(entry.Type, "a");
                write(");\n    default:\n        return -a;\n");
                break;
            case IrHelperKind::WrapAssign:
                write("target = ");
                writeOpenSigned(entry.Type);
                writeAsUnsigned(entry.Type, "target");
                write(" ");
                write(operation);
                write(" ");
                writeAsUnsigned(entry.SecondType, "value");
                write(");\n        return;\n    default:\n        target = target ");
                write(operation);
                write(" value;\n        return;\n");
                break;
            default:
                write("target = ");
                writeOpenSigned(entry.Type);
                writeAsUnsigned(entry.Type, "target");
                write(" ");
                write(operation);
                write(" ");
                writeLiteral(getUnsignedType(getScalarOf(entry.Type)), 1);
                write(");\n        return;\n    default:\n        target = target ");
                write(operation);
                write(" ");
                writeLiteral(getScalarOf(entry.Type), 1);
                write(";\n        return;\n");
                break;
        }
        write("    }\n}\n");
    }

    /// The scalar type index of `type`'s element.
    [[nodiscard]] uint32 getScalarOf(uint32 type) const noexcept
    {
        return findScalarType(getType(type).Scalar);
    }

    /// The scalar unsigned type with the width of `type`'s element.
    [[nodiscard]] uint32 getUnsignedType(uint32 type) const noexcept
    {
        return findScalarType(getUnsigned(getType(type).Scalar));
    }

    [[nodiscard]] uint32 findScalarType(ScalarKind kind) const noexcept
    {
        for (uint32 type = 0; type < mInputs.Model.Types.GetSize(); ++type)
        {
            if (getType(type).Kind == TypeKind::Scalar && getType(type).Scalar == kind)
            {
                return type;
            }
        }
        return NO_TYPE;
    }

    void writeCheckIndexHelper(uint32 helper) noexcept
    {
        const uint32 type = mModule.Helpers[helper].Type;
        writeType(type);
        write(" ");
        writeHelperName(helper);
        write("(");
        writeType(type);
        write(" index, ");
        writeType(type);
        write(" count, uint code)\n{\n    if (index < count)\n    {\n        return index;\n    }\n    if (");
        write(FAILURE_CODE_VARIABLE);
        write(" == 0u)\n    {\n        ");
        write(FAILURE_CODE_VARIABLE);
        write(" = code;\n    }\n    return count - ");
        writeLiteral(type, 1);
        write(";\n}\n");
    }

    /// Method adapters live in extensions of their struct, so they also serve `extern slang` structs.
    void writeMethodAdapters() noexcept
    {
        for (uint32 helper = 0; helper < mModule.Helpers.GetSize(); ++helper)
        {
            const IrHelper& entry = mModule.Helpers[helper];
            if (entry.Kind != IrHelperKind::CallAdapter || getSymbol(entry.Symbol).Parent == NO_SYMBOL)
            {
                continue;
            }
            write("extension ");
            write(getName(getSymbol(entry.Symbol).Parent));
            write("\n{\n");
            writeAdapter(helper);
            write("}\n");
        }
    }

    void writeAdapter(uint32 helper) noexcept
    {
        const IrHelper& entry = mModule.Helpers[helper];
        const Symbol& function = getSymbol(entry.Symbol);
        if (function.IsMutating == true)
        {
            write("[mutating]\n");
        }
        writeType(function.Type);
        write(" ");
        writeHelperName(helper);
        write("(");
        for (uint32 k = 0; k < entry.FieldCount; ++k)
        {
            const uint32 parameter =
                mInputs.Model.Members[function.FirstMember + mModule.HelperFields[entry.FirstField + k]];
            write(k == 0 ? "" : ", ");
            writeDirection(getSymbol(parameter).Direction);
            writeType(getSymbol(parameter).Type);
            write(" _luslParameter");
            writeUnsigned(k);
        }
        write(")\n{\n    ");
        if (function.Type != TypeTable::VOID_TYPE)
        {
            write("return ");
        }
        writeExpression(entry.Body, true);
        write(";\n}\n");
    }

    void writeDirection(ParameterDirection direction) noexcept
    {
        if (direction == ParameterDirection::Out)
        {
            write("out ");
        }
        else if (direction == ParameterDirection::Inout)
        {
            write("inout ");
        }
    }

    // ----- Types and literals --------------------------------------------------------------------

    [[nodiscard]] static std::string_view getScalarSpelling(ScalarKind scalar) noexcept
    {
        switch (scalar)
        {
            case ScalarKind::Bool:
                return "bool";
            case ScalarKind::Int16:
                return "int16_t";
            case ScalarKind::Uint16:
                return "uint16_t";
            case ScalarKind::Int32:
                return "int";
            case ScalarKind::Uint32:
                return "uint";
            case ScalarKind::Int64:
                return "int64_t";
            case ScalarKind::Uint64:
                return "uint64_t";
            case ScalarKind::Float16:
                return "half";
            case ScalarKind::Float32:
                return "float";
            case ScalarKind::Float64:
                return "double";
        }
        return "int";
    }

    /// True for scalars with HLSL vector and matrix names such as `float3` and `uint4x4`.
    [[nodiscard]] static bool hasShortName(ScalarKind scalar, bool isMatrix) noexcept
    {
        return scalar == ScalarKind::Int32 || scalar == ScalarKind::Uint32 || scalar == ScalarKind::Float16 ||
               scalar == ScalarKind::Float32 || scalar == ScalarKind::Float64 ||
               (isMatrix == false && scalar == ScalarKind::Bool);
    }

    /// Write a scalar or vector of `rows` components of `scalar`.
    void writeShape(ScalarKind scalar, uint32 rows) noexcept
    {
        if (rows <= 1)
        {
            write(getScalarSpelling(scalar));
            return;
        }
        if (hasShortName(scalar, false) == true)
        {
            write(getScalarSpelling(scalar));
            writeUnsigned(rows);
            return;
        }
        write("vector<");
        write(getScalarSpelling(scalar));
        write(", ");
        writeUnsigned(rows);
        write(">");
    }

    /// Write a type in value position: arrays use their alias.
    void writeType(uint32 type) noexcept
    {
        const SemanticType& shape = getType(type);
        switch (shape.Kind)
        {
            case TypeKind::Void:
                write("void");
                return;
            case TypeKind::Scalar:
                write(getScalarSpelling(shape.Scalar));
                return;
            case TypeKind::Vector:
                writeShape(shape.Scalar, shape.Rows);
                return;
            case TypeKind::Matrix:
                if (hasShortName(shape.Scalar, true) == true)
                {
                    write(getScalarSpelling(shape.Scalar));
                    writeUnsigned(shape.Rows);
                    write("x");
                    writeUnsigned(shape.Columns);
                    return;
                }
                write("matrix<");
                write(getScalarSpelling(shape.Scalar));
                write(", ");
                writeUnsigned(shape.Rows);
                write(", ");
                writeUnsigned(shape.Columns);
                write(">");
                return;
            case TypeKind::Array:
                write("_luslArray");
                writeUnsigned(type);
                return;
            case TypeKind::Struct:
            case TypeKind::Enum:
                write(getName(shape.Symbol));
                return;
            default:
                write("int");
                return;
        }
    }

    /// Write `type name[dimensions]`, the C declaration form.
    void writeDeclaration(uint32 type, std::string_view name) noexcept
    {
        uint32 element = type;
        while (getType(element).Kind == TypeKind::Array)
        {
            element = getType(element).Element;
        }
        writeType(element);
        write(" ");
        write(name);
        for (uint32 dimension = type; getType(dimension).Kind == TypeKind::Array;
             dimension = getType(dimension).Element)
        {
            write("[");
            writeUnsigned(getType(dimension).Count);
            write("]");
        }
    }

    /// Write the integer `value` as a literal of scalar `type`.
    void writeLiteral(uint32 type, int64 value) noexcept
    {
        const ScalarKind scalar = getType(type).Scalar;
        if (scalar == ScalarKind::Bool)
        {
            write(value != 0 ? "true" : "false");
            return;
        }
        const bool isNegative = value < 0;
        // The magnitude of the most negative int64 does not fit int64; compute it in uint64.
        const uint64 magnitude =
            isNegative == true ? uint64{0} - static_cast<uint64>(value) : static_cast<uint64>(value);
        const bool isFloat = IsFloatScalar(scalar);
        const bool isNarrow = scalar == ScalarKind::Int16 || scalar == ScalarKind::Uint16;
        if (isNarrow == true)
        {
            write(getScalarSpelling(scalar));
            write("(");
        }
        // The most negative value's magnitude is not a literal of its own type: spell -(max) - 1.
        const bool isMostNegative = (scalar == ScalarKind::Int32 && magnitude == INT32_MAGNITUDE_LIMIT) ||
                                    (scalar == ScalarKind::Int64 && magnitude == INT64_MAGNITUDE_LIMIT);
        if (isNegative == true)
        {
            write("(-");
        }
        writeUnsigned(isMostNegative == true ? magnitude - 1 : magnitude);
        if (isFloat == true)
        {
            write(".0");
        }
        writeSuffix(scalar);
        if (isMostNegative == true)
        {
            write(" - 1");
            writeSuffix(scalar);
        }
        if (isNegative == true)
        {
            write(")");
        }
        if (isNarrow == true)
        {
            write(")");
        }
    }

    void writeSuffix(ScalarKind scalar) noexcept
    {
        switch (scalar)
        {
            case ScalarKind::Uint32:
            case ScalarKind::Uint16:
                write("u");
                return;
            case ScalarKind::Int64:
                write("ll");
                return;
            case ScalarKind::Uint64:
                write("ull");
                return;
            case ScalarKind::Float16:
                write("h");
                return;
            case ScalarKind::Float32:
                write("f");
                return;
            case ScalarKind::Float64:
                write("l");
                return;
            default:
                return;
        }
    }

    void writeZero(uint32 type) noexcept
    {
        const SemanticType& shape = getType(type);
        if (shape.Kind == TypeKind::Scalar)
        {
            writeLiteral(type, 0);
            return;
        }
        if (shape.Kind == TypeKind::Vector || shape.Kind == TypeKind::Matrix)
        {
            writeType(type);
            write("(");
            writeLiteral(findScalarType(shape.Scalar), 0);
            write(")");
            return;
        }
        for (uint32 helper = 0; helper < mModule.Helpers.GetSize(); ++helper)
        {
            if (mModule.Helpers[helper].Kind == IrHelperKind::Zero && mModule.Helpers[helper].Type == type)
            {
                writeHelperName(helper);
                write("()");
                return;
            }
        }
        write("0");
    }

    // ----- Declarations --------------------------------------------------------------------------

    void writeFunctionName(uint32 symbol) noexcept
    {
        write(getName(symbol));
    }

    void writeItem(uint32 item) noexcept
    {
        const IrNode& node = at(item);
        switch (node.Kind)
        {
            case IrKind::Struct:
                writeStruct(item);
                return;
            case IrKind::Enum:
                writeEnum(item);
                return;
            case IrKind::Constant:
                mapLine(node.Location);
                write("static const ");
                writeDeclaration(node.Type, getName(node.Symbol));
                write(" = ");
                writeExpression(node.FirstChild, true);
                write(";");
                newline();
                return;
            case IrKind::Groupshared:
                mapLine(node.Location);
                write("groupshared ");
                writeDeclaration(node.Type, getName(node.Symbol));
                write(";");
                newline();
                return;
            case IrKind::Function:
                writeFunction(item);
                return;
            default:
                return;
        }
    }

    void writeStruct(uint32 item) noexcept
    {
        mapLine(at(item).Location);
        write("struct ");
        write(getName(at(item).Symbol));
        newline();
        write("{");
        newline();
        ++mIndent;
        for (uint32 member = at(item).FirstChild; member != NO_IR; member = at(member).NextSibling)
        {
            if (at(member).Kind == IrKind::Field)
            {
                mapLine(at(member).Location);
                writeIndent();
                writeDeclaration(at(member).Type, getName(at(member).Symbol));
                write(";");
                newline();
            }
            else
            {
                writeFunction(member);
            }
        }
        --mIndent;
        write("};");
        newline();
    }

    void writeEnum(uint32 item) noexcept
    {
        const uint32 type = at(item).Type;
        mapLine(at(item).Location);
        write("enum class ");
        write(getName(at(item).Symbol));
        write(" : ");
        write(getScalarSpelling(getType(type).Scalar));
        newline();
        write("{");
        newline();
        const uint32 underlying = findScalarType(getType(type).Scalar);
        for (uint32 enumerator = at(item).FirstChild; enumerator != NO_IR; enumerator = at(enumerator).NextSibling)
        {
            mapLine(at(enumerator).Location);
            write("    ");
            write(getName(at(enumerator).Symbol));
            write(" = ");
            writeLiteral(underlying, at(enumerator).Value);
            write(",");
            newline();
        }
        write("};");
        newline();
    }

    [[nodiscard]] static std::string_view getSemanticSpelling(uint32 semantic) noexcept
    {
        switch (static_cast<IrSemantic>(semantic))
        {
            case IrSemantic::DispatchThreadId:
                return "SV_DispatchThreadID";
            case IrSemantic::GroupThreadId:
                return "SV_GroupThreadID";
            case IrSemantic::GroupId:
                return "SV_GroupID";
            case IrSemantic::GroupIndex:
                return "SV_GroupIndex";
            default:
                return {};
        }
    }

    void writeFunction(uint32 item) noexcept
    {
        const IrNode& node = at(item);
        mapLine(node.Location);
        if ((node.Flags & IR_ENTRY) != 0)
        {
            const IrThreadGroup& group = mModule.ThreadGroups[static_cast<usize>(node.Value)];
            writeIndent();
            write("[shader(\"compute\")]");
            newline();
            writeIndent();
            write("[numthreads(");
            writeUnsigned(group.X);
            write(", ");
            writeUnsigned(group.Y);
            write(", ");
            writeUnsigned(group.Z);
            write(")]");
            newline();
        }
        if ((node.Flags & IR_NOINLINE) != 0)
        {
            writeIndent();
            write("[noinline]");
            newline();
        }
        if ((node.Flags & IR_FORCEINLINE) != 0)
        {
            writeIndent();
            write("[ForceInline]");
            newline();
        }
        if ((node.Flags & IR_MUTATING) != 0)
        {
            writeIndent();
            write("[mutating]");
            newline();
        }
        writeIndent();
        writeType(node.Type);
        write(" ");
        writeFunctionName(node.Symbol);
        write("(");
        bool isFirst = true;
        uint32 body = NO_IR;
        for (uint32 child = node.FirstChild; child != NO_IR; child = at(child).NextSibling)
        {
            if (at(child).Kind == IrKind::Block)
            {
                body = child;
                continue;
            }
            write(isFirst == true ? "" : ", ");
            isFirst = false;
            writeDirection(at(child).Direction);
            writeDeclaration(at(child).Type, getName(at(child).Symbol));
            const std::string_view semantic = getSemanticSpelling(at(child).Index);
            if (semantic.empty() == false)
            {
                write(" : ");
                write(semantic);
            }
        }
        if ((node.Flags & IR_HIDDEN_GROUP_INDEX) != 0)
        {
            write(isFirst == true ? "" : ", ");
            write("uint _luslGroupIndex : SV_GroupIndex");
        }
        write(")");
        newline();
        writeBlock(body);
    }

    // ----- Statements ----------------------------------------------------------------------------

    void writeBlock(uint32 block) noexcept
    {
        writeIndent();
        write("{");
        newline();
        ++mIndent;
        for (uint32 statement = block == NO_IR ? NO_IR : at(block).FirstChild; statement != NO_IR;
             statement = at(statement).NextSibling)
        {
            writeStatement(statement);
        }
        --mIndent;
        writeIndent();
        write("}");
        newline();
    }

    /// Write a statement as a block, wrapping a single statement in braces.
    void writeBody(uint32 statement) noexcept
    {
        if (statement != NO_IR && at(statement).Kind == IrKind::Block)
        {
            writeBlock(statement);
            return;
        }
        writeIndent();
        write("{");
        newline();
        ++mIndent;
        if (statement != NO_IR)
        {
            writeStatement(statement);
        }
        --mIndent;
        writeIndent();
        write("}");
        newline();
    }

    void writeAttributes(const IrNode& node) noexcept
    {
        if ((node.Flags & IR_UNROLL) != 0)
        {
            writeIndent();
            write("[unroll");
            if (node.Value > 0)
            {
                write("(");
                writeUnsigned(static_cast<uint64>(node.Value));
                write(")");
            }
            write("]");
            newline();
        }
        const bool hasOther =
            (node.Flags & IR_LOOP) != 0 || (node.Flags & IR_BRANCH) != 0 || (node.Flags & IR_FLATTEN) != 0;
        if (hasOther == true)
        {
            writeIndent();
            write((node.Flags & IR_LOOP) != 0 ? "[loop]" : ((node.Flags & IR_BRANCH) != 0 ? "[branch]" : "[flatten]"));
            newline();
        }
    }

    /// Write a simple statement without its `;`, as in a `for` header.
    void writeSimpleStatement(uint32 statement) noexcept
    {
        const IrNode& node = at(statement);
        switch (node.Kind)
        {
            case IrKind::Declare:
                writeDeclaration(node.Type, getName(node.Symbol));
                if (node.FirstChild != NO_IR)
                {
                    write(" = ");
                    writeExpression(node.FirstChild, true);
                }
                return;
            case IrKind::Assign: {
                const uint32 target = node.FirstChild;
                const uint32 value = at(target).NextSibling;
                writeExpression(target, true);
                write(" ");
                write(spellOperator(node.Operator));
                write(" ");
                if ((node.Flags & IR_MASK_SHIFT) != 0)
                {
                    writeMaskedCount(target, value);
                }
                else
                {
                    writeExpression(value, true);
                }
                return;
            }
            case IrKind::Increment:
                writeExpression(node.FirstChild, false);
                write(spellOperator(node.Operator));
                return;
            case IrKind::WrappedUpdate:
                writeHelperName(node.Index);
                write("(");
                writeArguments(node.FirstChild);
                write(")");
                return;
            case IrKind::Evaluate:
                writeExpression(node.FirstChild, true);
                return;
            default:
                return;
        }
    }

    void writeStatement(uint32 statement) noexcept
    {
        const IrNode& node = at(statement);
        switch (node.Kind)
        {
            case IrKind::Block:
                writeBlock(statement);
                return;
            case IrKind::Declare:
            case IrKind::Assign:
            case IrKind::Increment:
            case IrKind::WrappedUpdate:
            case IrKind::Evaluate:
                mapLine(node.Location);
                writeIndent();
                writeSimpleStatement(statement);
                write(";");
                newline();
                return;
            case IrKind::If: {
                mapLine(node.Location);
                writeAttributes(node);
                writeIndent();
                write("if (");
                const uint32 condition = node.FirstChild;
                writeExpression(condition, true);
                write(")");
                newline();
                const uint32 whenTrue = at(condition).NextSibling;
                writeBody(whenTrue);
                const uint32 whenFalse = whenTrue == NO_IR ? NO_IR : at(whenTrue).NextSibling;
                if (whenFalse != NO_IR)
                {
                    writeIndent();
                    write("else");
                    newline();
                    writeBody(whenFalse);
                }
                return;
            }
            case IrKind::For: {
                mapLine(node.Location);
                writeAttributes(node);
                const uint32 initializer = node.FirstChild;
                const uint32 condition = at(initializer).NextSibling;
                const uint32 increment = at(condition).NextSibling;
                writeIndent();
                write("for (");
                writeSimpleStatement(initializer);
                write(";");
                if (at(condition).Kind != IrKind::Empty)
                {
                    write(" ");
                    writeExpression(condition, true);
                }
                write(";");
                if (at(increment).Kind != IrKind::Empty)
                {
                    write(" ");
                    writeSimpleStatement(increment);
                }
                write(")");
                newline();
                writeBody(at(increment).NextSibling);
                return;
            }
            case IrKind::While:
                mapLine(node.Location);
                writeAttributes(node);
                writeIndent();
                write("while (");
                writeExpression(node.FirstChild, true);
                write(")");
                newline();
                writeBody(at(node.FirstChild).NextSibling);
                return;
            case IrKind::Do:
                mapLine(node.Location);
                writeAttributes(node);
                writeIndent();
                write("do");
                newline();
                writeBody(node.FirstChild);
                writeIndent();
                write("while (");
                writeExpression(at(node.FirstChild).NextSibling, true);
                write(");");
                newline();
                return;
            case IrKind::Switch:
                writeSwitch(statement);
                return;
            case IrKind::Break:
                mapLine(node.Location);
                writeIndent();
                write("break;");
                newline();
                return;
            case IrKind::Continue:
                mapLine(node.Location);
                writeIndent();
                write("continue;");
                newline();
                return;
            case IrKind::Discard:
                mapLine(node.Location);
                writeIndent();
                write("discard;");
                newline();
                return;
            case IrKind::Return:
                mapLine(node.Location);
                writeIndent();
                write("return");
                if (node.FirstChild != NO_IR)
                {
                    write(" ");
                    writeExpression(node.FirstChild, true);
                }
                write(";");
                newline();
                return;
            case IrKind::ClearGroupshared:
                writeClear(statement);
                return;
            case IrKind::GroupBarrier:
                writeIndent();
                write("GroupMemoryBarrierWithGroupSync();");
                newline();
                return;
            default:
                return;
        }
    }

    void writeSwitch(uint32 statement) noexcept
    {
        const IrNode& node = at(statement);
        mapLine(node.Location);
        writeAttributes(node);
        writeIndent();
        write("switch (");
        writeExpression(node.FirstChild, true);
        write(")");
        newline();
        writeIndent();
        write("{");
        newline();
        for (uint32 clause = at(node.FirstChild).NextSibling; clause != NO_IR; clause = at(clause).NextSibling)
        {
            mapLine(at(clause).Location);
            writeIndent();
            uint32 block = at(clause).FirstChild;
            if ((at(clause).Flags & IR_DEFAULT) != 0)
            {
                write("default:");
            }
            else
            {
                write("case ");
                writeExpression(block, true);
                write(":");
                block = at(block).NextSibling;
            }
            newline();
            ++mIndent;
            for (; block != NO_IR; block = at(block).NextSibling)
            {
                writeBlock(block);
            }
            --mIndent;
        }
        writeIndent();
        write("}");
        newline();
    }

    /// A cooperative clear: each thread zeroes a strided slice of the variable's elements.
    void writeClear(uint32 statement) noexcept
    {
        const IrNode& node = at(statement);
        const uint32 type = node.Type;
        const std::string_view name = getName(node.Symbol);
        uint64 total = 1;
        for (uint32 dimension = type; getType(dimension).Kind == TypeKind::Array;
             dimension = getType(dimension).Element)
        {
            total *= getType(dimension).Count;
            if (total > UINT32_ELEMENTS)
            {
                mSink.ReportError(DiagnosticCode::LoweringLimit,
                                  node.Location,
                                  SourceLocation{},
                                  ComposeMessage("'", name, "' has too many elements to clear"));
                return;
            }
        }
        writeIndent();
        if (getType(type).Kind != TypeKind::Array)
        {
            write("if (");
            writeExpression(node.FirstChild, true);
            write(" == 0u)");
            newline();
            writeIndent();
            write("{");
            newline();
            writeIndent();
            write("    ");
            write(name);
            write(" = ");
            writeZero(type);
            write(";");
            newline();
            writeIndent();
            write("}");
            newline();
            return;
        }
        write("for (uint _luslClear = ");
        writeExpression(node.FirstChild, true);
        write("; _luslClear < ");
        writeUnsigned(total);
        write("u; _luslClear += ");
        writeUnsigned(node.Index);
        write("u)");
        newline();
        writeIndent();
        write("{");
        newline();
        writeIndent();
        write("    ");
        write(name);
        uint64 stride = total;
        uint32 element = type;
        for (uint32 dimension = type; getType(dimension).Kind == TypeKind::Array;
             dimension = getType(dimension).Element)
        {
            const uint64 count = getType(dimension).Count;
            stride /= count;
            write("[(_luslClear / ");
            writeUnsigned(stride);
            write("u) % ");
            writeUnsigned(count);
            write("u]");
            element = getType(dimension).Element;
        }
        write(" = ");
        writeZero(element);
        write(";");
        newline();
        writeIndent();
        write("}");
        newline();
    }

    // ----- Expressions ---------------------------------------------------------------------------

    void writeArguments(uint32 first) noexcept
    {
        for (uint32 argument = first; argument != NO_IR; argument = at(argument).NextSibling)
        {
            write(argument == first ? "" : ", ");
            writeExpression(argument, true);
        }
    }

    /// Write the shift count `value` masked to the bit width of `target`'s element.
    void writeMaskedCount(uint32 target, uint32 value) noexcept
    {
        const ScalarKind scalar = getType(at(target).Type).Scalar;
        uint32 width = INT32_BITS;
        if (scalar == ScalarKind::Int16 || scalar == ScalarKind::Uint16)
        {
            width = INT16_BITS;
        }
        else if (scalar == ScalarKind::Int64 || scalar == ScalarKind::Uint64)
        {
            width = INT64_BITS;
        }
        write("(");
        writeExpression(value, false);
        write(" & ");
        writeLiteral(getScalarOf(at(value).Type), width - 1);
        write(")");
    }

    /// Write an expression; `isDelimited` is true where the context already separates it, such as
    /// an argument or initializer, so no parentheses are needed around an operator.
    void writeExpression(uint32 expression, bool isDelimited) noexcept
    {
        if (expression == NO_IR)
        {
            return;
        }
        const IrNode& node = at(expression);
        switch (node.Kind)
        {
            case IrKind::Literal:
                writeLiteral(node.Type, node.Value);
                return;
            case IrKind::FloatLiteral:
                write(spellToken(node.Token));
                writeSuffix(getType(node.Type).Scalar);
                return;
            case IrKind::Zero:
                writeZero(node.Type);
                return;
            case IrKind::Variable:
                write(getName(node.Symbol));
                return;
            case IrKind::HiddenGroupIndex:
                write("_luslGroupIndex");
                return;
            case IrKind::HelperParameter:
                write("_luslParameter");
                writeUnsigned(node.Index);
                return;
            case IrKind::ThisField:
                write("this.");
                write(getName(node.Symbol));
                return;
            case IrKind::EnumeratorReference:
                write(getName(getSymbol(node.Symbol).Parent));
                write("::");
                write(getName(node.Symbol));
                return;
            case IrKind::Member:
                writeExpression(node.FirstChild, false);
                write(".");
                write(getName(node.Symbol));
                return;
            case IrKind::Swizzle:
                writeExpression(node.FirstChild, false);
                write(".");
                write(spellToken(node.Token));
                return;
            case IrKind::IndexAccess:
                writeIndexAccess(expression);
                return;
            case IrKind::Unary:
                write(isDelimited == true ? "" : "(");
                write(spellOperator(node.Operator));
                writeExpression(node.FirstChild, false);
                write(isDelimited == true ? "" : ")");
                return;
            case IrKind::Binary: {
                const uint32 right = at(node.FirstChild).NextSibling;
                write(isDelimited == true ? "" : "(");
                writeExpression(node.FirstChild, false);
                write(" ");
                write(spellOperator(node.Operator));
                write(" ");
                if ((node.Flags & IR_MASK_SHIFT) != 0)
                {
                    writeMaskedCount(node.FirstChild, right);
                }
                else
                {
                    writeExpression(right, false);
                }
                write(isDelimited == true ? "" : ")");
                return;
            }
            case IrKind::MatrixProduct:
                write("mul(");
                writeArguments(node.FirstChild);
                write(")");
                return;
            case IrKind::WrappedOperation:
            case IrKind::Initialize:
                writeHelperName(node.Index);
                write("(");
                writeArguments(node.FirstChild);
                write(")");
                return;
            case IrKind::Conditional: {
                const uint32 whenTrue = at(node.FirstChild).NextSibling;
                write(isDelimited == true ? "" : "(");
                writeExpression(node.FirstChild, false);
                write(" ? ");
                writeExpression(whenTrue, false);
                write(" : ");
                writeExpression(at(whenTrue).NextSibling, false);
                write(isDelimited == true ? "" : ")");
                return;
            }
            case IrKind::Call:
                writeFunctionName(node.Symbol);
                write("(");
                writeArguments(node.FirstChild);
                write(")");
                return;
            case IrKind::MethodCall:
            case IrKind::AdaptedCall:
                writeMethodCall(expression);
                return;
            case IrKind::IntrinsicCall:
                write(getName(node.Symbol));
                write("(");
                writeArguments(node.FirstChild);
                write(")");
                return;
            case IrKind::Construct:
            case IrKind::Convert:
                writeType(node.Type);
                write("(");
                writeArguments(node.FirstChild);
                write(")");
                return;
            case IrKind::Argument:
                writeExpression(node.FirstChild, true);
                return;
            default:
                return;
        }
    }

    void writeMethodCall(uint32 expression) noexcept
    {
        const IrNode& node = at(expression);
        uint32 argument = node.FirstChild;
        const bool isMethod = getSymbol(node.Symbol).Parent != NO_SYMBOL;
        if ((node.Flags & IR_IMPLICIT_THIS) != 0)
        {
            write("this.");
        }
        else if (isMethod == true)
        {
            writeExpression(argument, false);
            write(".");
            argument = at(argument).NextSibling;
        }
        if (node.Kind == IrKind::AdaptedCall)
        {
            writeHelperName(node.Index);
        }
        else
        {
            writeFunctionName(node.Symbol);
        }
        write("(");
        writeArguments(argument);
        write(")");
    }

    void writeIndexAccess(uint32 expression) noexcept
    {
        const IrNode& node = at(expression);
        const uint32 object = node.FirstChild;
        const uint32 index = at(object).NextSibling;
        writeExpression(object, false);
        write("[");
        if (node.Index == NO_INDEX)
        {
            writeExpression(index, true);
            write("]");
            return;
        }
        const uint32 helper = static_cast<uint32>(node.Value);
        const uint32 indexType = mModule.Helpers[helper].Type;
        const SemanticType& shape = getType(at(object).Type);
        const uint32 count = shape.Kind == TypeKind::Array ? shape.Count : shape.Rows;
        writeHelperName(helper);
        write("(");
        writeType(indexType);
        write("(");
        writeExpression(index, true);
        write("), ");
        writeLiteral(indexType, count);
        write(", ");
        writeUnsigned(node.Index);
        write("u)]");
    }

    /// Largest element count a clear loop counts with a uint32.
    static constexpr uint64 UINT32_ELEMENTS = 0xffffffffU;

    const AnalysisInputs& mInputs;
    const IrModule& mModule;
    DiagnosticSink& mSink;
    EmitResult& mResult;
    TextWriter mWriter;
    Array<foundation::uint8> mIsAliasNeeded;
    uint32 mIndent = 0;
    bool mIsMapped = false;
    uint16 mMappedFile = NO_SOURCE;
    uint32 mMappedLine = 0;
};

// NOLINTEND(bugprone-easily-swappable-parameters)
} // namespace

void EmitModule(const AnalysisInputs& inputs,
                const IrModule& module,
                DiagnosticSink& inoutSink,
                EmitResult& outResult) noexcept
{
    SlangEmitter emitter(inputs, module, inoutSink, outResult);
    emitter.Run();
}
} // namespace ludus::tooling::shader_language::internal
