#include "internal/render_graph_comment_frame.h"
#include "internal/render_graph_document.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

namespace ludus::editor
{
using namespace foundation;
namespace
{
bool IsIntegerWithin(const QJsonValue& value, int32 minimum, int32 maximum)
{
    const float64 number = value.toDouble(static_cast<float64>(maximum) + 1);
    return value.isDouble() == true && math::IsFinite(number) == true && number >= minimum && number <= maximum &&
           number == static_cast<int32>(number);
}
} // namespace

bool AreRenderGraphCommentFramesValid(const QJsonArray& frames)
{
    if (frames.size() > layout_policy::MAX_COMMENT_FRAMES)
    {
        return false;
    }
    const QRegularExpression identity(
        QStringLiteral("\\A[A-Za-z][A-Za-z0-9_-]{0,%1}\\z").arg(RenderGraphDocument::MAX_IDENTIFIER_LENGTH - 1));
    const QRegularExpression colour(QStringLiteral("\\A#[0-9a-fA-F]{6}\\z"));
    const QStringList fields{QStringLiteral("colour"),
                             QStringLiteral("height"),
                             QStringLiteral("id"),
                             QStringLiteral("notes"),
                             QStringLiteral("title"),
                             QStringLiteral("width"),
                             QStringLiteral("x"),
                             QStringLiteral("y")};
    QSet<QString> claimed;
    for (const QJsonValueConstRef value : frames)
    {
        const QJsonObject frame = value.toObject();
        const QString name = frame.value(QStringLiteral("id")).toString();
        const QJsonValue title = frame.value(QStringLiteral("title"));
        const QJsonValue notes = frame.value(QStringLiteral("notes"));
        if (value.isObject() == false || frame.keys() != fields || identity.match(name).hasMatch() == false ||
            claimed.contains(name) == true || title.isString() == false ||
            title.toString().trimmed().isEmpty() == true ||
            title.toString().toUcs4().size() > layout_policy::MAX_COMMENT_TITLE_CODEPOINTS ||
            title.toString().contains(QLatin1Char('\n')) == true ||
            title.toString().contains(QLatin1Char('\r')) == true || notes.isString() == false ||
            notes.toString().toUcs4().size() > layout_policy::MAX_COMMENT_NOTES_CODEPOINTS ||
            colour.match(frame.value(QStringLiteral("colour")).toString()).hasMatch() == false)
        {
            return false;
        }
        claimed.insert(name);
        for (const QString& coordinate : {QStringLiteral("x"), QStringLiteral("y")})
        {
            if (IsIntegerWithin(frame.value(coordinate),
                                -layout_policy::MAX_COORDINATE,
                                layout_policy::MAX_COORDINATE) == false)
            {
                return false;
            }
        }
        if (IsIntegerWithin(frame.value(QStringLiteral("width")),
                            layout_policy::MIN_COMMENT_WIDTH,
                            2 * layout_policy::MAX_COORDINATE) == false ||
            IsIntegerWithin(frame.value(QStringLiteral("height")),
                            layout_policy::MIN_COMMENT_HEIGHT,
                            2 * layout_policy::MAX_COORDINATE) == false)
        {
            return false;
        }
        const QRectF bounds = GetRenderGraphCommentBounds(frame);
        if (bounds.right() > layout_policy::MAX_COORDINATE || bounds.bottom() > layout_policy::MAX_COORDINATE)
        {
            return false;
        }
    }
    return true;
}

QRectF GetRenderGraphCommentBounds(const QJsonObject& frame)
{
    return QRectF(frame.value(QStringLiteral("x")).toInt(),
                  frame.value(QStringLiteral("y")).toInt(),
                  frame.value(QStringLiteral("width")).toInt(),
                  frame.value(QStringLiteral("height")).toInt());
}

QJsonObject SetRenderGraphCommentBounds(QJsonObject frame, const QRectF& bounds)
{
    frame.insert(QStringLiteral("x"), qRound(bounds.x()));
    frame.insert(QStringLiteral("y"), qRound(bounds.y()));
    frame.insert(QStringLiteral("width"), qRound(bounds.width()));
    frame.insert(QStringLiteral("height"), qRound(bounds.height()));
    return frame;
}
} // namespace ludus::editor
