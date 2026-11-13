#include "internal/code_syntax_service.h"

#include <ludus/foundation/base/core.h>

#include "internal/code_file_limits.h"
#include "internal/code_language_registry.h"
#include "internal/code_syntax_grammars.h"
#include "ludus_editor_lusl_queries.h"

#include <QAtomicInteger>
#include <QByteArray>
#include <QByteArrayView>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QStringView>
#include <QSysInfo>
#include <QTextDocument>

#include <climits>
#include <cstdlib>

#if !defined(Q_OS_WASM)
#    include <QThreadPool>
#    include <QtConcurrentRun>
#endif

// Thanks to Max Brunsfeld and the Tree-sitter contributors, "Basic Parsing"
// and "Advanced Parsing", https://tree-sitter.github.io/tree-sitter/using-parsers/.
// The editor reuses an edited tree and walks the concrete syntax tree once to
// extract all products. Parsing Qt's UTF-16 keeps positions in the view's units.
namespace ludus::editor
{
namespace
{
using namespace foundation;
using Role = CodeSyntaxRole;

static_assert(static_cast<uint32>(Role::Count) <= CodeSyntaxToken::ROLE_MASK);
static_assert(CODE_FILE_NORMAL_LIMIT_BYTES <= CodeSyntaxToken::MAXIMUM_END);
static_assert(sizeof(CodeSyntaxToken) == 2 * sizeof(uint32));

constexpr int32 DEBOUNCE_MILLISECONDS = 85;
constexpr int32 SLICE_MILLISECONDS = 2;
constexpr int32 WORK_PER_CLOCK_CHECK = 64;
constexpr int32 MAXIMUM_LABEL_UNITS = 100;
constexpr int32 MAXIMUM_NODES = 400000;
constexpr int32 MAXIMUM_TOKENS = 750000;
constexpr int32 MAXIMUM_FOLDS = 12000;
constexpr int32 MAXIMUM_OPEN_REGIONS = 128;
constexpr int32 TAB_COLUMNS = 4;
constexpr uint32 CODE_UNIT_BYTES = sizeof(QChar);

enum class SyntaxPhase : uint8
{
    ScanLines,
    PrepareEdit,
    Parse,
    CopyPrefix,
    Extract,
    CopySuffix,
    Ready,
};

const TSLanguage* GetGrammarOrNull(CodeLanguage language)
{
    switch (language)
    {
        case CodeLanguage::Slang:
            return tree_sitter_slang();
        case CodeLanguage::Luau:
            return tree_sitter_luau();
        case CodeLanguage::C:
            return tree_sitter_c();
        case CodeLanguage::Cpp:
            return tree_sitter_cpp();
        case CodeLanguage::Json:
            return tree_sitter_json();
        case CodeLanguage::CMake:
            return tree_sitter_cmake();
        case CodeLanguage::Markdown:
            return tree_sitter_markdown();
        case CodeLanguage::Lusl:
            return tree_sitter_lusl();
        case CodeLanguage::PlainText:
        case CodeLanguage::Count:
            return nullptr;
    }
    return nullptr;
}

struct SyntaxQueryProfile final
{
    QByteArray Source;
    QByteArray Roots;
};

SyntaxQueryProfile GetQueryProfile(CodeLanguage language)
{
    switch (language)
    {
        case CodeLanguage::C:
        case CodeLanguage::Cpp:
        case CodeLanguage::Slang:
            return {"(function_declarator declarator: [(identifier) (field_identifier)] @function) "
                    "(parameter_declaration declarator: (identifier) @parameter)",
                    "function_declarator parameter_declaration"};
        case CodeLanguage::Lusl:
            return {QByteArray(LUSL_HIGHLIGHTS_QUERY) + LUSL_FOLDS_QUERY,
                    "function_declarator parameter_declaration field_expression initializer_pair enum_specifier "
                    "attribute_declaration extern_declaration compound_statement field_declaration_list parameter_list "
                    "initializer_list enumerator_list argument_list"};
        case CodeLanguage::Luau:
            return {"(fn_stmt function_name: (name) @function) (local_fn_stmt function_name: (name) @function) "
                    "(param parameter_name: (name) @parameter)",
                    "fn_stmt local_fn_stmt param"};
        case CodeLanguage::Json:
            return {"(pair key: (string) @property)", "pair"};
        case CodeLanguage::CMake:
            return {"(normal_command (identifier) @function)", "normal_command"};
        case CodeLanguage::Markdown:
            return {"[(atx_heading) (setext_heading)] @type", "atx_heading setext_heading"};
        case CodeLanguage::PlainText:
        case CodeLanguage::Count:
            return {};
    }
    return {};
}

uint64 GetCaptureKey(CodeSyntaxRange range)
{
    constexpr uint32 POSITION_BITS = sizeof(uint32) * CHAR_BIT;
    return (static_cast<uint64>(range.Start) << POSITION_BITS) | static_cast<uint64>(range.End);
}

struct TokenRule final
{
    QByteArrayView Type;
    Role Color;
};

constexpr TokenRule TOKEN_RULES[] = {
    {"comment", Role::Comment},
    {"line_comment", Role::Comment},
    {"bracket_comment", Role::Comment},
    {"string", Role::String},
    {"string_literal", Role::String},
    {"raw_string_literal", Role::String},
    {"char_literal", Role::String},
    {"system_lib_string", Role::String},
    {"quoted_argument", Role::String},
    {"bracket_argument", Role::String},
    {"fenced_code_block", Role::String},
    {"code_span", Role::String},
    {"escape_sequence", Role::Escape},
    {"simple_escape", Role::Escape},
    {"backslash_escape", Role::Escape},
    {"dec_byte_escape", Role::Escape},
    {"hex_byte_escape", Role::Escape},
    {"unicode_escape", Role::Escape},
    {"number", Role::Number},
    {"number_literal", Role::Number},
    {"primitive_type", Role::Type},
    {"type_identifier", Role::Type},
    {"type_name", Role::Type},
    {"boolean_literal", Role::Constant},
    {"reserved_keyword", Role::Invalid},
    {"namedtype", Role::Type},
    {"type_qualifier", Role::Keyword},
    {"namespace_identifier", Role::Namespace},
    {"field_identifier", Role::Property},
    {"preproc_directive", Role::Preprocessor},
    {"preproc_arg", Role::Preprocessor},
    {"atx_heading", Role::Type},
    {"setext_heading", Role::Type},
    {"string_interp", Role::String},
    {"boolean", Role::Constant},
    {"code_fence_content", Role::String},
    {"info_string", Role::Type},
    {"if", Role::Control},
    {"else", Role::Control},
    {"elseif", Role::Control},
    {"then", Role::Control},
    {"for", Role::Control},
    {"while", Role::Control},
    {"do", Role::Control},
    {"return", Role::Control},
    {"repeat", Role::Control},
    {"until", Role::Control},
    {"end", Role::Control},
    {"switch", Role::Control},
    {"case", Role::Control},
    {"break", Role::Control},
    {"continue", Role::Control},
    {"default", Role::Control},
    {"goto", Role::Control},
    {"discard", Role::Control},
    {"try", Role::Control},
    {"catch", Role::Control},
    {"throw", Role::Control},
    {"struct", Role::Keyword},
    {"class", Role::Keyword},
    {"enum", Role::Keyword},
    {"union", Role::Keyword},
    {"namespace", Role::Keyword},
    {"template", Role::Keyword},
    {"using", Role::Keyword},
    {"typedef", Role::Keyword},
    {"const", Role::Keyword},
    {"auto", Role::Keyword},
    {"constexpr", Role::Keyword},
    {"static", Role::Keyword},
    {"export", Role::Keyword},
    {"import", Role::Keyword},
    {"interface", Role::Keyword},
    {"let", Role::Keyword},
    {"var", Role::Keyword},
    {"local", Role::Keyword},
    {"function", Role::Keyword},
    {"type", Role::Keyword},
    {"typename", Role::Keyword},
    {"new", Role::Keyword},
    {"delete", Role::Keyword},
    {"public", Role::Keyword},
    {"private", Role::Keyword},
    {"in", Role::Keyword},
    {"out", Role::Keyword},
    {"inout", Role::Keyword},
    {"true", Role::Constant},
    {"false", Role::Constant},
    {"nil", Role::Constant},
    {"null", Role::Constant},
    {"nullptr", Role::Constant},
    {"#include", Role::Preprocessor},
    {"#define", Role::Preprocessor},
    {"#if", Role::Preprocessor},
    {"#ifdef", Role::Preprocessor},
    {"#ifndef", Role::Preprocessor},
    {"#else", Role::Preprocessor},
    {"#elif", Role::Preprocessor},
    {"#endif", Role::Preprocessor},
};

constexpr QByteArrayView FOLD_TYPES[] = {
    "compound_statement",
    "field_declaration_list",
    "declaration_list",
    "enumerator_list",
    "initializer_list",
    "parameter_list",
    "argument_list",
    "block",
    "tbtype",
    "table",
    "tableconstructor",
    "object",
    "array",
    "fn_stmt",
    "local_fn_stmt",
    "type_fn_stmt",
    "if_stmt",
    "for_in_stmt",
    "for_range_stmt",
    "while_stmt",
    "repeat_stmt",
    "do_stmt",
    "function_def",
    "macro_def",
    "if_condition",
    "foreach_loop",
    "while_loop",
    "block_def",
    "section",
    "fenced_code_block",
    "list",
    "block_quote",
    "preproc_if",
    "preproc_ifdef",
};

constexpr QByteArrayView OUTLINE_TYPES[] = {
    "function_definition",
    "namespace_definition",
    "class_specifier",
    "struct_specifier",
    "union_specifier",
    "enum_specifier",
    "interface_specifier",
    "extension_declaration",
    "type_definition",
    "alias_declaration",
    "concept_definition",
    "fn_stmt",
    "local_fn_stmt",
    "type_stmt",
    "type_fn_stmt",
    "function_def",
    "macro_def",
    "section",
    "pair",
};

TSNode GetField(TSNode node, QByteArrayView field)
{
    return ts_node_child_by_field_name(node, field.data(), static_cast<uint32>(field.size()));
}

CodeSyntaxRange GetRange(TSNode node)
{
    return {static_cast<int32>(ts_node_start_byte(node) / CODE_UNIT_BYTES),
            static_cast<int32>(ts_node_end_byte(node) / CODE_UNIT_BYTES)};
}

QString ReadLabel(QStringView source, TSNode node)
{
    if (ts_node_is_null(node) == true)
    {
        return {};
    }
    const CodeSyntaxRange range = GetRange(node);
    if (range.End > source.size())
    {
        return {};
    }
    QStringView label = source.sliced(range.Start, qMin(range.End - range.Start, MAXIMUM_LABEL_UNITS));
    const qsizetype newline = label.indexOf(u'\n');
    if (newline >= 0)
    {
        label = label.first(newline);
    }
    return label.trimmed().toString();
}

QString ReadOutlineLabel(QStringView source, TSNode node, QByteArrayView type)
{
    if (type == "section")
    {
        const TSNode heading = ts_node_named_child(node, 0);
        const QByteArrayView headingType(ts_node_is_null(heading) == true ? nullptr : ts_node_type(heading));
        if (headingType == "atx_heading" || headingType == "setext_heading")
        {
            return ReadLabel(source, heading);
        }
        return {};
    }
    if (type == "function_def" || type == "macro_def")
    {
        const TSNode command = ts_node_named_child(node, 0);
        if (ts_node_is_null(command) == true)
        {
            return {};
        }
        const uint32 count = ts_node_named_child_count(command);
        for (uint32 index = 0; index < count; ++index)
        {
            const TSNode child = ts_node_named_child(command, index);
            if (QByteArrayView(ts_node_type(child)) == "argument_list")
            {
                return ReadLabel(source, ts_node_named_child(child, 0));
            }
        }
        return {};
    }
    if (type == "pair")
    {
        return ReadLabel(source, GetField(node, "key"));
    }
    TSNode name = GetField(node, "name");
    if (type == "fn_stmt" || type == "local_fn_stmt" || type == "type_fn_stmt")
    {
        name = GetField(node, "function_name");
        if (ts_node_is_null(name) == true)
        {
            name = GetField(node, "method_name");
        }
    }
    else if (type == "type_stmt")
    {
        name = GetField(node, "left");
    }
    TSNode declarator = GetField(node, "declarator");
    while (ts_node_is_null(name) == true && ts_node_is_null(declarator) == false)
    {
        const TSNode named = GetField(declarator, "name");
        const TSNode nested = GetField(declarator, "declarator");
        name = ts_node_is_null(named) == false ? named : ts_node_is_null(nested) == true ? declarator : TSNode();
        declarator = nested;
    }
    return ReadLabel(source, name);
}

Role GetIdentifierRole(QByteArrayView owner, QByteArrayView type)
{
    if (type != "identifier" && type != "name" && type != "field_identifier")
    {
        return Role::Default;
    }
    if (owner == "function_declarator" || owner == "call_expression" || owner == "normal_command" ||
        owner == "fn_stmt" || owner == "local_fn_stmt" || owner == "call_stmt")
    {
        return type == "field_identifier" ? Role::Method : Role::Function;
    }
    if (owner == "parameter_declaration" || owner == "paramlist" || owner == "param")
    {
        return Role::Parameter;
    }
    if (owner == "preproc_def" || owner == "preproc_function_def")
    {
        return Role::Macro;
    }
    return type == "field_identifier" ? Role::Property : Role::Variable;
}

Role GetPunctuationRole(QByteArrayView type)
{
    if (type.size() != 1)
    {
        return Role::Default;
    }
    if (QByteArrayView("()[]{}:;,.").contains(type.front()) == true)
    {
        return Role::Punctuation;
    }
    return QByteArrayView("+-*/%=!<>~&|^?").contains(type.front()) == true ? Role::Operator : Role::Default;
}

const char* ReadSource(void* payload, uint32 offset, TSPoint, uint32* outCount)
{
    const QString& text = *static_cast<const QString*>(payload);
    const uint32 byteCount = static_cast<uint32>(text.size()) * CODE_UNIT_BYTES;
    *outCount = offset < byteCount ? byteCount - offset : 0;
    return *outCount == 0 ? nullptr : reinterpret_cast<const char*>(text.constData()) + offset;
}

bool HasSliceExpired(TSParseState* state)
{
    return static_cast<const QElapsedTimer*>(state->payload)->elapsed() >= SLICE_MILLISECONDS;
}

#if !defined(Q_OS_WASM)
QThreadPool& GetSyntaxThreadPool()
{
    static QThreadPool pool;
    pool.setMaxThreadCount(1);
    return pool;
}
#endif
} // namespace

class CodeSyntaxParser final
{
public:
    CodeSyntaxParser() : mParser(ts_parser_new()) {}

    ~CodeSyntaxParser()
    {
        if (mHasCursor == true)
        {
            ts_tree_cursor_delete(&mCursor);
        }
        ts_tree_delete(mTreeOrNull);
        ts_query_delete(mQueryOrNull);
        ts_query_cursor_delete(mQueryCursor);
        ts_parser_delete(mInlineParserOrNull);
        ts_parser_delete(mParser);
    }

    void Begin(CodeLanguage language, QString source, uint64 revision, bool isLargeFile)
    {
        mJobClock.start();
        mSource = Move(source);
        mResult = {};
        mResult.Revision = revision;
        mLineStarts = {0};
        mIndentation.clear();
        mRegions.clear();
        mIndentationFolds.clear();
        mParents.clear();
        mCaptureRoles.clear();
        mCaptureFolds.clear();
        mDamage = {};
        mPosition = 0;
        mTokenEnd = 0;
        mLineStart = 0;
        mPreviousContentLine = -1;
        mPreviousIndentation = 0;
        mPrefix = 0;
        mSuffix = 0;
        mPhase = SyntaxPhase::ScanLines;
        mIsLarge = isLargeFile == true || mSource.size() > static_cast<qsizetype>(CODE_FILE_NORMAL_LIMIT_BYTES);
        if (language != mLanguage)
        {
            clearTree();
            mLanguage = language;
            mGrammarOrNull = GetGrammarOrNull(language);
            if (mGrammarOrNull != nullptr && ts_parser_set_language(mParser, mGrammarOrNull) == true)
            {
                buildRules();
            }
            else
            {
                mGrammarOrNull = nullptr;
            }
        }
    }

    [[nodiscard]] bool Advance()
    {
        QElapsedTimer clock;
        clock.start();
        int32 work = 0;
        while (mPhase != SyntaxPhase::Ready)
        {
            switch (mPhase)
            {
                case SyntaxPhase::ScanLines:
                    scanLineChunk();
                    break;
                case SyntaxPhase::PrepareEdit:
                    prepareEditChunk();
                    break;
                case SyntaxPhase::Parse:
                    if (parse(clock) == false)
                    {
                        return false;
                    }
                    break;
                case SyntaxPhase::CopyPrefix:
                    copyDamageProducts(false);
                    break;
                case SyntaxPhase::Extract:
                    extractNode();
                    break;
                case SyntaxPhase::CopySuffix:
                    copyDamageProducts(true);
                    break;
                case SyntaxPhase::Ready:
                    break;
            }
            ++work;
            if (work == WORK_PER_CLOCK_CHECK)
            {
                if (clock.elapsed() >= SLICE_MILLISECONDS)
                {
                    return false;
                }
                work = 0;
            }
        }
        return true;
    }

    void SetRequestedRevision(uint64 revision) noexcept
    {
        mRequestedRevision.storeRelaxed(revision);
    }

    [[nodiscard]] bool IsRequested(uint64 revision) const noexcept
    {
        return mRequestedRevision.loadRelaxed() == revision;
    }

    CodeSyntaxResult TakeResult()
    {
        constexpr int64 NANOSECONDS_PER_MICROSECOND = 1000;
        mResult.WorkerMicroseconds = mJobClock.nsecsElapsed() / NANOSECONDS_PER_MICROSECOND;
        mResult.HasReusedProducts = mDamage.IsReusable;
        mPreviousProducts = mResult;
        return Move(mResult);
    }

    void Abort()
    {
        if (mHasCursor == true)
        {
            ts_tree_cursor_delete(&mCursor);
            mHasCursor = false;
        }
        clearTree();
        mSource.clear();
        mResult = {};
        mPhase = SyntaxPhase::Ready;
    }

private:
    struct ParentFrame final
    {
        int32 NodeIndex;
        CodeSyntaxRange Range;
        Role Color;
        QByteArrayView Type;
    };

    struct Damage final
    {
        CodeSyntaxRange OldRange;
        CodeSyntaxRange NewRange;
        int32 FirstLine = 0;
        int32 LastLine = 0;
        int32 LineDelta = 0;
        int32 UnitDelta = 0;
        qsizetype TokenIndex = 0;
        qsizetype NodeIndex = 1;
        qsizetype FoldIndex = 0;
        int32 SuffixIndexDelta = 0;
        bool HasCandidate = false;
        bool IsReusable = false;
    };

    struct Indentation final
    {
        int32 Line;
        int32 Width;
    };

    struct Region final
    {
        int32 Line;
        QString Label;
    };

    void clearTree()
    {
        ts_parser_reset(mParser);
        ts_tree_delete(mTreeOrNull);
        mTreeOrNull = nullptr;
        mPrevious.clear();
        mPreviousLineStarts.clear();
        mPreviousProducts = {};
    }

    void buildRules()
    {
        if (mLanguage == CodeLanguage::Markdown && mInlineParserOrNull == nullptr)
        {
            mInlineParserOrNull = ts_parser_new();
            if (ts_parser_set_language(mInlineParserOrNull, tree_sitter_markdown_inline()) == false)
            {
                ts_parser_delete(mInlineParserOrNull);
                mInlineParserOrNull = nullptr;
            }
        }
        const SyntaxQueryProfile profile = GetQueryProfile(mLanguage);
        ts_query_delete(mQueryOrNull);
        uint32 errorOffset = 0;
        TSQueryError error = TSQueryErrorNone;
        mQueryOrNull = ts_query_new(mGrammarOrNull,
                                    profile.Source.constData(),
                                    static_cast<uint32>(profile.Source.size()),
                                    &errorOffset,
                                    &error);
        LUDUS_ASSERT(mQueryOrNull != nullptr);
        ts_query_cursor_set_max_start_depth(mQueryCursor, 0);
        const QList<QByteArray> queryRoots = profile.Roots.split(' ');
        const qsizetype symbolCount = ts_language_symbol_count(mGrammarOrNull);
        mQuerySymbols.fill(false, symbolCount);
        mRoles.fill(Role::Default, symbolCount);
        mFoldSymbols.fill(false, symbolCount);
        mOutlineSymbols.fill(false, symbolCount);
        for (qsizetype index = 0; index < symbolCount; ++index)
        {
            const QByteArrayView type(ts_language_symbol_name(mGrammarOrNull, static_cast<TSSymbol>(index)));
            mQuerySymbols[index] = queryRoots.contains(type.toByteArray());
            mRoles[index] = GetPunctuationRole(type);
            for (const TokenRule& rule : TOKEN_RULES)
            {
                if (type == rule.Type)
                {
                    mRoles[index] = rule.Color;
                    break;
                }
            }
            for (QByteArrayView foldType : FOLD_TYPES)
            {
                if (type == foldType)
                {
                    mFoldSymbols[index] = true;
                    break;
                }
            }
            for (QByteArrayView outlineType : OUTLINE_TYPES)
            {
                if (type == outlineType)
                {
                    mOutlineSymbols[index] = true;
                    break;
                }
            }
        }
    }

    void appendFold(QList<CodeSyntaxFold>& inoutFolds, CodeSyntaxFold fold)
    {
        if (fold.FirstLine >= fold.LastLine)
        {
            return;
        }
        if (inoutFolds.size() >= MAXIMUM_FOLDS)
        {
            mResult.HasPartialFolds = true;
            return;
        }
        inoutFolds.append(Move(fold));
    }

    void scanLineChunk()
    {
        // Bound work even for a single huge line, before calling a grammar or lexer.
        constexpr int32 SCAN_CHUNK_UNITS = 4096;
        const int32 end = qMin(static_cast<int32>(mSource.size()), mPosition + SCAN_CHUNK_UNITS);
        const QStringView chunk = QStringView(mSource).sliced(mPosition, end - mPosition);
        qsizetype newline = chunk.indexOf(u'\n');
        while (newline >= 0)
        {
            const int32 position = mPosition + static_cast<int32>(newline);
            scanLine(position);
            mLineStart = position + 1;
            mLineStarts.append(mLineStart);
            newline = chunk.indexOf(u'\n', newline + 1);
        }
        mPosition = end;
        if (mPosition < mSource.size())
        {
            return;
        }
        scanLine(mPosition);
        closeIndentation(0, static_cast<int32>(mLineStarts.size()) - 1);
        mIsLarge = mIsLarge == true || static_cast<usize>(mLineStarts.size()) > CODE_FILE_NORMAL_LIMIT_LINES;
        if (mIsLarge == true || mGrammarOrNull == nullptr)
        {
            clearTree();
            for (CodeSyntaxFold& fold : mIndentationFolds)
            {
                appendFold(mResult.Folds, Move(fold));
            }
            mPhase = SyntaxPhase::Ready;
            return;
        }
        mPhase = SyntaxPhase::PrepareEdit;
    }

    void scanLine(int32 end)
    {
        const int32 length = end - mLineStart;
        if (static_cast<usize>(length) > CODE_LONG_LINE_UTF16_UNITS)
        {
            return;
        }
        const int32 line = static_cast<int32>(mLineStarts.size()) - 1;
        const QStringView text = QStringView(mSource).sliced(mLineStart, length);
        int32 spaces = 0;
        int32 start = 0;
        while (start < text.size() && (text.at(start) == u' ' || text.at(start) == u'\t'))
        {
            spaces += text.at(start) == u'\t' ? TAB_COLUMNS : 1;
            ++start;
        }
        if (start == text.size())
        {
            return;
        }
        scanRegion(text.sliced(start), line);
        if (mIsLarge == false && mGrammarOrNull != nullptr)
        {
            return;
        }
        if (spaces > mPreviousIndentation && mPreviousContentLine >= 0)
        {
            mIndentation.append({mPreviousContentLine, spaces});
        }
        closeIndentation(spaces, mPreviousContentLine);
        mPreviousIndentation = spaces;
        mPreviousContentLine = line;
    }

    void closeIndentation(int32 width, int32 lastLine)
    {
        while (mIndentation.isEmpty() == false && mIndentation.constLast().Width > width)
        {
            const Indentation range = mIndentation.takeLast();
            appendFold(mIndentationFolds, {range.Line, lastLine, CodeFoldKind::Indentation, {}});
        }
    }

    void scanRegion(QStringView text, int32 line)
    {
        const BorrowedView<const char> comment = CodeLanguageRegistry::Get().Describe(mLanguage).LineComment;
        const QLatin1StringView prefix(comment.GetDataOrNull(), static_cast<qsizetype>(comment.GetSize()));
        if (prefix.isEmpty() == false && text.startsWith(prefix) == true)
        {
            text = text.sliced(prefix.size()).trimmed();
        }
        else if ((mLanguage == CodeLanguage::C || mLanguage == CodeLanguage::Cpp) &&
                 text.startsWith(u"#pragma ") == true)
        {
            constexpr QStringView PRAGMA = u"#pragma ";
            text = text.sliced(PRAGMA.size()).trimmed();
        }
        else
        {
            return;
        }
        constexpr QStringView REGION = u"#region";
        constexpr QStringView END_REGION = u"#endregion";
        constexpr QStringView PRAGMA_REGION = u"region";
        constexpr QStringView PRAGMA_END_REGION = u"endregion";
        if (text == END_REGION || text == PRAGMA_END_REGION)
        {
            if (mRegions.isEmpty() == false)
            {
                Region opening = mRegions.takeLast();
                appendFold(mResult.Folds, {opening.Line, line, CodeFoldKind::Region, Move(opening.Label)});
            }
        }
        else
        {
            const QStringView marker = text.startsWith(REGION) == true ? REGION : PRAGMA_REGION;
            if (text.startsWith(marker) == true &&
                (text.size() == marker.size() || text.at(marker.size()).isSpace() == true))
            {
                if (mRegions.size() < MAXIMUM_OPEN_REGIONS)
                {
                    mRegions.append({line,
                                     text.sliced(marker.size())
                                         .trimmed()
                                         .first(qMin(text.sliced(marker.size()).trimmed().size(),
                                                     static_cast<qsizetype>(MAXIMUM_LABEL_UNITS)))
                                         .toString()});
                }
                else
                {
                    mResult.HasPartialFolds = true;
                }
            }
        }
    }

    static TSPoint GetPoint(const QList<int32>& starts, int32 position)
    {
        qsizetype first = 0;
        qsizetype last = starts.size();
        while (first + 1 < last)
        {
            const qsizetype middle = first + (last - first) / 2;
            if (starts.at(middle) <= position)
            {
                first = middle;
            }
            else
            {
                last = middle;
            }
        }
        return {static_cast<uint32>(first), static_cast<uint32>(position - starts.at(first)) * CODE_UNIT_BYTES};
    }

    static TSNode GetRootChild(TSNode root, int32 position)
    {
        const uint32 offset = static_cast<uint32>(position) * CODE_UNIT_BYTES;
        TSNode child = ts_node_named_descendant_for_byte_range(root, offset, offset);
        while (ts_node_is_null(child) == false && ts_node_eq(child, root) == false)
        {
            const TSNode parent = ts_node_parent(child);
            if (ts_node_eq(parent, root) == true)
            {
                return child;
            }
            child = parent;
        }
        return {};
    }

    void locateDamage(int32 oldEnd, int32 newEnd)
    {
        if (mPreviousProducts.Nodes.isEmpty() == true || mPreviousProducts.Nodes.size() >= MAXIMUM_NODES ||
            mPreviousProducts.Tokens.size() >= MAXIMUM_TOKENS)
        {
            return;
        }
        const TSNode root = ts_tree_root_node(mTreeOrNull);
        if (ts_node_has_error(root) == true)
        {
            return;
        }
        const TSNode child = GetRootChild(root, mPrefix);
        if (ts_node_is_null(child) == true)
        {
            return;
        }
        const CodeSyntaxRange range = GetRange(child);
        const TSPoint first = ts_node_start_point(child);
        const TSPoint last = ts_node_end_point(child);
        const qsizetype nextLine = static_cast<qsizetype>(last.row) + 1;
        const int32 lineEnd = nextLine < mPreviousLineStarts.size() ? mPreviousLineStarts.at(nextLine) - 1
                                                                    : static_cast<int32>(mPrevious.size());
        // Whole-line boundaries keep adjacent declaration folds and captures
        // outside the replacement. Edits across declarations take the full path.
        if (range.Start >= mPrefix || oldEnd >= range.End || first.column != 0 || range.End != lineEnd)
        {
            return;
        }
        mDamage.OldRange = range;
        mDamage.UnitDelta = newEnd - oldEnd;
        mDamage.NewRange = {range.Start, range.End + mDamage.UnitDelta};
        mDamage.FirstLine = static_cast<int32>(first.row);
        mDamage.LastLine = static_cast<int32>(last.row);
        mDamage.LineDelta = static_cast<int32>(mLineStarts.size() - mPreviousLineStarts.size());
        mDamage.HasCandidate = true;
    }

    TSNode validateDamage(TSTree* nextTree, TSNode root)
    {
        if (mDamage.HasCandidate == false || ts_node_has_error(root) == true)
        {
            return {};
        }
        const TSNode child = GetRootChild(root, mPrefix);
        if (ts_node_is_null(child) == true || GetRange(child) != mDamage.NewRange)
        {
            return {};
        }
        uint32 count = 0;
        TSRange* rangesOrNull = ts_tree_get_changed_ranges(mTreeOrNull, nextTree, &count);
        const uint32 start = static_cast<uint32>(mDamage.NewRange.Start) * CODE_UNIT_BYTES;
        const uint32 end = static_cast<uint32>(mDamage.NewRange.End) * CODE_UNIT_BYTES;
        bool isInside = true;
        for (uint32 index = 0; index < count; ++index)
        {
            const TSRange& range = rangesOrNull[index];
            if (range.start_byte < start || range.end_byte > end)
            {
                isInside = false;
                break;
            }
        }
        // Tree-sitter returns this array from its C allocator (api.h).
        std::free(rangesOrNull);
        return isInside == true ? child : TSNode();
    }

    void appendOldNode(const CodeSyntaxNode& old, int32 indexDelta, int32 unitDelta)
    {
        if (mResult.Nodes.size() >= MAXIMUM_NODES)
        {
            return;
        }
        CodeSyntaxNode node = old;
        node.Range.Start += unitDelta;
        node.Range.End += unitDelta;
        if (node.Parent > 0)
        {
            node.Parent += indexDelta;
        }
        if (node.LabelIndex >= 0)
        {
            mResult.Outline.append(static_cast<int32>(mResult.Nodes.size()));
            node.LabelIndex = static_cast<int32>(mResult.Labels.size());
            mResult.Labels.append(mPreviousProducts.Labels.at(old.LabelIndex));
        }
        mResult.Nodes.append(node);
    }

    void copyDamageProducts(bool isSuffix)
    {
        // Copy a bounded chunk per pass so the serial browser scheduler can
        // yield even when an edit moves the offsets of most of the document.
        constexpr qsizetype COPY_BATCH = 256;
        qsizetype count = 0;
        while (mDamage.TokenIndex < mPreviousProducts.Tokens.size() && count < COPY_BATCH)
        {
            const CodeSyntaxToken token = mPreviousProducts.Tokens.at(mDamage.TokenIndex);
            if (isSuffix == false && token.GetEnd() > mDamage.OldRange.Start)
            {
                if (token.GetStart() < mDamage.OldRange.Start)
                {
                    appendToken({qMax(token.GetStart(), mTokenEnd), mDamage.OldRange.Start}, token.GetRole());
                }
                break;
            }
            ++mDamage.TokenIndex;
            ++count;
            if (isSuffix == false || token.GetEnd() > mDamage.OldRange.End)
            {
                const int32 start = isSuffix == true ? qMax(token.GetStart(), mDamage.OldRange.End) + mDamage.UnitDelta
                                                     : token.GetStart();
                const int32 end = token.GetEnd() + (isSuffix == true ? mDamage.UnitDelta : 0);
                appendToken({start, end}, token.GetRole());
            }
        }
        while (mDamage.NodeIndex < mPreviousProducts.Nodes.size() && count < COPY_BATCH)
        {
            const CodeSyntaxNode& node = mPreviousProducts.Nodes.at(mDamage.NodeIndex);
            if (isSuffix == false && node.Range.Start >= mDamage.OldRange.Start)
            {
                break;
            }
            ++mDamage.NodeIndex;
            ++count;
            if (isSuffix == false || node.Range.Start >= mDamage.OldRange.End)
            {
                appendOldNode(node,
                              isSuffix == true ? mDamage.SuffixIndexDelta : 0,
                              isSuffix == true ? mDamage.UnitDelta : 0);
            }
        }
        if (count >= COPY_BATCH)
        {
            return;
        }
        if (isSuffix == false)
        {
            mTokenEnd = mDamage.NewRange.Start;
            mPhase = SyntaxPhase::Extract;
            return;
        }
        while (mDamage.FoldIndex < mPreviousProducts.Folds.size() && count < COPY_BATCH)
        {
            CodeSyntaxFold fold = mPreviousProducts.Folds.at(mDamage.FoldIndex);
            ++mDamage.FoldIndex;
            ++count;
            if (fold.Kind == CodeFoldKind::Region ||
                (fold.FirstLine <= mDamage.LastLine && fold.LastLine >= mDamage.FirstLine))
            {
                continue;
            }
            if (fold.FirstLine > mDamage.LastLine)
            {
                fold.FirstLine += mDamage.LineDelta;
                fold.LastLine += mDamage.LineDelta;
            }
            appendFold(mResult.Folds, Move(fold));
        }
        if (mDamage.FoldIndex == mPreviousProducts.Folds.size())
        {
            mPhase = SyntaxPhase::Ready;
        }
    }

    void prepareEditChunk()
    {
        if (mTreeOrNull != nullptr)
        {
            constexpr int32 EDIT_SCAN_UNITS = 4096;
            const int32 oldSize = static_cast<int32>(mPrevious.size());
            const int32 newSize = static_cast<int32>(mSource.size());
            const int32 commonSize = qMin(oldSize, newSize);
            int32 work = 0;
            if (commonSize - mPrefix >= EDIT_SCAN_UNITS && QStringView(mPrevious).sliced(mPrefix, EDIT_SCAN_UNITS) ==
                                                               QStringView(mSource).sliced(mPrefix, EDIT_SCAN_UNITS))
            {
                mPrefix += EDIT_SCAN_UNITS;
                return;
            }
            while (mPrefix < commonSize && mPrevious.at(mPrefix) == mSource.at(mPrefix))
            {
                ++mPrefix;
                if (++work == EDIT_SCAN_UNITS)
                {
                    return;
                }
            }
            // An edit at a surrogate boundary must include both UTF-16 code units.
            const int32 suffixLimit = commonSize - mPrefix;
            if (suffixLimit - mSuffix >= EDIT_SCAN_UNITS &&
                QStringView(mPrevious).sliced(oldSize - mSuffix - EDIT_SCAN_UNITS, EDIT_SCAN_UNITS) ==
                    QStringView(mSource).sliced(newSize - mSuffix - EDIT_SCAN_UNITS, EDIT_SCAN_UNITS))
            {
                mSuffix += EDIT_SCAN_UNITS;
                return;
            }
            while (mSuffix < suffixLimit && mPrevious.at(oldSize - mSuffix - 1) == mSource.at(newSize - mSuffix - 1))
            {
                ++mSuffix;
                if (++work == EDIT_SCAN_UNITS)
                {
                    return;
                }
            }
            if (mPrefix < commonSize && mPrefix > 0 && mPrevious.at(mPrefix - 1).isHighSurrogate() == true)
            {
                --mPrefix;
            }
            if (mSuffix > 0 && mPrevious.at(oldSize - mSuffix).isLowSurrogate() == true)
            {
                --mSuffix;
            }
            const int32 oldEnd = oldSize - mSuffix;
            const int32 newEnd = newSize - mSuffix;
            locateDamage(oldEnd, newEnd);
            const TSInputEdit edit
            {
                .start_byte = static_cast<uint32>(mPrefix) * CODE_UNIT_BYTES,
                .old_end_byte = static_cast<uint32>(oldEnd) * CODE_UNIT_BYTES,
                .new_end_byte = static_cast<uint32>(newEnd) * CODE_UNIT_BYTES,
                .start_point = GetPoint(mPreviousLineStarts, mPrefix),
                .old_end_point = GetPoint(mPreviousLineStarts, oldEnd),
                .new_end_point = GetPoint(mLineStarts, newEnd),
            };
            ts_tree_edit(mTreeOrNull, &edit);
        }
        mPhase = SyntaxPhase::Parse;
    }

    bool parse(QElapsedTimer& inoutClock)
    {
        constexpr TSInputEncoding ENCODING =
            QSysInfo::ByteOrder == QSysInfo::LittleEndian ? TSInputEncodingUTF16LE : TSInputEncodingUTF16BE;
        const TSInput input
        {
            .payload = &mSource,
            .read = ReadSource,
            .encoding = ENCODING,
            .decode = nullptr,
        };
        const TSParseOptions options
        {
            .payload = &inoutClock,
            .progress_callback = HasSliceExpired,
        };
        TSTree* treeOrNull = ts_parser_parse_with_options(mParser, mTreeOrNull, input, options);
        if (treeOrNull == nullptr)
        {
            return false;
        }
        const TSNode root = ts_tree_root_node(treeOrNull);
        const TSNode changed = validateDamage(treeOrNull, root);
        ts_tree_delete(mTreeOrNull);
        mTreeOrNull = treeOrNull;
        mPrevious = mSource;
        mPreviousLineStarts = mLineStarts;
        mDamage.IsReusable = ts_node_is_null(changed) == false;
        mCursor = ts_tree_cursor_new(mDamage.IsReusable == true ? changed : root);
        mHasCursor = true;
        if (mDamage.IsReusable == true)
        {
            const CodeSyntaxRange rootRange = GetRange(root);
            mResult.HasPartialFolds = mResult.HasPartialFolds == true || mPreviousProducts.HasPartialFolds == true;
            mResult.Nodes.append({rootRange, -1, -1});
            mResult.Nodes.reserve(mPreviousProducts.Nodes.size());
            mResult.Tokens.reserve(mPreviousProducts.Tokens.size());
            mParents.append({0, rootRange, Role::Default, QByteArrayView(ts_node_type(root))});
            mPhase = SyntaxPhase::CopyPrefix;
        }
        else
        {
            mParents.append({-1, {}, Role::Default, {}});
            mPhase = SyntaxPhase::Extract;
        }
        return true;
    }

    void appendToken(CodeSyntaxRange range, Role role)
    {
        if (range.Start < range.End && role != Role::Default && mResult.Tokens.size() < MAXIMUM_TOKENS)
        {
            if (mResult.Tokens.isEmpty() == false && mResult.Tokens.constLast().GetRole() == role &&
                mResult.Tokens.constLast().GetEnd() == range.Start)
            {
                mResult.Tokens.last() = CodeSyntaxToken({mResult.Tokens.constLast().GetStart(), range.End}, role);
            }
            else
            {
                mResult.Tokens.append({range, role});
            }
        }
        mTokenEnd = qMax(mTokenEnd, range.End);
    }

    void captureQuery(TSNode node)
    {
        if (mQueryOrNull == nullptr)
        {
            return;
        }
        ts_query_cursor_exec(mQueryCursor, mQueryOrNull, node);
        TSQueryMatch match = {};
        while (ts_query_cursor_next_match(mQueryCursor, &match) == true)
        {
            for (uint16 index = 0; index < match.capture_count; ++index)
            {
                const TSQueryCapture capture = match.captures[index];
                uint32 length = 0;
                const char* name = ts_query_capture_name_for_id(mQueryOrNull, capture.index, &length);
                const QByteArrayView kind(name, length);
                const uint64 key = GetCaptureKey(GetRange(capture.node));
                if (kind == "fold")
                {
                    mCaptureFolds.insert(key);
                }
                else
                {
                    const Role role = kind == "function"    ? Role::Function
                                      : kind == "parameter" ? Role::Parameter
                                      : kind == "property"  ? Role::Property
                                      : kind == "keyword"   ? Role::Keyword
                                                            : Role::Type;
                    mCaptureRoles.insert(key, role);
                }
            }
        }
    }

    bool appendMarkdownInline(CodeSyntaxRange range, Role parentRole)
    {
        constexpr int32 INLINE_PARSE_UNITS = 4096;
        if (mInlineParserOrNull == nullptr || range.End - range.Start > INLINE_PARSE_UNITS)
        {
            return false;
        }
        constexpr TSInputEncoding ENCODING =
            QSysInfo::ByteOrder == QSysInfo::LittleEndian ? TSInputEncodingUTF16LE : TSInputEncodingUTF16BE;
        QString text = QString::fromRawData(mSource.constData() + range.Start, range.End - range.Start);
        const TSInput input
        {
            .payload = &text,
            .read = ReadSource,
            .encoding = ENCODING,
            .decode = nullptr,
        };
        QElapsedTimer clock;
        clock.start();
        const TSParseOptions options
        {
            .payload = &clock,
            .progress_callback = HasSliceExpired,
        };
        TSTree* treeOrNull = ts_parser_parse_with_options(mInlineParserOrNull, nullptr, input, options);
        if (treeOrNull == nullptr)
        {
            ts_parser_reset(mInlineParserOrNull);
            return false;
        }
        TSTreeCursor cursor = ts_tree_cursor_new(ts_tree_root_node(treeOrNull));
        bool hasNext = true;
        while (hasNext == true)
        {
            const TSNode node = ts_tree_cursor_current_node(&cursor);
            const QByteArrayView type(ts_node_type(node));
            const Role role = type == "code_span"                                 ? Role::String
                              : type == "link_destination" || type == "link_text" ? Role::Property
                              : type == "emphasis" || type == "strong_emphasis"   ? Role::Keyword
                              : type == "backslash_escape"                        ? Role::Escape
                                                                                  : Role::Default;
            if (role != Role::Default)
            {
                const CodeSyntaxRange local = GetRange(node);
                appendToken({mTokenEnd, range.Start + local.Start}, parentRole);
                appendToken({range.Start + local.Start, range.Start + local.End}, role);
            }
            else if (ts_tree_cursor_goto_first_child(&cursor) == true)
            {
                continue;
            }
            while (ts_tree_cursor_goto_next_sibling(&cursor) == false)
            {
                if (ts_tree_cursor_goto_parent(&cursor) == false)
                {
                    hasNext = false;
                    break;
                }
            }
        }
        ts_tree_cursor_delete(&cursor);
        ts_tree_delete(treeOrNull);
        appendToken({mTokenEnd, range.End}, parentRole);
        return true;
    }

    void extractNode()
    {
        const TSNode node = ts_tree_cursor_current_node(&mCursor);
        const CodeSyntaxRange range = GetRange(node);
        const TSSymbol symbol = ts_node_symbol(node);
        const QByteArrayView type(ts_node_type(node));
        const bool hasSymbol = symbol < mRoles.size();
        const bool isValidRange = range.Start < range.End && range.End <= mSource.size();
        if (isValidRange == true && hasSymbol == true && mQuerySymbols.at(symbol) == true)
        {
            captureQuery(node);
        }
        const uint64 captureKey = GetCaptureKey(range);
        const ParentFrame parentFrame = mParents.constLast();
        int32 parent = parentFrame.NodeIndex;
        if (isValidRange == true && ts_node_is_named(node) == true && mResult.Nodes.size() < MAXIMUM_NODES)
        {
            const bool hasLabel = hasSymbol == true && mOutlineSymbols.at(symbol) == true;
            const QString label = hasLabel == true ? ReadOutlineLabel(mSource, node, type) : QString();
            const int32 index = static_cast<int32>(mResult.Nodes.size());
            int32 labelIndex = -1;
            if (label.isEmpty() == false)
            {
                labelIndex = static_cast<int32>(mResult.Labels.size());
                mResult.Labels.append(label);
                mResult.Outline.append(index);
            }
            mResult.Nodes.append({range, parent, labelIndex});
            parent = index;
        }
        const Role captured = mCaptureRoles.isEmpty() == false ? mCaptureRoles.take(captureKey) : Role::Default;
        Role role = captured != Role::Default ? captured : hasSymbol == true ? mRoles.at(symbol) : Role::Default;
        const bool hasCapturedFold = mCaptureFolds.isEmpty() == false && mCaptureFolds.remove(captureKey) == true;
        if (isValidRange == true && role == Role::Comment &&
            (QStringView(mSource).sliced(range.Start, range.End - range.Start).startsWith(u"///") == true ||
             QStringView(mSource).sliced(range.Start, range.End - range.Start).startsWith(u"/**") == true))
        {
            role = Role::Documentation;
        }
        if (isValidRange == true && (role == Role::Comment || role == Role::Documentation || hasCapturedFold == true ||
                                     (hasSymbol == true && mFoldSymbols.at(symbol) == true)))
        {
            const TSPoint first = ts_node_start_point(node);
            const TSPoint last = ts_node_end_point(node);
            const int32 end = static_cast<int32>(last.row) - (last.column == 0 ? 1 : 0);
            const CodeFoldKind kind =
                role == Role::Comment || role == Role::Documentation ? CodeFoldKind::Comment : CodeFoldKind::Syntax;
            appendFold(mResult.Folds, {static_cast<int32>(first.row), end, kind, {}});
        }
        const bool hasChildren = ts_node_child_count(node) > 0;
        if (isValidRange == true && (role == Role::Default || role == Role::Property) && hasChildren == false)
        {
            const Role identifier = GetIdentifierRole(parentFrame.Type, type);
            if (identifier != Role::Default)
            {
                role = identifier;
            }
            else if (ts_node_is_error(node) == true)
            {
                role = Role::Invalid;
            }
        }
        if (role == Role::Default || (parentFrame.Color == Role::String && role == Role::Punctuation))
        {
            role = parentFrame.Color;
        }
        appendToken({mTokenEnd, range.Start}, parentFrame.Color);
        const bool hasInlineTokens = isValidRange == true && mLanguage == CodeLanguage::Markdown && type == "inline" &&
                                     appendMarkdownInline(range, role) == true;
        const bool isAtomic = role == Role::Comment || role == Role::Documentation || hasInlineTokens == true;
        if (isAtomic == false && hasChildren == true && ts_tree_cursor_goto_first_child(&mCursor) == true)
        {
            mParents.append({parent, range, role, type});
            return;
        }
        if (isValidRange == true)
        {
            appendToken({qMax(range.Start, mTokenEnd), range.End}, role);
        }
        while (ts_tree_cursor_goto_next_sibling(&mCursor) == false)
        {
            if (ts_tree_cursor_goto_parent(&mCursor) == false)
            {
                ts_tree_cursor_delete(&mCursor);
                mHasCursor = false;
                if (mDamage.IsReusable == true)
                {
                    qsizetype suffix = mDamage.NodeIndex;
                    while (suffix < mPreviousProducts.Nodes.size() &&
                           mPreviousProducts.Nodes.at(suffix).Range.Start < mDamage.OldRange.End)
                    {
                        ++suffix;
                    }
                    mDamage.NodeIndex = suffix;
                    mDamage.SuffixIndexDelta = static_cast<int32>(mResult.Nodes.size() - suffix);
                    mPhase = SyntaxPhase::CopySuffix;
                }
                else
                {
                    mPhase = SyntaxPhase::Ready;
                }
                return;
            }
            const ParentFrame completed = mParents.takeLast();
            appendToken({mTokenEnd, completed.Range.End}, completed.Color);
        }
    }

    QAtomicInteger<uint64> mRequestedRevision{1};
    TSParser* mParser;
    TSParser* mInlineParserOrNull = nullptr;
    TSQuery* mQueryOrNull = nullptr;
    TSQueryCursor* mQueryCursor = ts_query_cursor_new();
    QHash<uint64, Role> mCaptureRoles;
    QSet<uint64> mCaptureFolds;
    QList<bool> mQuerySymbols;
    const TSLanguage* mGrammarOrNull = nullptr;
    TSTree* mTreeOrNull = nullptr;
    TSTreeCursor mCursor = {};
    bool mHasCursor = false;
    CodeLanguage mLanguage = CodeLanguage::Count;
    SyntaxPhase mPhase = SyntaxPhase::Ready;
    QElapsedTimer mJobClock;
    CodeSyntaxResult mResult;
    CodeSyntaxResult mPreviousProducts;
    Damage mDamage;
    QString mSource;
    QString mPrevious;
    QList<int32> mLineStarts;
    QList<int32> mPreviousLineStarts;
    QList<Role> mRoles;
    QList<bool> mFoldSymbols;
    QList<bool> mOutlineSymbols;
    QList<ParentFrame> mParents;
    QList<Indentation> mIndentation;
    QList<Region> mRegions;
    QList<CodeSyntaxFold> mIndentationFolds;
    int32 mPosition = 0;
    int32 mTokenEnd = 0;
    int32 mLineStart = 0;
    int32 mPreviousContentLine = -1;
    int32 mPreviousIndentation = 0;
    int32 mPrefix = 0;
    int32 mSuffix = 0;
    bool mIsLarge = false;
};

QString CodeSyntaxResult::GetLabel(const CodeSyntaxNode& node) const
{
    return node.LabelIndex >= 0 ? Labels.at(node.LabelIndex) : QString();
}

int32 CodeSyntaxResult::FindContainingNode(CodeSyntaxRange range) const noexcept
{
    qsizetype first = 0;
    qsizetype last = Nodes.size();
    while (first < last)
    {
        const qsizetype middle = first + (last - first) / 2;
        if (Nodes.at(middle).Range.Start <= range.Start)
        {
            first = middle + 1;
        }
        else
        {
            last = middle;
        }
    }
    int32 index = static_cast<int32>(first) - 1;
    while (index >= 0)
    {
        const CodeSyntaxNode& node = Nodes.at(index);
        const bool hasContainingStart = range.Start < node.Range.End;
        if (hasContainingStart == true && range.End <= node.Range.End)
        {
            return index;
        }
        index = node.Parent;
    }
    return -1;
}

CodeSyntaxService::CodeSyntaxService(QTextDocument* document)
    : QObject(document), mDocument(document), mParser(QSharedPointer<CodeSyntaxParser>::create()),
      mDocumentRevision(document->revision())
{
    LUDUS_ASSERT(document != nullptr);
    mDebounce.setSingleShot(true);
    mDebounce.setInterval(DEBOUNCE_MILLISECONDS);
    connect(&mDebounce, &QTimer::timeout, this, &CodeSyntaxService::submit);
    connect(document, &QTextDocument::contentsChange, this, [this]() {
        const int32 revision = mDocument->revision();
        if (mDocumentRevision != revision)
        {
            mDocumentRevision = revision;
            invalidate();
        }
    });
#if !defined(Q_OS_WASM)
    connect(&mWatcher, &QFutureWatcher<CodeSyntaxResult>::finished, this, [this]() { publish(mWatcher.result()); });
#endif
}

CodeSyntaxService::~CodeSyntaxService()
{
    mParser->SetRequestedRevision(0);
}

void CodeSyntaxService::SetLanguage(CodeLanguage language)
{
    if (mLanguage != language)
    {
        mLanguage = language;
        invalidate();
    }
}

void CodeSyntaxService::SetLargeFileMode(bool isLarge)
{
    if (mIsLargeFile != isLarge)
    {
        mIsLargeFile = isLarge;
        invalidate();
    }
}

const CodeSyntaxResult* CodeSyntaxService::GetCurrentResultOrNull() const noexcept
{
    return mResult.Revision == mRevision ? &mResult : nullptr;
}

void CodeSyntaxService::invalidate()
{
    const bool hasOldResult = mResult.Revision == mRevision;
    ++mRevision;
    mParser->SetRequestedRevision(mRevision);
    mDebounce.start();
    if (hasOldResult == true)
    {
        Q_EMIT ResultChanged();
    }
}

void CodeSyntaxService::submit()
{
    if (mIsRunning == true)
    {
        mHasPendingSnapshot = true;
        return;
    }
    mHasPendingSnapshot = false;
    mIsRunning = true;
    const QString snapshot = mDocument->toRawText().replace(QChar::ParagraphSeparator, u'\n');
    const bool isLarge =
        mIsLargeFile == true || static_cast<usize>(mDocument->blockCount()) > CODE_FILE_NORMAL_LIMIT_LINES;
#if defined(Q_OS_WASM)
    mActiveRevision = mRevision;
    mParser->Begin(mLanguage, snapshot, mActiveRevision, isLarge);
    QTimer::singleShot(0, this, &CodeSyntaxService::advance);
#else
    mWatcher.setFuture(QtConcurrent::run(
        &GetSyntaxThreadPool(),
        [parser = mParser, language = mLanguage, snapshot, revision = mRevision, isLargeFile = isLarge]() {
            if (parser->IsRequested(revision) == false)
            {
                return CodeSyntaxResult();
            }
            parser->Begin(language, snapshot, revision, isLargeFile);
            while (parser->IsRequested(revision) == true)
            {
                if (parser->Advance() == true)
                {
                    return parser->TakeResult();
                }
            }
            parser->Abort();
            return CodeSyntaxResult();
        }));
#endif
}

#if defined(Q_OS_WASM)
void CodeSyntaxService::advance()
{
    if (mActiveRevision != mRevision)
    {
        mParser->Abort();
        publish({});
    }
    else if (mParser->Advance() == true)
    {
        publish(mParser->TakeResult());
    }
    else
    {
        QTimer::singleShot(0, this, &CodeSyntaxService::advance);
    }
}
#endif

void CodeSyntaxService::publish(CodeSyntaxResult result)
{
    mIsRunning = false;
    if (result.Revision == mRevision)
    {
        mResult = Move(result);
        Q_EMIT ResultChanged();
    }
    if (mHasPendingSnapshot == true && mDebounce.isActive() == false)
    {
        submit();
    }
}
} // namespace ludus::editor
