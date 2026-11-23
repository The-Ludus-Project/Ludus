#include "internal/code_navigation_results.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

#include <algorithm>

// Thanks to Microsoft and the LSP contributors, Language Server Protocol 3.17,
// Language Features: Document Symbols, Goto Definition and Find References.
// Handle hierarchical/flat symbols, Location and LocationLink; map only captured sources.
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
namespace ludus::editor
{
namespace
{
using namespace foundation;
constexpr int32 MAX_RESULTS = 256;
constexpr int32 MAX_EXAMINED_RESULTS = 8192;
constexpr int32 MAX_LABEL_UNITS = 2048;
constexpr int32 SINGULAR_COUNT = 1;

QString DescribeLocationCount(int32 count)
{
    if (count == SINGULAR_COUNT)
    {
        return QStringLiteral("1 location");
    }
    return QStringLiteral("%1 locations").arg(count);
}
} // namespace

bool GetCodeProtocolOffset(const QString& text, const QJsonObject& position, foundation::int32& outOffset)
{
    return GetLanguageOffset(text, position, LanguagePositionEncoding::Utf16, outOffset);
}

QList<CodeNavigationTarget> ReadCodeNavigationResults(CodeNavigationKind kind,
                                                      const QJsonValue& value,
                                                      const QString& uri,
                                                      const QString& unit,
                                                      const QString& prefix,
                                                      const QString& source,
                                                      const QString& file,
                                                      const CodeNavigationSources& sources,
                                                      QString& outMessage,
                                                      LanguagePositionEncoding encoding)
{
    using namespace foundation;
    QList<CodeNavigationTarget> result;
    QJsonArray pending =
        value.isArray() == true ? value.toArray() : (value.isObject() == true ? QJsonArray{value} : QJsonArray{});
    QMap<QString, QList<int32>> indexedLines;
    QSet<QString> seen;
    const int32 prefixLines = static_cast<int32>(prefix.count(QLatin1Char('\n')));
    int32 examined = 0;
    int32 rejected = 0;
    while (pending.isEmpty() == false && examined++ < MAX_EXAMINED_RESULTS && result.size() < MAX_RESULTS)
    {
        const QJsonObject item = pending.first().toObject();
        pending.removeFirst();
        if (kind == CodeNavigationKind::Symbols)
        {
            for (const QJsonValue& child : item.value(QStringLiteral("children")).toArray())
            {
                if (pending.size() + examined >= MAX_EXAMINED_RESULTS)
                {
                    break;
                }
                pending.append(child);
            }
        }
        QJsonObject location = item.value(QStringLiteral("location")).toObject();
        if (location.isEmpty() == true)
        {
            location = item;
        }
        const QString targetUri =
            QUrl(location.value(QStringLiteral("targetUri"))
                     .toString(location.value(QStringLiteral("uri"))
                                   .toString(kind == CodeNavigationKind::Symbols &&
                                                     location.contains(QStringLiteral("selectionRange")) == true
                                                 ? uri
                                                 : QString{})))
                .toString();
        const QJsonObject range = location.value(QStringLiteral("targetSelectionRange"))
                                      .toObject(location.value(QStringLiteral("selectionRange"))
                                                    .toObject(location.value(QStringLiteral("range")).toObject()));
        QString text;
        QString targetFile;
        bool isReadOnly = false;
        if (QUrl(targetUri) == QUrl(uri))
        {
            text = unit;
            targetFile = file;
        }
        else if (sources.contains(targetUri) == true)
        {
            text = sources.value(targetUri).Text;
            targetFile = sources.value(targetUri).File;
            isReadOnly = targetFile.isEmpty();
        }
        else
        {
            ++rejected;
            continue;
        }
        if (indexedLines.contains(targetUri) == false)
        {
            indexedLines.insert(targetUri, IndexLanguageLines(text));
        }
        const QList<int32> lines = indexedLines.value(targetUri);
        int32 displayLine = range.value(QStringLiteral("start")).toObject().value(QStringLiteral("line")).toInt() + 1;
        int32 start = 0;
        int32 end = 0;
        if (GetLanguageOffset(text, lines, range.value(QStringLiteral("start")).toObject(), encoding, start) == false ||
            GetLanguageOffset(text, lines, range.value(QStringLiteral("end")).toObject(), encoding, end) == false ||
            end < start)
        {
            ++rejected;
            continue;
        }
        if (QUrl(targetUri) == QUrl(uri))
        {
            const int32 begin = static_cast<int32>(prefix.size());
            if (start >= begin && end <= begin + source.size())
            {
                start -= begin;
                end -= begin;
                text = source;
                displayLine -= prefixLines;
            }
            else
            {
                isReadOnly = true;
                targetFile.clear();
            }
        }
        if (kind == CodeNavigationKind::Symbols && isReadOnly == true)
        {
            continue;
        }
        const QString identity = targetUri + QStringLiteral(":%1:%2").arg(start).arg(end);
        if (seen.contains(identity) == true)
        {
            continue;
        }
        seen.insert(identity);
        QString name = item.value(QStringLiteral("name")).toString().left(MAX_LABEL_UNITS);
        QString label;
        if (isReadOnly == true)
        {
            // A definition in code the compiler captured with the source (generated declarations or a library
            // include) is named by its symbol, where it lives and its line, not by an internal path.
            const QString symbol = text.mid(start, end - start);
            if (name.isEmpty() == true && symbol.isEmpty() == false && symbol.size() <= MAX_LABEL_UNITS &&
                std::ranges::none_of(symbol, [](QChar character) { return character.isSpace(); }))
            {
                name = symbol;
            }
            const QString origin = QUrl(targetUri) == QUrl(uri) || QUrl(targetUri).fileName().isEmpty() == true
                                       ? QStringLiteral("shader context")
                                       : QUrl(targetUri).fileName();
            label = (name.isEmpty() == true ? QString{} : name + QStringLiteral(" — ")) +
                    QStringLiteral("%1, line %2 (read-only)").arg(origin).arg(displayLine);
        }
        else
        {
            label = (name.isEmpty() == true ? QString{} : name + QStringLiteral(" · ")) +
                    (targetFile.isEmpty() == true ? QStringLiteral("Current source") : targetFile) +
                    QStringLiteral(":%1").arg(displayLine);
        }
        result.append({label, targetFile, text, start, end, isReadOnly});
    }
    outMessage =
        result.isEmpty() == true
            ? QStringLiteral("No mapped locations. Check the shader/program context and retry.")
            : QStringLiteral("%1. Enter to jump.").arg(DescribeLocationCount(static_cast<int32>(result.size())));
    if (pending.isEmpty() == false)
    {
        outMessage += QStringLiteral(" Result limit reached; narrow the query.");
    }
    if (rejected > 0)
    {
        outMessage += QStringLiteral(" %1 outside the captured source map or with invalid ranges were omitted.")
                          .arg(DescribeLocationCount(rejected));
    }
    return result;
}
} // namespace ludus::editor
