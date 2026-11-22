#include "internal/render_graph_shader_edits.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace ludus::editor
{
namespace
{
QString MaskShaderComments(const QString& source, bool shouldMaskStrings = false)
{
    static const QRegularExpression TOKENS(
        QStringLiteral(R"token("(?:\\.|[^"\\])*"|/\*[\s\S]*?(?:\*/|\z)|//[^\r\n]*)token"));
    QString text = source;
    QRegularExpressionMatchIterator matches = TOKENS.globalMatch(source);
    while (matches.hasNext() == true)
    {
        const QRegularExpressionMatch match = matches.next();
        if (shouldMaskStrings == false && source.at(match.capturedStart()) != QLatin1Char('/'))
        {
            continue;
        }
        for (qsizetype offset = match.capturedStart(); offset < match.capturedEnd(); ++offset)
        {
            if (source.at(offset) != QLatin1Char('\n') && source.at(offset) != QLatin1Char('\r'))
            {
                text[offset] = QLatin1Char(' ');
            }
        }
    }
    return text;
}
} // namespace

QString GetSharedShaderIncludePath(const QString& target, const QString& header)
{
    const bool isLusl = target.endsWith(QStringLiteral(".lusl")) || target.endsWith(QStringLiteral(".luslh"));
    const bool isSlang = target.endsWith(QStringLiteral(".slang"));
    const QString suffix = isLusl == true ? QStringLiteral(".luslh") : QStringLiteral(".slang");
    if (target == header || (isLusl == false && isSlang == false) || header.endsWith(suffix) == false ||
        header.contains(QLatin1Char('"')) == true || header.contains(QLatin1Char('\\')) == true ||
        header.contains(QLatin1Char('\n')) == true || header.contains(QLatin1Char('\r')) == true)
    {
        return {};
    }
    const QString path = QDir(QFileInfo(QLatin1Char('/') + target).path()).relativeFilePath(QLatin1Char('/') + header);
    if (isSlang == true && path.startsWith(QStringLiteral("../")) == true)
    {
        return {};
    }
    return path;
}

QByteArray InsertShaderInclude(const QByteArray& source, const QString& path)
{
    QString text = QString::fromUtf8(source);
    const bool hasBom = source.startsWith("\xef\xbb\xbf");
    if (hasBom == true && text.startsWith(QChar::ByteOrderMark) == false)
    {
        text.prepend(QChar::ByteOrderMark);
    }
    const QString masked = MaskShaderComments(text);
    const QRegularExpression include(
        QStringLiteral(R"include(^\x{FEFF}?[\t ]*#[\t ]*include[\t ]+"%1"[\t \r]*$)include")
            .arg(QRegularExpression::escape(path)),
        QRegularExpression::MultilineOption);
    if (include.match(masked).hasMatch() == true)
    {
        return source;
    }
    static const QRegularExpression LEADING_PRAGMA(
        QStringLiteral(R"pragma(\A\x{FEFF}?\s*#[\t ]*pragma[\t ]+once[\t ]*\r?\n)pragma"));
    const QRegularExpressionMatch pragma = LEADING_PRAGMA.match(masked);
    const qsizetype offset = pragma.hasMatch() == true ? pragma.capturedEnd() : (hasBom == true ? 1 : 0);
    const QString newline =
        text.contains(QStringLiteral("\r\n")) == true ? QStringLiteral("\r\n") : QStringLiteral("\n");
    // Shader dependency discovery expects the first include after a BOM to start on its own line.
    const QString leadingBreak = hasBom == true && pragma.hasMatch() == false ? newline : QString();
    text.insert(offset, QStringLiteral("%1#include \"%2\"%3").arg(leadingBreak, path, newline));
    return text.toUtf8();
}

QJsonArray FindShaderBufferCallbacks(const QByteArray& manifest, const QString& source)
{
    QJsonArray callbacks;
    const QJsonArray programs = QJsonDocument::fromJson(manifest).object().value(QStringLiteral("programs")).toArray();
    for (const QJsonValueConstRef entry : programs)
    {
        const QJsonObject program = entry.toObject();
        const QString stage = program.value(QStringLiteral("stage")).toString();
        if (program.value(QStringLiteral("source")).toString() == source &&
            program.value(QStringLiteral("output")).toString() == QStringLiteral("Radiance") &&
            program.value(QStringLiteral("input")).toString() == QStringLiteral("RayLaunch") &&
            (stage == QStringLiteral("compute") || stage == QStringLiteral("ray_generation")))
        {
            callbacks.append(program);
        }
    }
    return callbacks;
}

QByteArray AddShaderBufferParameter(const QByteArray& source, const QString& function, bool isLusl, QString& outError)
{
    outError.clear();
    QString text = QString::fromUtf8(source);
    if (source.startsWith("\xef\xbb\xbf") == true && text.startsWith(QChar::ByteOrderMark) == false)
    {
        text.prepend(QChar::ByteOrderMark);
    }
    const QString returnType = isLusl == true ? QStringLiteral("float32x3") : QStringLiteral("float3");
    const QString resourceType = function + QStringLiteral("Resources");
    const QRegularExpression signature(
        QStringLiteral(
            R"signature(\b%1\s+%2\s*\(\s*(?:in\s+)?SceneContext\s+\w+\s*,\s*(?:in\s+)?RayLaunch\s+\w+\s*,\s*inout\s+RayQueryContext\s+\w+((?:\s*,\s*(?:in\s+)?%3\s+\w+)?)(\s*\)\s*\{))signature")
            .arg(returnType, QRegularExpression::escape(function), QRegularExpression::escape(resourceType)));
    const QString masked = MaskShaderComments(text, true);
    QRegularExpressionMatchIterator matches = signature.globalMatch(masked);
    const QRegularExpressionMatch match = matches.hasNext() == true ? matches.next() : QRegularExpressionMatch();
    if (match.hasMatch() == false || matches.hasNext() == true)
    {
        outError = QStringLiteral("Add the final %1%2 resources parameter manually. "
                                  "Ludus could not identify one standard definition of %3.")
                       .arg(isLusl == true ? QStringLiteral("in ") : QString(), resourceType, function);
        if (isLusl == true)
        {
            outError += QStringLiteral(" Add #include \"graph_resources.luslh\" if it is missing.");
        }
        return {};
    }
    if (match.capturedLength(1) == 0)
    {
        text.insert(
            match.capturedStart(2),
            QStringLiteral(", %1%2 resources").arg(isLusl == true ? QStringLiteral("in ") : QString(), resourceType));
    }
    const QByteArray updated = text.toUtf8();
    return isLusl == true ? InsertShaderInclude(updated, QStringLiteral("graph_resources.luslh")) : updated;
}
} // namespace ludus::editor
