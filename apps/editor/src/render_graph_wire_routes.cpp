#include "internal/render_graph_wire_routes.h"
#include "internal/render_graph_document.h"

#include <ludus/foundation/math/scalar.hpp>

#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace ludus::editor
{
using namespace foundation;
namespace
{
QString MakeConnectionKey(const QString& from, const QString& to)
{
    return from + QLatin1Char('\n') + to;
}
QSet<QString> GetConnections(const QJsonObject& configuration)
{
    QSet<QString> connections;
    for (const QJsonValueConstRef value : configuration.value(QStringLiteral("edges")).toArray())
    {
        const QJsonObject edge = value.toObject();
        connections.insert(MakeConnectionKey(edge.value(QStringLiteral("from")).toString(),
                                             edge.value(QStringLiteral("to")).toString()));
    }
    for (const QJsonValueConstRef value : configuration.value(QStringLiteral("outputs")).toArray())
    {
        const QJsonObject output = value.toObject();
        if (output.value(QStringLiteral("source")).toString().isEmpty() == false)
        {
            connections.insert(
                MakeConnectionKey(output.value(QStringLiteral("source")).toString(),
                                  QStringLiteral("#outputs.") + output.value(QStringLiteral("id")).toString()));
        }
    }
    return connections;
}
bool IsCoordinate(const QJsonValue& value)
{
    const float64 number = value.toDouble(static_cast<float64>(layout_policy::MAX_COORDINATE) + 1);
    return value.isDouble() == true && math::IsFinite(number) == true && number >= -layout_policy::MAX_COORDINATE &&
           number <= layout_policy::MAX_COORDINATE && number == static_cast<int32>(number);
}
} // namespace

bool AreRenderGraphWireRoutesValid(const QJsonArray& routes,
                                   const QJsonObject& configuration,
                                   bool shouldRequireConnection)
{
    if (routes.size() > layout_policy::MAX_WIRE_ROUTES)
    {
        return false;
    }
    const QString identifier =
        QStringLiteral("[A-Za-z][A-Za-z0-9_-]{0,%1}").arg(RenderGraphDocument::MAX_IDENTIFIER_LENGTH - 1);
    const QRegularExpression sourcePattern(QStringLiteral("\\A%1\\.%1\\z").arg(identifier));
    const QRegularExpression targetPattern(QStringLiteral("\\A(?:%1|#outputs)\\.%1\\z").arg(identifier));
    const QSet<QString> connections = GetConnections(configuration);
    QSet<QString> claimed;
    const QStringList fields{QStringLiteral("from"), QStringLiteral("points"), QStringLiteral("to")};
    const QStringList coordinates{QStringLiteral("x"), QStringLiteral("y")};
    for (const QJsonValueConstRef value : routes)
    {
        const QJsonObject route = value.toObject();
        const QString from = route.value(QStringLiteral("from")).toString();
        const QString to = route.value(QStringLiteral("to")).toString();
        const QString key = MakeConnectionKey(from, to);
        const QJsonValue points = route.value(QStringLiteral("points"));
        if (value.isObject() == false || route.keys() != fields || sourcePattern.match(from).hasMatch() == false ||
            targetPattern.match(to).hasMatch() == false || claimed.contains(key) == true ||
            (shouldRequireConnection == true && connections.contains(key) == false) || points.isArray() == false ||
            points.toArray().isEmpty() == true || points.toArray().size() > layout_policy::MAX_WIRE_REROUTE_POINTS)
        {
            return false;
        }
        claimed.insert(key);
        for (const QJsonValueConstRef pointValue : points.toArray())
        {
            const QJsonObject point = pointValue.toObject();
            if (pointValue.isObject() == false || point.keys() != coordinates ||
                IsCoordinate(point.value(QStringLiteral("x"))) == false ||
                IsCoordinate(point.value(QStringLiteral("y"))) == false)
            {
                return false;
            }
        }
    }
    return true;
}

bool PruneRenderGraphWireRoutes(QJsonObject& inoutConfiguration)
{
    QJsonObject layout = inoutConfiguration.value(QStringLiteral("layout")).toObject();
    const QJsonValue value = layout.value(QStringLiteral("routes"));
    const QJsonArray routes = value.toArray();
    if (value.isArray() == false || AreRenderGraphWireRoutesValid(routes, inoutConfiguration, false) == false)
    {
        return false;
    }
    const QSet<QString> connections = GetConnections(inoutConfiguration);
    QJsonArray kept;
    for (const QJsonValueConstRef routeValue : routes)
    {
        const QJsonObject route = routeValue.toObject();
        if (connections.contains(MakeConnectionKey(route.value(QStringLiteral("from")).toString(),
                                                   route.value(QStringLiteral("to")).toString())) == true)
        {
            kept.append(route);
        }
    }
    if (kept == routes)
    {
        return false;
    }
    layout.insert(QStringLiteral("routes"), kept);
    inoutConfiguration.insert(QStringLiteral("layout"), layout);
    return true;
}
} // namespace ludus::editor
