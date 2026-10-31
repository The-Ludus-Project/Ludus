#include "internal/code_navigation_results.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

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
} // namespace

namespace
{
using ProtocolLineOffsets = QList<foundation::int32>;
ProtocolLineOffsets BuildProtocolLines(const QString& text)
{
    ProtocolLineOffsets lines{0};
    qsizetype position = 0;
    while ((position = text.indexOf(QLatin1Char('\n'), position)) >= 0)
    {
        lines.append(static_cast<foundation::int32>(++position));
    }
    return lines;
}

bool ResolveProtocolOffset(const QString& text,
                           const ProtocolLineOffsets& lines,
                           const QJsonObject& position,
                           foundation::int32& outOffset)
{
    const foundation::int32 line = position.value(QStringLiteral("line")).toInt(-1);
    const foundation::int32 column = position.value(QStringLiteral("character")).toInt(-1);
    if (line < 0 || line >= lines.size() || column < 0)
    {
        return false;
    }
    const foundation::int32 start = lines.at(line);
    const foundation::int32 end =
        line + 1 < lines.size() ? lines.at(line + 1) - 1 : static_cast<foundation::int32>(text.size());
    if (column > end - start)
    {
        return false;
    }
    const foundation::int32 offset = start + column;
    if (offset > 0 && offset < text.size() && text.at(offset).isLowSurrogate() == true)
    {
        return false;
    }
    outOffset = offset;
    return true;
}
} // namespace

bool GetCodeProtocolOffset(const QString& text, const QJsonObject& position, foundation::int32& outOffset)
{
    return ResolveProtocolOffset(text, BuildProtocolLines(text), position, outOffset);
}

QList<CodeNavigationTarget> ReadCodeNavigationResults(CodeNavigationKind kind,
                                                      const QJsonValue& value,
                                                      const QString& uri,
                                                      const QString& unit,
                                                      const QString& prefix,
                                                      const QString& source,
                                                      const QString& file,
                                                      const CodeNavigationSources& sources,
                                                      QString& outMessage)
{
    using namespace foundation;
    QList<CodeNavigationTarget> result;
    QJsonArray pending =
        value.isArray() == true ? value.toArray() : (value.isObject() == true ? QJsonArray{value} : QJsonArray{});
    QMap<QString, ProtocolLineOffsets> indexedLines;
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
            indexedLines.insert(targetUri, BuildProtocolLines(text));
        }
        const ProtocolLineOffsets lines = indexedLines.value(targetUri);
        int32 displayLine = range.value(QStringLiteral("start")).toObject().value(QStringLiteral("line")).toInt() + 1;
        int32 start = 0;
        int32 end = 0;
        if (ResolveProtocolOffset(text, lines, range.value(QStringLiteral("start")).toObject(), start) == false ||
            ResolveProtocolOffset(text, lines, range.value(QStringLiteral("end")).toObject(), end) == false ||
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
        const QString name = item.value(QStringLiteral("name")).toString().left(MAX_LABEL_UNITS);
        const QString path = isReadOnly == true
                                 ? QStringLiteral("Captured shader context (read-only)")
                                 : (targetFile.isEmpty() == true ? QStringLiteral("Current source") : targetFile);
        const QString label = (name.isEmpty() == true ? QString{} : name + QStringLiteral(" · ")) + path +
                              QStringLiteral(":%1").arg(displayLine);
        result.append({label, targetFile, text, start, end, isReadOnly});
    }
    outMessage = result.isEmpty() == true
                     ? QStringLiteral("No mapped locations. Check the shader/program context and retry.")
                     : QStringLiteral("%1 locations. Filter, then Enter to jump; Escape keeps the current view.")
                           .arg(result.size());
    if (pending.isEmpty() == false)
    {
        outMessage += QStringLiteral(" Result limit reached; narrow the query.");
    }
    if (rejected > 0)
    {
        outMessage +=
            QStringLiteral(" %1 locations outside the captured source map or with invalid ranges were omitted.")
                .arg(rejected);
    }
    return result;
}
} // namespace ludus::editor
