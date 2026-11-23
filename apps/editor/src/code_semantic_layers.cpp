#include "internal/code_semantic_layers.h"

#include <ludus/foundation/base/compiler.h>

#include "internal/code_editor.h"
#include "internal/code_structure_view.h"
#include "internal/language_positions.h"

#include <QHash>

// Thanks to Microsoft, Language Server Protocol 3.17, "Semantic Tokens" and
// "Folding Range Request": tokens are five integers relative to the previous
// token, typed through the server's legend, and folding ranges are line spans
// with an optional kind and collapsed text.
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
namespace ludus::editor
{
namespace
{
using namespace foundation;
// Delta line, delta start, length, token type and modifier bits.
constexpr qsizetype SEMANTIC_TOKEN_FIELDS = 5;
constexpr qsizetype DELTA_LINE_FIELD = 0;
constexpr qsizetype DELTA_START_FIELD = 1;
constexpr qsizetype LENGTH_FIELD = 2;
constexpr qsizetype TYPE_FIELD = 3;
constexpr qsizetype MODIFIERS_FIELD = 4;
constexpr qsizetype MODIFIER_BITS = 32;
// A collapsed range's placeholder names at most this many UTF-16 units.
constexpr qsizetype MAX_COLLAPSED_TEXT_UNITS = 100;
} // namespace

bool ReadSemanticTokenRole(const QString& type, bool isReadonly, CodeSyntaxRole& outRole)
{
    // One table maps the standard LSP 3.17 token types onto the design-token syntax
    // roles that tree-sitter captures use. Types without a role keep syntax colors.
    static const QHash<QString, CodeSyntaxRole> ROLES{
        {QStringLiteral("namespace"), CodeSyntaxRole::Namespace},
        {QStringLiteral("type"), CodeSyntaxRole::Type},
        {QStringLiteral("class"), CodeSyntaxRole::Type},
        {QStringLiteral("enum"), CodeSyntaxRole::Type},
        {QStringLiteral("interface"), CodeSyntaxRole::Type},
        {QStringLiteral("struct"), CodeSyntaxRole::Type},
        {QStringLiteral("typeParameter"), CodeSyntaxRole::Type},
        {QStringLiteral("parameter"), CodeSyntaxRole::Parameter},
        {QStringLiteral("variable"), CodeSyntaxRole::Variable},
        {QStringLiteral("property"), CodeSyntaxRole::Property},
        {QStringLiteral("enumMember"), CodeSyntaxRole::Constant},
        {QStringLiteral("event"), CodeSyntaxRole::Property},
        {QStringLiteral("function"), CodeSyntaxRole::Function},
        {QStringLiteral("method"), CodeSyntaxRole::Method},
        {QStringLiteral("macro"), CodeSyntaxRole::Macro},
        {QStringLiteral("keyword"), CodeSyntaxRole::Keyword},
        {QStringLiteral("modifier"), CodeSyntaxRole::Keyword},
        {QStringLiteral("comment"), CodeSyntaxRole::Comment},
        {QStringLiteral("string"), CodeSyntaxRole::String},
        {QStringLiteral("number"), CodeSyntaxRole::Number},
        {QStringLiteral("regexp"), CodeSyntaxRole::String},
        {QStringLiteral("operator"), CodeSyntaxRole::Operator},
        {QStringLiteral("decorator"), CodeSyntaxRole::Macro},
    };
    const QHash<QString, CodeSyntaxRole>::const_iterator found = ROLES.constFind(type);
    if (found == ROLES.cend())
    {
        return false;
    }
    outRole =
        isReadonly == true && found.value() == CodeSyntaxRole::Variable ? CodeSyntaxRole::Constant : found.value();
    return true;
}

QList<CodeSyntaxToken> DecodeSemanticTokens(const QJsonArray& data,
                                            const QJsonObject& legend,
                                            const QString& unit,
                                            LanguagePositionEncoding encoding,
                                            const CodeSemanticSource& source)
{
    const QJsonArray types = legend.value(QStringLiteral("tokenTypes")).toArray();
    const QJsonArray modifiers = legend.value(QStringLiteral("tokenModifiers")).toArray();
    qint64 readonlyMask = 0;
    for (qsizetype index = 0; index < modifiers.size() && index < MODIFIER_BITS; ++index)
    {
        if (modifiers.at(index).toString() == QStringLiteral("readonly"))
        {
            readonlyMask = qint64{1} << index;
        }
    }
    const QList<int32> lines = IndexLanguageLines(unit);
    QList<CodeSyntaxToken> tokens;
    int32 line = 0;
    int32 character = 0;
    int32 previousEnd = 0;
    for (qsizetype index = 0;
         index + SEMANTIC_TOKEN_FIELDS <= data.size() && tokens.size() < CodeSemanticLayers::MAX_SEMANTIC_TOKENS;
         index += SEMANTIC_TOKEN_FIELDS)
    {
        const int32 deltaLine = data.at(index + DELTA_LINE_FIELD).toInt(-1);
        const int32 deltaStart = data.at(index + DELTA_START_FIELD).toInt(-1);
        const int32 length = data.at(index + LENGTH_FIELD).toInt(-1);
        const int32 type = data.at(index + TYPE_FIELD).toInt(-1);
        // Later tokens are relative to this one: a malformed entry ends the decode
        // rather than misplacing everything after it.
        if (deltaLine < 0 || deltaLine > lines.size() - line || deltaStart < 0 || deltaStart > unit.size() ||
            length < 0 || length > unit.size())
        {
            break;
        }
        line += deltaLine;
        character = deltaLine == 0 ? character + deltaStart : deltaStart;
        if (character > unit.size())
        {
            break;
        }
        int32 start = 0;
        int32 end = 0;
        CodeSyntaxRole role = CodeSyntaxRole::Default;
        if (length == 0 || type < 0 || type >= types.size() ||
            GetLanguageOffset(unit, lines, line, character, encoding, start) == false ||
            GetLanguageOffset(unit, lines, line, character + length, encoding, end) == false ||
            ReadSemanticTokenRole(types.at(type).toString(),
                                  (data.at(index + MODIFIERS_FIELD).toInteger(0) & readonlyMask) != 0,
                                  role) == false ||
            start < source.Start || end > source.End || start < previousEnd)
        {
            continue;
        }
        tokens.append(CodeSyntaxToken({start - source.Start, end - source.Start}, role));
        previousEnd = end;
    }
    return tokens;
}

QList<CodeSyntaxFold>
DecodeFoldingRanges(const QJsonArray& ranges, const QString& unit, const CodeSemanticSource& source)
{
    const int32 sourceLastLine =
        static_cast<int32>(QStringView(unit).sliced(source.Start, source.End - source.Start).count(QLatin1Char('\n')));
    QList<CodeSyntaxFold> folds;
    for (const QJsonValue value : ranges)
    {
        if (folds.size() == CodeStructureView::MAX_SERVER_FOLDS)
        {
            break;
        }
        const QJsonObject range = value.toObject();
        const int32 first = range.value(QStringLiteral("startLine")).toInt(-1) - source.FirstLine;
        const int32 last = range.value(QStringLiteral("endLine")).toInt(-1) - source.FirstLine;
        if (first < 0 || last <= first || last > sourceLastLine)
        {
            continue;
        }
        const QString kind = range.value(QStringLiteral("kind")).toString();
        CodeFoldKind foldKind = CodeFoldKind::Syntax;
        if (kind == QStringLiteral("comment"))
        {
            foldKind = CodeFoldKind::Comment;
        }
        else if (kind == QStringLiteral("region"))
        {
            foldKind = CodeFoldKind::Region;
        }
        else if (kind == QStringLiteral("imports"))
        {
            foldKind = CodeFoldKind::Imports;
        }
        folds.append(
        {
            .FirstLine = first,
            .LastLine = last,
            .Kind = foldKind,
            .Label = range.value(QStringLiteral("collapsedText")).toString().left(MAX_COLLAPSED_TEXT_UNITS),
            .IsFromServer = true,
        });
    }
    return folds;
}

CodeSemanticLayers::CodeSemanticLayers(CodeEditor& inoutEditor, LanguageClient& inoutClient)
    : mEditor(inoutEditor), mClient(inoutClient)
{
}

CodeSemanticLayers::Pending& CodeSemanticLayers::getPending(LanguageRequestKind kind) noexcept
{
    return kind == LanguageRequestKind::SemanticTokens ? mTokens : mFolds;
}

void CodeSemanticLayers::Refresh(const CodeDocumentIdentity& identity,
                                 uint64 context,
                                 const QString& unit,
                                 const CodeSemanticSource& source)
{
    for (const LanguageRequestKind kind : {LanguageRequestKind::SemanticTokens, LanguageRequestKind::FoldingRanges})
    {
        Pending& pending = getPending(kind);
        // Background layers never report an unadvertised kind: that would cancel
        // the author's own request with an unavailability message.
        if (mClient.CanRequest(kind) == false)
        {
            pending = {};
            continue;
        }
        pending =
        {
            .Request = mClient.Request(kind, {}),
            .Identity = identity,
            .Context = context,
            .Unit = unit,
            .Source = source,
        };
    }
}

bool CodeSemanticLayers::Receive(int32 request,
                                 LanguageRequestKind kind,
                                 const QJsonValue& value,
                                 const CodeDocumentIdentity& currentIdentity,
                                 uint64 currentContext)
{
    if (kind != LanguageRequestKind::SemanticTokens && kind != LanguageRequestKind::FoldingRanges)
    {
        return false;
    }
    Pending& pending = getPending(kind);
    if (request == 0 || request != pending.Request)
    {
        return true;
    }
    const Pending reply = Move(pending);
    pending = {};
    // A reply describes the text it was asked for. After an edit the shown layer
    // keeps moving with the text until the next synchronization asks again.
    if (reply.Identity != currentIdentity || reply.Context != currentContext)
    {
        return true;
    }
    if (kind == LanguageRequestKind::SemanticTokens)
    {
        const QJsonObject legend = mClient.GetCapabilities()
                                       .value(QStringLiteral("semanticTokensProvider"))
                                       .toObject()
                                       .value(QStringLiteral("legend"))
                                       .toObject();
        mEditor.SetSemanticTokens(DecodeSemanticTokens(value.toObject().value(QStringLiteral("data")).toArray(),
                                                       legend,
                                                       reply.Unit,
                                                       mClient.GetPositionEncoding(),
                                                       reply.Source));
    }
    else
    {
        mEditor.GetStructure().SetServerFolds(DecodeFoldingRanges(value.toArray(), reply.Unit, reply.Source));
    }
    return true;
}

void CodeSemanticLayers::Clear()
{
    for (const LanguageRequestKind kind : {LanguageRequestKind::SemanticTokens, LanguageRequestKind::FoldingRanges})
    {
        Pending& pending = getPending(kind);
        if (pending.Request != 0)
        {
            mClient.Cancel(kind);
        }
        pending = {};
    }
    mEditor.SetSemanticTokens({});
    mEditor.GetStructure().SetServerFolds({});
}
} // namespace ludus::editor
