#include "internal/render_graph_document.h"
#include "internal/render_graph_comment_frame.h"
#include "internal/render_graph_configuration_template.h"
#include "internal/render_graph_options.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_scene_bundle.h"
#include "internal/render_graph_shader_creation.h"
#include "internal/render_graph_upscale_node.h"
#include "internal/render_graph_wire_routes.h"

#include <ludus/foundation/strings/utf8.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>

#include <cmath>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 INDENT_WIDTH = 2;
constexpr char32_t FIRST_PRINTABLE = 0x20;
constexpr char32_t LAST_PRINTABLE = 0x7E;
constexpr float64 MAX_EXACT_INTEGER = 9007199254740992.0;
const QString LOCK_NAME = QStringLiteral(".ludus-editor-render-graph.lock");

bool IsAuthoredSource(const QString& path)
{
    return path.endsWith(QStringLiteral(".lurender.json")) || path.endsWith(QStringLiteral(".lushader.json")) ||
           path.endsWith(QStringLiteral(".slang"));
}

void AppendString(const QString& text, QByteArray& inoutBytes)
{
    inoutBytes.append('"');
    for (const QChar character : text)
    {
        const char16_t code = character.unicode();
        switch (code)
        {
            case u'"':
                inoutBytes.append("\\\"");
                break;
            case u'\\':
                inoutBytes.append("\\\\");
                break;
            case u'\n':
                inoutBytes.append("\\n");
                break;
            case u'\r':
                inoutBytes.append("\\r");
                break;
            case u'\t':
                inoutBytes.append("\\t");
                break;
            case u'\b':
                inoutBytes.append("\\b");
                break;
            case u'\f':
                inoutBytes.append("\\f");
                break;
            default:
                if (code >= FIRST_PRINTABLE && code <= LAST_PRINTABLE)
                {
                    inoutBytes.append(static_cast<char>(code));
                }
                else
                {
                    // Python's ensure_ascii writes UTF-16 code units in lowercase hex.
                    inoutBytes.append("\\u");
                    inoutBytes.append(QByteArray::number(code, 16).rightJustified(4, '0'));
                }
                break;
        }
    }
    inoutBytes.append('"');
}

void AppendValue(const QJsonValue& value, int32 depth, QByteArray& inoutBytes);

void AppendIndent(int32 depth, QByteArray& inoutBytes)
{
    inoutBytes.append('\n');
    inoutBytes.append(QByteArray(static_cast<qsizetype>(depth) * INDENT_WIDTH, ' '));
}

void AppendObject(const QJsonObject& object, int32 depth, QByteArray& inoutBytes)
{
    if (object.isEmpty() == true)
    {
        inoutBytes.append("{}");
        return;
    }
    inoutBytes.append('{');
    bool isFirst = true;
    // QJsonObject iterates keys in sorted order, matching sort_keys for ASCII identifiers.
    for (QJsonObject::const_iterator item = object.constBegin(); item != object.constEnd(); ++item)
    {
        if (isFirst == false)
        {
            inoutBytes.append(',');
        }
        isFirst = false;
        AppendIndent(depth + 1, inoutBytes);
        AppendString(item.key(), inoutBytes);
        inoutBytes.append(": ");
        AppendValue(item.value(), depth + 1, inoutBytes);
    }
    AppendIndent(depth, inoutBytes);
    inoutBytes.append('}');
}

void AppendValue(const QJsonValue& value, int32 depth, QByteArray& inoutBytes)
{
    switch (value.type())
    {
        case QJsonValue::Null:
        case QJsonValue::Undefined:
            inoutBytes.append("null");
            break;
        case QJsonValue::Bool:
            inoutBytes.append(value.toBool() == true ? "true" : "false");
            break;
        case QJsonValue::Double: {
            const float64 number = value.toDouble();
            if (std::trunc(number) == number && std::fabs(number) < MAX_EXACT_INTEGER)
            {
                inoutBytes.append(QByteArray::number(static_cast<qint64>(number)));
            }
            else
            {
                inoutBytes.append(QByteArray::number(number, 'g', QLocale::FloatingPointShortest));
            }
            break;
        }
        case QJsonValue::String:
            AppendString(value.toString(), inoutBytes);
            break;
        case QJsonValue::Array: {
            const QJsonArray array = value.toArray();
            if (array.isEmpty() == true)
            {
                inoutBytes.append("[]");
                break;
            }
            inoutBytes.append('[');
            for (qsizetype i = 0; i < array.size(); ++i)
            {
                if (i != 0)
                {
                    inoutBytes.append(',');
                }
                AppendIndent(depth + 1, inoutBytes);
                AppendValue(array.at(i), depth + 1, inoutBytes);
            }
            AppendIndent(depth, inoutBytes);
            inoutBytes.append(']');
            break;
        }
        case QJsonValue::Object:
            AppendObject(value.toObject(), depth, inoutBytes);
            break;
    }
}

bool ReadBounded(const QString& path, QByteArray& outBytes)
{
    QFile file(path);
    if (QFileInfo(path).isSymLink() == true || file.open(QIODevice::ReadOnly) == false)
    {
        return false;
    }
    QByteArray bytes = file.read(RenderGraphDocument::MAX_FILE_BYTES + 1);
    if (bytes.size() > RenderGraphDocument::MAX_FILE_BYTES)
    {
        return false;
    }
    outBytes = bytes;
    return true;
}

qsizetype FindConfiguration(const QJsonArray& configurations, const QString& identity)
{
    for (qsizetype i = 0; i < configurations.size(); ++i)
    {
        if (configurations.at(i).toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return i;
        }
    }
    return -1;
}

qsizetype FindNode(const QJsonArray& nodes, const QString& identity)
{
    for (qsizetype i = 0; i < nodes.size(); ++i)
    {
        if (nodes.at(i).toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return i;
        }
    }
    return -1;
}

bool ReplaceNodeChecked(QJsonObject& inoutConfiguration, const QStringList& arguments, QString* outErrorOrNull)
{
    QJsonArray nodes = inoutConfiguration.value(QStringLiteral("nodes")).toArray();
    QJsonObject node = QJsonDocument::fromJson(arguments.at(0).toUtf8()).object();
    if (node.value(QStringLiteral("kind")) == QStringLiteral("Upscale"))
    {
        if (node.value(QStringLiteral("algorithm")) == QStringLiteral("metalfx-spatial"))
        {
            if (node.contains(QStringLiteral("color_processing")) == false)
            {
                node.insert(QStringLiteral("color_processing"), QStringLiteral("Linear"));
            }
        }
        else
        {
            node.remove(QStringLiteral("color_processing"));
        }
    }
    const qsizetype index = FindNode(nodes, node.value(QStringLiteral("id")).toString());
    if (index < 0)
    {
        return false;
    }
    const QJsonObject previous = nodes.at(index).toObject();
    const bool isRasterMigration = previous.value(QStringLiteral("kind")).toString() == QStringLiteral("Raster") &&
                                   node.value(QStringLiteral("kind")).toString() == QStringLiteral("GraphicsPipeline");
    if (node.value(QStringLiteral("kind")) != previous.value(QStringLiteral("kind")) && isRasterMigration == false)
    {
        return false;
    }
    const QString previousExtent = previous.value(QStringLiteral("extent")).toString();
    if ((previous.value(QStringLiteral("kind")).toString() == QStringLiteral("Target") &&
         previousExtent == QStringLiteral("RenderSize")) ||
        (previous.value(QStringLiteral("kind")).toString() == QStringLiteral("RenderSize") &&
         (previousExtent == QStringLiteral("Texture") || previousExtent == QStringLiteral("OutputSize"))))
    {
        const QString removedInput = previous.value(QStringLiteral("id")).toString() +
                                     (previousExtent == QStringLiteral("RenderSize")   ? QStringLiteral(".size")
                                      : previousExtent == QStringLiteral("OutputSize") ? QStringLiteral(".output")
                                                                                       : QStringLiteral(".texture"));
        if (node.value(QStringLiteral("extent")).toString() != previousExtent)
        {
            for (const QJsonValueConstRef edge : inoutConfiguration.value(QStringLiteral("edges")).toArray())
            {
                if (edge.toObject().value(QStringLiteral("to")).toString() == removedInput)
                {
                    if (outErrorOrNull != nullptr)
                    {
                        *outErrorOrNull = QStringLiteral("Disconnect %1 before changing its size source. "
                                                         "The connected size policy was preserved.")
                                              .arg(removedInput);
                    }
                    return false;
                }
            }
        }
    }
    if (IsPipelineNode(previous.value(QStringLiteral("kind")).toString()) == true ||
        previous.value(QStringLiteral("kind")).toString() == QStringLiteral("Upscale"))
    {
        const auto getType = [](const QJsonObject& record, const QString& direction, const QString& port) -> QString {
            if (record.value(QStringLiteral("kind")).toString() == QStringLiteral("Upscale"))
            {
                if (direction == QStringLiteral("outputs"))
                {
                    return port == QStringLiteral("color") ? QStringLiteral("ColorTexture") : QString{};
                }
                for (const QJsonValueConstRef input : GetUpscaleInputPorts(record))
                {
                    if (input.toObject().value(QStringLiteral("id")).toString() == port)
                    {
                        return input.toObject().value(QStringLiteral("type")).toString();
                    }
                }
                return {};
            }
            const QJsonArray ports = record.value(direction).toArray();
            for (const QJsonValueConstRef value : ports)
            {
                if (value.toObject().value(QStringLiteral("id")).toString() == port)
                {
                    return value.toObject().value(QStringLiteral("type")).toString();
                }
            }
            return direction == QStringLiteral("inputs") ? GetSceneBundlePortType(record, port) : QString{};
        };
        const QString identity = node.value(QStringLiteral("id")).toString();
        const auto isPreserved = [&node, &previous, &identity, &getType, outErrorOrNull](const QString& endpoint,
                                                                                         const QString& direction) {
            if (endpoint.section(QLatin1Char('.'), 0, 0) != identity)
            {
                return true;
            }
            const QString port = endpoint.section(QLatin1Char('.'), 1);
            const QString type = getType(node, direction, port);
            const QString previousType = getType(previous, direction, port);
            const bool hasCompatibleType = type.isEmpty() == false && type == previousType;
            if (hasCompatibleType == false && outErrorOrNull != nullptr)
            {
                *outErrorOrNull = QStringLiteral("Cannot apply: connected %1 port '%2' (%3) would be removed, renamed "
                                                 "or changed to '%4'. Disconnect its wire or configuration output "
                                                 "before editing this port; then reconnect after applying.")
                                      .arg(direction,
                                           endpoint,
                                           previousType,
                                           type.isEmpty() == true ? QStringLiteral("missing") : type);
            }
            return hasCompatibleType;
        };
        const QJsonArray edges = inoutConfiguration.value(QStringLiteral("edges")).toArray();
        for (const QJsonValueConstRef edge : edges)
        {
            if (isPreserved(edge.toObject().value(QStringLiteral("from")).toString(), QStringLiteral("outputs")) ==
                    false ||
                isPreserved(edge.toObject().value(QStringLiteral("to")).toString(), QStringLiteral("inputs")) == false)
            {
                return false;
            }
        }
        const QJsonArray outputs = inoutConfiguration.value(QStringLiteral("outputs")).toArray();
        for (const QJsonValueConstRef output : outputs)
        {
            if (isPreserved(output.toObject().value(QStringLiteral("source")).toString(), QStringLiteral("outputs")) ==
                false)
            {
                return false;
            }
        }
    }
    nodes.replace(index, node);
    inoutConfiguration.insert(QStringLiteral("nodes"), nodes);
    return true;
}

bool ReplaceNodeEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    return ReplaceNodeChecked(inoutConfiguration, arguments, nullptr);
}

bool AddNodeEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    QJsonArray nodes = inoutConfiguration.value(QStringLiteral("nodes")).toArray();
    const QJsonObject node = QJsonDocument::fromJson(arguments.at(0).toUtf8()).object();
    const QString identity = node.value(QStringLiteral("id")).toString();
    if (identity.isEmpty() == true || identity == QStringLiteral("input") || FindNode(nodes, identity) >= 0)
    {
        return false;
    }
    nodes.append(node);
    inoutConfiguration.insert(QStringLiteral("nodes"), nodes);
    return true;
}

bool AddUpscalePresetEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    return AppendUpscalePreset(inoutConfiguration, arguments.at(0), arguments.at(1));
}

bool IsEndpointOf(const QString& endpoint, const QString& node)
{
    return endpoint.section(QLatin1Char('.'), 0, 0) == node;
}

bool RemoveNodeEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    const QString& identity = arguments.at(0);
    QJsonArray nodes = inoutConfiguration.value(QStringLiteral("nodes")).toArray();
    const qsizetype index = FindNode(nodes, identity);
    if (index < 0)
    {
        return false;
    }
    nodes.removeAt(index);
    inoutConfiguration.insert(QStringLiteral("nodes"), nodes);
    // Removing a node also removes its connections and layout position by stable ID.
    QJsonArray edges;
    const QJsonArray previousEdges = inoutConfiguration.value(QStringLiteral("edges")).toArray();
    for (const QJsonValueConstRef edge : previousEdges)
    {
        const QJsonObject record = edge.toObject();
        if (IsEndpointOf(record.value(QStringLiteral("from")).toString(), identity) == false &&
            IsEndpointOf(record.value(QStringLiteral("to")).toString(), identity) == false)
        {
            edges.append(record);
        }
    }
    inoutConfiguration.insert(QStringLiteral("edges"), edges);
    QJsonArray outputs = inoutConfiguration.value(QStringLiteral("outputs")).toArray();
    for (qsizetype i = 0; i < outputs.size(); ++i)
    {
        QJsonObject output = outputs.at(i).toObject();
        if (IsEndpointOf(output.value(QStringLiteral("source")).toString(), identity) == true)
        {
            output.insert(QStringLiteral("source"), QString());
            outputs.replace(i, output);
        }
    }
    inoutConfiguration.insert(QStringLiteral("outputs"), outputs);
    if (inoutConfiguration.contains(QStringLiteral("layout")) == true)
    {
        QJsonObject layout = inoutConfiguration.value(QStringLiteral("layout")).toObject();
        QJsonArray positions;
        const QJsonArray previousPositions = layout.value(QStringLiteral("positions")).toArray();
        for (const QJsonValueConstRef position : previousPositions)
        {
            if (position.toObject().value(QStringLiteral("node")).toString() != identity)
            {
                positions.append(position);
            }
        }
        layout.insert(QStringLiteral("positions"), positions);
        inoutConfiguration.insert(QStringLiteral("layout"), layout);
    }
    return true;
}

bool ConnectEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    if (arguments.at(1).startsWith(QStringLiteral("#outputs.")) == true)
    {
        QJsonArray outputs = inoutConfiguration.value(QStringLiteral("outputs")).toArray();
        for (qsizetype i = 0; i < outputs.size(); ++i)
        {
            QJsonObject output = outputs.at(i).toObject();
            if (output.value(QStringLiteral("id")).toString() == arguments.at(1).section(QLatin1Char('.'), 1) &&
                output.value(QStringLiteral("source")).toString().isEmpty() == true)
            {
                output.insert(QStringLiteral("source"), arguments.at(0));
                outputs.replace(i, output);
                inoutConfiguration.insert(QStringLiteral("outputs"), outputs);
                return true;
            }
        }
        return false;
    }
    QJsonArray edges = inoutConfiguration.value(QStringLiteral("edges")).toArray();
    const QJsonArray existing = edges;
    for (const QJsonValueConstRef edge : existing)
    {
        if (edge.toObject().value(QStringLiteral("from")).toString() == arguments.at(0) &&
            edge.toObject().value(QStringLiteral("to")).toString() == arguments.at(1))
        {
            return false;
        }
    }
    QJsonObject edge;
    edge.insert(QStringLiteral("from"), arguments.at(0));
    edge.insert(QStringLiteral("to"), arguments.at(1));
    edges.append(edge);
    inoutConfiguration.insert(QStringLiteral("edges"), edges);
    return true;
}

bool ExpandSceneBundleEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    const QJsonArray nodes = inoutConfiguration.value(QStringLiteral("nodes")).toArray();
    QJsonArray expanded;
    for (const QJsonValueConstRef value : inoutConfiguration.value(QStringLiteral("edges")).toArray())
    {
        const QJsonObject edge = value.toObject();
        const QString from = edge.value(QStringLiteral("from")).toString();
        const QString to = edge.value(QStringLiteral("to")).toString();
        bool isScene = false;
        bool isSceneInput = false;
        for (const QJsonValueConstRef entry : nodes)
        {
            const QJsonObject node = entry.toObject();
            const QString identity = node.value(QStringLiteral("id")).toString();
            isScene = isScene == true || (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Scene") &&
                                          from == identity + QStringLiteral(".scene"));
            if (IsPipelineNode(node.value(QStringLiteral("kind")).toString()) == true)
            {
                for (const QJsonValueConstRef port : node.value(QStringLiteral("inputs")).toArray())
                {
                    isSceneInput =
                        isSceneInput == true ||
                        (port.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("Scene") &&
                         to == identity + QLatin1Char('.') + port.toObject().value(QStringLiteral("id")).toString());
                }
            }
        }
        if (isScene == true && isSceneInput == true &&
            (from.section(QLatin1Char('.'), 0, 0) == arguments.at(0) ||
             to.section(QLatin1Char('.'), 0, 0) == arguments.at(0)))
        {
            for (const QString& part :
                 {QStringLiteral("geometry"), QStringLiteral("materials"), QStringLiteral("lights")})
            {
                expanded.append(QJsonObject{
                    {QStringLiteral("from"), from.section(QLatin1Char('.'), 0, 0) + QLatin1Char('.') + part},
                    {QStringLiteral("to"), to + QLatin1Char('_') + part}});
            }
        }
        else
        {
            expanded.append(edge);
        }
    }
    if (expanded.size() > RenderGraphDocument::MAX_EDGES)
    {
        return false;
    }
    inoutConfiguration.insert(QStringLiteral("edges"), expanded);
    return true;
}

bool DisconnectEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    if (arguments.at(1).startsWith(QStringLiteral("#outputs.")) == true)
    {
        QJsonArray outputs = inoutConfiguration.value(QStringLiteral("outputs")).toArray();
        for (qsizetype i = 0; i < outputs.size(); ++i)
        {
            QJsonObject output = outputs.at(i).toObject();
            if (output.value(QStringLiteral("id")).toString() == arguments.at(1).section(QLatin1Char('.'), 1) &&
                output.value(QStringLiteral("source")).toString() == arguments.at(0))
            {
                output.insert(QStringLiteral("source"), QString());
                outputs.replace(i, output);
                inoutConfiguration.insert(QStringLiteral("outputs"), outputs);
                return true;
            }
        }
        return false;
    }
    QJsonArray edges = inoutConfiguration.value(QStringLiteral("edges")).toArray();
    for (qsizetype i = 0; i < edges.size(); ++i)
    {
        const QJsonObject edge = edges.at(i).toObject();
        if (edge.value(QStringLiteral("from")).toString() == arguments.at(0) &&
            edge.value(QStringLiteral("to")).toString() == arguments.at(1))
        {
            edges.removeAt(i);
            inoutConfiguration.insert(QStringLiteral("edges"), edges);
            return true;
        }
    }
    return false;
}

bool ReplaceConnectionEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    if (arguments.at(1).startsWith(QStringLiteral("#outputs.")) == true)
    {
        QJsonArray outputs = inoutConfiguration.value(QStringLiteral("outputs")).toArray();
        for (qsizetype i = 0; i < outputs.size(); ++i)
        {
            QJsonObject output = outputs.at(i).toObject();
            if (output.value(QStringLiteral("id")).toString() == arguments.at(1).section(QLatin1Char('.'), 1))
            {
                output.insert(QStringLiteral("source"), arguments.at(0));
                outputs.replace(i, output);
                inoutConfiguration.insert(QStringLiteral("outputs"), outputs);
                return true;
            }
        }
        return false;
    }
    QJsonArray edges;
    bool hasReplaced = false;
    const QJsonArray previous = inoutConfiguration.value(QStringLiteral("edges")).toArray();
    for (const QJsonValueConstRef value : previous)
    {
        QJsonObject edge = value.toObject();
        if (edge.value(QStringLiteral("to")).toString() == arguments.at(1))
        {
            if (hasReplaced == true)
            {
                continue;
            }
            edge.insert(QStringLiteral("from"), arguments.at(0));
            hasReplaced = true;
        }
        edges.append(edge);
    }
    inoutConfiguration.insert(QStringLiteral("edges"), edges);
    return hasReplaced == true || ConnectEdit(inoutConfiguration, arguments);
}

bool MoveNodeEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    if (FindNode(inoutConfiguration.value(QStringLiteral("nodes")).toArray(), arguments.at(0)) < 0)
    {
        return false;
    }
    QJsonObject layout = inoutConfiguration.value(QStringLiteral("layout")).toObject();
    QJsonArray positions = layout.value(QStringLiteral("positions")).toArray();
    QJsonObject position;
    position.insert(QStringLiteral("node"), arguments.at(0));
    position.insert(QStringLiteral("x"), arguments.at(1).toInt());
    position.insert(QStringLiteral("y"), arguments.at(2).toInt());
    bool isReplaced = false;
    for (qsizetype i = 0; i < positions.size(); ++i)
    {
        if (positions.at(i).toObject().value(QStringLiteral("node")).toString() == arguments.at(0))
        {
            positions.replace(i, position);
            isReplaced = true;
        }
    }
    if (isReplaced == false)
    {
        positions.append(position);
    }
    layout.insert(QStringLiteral("positions"), positions);
    inoutConfiguration.insert(QStringLiteral("layout"), layout);
    return true;
}
// Arguments are the edit's own arguments followed by the placed node, x and y.
constexpr qsizetype NODE_POSITION_ARGUMENT_COUNT = 3;
// A connection's arguments are its from and to endpoints.
constexpr qsizetype CONNECTION_ARGUMENT_COUNT = 2;

bool AddNodeAtEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    return AddNodeEdit(inoutConfiguration, arguments.mid(0, 1)) == true &&
           MoveNodeEdit(inoutConfiguration, arguments.mid(1, NODE_POSITION_ARGUMENT_COUNT)) == true;
}

bool AddUpscalePresetAtEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    return AddUpscalePresetEdit(inoutConfiguration, arguments.mid(0, 2)) == true &&
           MoveNodeEdit(inoutConfiguration, arguments.mid(2, NODE_POSITION_ARGUMENT_COUNT)) == true;
}

// Arguments are the added node, x and y (empty keeps automatic placement), then from and to.
bool PlaceAndConnectEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    const QStringList position = arguments.mid(0, NODE_POSITION_ARGUMENT_COUNT);
    const QStringList connection = arguments.mid(NODE_POSITION_ARGUMENT_COUNT);
    if (connection.size() != CONNECTION_ARGUMENT_COUNT || IsEndpointOf(connection.at(1), position.at(0)) == false)
    {
        return false;
    }
    if (position.at(1).isEmpty() == false && MoveNodeEdit(inoutConfiguration, position) == false)
    {
        return false;
    }
    return ConnectEdit(inoutConfiguration, connection);
}

// Arguments are the node record, then PlaceAndConnectEdit's arguments.
bool AddNodeConnectedEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    return AddNodeEdit(inoutConfiguration, arguments.mid(0, 1)) == true &&
           PlaceAndConnectEdit(inoutConfiguration, arguments.mid(1)) == true;
}

// Arguments are the algorithm, then PlaceAndConnectEdit's arguments starting with the identity.
bool AddUpscalePresetConnectedEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    const QStringList placement = arguments.mid(1);
    return placement.size() == NODE_POSITION_ARGUMENT_COUNT + CONNECTION_ARGUMENT_COUNT &&
           AppendUpscalePreset(inoutConfiguration,
                               arguments.at(0),
                               placement.at(0),
                               placement.last().section(QLatin1Char('.'), 1)) == true &&
           PlaceAndConnectEdit(inoutConfiguration, placement) == true;
}

bool MoveNodesEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    // Arguments are node, x, y triples; one failed node rejects the whole gesture.
    for (qsizetype i = 0; i + 2 < arguments.size(); i += 3)
    {
        if (MoveNodeEdit(inoutConfiguration, arguments.mid(i, 3)) == false)
        {
            return false;
        }
    }
    return arguments.isEmpty() == false && arguments.size() % 3 == 0;
}
bool ReplaceWireRoutesEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    const QJsonDocument parsed = QJsonDocument::fromJson(arguments.first().toUtf8());
    if (parsed.isArray() == false || AreRenderGraphWireRoutesValid(parsed.array(), inoutConfiguration) == false)
    {
        return false;
    }
    QJsonObject layout = inoutConfiguration.value(QStringLiteral("layout")).toObject();
    if (layout.contains(QStringLiteral("positions")) == false)
    {
        layout.insert(QStringLiteral("positions"), QJsonArray{});
    }
    layout.insert(QStringLiteral("routes"), parsed.array());
    inoutConfiguration.insert(QStringLiteral("layout"), layout);
    return true;
}
bool ReplaceCommentFramesEdit(QJsonObject& inoutConfiguration, const QStringList& arguments)
{
    const QJsonDocument parsed = QJsonDocument::fromJson(arguments.first().toUtf8());
    if (parsed.isArray() == false || AreRenderGraphCommentFramesValid(parsed.array()) == false)
    {
        return false;
    }
    if (arguments.size() > 1 && MoveNodesEdit(inoutConfiguration, arguments.mid(1)) == false)
    {
        return false;
    }
    QJsonObject layout = inoutConfiguration.value(QStringLiteral("layout")).toObject();
    if (layout.contains(QStringLiteral("positions")) == false)
    {
        layout.insert(QStringLiteral("positions"), QJsonArray{});
    }
    layout.insert(QStringLiteral("frames"), parsed.array());
    inoutConfiguration.insert(QStringLiteral("layout"), layout);
    return true;
}
} // namespace

QByteArray EncodeCanonicalJson(const QJsonObject& value)
{
    QByteArray bytes;
    AppendObject(value, 0, bytes);
    bytes.append('\n');
    return bytes;
}

namespace
{
constexpr qsizetype BYTES_PER_KIBIBYTE = 1024;

/// The canvas actions that free package sources, named as the menus show them.
QString GetFreeSourcesRepair()
{
    return QStringLiteral("To make room, remove a template or library you no longer use: right-click its Subgraph "
                          "node on the root canvas and choose Delete Subgraph and Library. That deletes its library "
                          "graph and the shader programs and sources only it uses, in one undoable edit. "
                          "Remove Unused Shaders (right-click the canvas) also drops programs no graph uses.");
}
} // namespace

QString DescribePackageCapacity()
{
    return QStringLiteral("The package exceeds the render-graph source limits: %1 files, %2 KiB per file and %3 KiB "
                          "in total. %4")
        .arg(RenderGraphDocument::MAX_FILES)
        .arg(RenderGraphDocument::MAX_FILE_BYTES / BYTES_PER_KIBIBYTE)
        .arg(RenderGraphDocument::MAX_TOTAL_BYTES / BYTES_PER_KIBIBYTE)
        .arg(GetFreeSourcesRepair());
}

QString DescribeTemplateFileCapacity(const QString& templateName, usize existingFiles, usize newFiles)
{
    return QStringLiteral("Template %1 needs %2 new files (its library graph and shader sources), but this package "
                          "already has %3 of its %4 files. %5 Then apply the template again. The draft was preserved.")
        .arg(templateName)
        .arg(newFiles)
        .arg(existingFiles)
        .arg(RenderGraphDocument::MAX_FILES)
        .arg(GetFreeSourcesRepair());
}

bool RemoveRenderGraphNodeRecord(QJsonObject& inoutConfiguration, const QString& node)
{
    return RemoveNodeEdit(inoutConfiguration, {node});
}

QString DescribeConfigurationCapacity(const QString& templateName, const QString& file)
{
    return QStringLiteral("%1 cannot add a configuration: %2 already has %3, the most one graph file can hold. "
                          "Open the root graph and apply the template there to give it its own library file, or "
                          "delete a configuration you no longer use (right-click the canvas and choose "
                          "Configurations > Delete Configuration). The draft was preserved.")
        .arg(templateName, file)
        .arg(RenderGraphDocument::MAX_CONFIGURATIONS);
}

namespace
{
/// Read every authored source below root into outSnapshot (bounded, no symlinks, no hidden or
/// `out` directories). root may be a Qt resource directory. outSnapshot is unchanged on failure.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — named package directory and root-graph identities.
RenderGraphDocumentStatus ReadPackage(const QString& root, const QString& rootGraph, RenderGraphSnapshot& outSnapshot)
{
    RenderGraphSnapshot snapshot;
    qsizetype total = 0;
    QStringList directories{root};
    while (directories.isEmpty() == false)
    {
        const QString directory = directories.takeFirst();
        const QFileInfoList entries =
            QDir(directory).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                          QDir::Name);
        for (const QFileInfo& entry : entries)
        {
            if (entry.fileName().startsWith(QLatin1Char('.')) == true)
            {
                continue;
            }
            if (entry.isDir() == true)
            {
                // Build products and generated drafts never become authored sources.
                if (entry.fileName() != QStringLiteral("out"))
                {
                    directories.append(entry.absoluteFilePath());
                }
                continue;
            }
            const QString relative = QDir(root).relativeFilePath(entry.absoluteFilePath());
            if (IsAuthoredSource(relative) == false)
            {
                continue;
            }
            QByteArray bytes;
            if (static_cast<usize>(snapshot.Files.size()) >= RenderGraphDocument::MAX_FILES ||
                ReadBounded(entry.absoluteFilePath(), bytes) == false)
            {
                return RenderGraphDocumentStatus::Capacity;
            }
            total += bytes.size();
            if (total > RenderGraphDocument::MAX_TOTAL_BYTES)
            {
                return RenderGraphDocumentStatus::Capacity;
            }
            snapshot.Files.insert(relative, bytes);
        }
    }
    if (rootGraph.endsWith(QStringLiteral(".lurender.json")) == false || snapshot.Files.contains(rootGraph) == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    outSnapshot.Files.swap(snapshot.Files);
    return RenderGraphDocumentStatus::Ok;
}
} // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — named package/file/configuration identities.
RenderGraphDocumentStatus RenderGraphDocument::Load(const QString& packageRoot, const QString& rootGraph)
{
    const QString root = QFileInfo(packageRoot).canonicalFilePath();
    if (root.isEmpty() == true || QFileInfo(root).isDir() == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    RenderGraphSnapshot snapshot;
    const RenderGraphDocumentStatus status = ReadPackage(root, rootGraph, snapshot);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        return status;
    }
    adopt(root, rootGraph, snapshot, false);
    return RenderGraphDocumentStatus::Ok;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — named source/file/destination identities.
RenderGraphDocumentStatus
RenderGraphDocument::LoadBuiltIn(const QString& sourceRoot, const QString& rootGraph, const QString& destinationRoot)
{
    if (QFileInfo(sourceRoot).isDir() == false || QDir::isAbsolutePath(destinationRoot) == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    RenderGraphSnapshot snapshot;
    const RenderGraphDocumentStatus status = ReadPackage(sourceRoot, rootGraph, snapshot);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        return status;
    }
    adopt(QDir::cleanPath(destinationRoot), rootGraph, snapshot, true);
    return RenderGraphDocumentStatus::Ok;
}

void RenderGraphDocument::adopt(const QString& packageRoot,
                                const QString& rootGraph,
                                const RenderGraphSnapshot& snapshot,
                                bool isBuiltIn)
{
    mPackageRoot = packageRoot;
    mRootGraph = rootGraph;
    mSaved = snapshot;
    mSavedRevision = 0;
    mDraft = snapshot;
    mIsBuiltIn = isBuiltIn;
    mLastMove.clear();
    mLastTextFile.clear();
    // A new document identity is the only reason to reset the monotonic revision.
    mHistory.Reset();
}

void RenderGraphDocument::Reset()
{
    mIsBuiltIn = false;
    mPackageRoot.clear();
    mRootGraph.clear();
    mSaved = {};
    mSavedRevision = 0;
    mDraft = {};
    mLastMove.clear();
    mLastTextFile.clear();
    mHistory.Reset();
}

QStringList RenderGraphDocument::GetFiles(RenderGraphSource source) const
{
    return (source == RenderGraphSource::Saved ? mSaved : mDraft).Files.keys();
}

QByteArray RenderGraphDocument::GetDraft(const QString& file) const
{
    return mDraft.Files.value(file);
}

BorrowedView<const char> RenderGraphDocument::GetDraftView(const QString& file) const noexcept
{
    const QMap<QString, QByteArray>::const_iterator found = mDraft.Files.constFind(file);
    if (found == mDraft.Files.cend())
    {
        return {};
    }
    return {found.value().constData(), static_cast<usize>(found.value().size())};
}

QByteArray RenderGraphDocument::GetSaved(const QString& file) const
{
    return mSaved.Files.value(file);
}

bool RenderGraphDocument::IsFileDirty(const QString& file) const
{
    return mDraft.Files.value(file) != mSaved.Files.value(file);
}

bool RenderGraphDocument::IsDirty() const
{
    return (mDraft == mSaved) == false;
}

bool RenderGraphDocument::GetGraph(const QString& file, QJsonObject& outGraph) const
{
    if (file.endsWith(QStringLiteral(".lurender.json")) == false || mDraft.Files.contains(file) == false)
    {
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(mDraft.Files.value(file), &error);
    if (error.error != QJsonParseError::NoError || document.isObject() == false)
    {
        return false;
    }
    outGraph = document.object();
    return true;
}

RenderGraphDocumentStatus RenderGraphDocument::commit(const RenderGraphSnapshot& candidate,
                                                      bool shouldMerge,
                                                      const RenderGraphTextState& beforeText,
                                                      const RenderGraphTextState& afterText,
                                                      const RenderGraphNodeSelection& beforeSelection,
                                                      const RenderGraphNodeSelection& afterSelection,
                                                      CommitKind kind)
{
    RenderGraphSnapshot after = candidate;
    if (kind == CommitKind::Authoring)
    {
        for (QMap<QString, QByteArray>::iterator file = after.Files.begin(); file != after.Files.end(); ++file)
        {
            if (file.key().endsWith(QStringLiteral(".lurender.json")) == false ||
                file.value() == mDraft.Files.value(file.key()))
            {
                continue;
            }
            const QJsonDocument parsed = QJsonDocument::fromJson(file.value());
            if (parsed.isObject() == false)
            {
                continue;
            }
            QJsonObject graph = parsed.object();
            QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
            bool hasChanged = false;
            for (qsizetype index = 0; index < configurations.size(); ++index)
            {
                QJsonObject configuration = configurations.at(index).toObject();
                if (PruneRenderGraphWireRoutes(configuration) == true)
                {
                    configurations.replace(index, configuration);
                    hasChanged = true;
                }
            }
            if (hasChanged == true)
            {
                graph.insert(QStringLiteral("configurations"), configurations);
                file.value() = EncodeCanonicalJson(graph);
            }
        }
    }
    qsizetype total = 0;
    for (const QByteArray& bytes : after.Files)
    {
        if (bytes.size() > MAX_FILE_BYTES)
        {
            return RenderGraphDocumentStatus::Capacity;
        }
        total += bytes.size();
    }
    if (total > MAX_TOTAL_BYTES || static_cast<usize>(after.Files.size()) > MAX_FILES)
    {
        return RenderGraphDocumentStatus::Capacity;
    }
    RenderGraphSnapshot before = mDraft;
    before.TextState = beforeText;
    before.NodeSelection = beforeSelection;
    after.TextState = afterText;
    after.NodeSelection = afterSelection;
    if (mHistory.Commit(before, after, shouldMerge) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    mDraft = after;
    mLastTextFile.clear();
    return RenderGraphDocumentStatus::Ok;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — named package/file/configuration identities.
RenderGraphDocumentStatus RenderGraphDocument::editConfiguration(const QString& file,
                                                                 const QString& configuration,
                                                                 const QStringList& arguments,
                                                                 EditFunction edit,
                                                                 bool shouldMerge)
{
    QJsonObject graph;
    if (GetGraph(file, graph) == false)
    {
        return RenderGraphDocumentStatus::Malformed;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    const qsizetype index = FindConfiguration(configurations, configuration);
    if (index < 0)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject record = configurations.at(index).toObject();
    if (edit(record, arguments) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    configurations.replace(index, record);
    graph.insert(QStringLiteral("configurations"), configurations);
    const QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValueConstRef node : nodes)
    {
        if (IsPipelineNode(node.toObject().value(QStringLiteral("kind")).toString()) == true ||
            node.toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("Upscale") ||
            node.toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("RenderSize") ||
            node.toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("OutputSize") ||
            node.toObject().value(QStringLiteral("kind")).toString() == QStringLiteral("StructuredBuffer") ||
            node.toObject().value(QStringLiteral("extent")).toString() == QStringLiteral("RenderSize"))
        {
            graph.insert(QStringLiteral("version"), 2);
            break;
        }
    }
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(file, EncodeCanonicalJson(graph));
    if (edit == &ConnectEdit || edit == &ReplaceConnectionEdit || edit == &ExpandSceneBundleEdit ||
        edit == &AddNodeConnectedEdit || edit == &AddUpscalePresetConnectedEdit)
    {
        const RenderGraphDocumentStatus status = PrepareConnectedSceneShaders(candidate, mRootGraph, mPackageRoot);
        if (status != RenderGraphDocumentStatus::Ok)
        {
            return status;
        }
    }
    return commit(candidate, shouldMerge);
}

RenderGraphDocumentStatus RenderGraphDocument::ReplaceNode(const QString& file,
                                                           const QString& configuration,
                                                           const QJsonObject& node,
                                                           QString* outErrorOrNull)
{
    if (IsPipelineNode(node.value(QStringLiteral("kind")).toString()) == true)
    {
        return ApplyPipeline(file, configuration, node, outErrorOrNull);
    }
    if (node.value(QStringLiteral("kind")) == QStringLiteral("Subgraph"))
    {
        RenderGraphSnapshot candidate = mDraft;
        QString error;
        const RenderGraphDocumentStatus prepared =
            prepareSubgraphReplacement(candidate, file, configuration, node, error);
        if (outErrorOrNull != nullptr)
        {
            *outErrorOrNull = error;
        }
        if (prepared == RenderGraphDocumentStatus::Ok &&
            ValidateRenderGraphOptionInterfaces(candidate.Files, error) == false)
        {
            if (outErrorOrNull != nullptr)
            {
                *outErrorOrNull = error;
            }
            return RenderGraphDocumentStatus::InvalidEdit;
        }
        if (prepared != RenderGraphDocumentStatus::Ok || candidate == mDraft)
        {
            return prepared;
        }
        mLastMove.clear();
        return commit(candidate, false);
    }
    if (outErrorOrNull != nullptr)
    {
        *outErrorOrNull =
            QStringLiteral("Cannot apply pipeline: the selected graph, configuration or node is no longer "
                           "available, or its kind changed. Reselect the node and retry.");
        QJsonObject graph;
        if (GetGraph(file, graph) == true)
        {
            const QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
            const qsizetype index = FindConfiguration(configurations, configuration);
            if (index >= 0)
            {
                QJsonObject candidate = configurations.at(index).toObject();
                if (ReplaceNodeChecked(candidate,
                                       {QString::fromUtf8(QJsonDocument(node).toJson(QJsonDocument::Compact))},
                                       outErrorOrNull) == false)
                {
                    return RenderGraphDocumentStatus::InvalidEdit;
                }
            }
        }
    }
    mLastMove.clear();
    const RenderGraphDocumentStatus status =
        editConfiguration(file,
                          configuration,
                          {QString::fromUtf8(QJsonDocument(node).toJson(QJsonDocument::Compact))},
                          &ReplaceNodeEdit,
                          false);
    if (status == RenderGraphDocumentStatus::Ok && outErrorOrNull != nullptr)
    {
        outErrorOrNull->clear();
    }
    return status;
}

RenderGraphDocumentStatus RenderGraphDocument::ApplyPipeline(const QString& file,
                                                             const QString& configuration,
                                                             const QJsonObject& node,
                                                             QString* outErrorOrNull)
{
    if (outErrorOrNull != nullptr)
    {
        *outErrorOrNull = QStringLiteral(
            "Cannot create shader sources: check program stages, names, file conflicts "
            "and the manifest configuration budget or supported package limits. The draft was preserved.");
    }
    if (IsPipelineNode(node.value(QStringLiteral("kind")).toString()) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject graph;
    if (GetGraph(file, graph) == false)
    {
        return RenderGraphDocumentStatus::Malformed;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    const qsizetype index = FindConfiguration(configurations, configuration);
    if (index < 0)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    RenderGraphSnapshot candidate = mDraft;
    QJsonObject pipeline = node;
    QString manifest = mRootGraph;
    manifest.chop(QStringLiteral(".lurender.json").size());
    manifest += QStringLiteral(".lushader.json");
    QJsonObject previousNode;
    for (const QJsonValueConstRef value : configurations.at(index).toObject().value(QStringLiteral("nodes")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")) == node.value(QStringLiteral("id")))
        {
            previousNode = value.toObject();
        }
    }
    const RenderGraphDocumentStatus status =
        PreparePipelineShaders(candidate, pipeline, previousNode, manifest, mPackageRoot, outErrorOrNull);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        return status;
    }
    // Shader renames may update references in other nodes in this graph.
    graph = QJsonDocument::fromJson(candidate.Files.value(file)).object();
    configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject record = configurations.at(index).toObject();
    if (ReplaceNodeChecked(record, {QString::fromUtf8(EncodeCanonicalJson(pipeline))}, outErrorOrNull) == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    configurations.replace(index, record);
    graph.insert(QStringLiteral("configurations"), configurations);
    graph.insert(QStringLiteral("version"), 2);
    candidate.Files.insert(file, EncodeCanonicalJson(graph));
    QString techniqueError;
    const RenderGraphDocumentStatus techniqueStatus =
        PrepareRenderGraphTechniques(candidate, mRootGraph, techniqueError);
    if (techniqueStatus != RenderGraphDocumentStatus::Ok)
    {
        if (outErrorOrNull != nullptr)
        {
            *outErrorOrNull = techniqueError;
        }
        return techniqueStatus;
    }
    const RenderGraphDocumentStatus sceneStatus =
        PrepareConnectedSceneShaders(candidate, mRootGraph, mPackageRoot, outErrorOrNull);
    if (sceneStatus != RenderGraphDocumentStatus::Ok)
    {
        return sceneStatus;
    }
    QString optionError;
    if (ValidateRenderGraphOptionInterfaces(candidate.Files, optionError) == false)
    {
        if (outErrorOrNull != nullptr)
        {
            *outErrorOrNull = optionError;
        }
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    if (outErrorOrNull != nullptr)
    {
        outErrorOrNull->clear();
    }
    if (candidate == mDraft)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    mLastMove.clear();
    return commit(candidate, false);
}

RenderGraphDocumentStatus RenderGraphDocument::EnableSceneMaterials(QString& outError)
{
    RenderGraphSnapshot candidate = mDraft;
    RenderGraphDocumentStatus status = PrepareRenderGraphTechniques(candidate, mRootGraph, outError);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        return status;
    }
    const QString manifest =
        mRootGraph.chopped(QStringLiteral(".lurender.json").size()) + QStringLiteral(".lushader.json");
    status = ConvertSceneRayInterface(candidate, manifest, mPackageRoot, outError);
    if (status != RenderGraphDocumentStatus::Ok || candidate == mDraft)
    {
        return status;
    }
    mLastMove.clear();
    return commit(candidate, false);
}

RenderGraphDocumentStatus RenderGraphDocument::PrepareTechniques(QString& outError)
{
    RenderGraphSnapshot candidate = mDraft;
    const RenderGraphDocumentStatus status = PrepareRenderGraphTechniques(candidate, mRootGraph, outError);
    if (status != RenderGraphDocumentStatus::Ok || candidate == mDraft)
    {
        return status;
    }
    mLastMove.clear();
    return commit(candidate, false);
}

namespace
{
/// x and y arguments for an optional layout position, clamped to ±MAX_COORDINATE; empty without one.
QStringList GetPositionArguments(const QPoint* positionOrNull)
{
    if (positionOrNull == nullptr)
    {
        return {QString{}, QString{}};
    }
    return {
        QString::number(
            qBound(-RenderGraphDocument::MAX_COORDINATE, positionOrNull->x(), RenderGraphDocument::MAX_COORDINATE)),
        QString::number(
            qBound(-RenderGraphDocument::MAX_COORDINATE, positionOrNull->y(), RenderGraphDocument::MAX_COORDINATE))};
}
} // namespace

RenderGraphDocumentStatus RenderGraphDocument::AddNode(const QString& file,
                                                       const QString& configuration,
                                                       const QJsonObject& node,
                                                       const QPoint* positionOrNull,
                                                       const RenderGraphNodeConnection* connectionOrNull)
{
    mLastMove.clear();
    const QString record = QString::fromUtf8(QJsonDocument(node).toJson(QJsonDocument::Compact));
    if (connectionOrNull != nullptr)
    {
        return editConfiguration(file,
                                 configuration,
                                 QStringList{record, node.value(QStringLiteral("id")).toString()} +
                                     GetPositionArguments(positionOrNull) +
                                     QStringList{connectionOrNull->From, connectionOrNull->To},
                                 &AddNodeConnectedEdit,
                                 false);
    }
    if (positionOrNull == nullptr)
    {
        return editConfiguration(file, configuration, {record}, &AddNodeEdit, false);
    }
    return editConfiguration(file,
                             configuration,
                             {record,
                              node.value(QStringLiteral("id")).toString(),
                              QString::number(qBound(-MAX_COORDINATE, positionOrNull->x(), MAX_COORDINATE)),
                              QString::number(qBound(-MAX_COORDINATE, positionOrNull->y(), MAX_COORDINATE))},
                             &AddNodeAtEdit,
                             false);
}

RenderGraphDocumentStatus RenderGraphDocument::AddUpscalePreset(const QString& file,
                                                                const QString& configuration,
                                                                const QString& algorithm,
                                                                const QString& identity,
                                                                const QPoint* positionOrNull,
                                                                const RenderGraphNodeConnection* connectionOrNull)
{
    mLastMove.clear();
    if (connectionOrNull != nullptr)
    {
        return editConfiguration(file,
                                 configuration,
                                 QStringList{algorithm, identity} + GetPositionArguments(positionOrNull) +
                                     QStringList{connectionOrNull->From, connectionOrNull->To},
                                 &AddUpscalePresetConnectedEdit,
                                 false);
    }
    if (positionOrNull == nullptr)
    {
        return editConfiguration(file, configuration, {algorithm, identity}, &AddUpscalePresetEdit, false);
    }
    return editConfiguration(file,
                             configuration,
                             {algorithm,
                              identity,
                              identity,
                              QString::number(qBound(-MAX_COORDINATE, positionOrNull->x(), MAX_COORDINATE)),
                              QString::number(qBound(-MAX_COORDINATE, positionOrNull->y(), MAX_COORDINATE))},
                             &AddUpscalePresetAtEdit,
                             false);
}

// File, child configuration and parent configuration are distinct authored identities.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus RenderGraphDocument::ApplyRestirTemplate(const QString& file,
                                                                   const QString& configuration,
                                                                   QString& outError,
                                                                   const QString& parentConfiguration)
{
    return ApplyConfigurationTemplate(file, configuration, QStringLiteral("restir-di"), outError, parentConfiguration);
}
// File, configuration and template identify distinct authoring records.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
RenderGraphDocumentStatus
RenderGraphDocument::ApplyConfigurationTemplate(const QString& file,
                                                const QString& configuration,
                                                const QString& templateIdentity,
                                                QString& outError,
                                                const QString& parentConfiguration,
                                                RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    if (inoutBudgetOrNull != nullptr)
    {
        inoutBudgetOrNull->CurrentPrograms = 0;
        inoutBudgetOrNull->RequiredPrograms = 0;
    }
    RenderGraphSnapshot candidate = mDraft;
    const RenderGraphDocumentStatus prepared = PrepareConfigurationTemplate(candidate,
                                                                            mRootGraph,
                                                                            file,
                                                                            configuration,
                                                                            parentConfiguration,
                                                                            mPackageRoot,
                                                                            templateIdentity,
                                                                            outError,
                                                                            inoutBudgetOrNull);
    if (prepared != RenderGraphDocumentStatus::Ok)
    {
        return prepared;
    }
    mLastMove.clear();
    const RenderGraphDocumentStatus status = commit(candidate, false);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        outError =
            QStringLiteral("The template exceeds this package's file or history limits. The draft was preserved.");
    }
    return status;
}

RenderGraphDocumentStatus RenderGraphDocument::checkNewGraphAsset(const QString& name) const
{
    const QString asset = name + QStringLiteral(".lurender.json");
    const QFileInfo destination(QDir(mPackageRoot).filePath(asset));
    if (mDraft.Files.contains(asset) == true || mSaved.Files.contains(asset) == true || destination.exists() == true ||
        destination.isSymLink() == true)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    for (const QString& source : GetFiles())
    {
        // Keep draft filenames portable to case-insensitive project filesystems too.
        if (source.compare(asset, Qt::CaseInsensitive) == 0)
        {
            return RenderGraphDocumentStatus::Conflict;
        }
        QJsonObject existing;
        if (GetGraph(source, existing) == true && existing.value(QStringLiteral("id")).toString() == name)
        {
            return RenderGraphDocumentStatus::InvalidEdit;
        }
    }
    return RenderGraphDocumentStatus::Ok;
}

RenderGraphDocumentStatus RenderGraphDocument::CreateSubgraph(const QString& file,
                                                              const QString& configuration,
                                                              const QString& node,
                                                              const QString& name)
{
    const auto isIdentifier = [](const QString& value) {
        if (value.isEmpty() == true || value.size() > MAX_IDENTIFIER_LENGTH)
        {
            return false;
        }
        for (qsizetype i = 0; i < value.size(); ++i)
        {
            const QChar character = value.at(i);
            const bool isLetter = (character >= QLatin1Char('A') && character <= QLatin1Char('Z')) ||
                                  (character >= QLatin1Char('a') && character <= QLatin1Char('z'));
            const bool isDigit = character >= QLatin1Char('0') && character <= QLatin1Char('9');
            if (isLetter == false &&
                (i == 0 || (isDigit == false && character != QLatin1Char('_') && character != QLatin1Char('-'))))
            {
                return false;
            }
        }
        return true;
    };
    if (isIdentifier(name) == false || isIdentifier(node) == false || node == QStringLiteral("input"))
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    const RenderGraphDocumentStatus available = checkNewGraphAsset(name);
    if (available != RenderGraphDocumentStatus::Ok)
    {
        return available;
    }
    const QString asset = name + QStringLiteral(".lurender.json");
    QJsonObject graph;
    if (GetGraph(file, graph) == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    const qsizetype configurationIndex = FindConfiguration(configurations, configuration);
    if (configurationIndex < 0)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    QJsonObject parent = configurations.at(configurationIndex).toObject();
    QJsonArray nodes = parent.value(QStringLiteral("nodes")).toArray();
    const qsizetype nodeIndex = FindNode(nodes, node);
    QJsonObject reference = nodeIndex < 0 ? QJsonObject{} : nodes.at(nodeIndex).toObject();
    if (nodeIndex >= 0 && reference.value(QStringLiteral("kind")).toString() != QStringLiteral("Subgraph"))
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    reference.insert(QStringLiteral("id"), node);
    reference.insert(QStringLiteral("kind"), QStringLiteral("Subgraph"));
    reference.insert(QStringLiteral("asset"), asset);
    reference.insert(QStringLiteral("configuration"), QStringLiteral("Default"));
    if (nodeIndex < 0)
    {
        nodes.append(reference);
    }
    else
    {
        nodes.replace(nodeIndex, reference);
    }
    parent.insert(QStringLiteral("nodes"), nodes);
    configurations.replace(configurationIndex, parent);
    graph.insert(QStringLiteral("configurations"), configurations);
    const QJsonObject emptyConfiguration{{QStringLiteral("id"), QStringLiteral("Default")},
                                         {QStringLiteral("inputs"), QJsonArray{}},
                                         {QStringLiteral("outputs"), QJsonArray{}},
                                         {QStringLiteral("nodes"), QJsonArray{}},
                                         {QStringLiteral("edges"), QJsonArray{}}};
    const QJsonObject library{{QStringLiteral("version"), 1},
                              {QStringLiteral("id"), name},
                              {QStringLiteral("default"), QStringLiteral("Default")},
                              {QStringLiteral("configurations"), QJsonArray{emptyConfiguration}}};
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(file, EncodeCanonicalJson(graph));
    candidate.Files.insert(asset, EncodeCanonicalJson(library));
    QString error;
    const RenderGraphDocumentStatus prepared =
        prepareSubgraphReplacement(candidate, file, configuration, reference, error);
    if (prepared != RenderGraphDocumentStatus::Ok)
    {
        return prepared;
    }
    mLastMove.clear();
    return commit(candidate, false);
}

RenderGraphDocumentStatus
RenderGraphDocument::RemoveNodes(const QString& file, const QString& configuration, const QStringList& nodes)
{
    mLastMove.clear();
    return editConfiguration(
        file,
        configuration,
        nodes,
        [](QJsonObject& inoutConfiguration, const QStringList& identities) {
            if (identities.isEmpty() == true)
            {
                return false;
            }
            for (const QString& identity : identities)
            {
                if (RemoveNodeEdit(inoutConfiguration, {identity}) == false)
                {
                    return false;
                }
            }
            return true;
        },
        false);
}

RenderGraphDocumentStatus
RenderGraphDocument::RemoveNode(const QString& file, const QString& configuration, const QString& node)
{
    mLastMove.clear();
    return editConfiguration(file, configuration, {node}, &RemoveNodeEdit, false);
}

RenderGraphDocumentStatus
RenderGraphDocument::ExpandSceneBundle(const QString& file, const QString& configuration, const QString& node)
{
    mLastMove.clear();
    return editConfiguration(file, configuration, {node}, &ExpandSceneBundleEdit, false);
}

RenderGraphDocumentStatus
RenderGraphDocument::Connect(const QString& file, const QString& configuration, const QString& from, const QString& to)
{
    mLastMove.clear();
    return editConfiguration(file, configuration, {from, to}, &ConnectEdit, false);
}

RenderGraphDocumentStatus RenderGraphDocument::ReplaceConnection(const QString& file,
                                                                 const QString& configuration,
                                                                 const QString& from,
                                                                 const QString& to)
{
    mLastMove.clear();
    return editConfiguration(file, configuration, {from, to}, &ReplaceConnectionEdit, false);
}

RenderGraphDocumentStatus RenderGraphDocument::Disconnect(const QString& file,
                                                          const QString& configuration,
                                                          const QString& from,
                                                          const QString& to)
{
    mLastMove.clear();
    return editConfiguration(file, configuration, {from, to}, &DisconnectEdit, false);
}

RenderGraphDocumentStatus
RenderGraphDocument::MoveNode(const QString& file, const QString& configuration, const QString& node, int32 x, int32 y)
{
    const QString key = file + QLatin1Char('#') + configuration + QLatin1Char('/') + node;
    const bool shouldMerge = mLastMove == key;
    const RenderGraphDocumentStatus status = editConfiguration(file,
                                                               configuration,
                                                               {node, QString::number(x), QString::number(y)},
                                                               &MoveNodeEdit,
                                                               shouldMerge);
    mLastMove = status == RenderGraphDocumentStatus::Ok ? key : QString();
    return status;
}

RenderGraphDocumentStatus RenderGraphDocument::MoveNodes(const QString& file,
                                                         const QString& configuration,
                                                         const QVector<RenderGraphNodePosition>& positions)
{
    mLastMove.clear();
    QStringList arguments;
    for (const RenderGraphNodePosition& position : positions)
    {
        arguments += {position.Node, QString::number(position.X), QString::number(position.Y)};
    }
    return editConfiguration(file, configuration, arguments, &MoveNodesEdit, false);
}

RenderGraphDocumentStatus RenderGraphDocument::ReplaceCommentFrames(const QString& file,
                                                                    const QString& configuration,
                                                                    const QJsonArray& frames,
                                                                    const QVector<RenderGraphNodePosition>& positions)
{
    mLastMove.clear();
    QStringList arguments{QString::fromUtf8(QJsonDocument(frames).toJson(QJsonDocument::Compact))};
    for (const RenderGraphNodePosition& position : positions)
    {
        arguments += {position.Node, QString::number(position.X), QString::number(position.Y)};
    }
    return editConfiguration(file, configuration, arguments, &ReplaceCommentFramesEdit, false);
}

RenderGraphDocumentStatus
RenderGraphDocument::ReplaceWireRoutes(const QString& file, const QString& configuration, const QJsonArray& routes)
{
    mLastMove.clear();
    return editConfiguration(file,
                             configuration,
                             {QString::fromUtf8(QJsonDocument(routes).toJson(QJsonDocument::Compact))},
                             &ReplaceWireRoutesEdit,
                             false);
}

RenderGraphDocumentStatus RenderGraphDocument::SetText(const QString& file, const QByteArray& bytes)
{
    mLastMove.clear();
    if (mDraft.Files.contains(file) == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(file, bytes);
    return commit(candidate, false, {}, {}, {}, {}, CommitKind::Source);
}

CodeEditStatus RenderGraphDocument::ApplyTextEdit(const QString& file,
                                                  uint64 expectedRevision,
                                                  const CodeTextEdit& edit,
                                                  CodeTextSelection before,
                                                  CodeTextSelection after,
                                                  bool shouldMerge)
{
    if (expectedRevision != GetRevision())
    {
        return CodeEditStatus::Conflict;
    }
    if (mDraft.Files.contains(file) == false)
    {
        return CodeEditStatus::Unavailable;
    }
    const BorrowedView<const char> original = GetDraftView(file);
    if (edit.Inserted.IsValid() == false || edit.StartBytes > original.GetSize() ||
        edit.RemovedBytes > original.GetSize() - edit.StartBytes)
    {
        return CodeEditStatus::InvalidText;
    }
    const usize retainedBytes = original.GetSize() - edit.RemovedBytes;
    if (retainedBytes > static_cast<usize>(MAX_FILE_BYTES) ||
        edit.Inserted.GetSize() > static_cast<usize>(MAX_FILE_BYTES) - retainedBytes)
    {
        return CodeEditStatus::Capacity;
    }
    // Adapt bounded views to FoundationStrings' existing validation boundary;
    // prefixes ending inside a UTF-8 scalar are rejected without allocating.
    const auto isValidPrefix = [](BorrowedView<const char> bytes, usize count) {
        if (count == 0)
        {
            return true;
        }
        Utf8View proof;
        usize errorOffset = 0;
        return ValidateUtf8({bytes.GetDataOrNull(), count}, proof, errorOffset) == StringStatus::Ok;
    };
    if (isValidPrefix(original, edit.StartBytes) == false ||
        isValidPrefix(original, edit.StartBytes + edit.RemovedBytes) == false ||
        isValidPrefix(edit.Inserted, edit.Inserted.GetSize()) == false || before.AnchorBytes > original.GetSize() ||
        before.CaretBytes > original.GetSize() || isValidPrefix(original, before.AnchorBytes) == false ||
        isValidPrefix(original, before.CaretBytes) == false)
    {
        return CodeEditStatus::InvalidText;
    }
    const usize newSize = retainedBytes + edit.Inserted.GetSize();
    if (after.AnchorBytes > newSize || after.CaretBytes > newSize)
    {
        return CodeEditStatus::InvalidText;
    }
    QByteArray text = mDraft.Files.value(file);
    text.replace(static_cast<isize>(edit.StartBytes),
                 static_cast<isize>(edit.RemovedBytes),
                 edit.Inserted.GetDataOrNull(),
                 static_cast<isize>(edit.Inserted.GetSize()));
    const BorrowedView<const char> result{text.constData(), static_cast<usize>(text.size())};
    if (isValidPrefix(result, after.AnchorBytes) == false || isValidPrefix(result, after.CaretBytes) == false)
    {
        return CodeEditStatus::InvalidText;
    }
    if (text == mDraft.Files.value(file))
    {
        return CodeEditStatus::Ok;
    }
    RenderGraphSnapshot candidate = mDraft;
    candidate.Files.insert(file, text);
    const bool canMerge = shouldMerge == true && mLastTextFile == file;
    const RenderGraphDocumentStatus status =
        commit(candidate, canMerge, {file, before}, {file, after}, {}, {}, CommitKind::Source);
    if (status == RenderGraphDocumentStatus::Ok)
    {
        mLastMove.clear();
        mLastTextFile = file;
        return CodeEditStatus::Ok;
    }
    return status == RenderGraphDocumentStatus::Capacity ? CodeEditStatus::Capacity : CodeEditStatus::Conflict;
}

void RenderGraphDocument::BreakTextGroup() noexcept
{
    mLastTextFile.clear();
    mHistory.BreakGroup();
}

bool RenderGraphDocument::Undo()
{
    BreakTextGroup();
    mLastMove.clear();
    return mHistory.Undo(mDraft);
}

bool RenderGraphDocument::Redo()
{
    BreakTextGroup();
    mLastMove.clear();
    return mHistory.Redo(mDraft);
}

RenderGraphDocumentStatus RenderGraphDocument::WriteSnapshot(const QString& directory, RenderGraphSource source) const
{
    QDir target(directory);
    if (IsLoaded() == false || target.exists() == false ||
        target.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty() == false)
    {
        return RenderGraphDocumentStatus::InvalidEdit;
    }
    const RenderGraphSnapshot& snapshot = source == RenderGraphSource::Saved ? mSaved : mDraft;
    for (QMap<QString, QByteArray>::const_iterator file = snapshot.Files.constBegin();
         file != snapshot.Files.constEnd();
         ++file)
    {
        const QString path = target.filePath(file.key());
        QFile output(path);
        if (QDir().mkpath(QFileInfo(path).absolutePath()) == false || output.open(QIODevice::WriteOnly) == false ||
            output.write(file.value()) != file.value().size())
        {
            return RenderGraphDocumentStatus::WriteFailed;
        }
    }
    return RenderGraphDocumentStatus::Ok;
}

RenderGraphDocumentStatus RenderGraphDocument::RefreshFromDisk(bool& outHasChanged)
{
    outHasChanged = false;
    if (IsLoaded() == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    if (mIsBuiltIn == true)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    if (QFileInfo(mPackageRoot).canonicalFilePath() != mPackageRoot)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    RenderGraphSnapshot candidate;
    const RenderGraphDocumentStatus status = ReadPackage(mPackageRoot, mRootGraph, candidate);
    if (status != RenderGraphDocumentStatus::Ok || candidate == mSaved)
    {
        return status;
    }
    if (IsDirty() == true)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    const RenderGraphDocumentStatus committed = commit(candidate, false, {}, {}, {}, {}, CommitKind::Source);
    if (committed == RenderGraphDocumentStatus::Ok)
    {
        mSaved = candidate;
        mSavedRevision = mHistory.Revision();
        mLastMove.clear();
        outHasChanged = true;
    }
    return committed;
}

RenderGraphDocumentStatus RenderGraphDocument::Save(QStringList& outSavedFiles)
{
    BreakTextGroup();
    outSavedFiles.clear();
    if (IsLoaded() == false)
    {
        return RenderGraphDocumentStatus::NotFound;
    }
    if (mIsBuiltIn == true)
    {
        return createPackage(outSavedFiles);
    }
    QLockFile lock(QDir(mPackageRoot).filePath(LOCK_NAME));
    if (lock.tryLock(0) == false)
    {
        return RenderGraphDocumentStatus::Locked;
    }
    if (QFileInfo(mPackageRoot).canonicalFilePath() != mPackageRoot)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    QStringList removed;
    for (QMap<QString, QByteArray>::const_iterator file = mSaved.Files.constBegin(); file != mSaved.Files.constEnd();
         ++file)
    {
        if (mDraft.Files.contains(file.key()) == false)
        {
            QByteArray disk;
            if (ReadBounded(QDir(mPackageRoot).filePath(file.key()), disk) == false || disk != file.value())
            {
                return RenderGraphDocumentStatus::Conflict;
            }
            removed.append(file.key());
        }
    }
    QStringList dirty;
    for (QMap<QString, QByteArray>::const_iterator file = mDraft.Files.constBegin(); file != mDraft.Files.constEnd();
         ++file)
    {
        if (IsFileDirty(file.key()) == false)
        {
            continue;
        }
        // Another writer changed the file since it was loaded or saved: write nothing.
        const QString path = QDir(mPackageRoot).filePath(file.key());
        QByteArray disk;
        if (mSaved.Files.contains(file.key()) == true)
        {
            if (ReadBounded(path, disk) == false || disk != mSaved.Files.value(file.key()))
            {
                return RenderGraphDocumentStatus::Conflict;
            }
        }
        else if (QFileInfo::exists(path) == true || QFileInfo(path).isSymLink() == true)
        {
            return RenderGraphDocumentStatus::Conflict;
        }
        dirty.append(file.key());
    }
    // Templates add nested shader sources to existing packages. Prepare every
    // parent directory before publishing manifests that can refer to those files.
    // Reject linked parents so source creation cannot escape the package root.
    for (const QString& file : dirty)
    {
        QDir directory(mPackageRoot);
        const QStringList components = QFileInfo(file).path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString& component : components)
        {
            if (component == QStringLiteral("."))
            {
                continue;
            }
            const QString path = directory.filePath(component);
            const QFileInfo parent(path);
            if (component == QStringLiteral("..") || parent.isSymLink() == true)
            {
                return RenderGraphDocumentStatus::Conflict;
            }
            if ((parent.exists() == true && parent.isDir() == false) ||
                (parent.exists() == false && directory.mkdir(component) == false))
            {
                return RenderGraphDocumentStatus::WriteFailed;
            }
            directory.setPath(path);
        }
    }
    for (const QString& file : dirty)
    {
        const QString path = QDir(mPackageRoot).filePath(file);
        const QByteArray bytes = mDraft.Files.value(file);
        if (mSaved.Files.contains(file) == false)
        {
            QFile output(path);
            if (output.open(QIODevice::WriteOnly | QIODevice::NewOnly) == false)
            {
                return RenderGraphDocumentStatus::WriteFailed;
            }
            if (output.write(bytes) != bytes.size() || output.flush() == false)
            {
                static_cast<void>(output.remove());
                return RenderGraphDocumentStatus::WriteFailed;
            }
            mSaved.Files.insert(file, bytes);
            outSavedFiles.append(file);
            continue;
        }
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (QFileInfo(path).isSymLink() == true || output.open(QIODevice::WriteOnly) == false ||
            output.write(bytes) != bytes.size() || output.commit() == false)
        {
            return RenderGraphDocumentStatus::WriteFailed;
        }
        mSaved.Files.insert(file, bytes);
        outSavedFiles.append(file);
    }
    for (const QString& file : removed)
    {
        if (QFile::remove(QDir(mPackageRoot).filePath(file)) == false)
        {
            return RenderGraphDocumentStatus::WriteFailed;
        }
        mSaved.Files.remove(file);
        outSavedFiles.append(file);
    }
    mSavedRevision = mHistory.Revision();
    return RenderGraphDocumentStatus::Ok;
}

RenderGraphDocumentStatus RenderGraphDocument::createPackage(QStringList& outSavedFiles)
{
    // A customized built-in becomes a new package: every file is created, none replaced.
    const QDir destination(mPackageRoot);
    const auto hasExistingFile = [this, &destination]() {
        for (QMap<QString, QByteArray>::const_iterator file = mDraft.Files.constBegin();
             file != mDraft.Files.constEnd();
             ++file)
        {
            if (QFileInfo::exists(destination.filePath(file.key())) == true ||
                QFileInfo(destination.filePath(file.key())).isSymLink() == true)
            {
                return true;
            }
        }
        return false;
    };
    if (hasExistingFile() == true)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    if (QDir().mkpath(mPackageRoot) == false)
    {
        return RenderGraphDocumentStatus::WriteFailed;
    }
    QLockFile lock(destination.filePath(LOCK_NAME));
    if (lock.tryLock(0) == false)
    {
        return RenderGraphDocumentStatus::Locked;
    }
    // Another writer may have created a file between the first check and the lock.
    if (hasExistingFile() == true)
    {
        return RenderGraphDocumentStatus::Conflict;
    }
    QStringList created;
    for (QMap<QString, QByteArray>::const_iterator file = mDraft.Files.constBegin(); file != mDraft.Files.constEnd();
         ++file)
    {
        const QString path = destination.filePath(file.key());
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (QDir().mkpath(QFileInfo(path).absolutePath()) == false || output.open(QIODevice::WriteOnly) == false ||
            output.write(file.value()) != file.value().size() || output.commit() == false)
        {
            // Remove this call's files so a retry still finds an empty destination.
            for (const QString& written : created)
            {
                static_cast<void>(QFile::remove(destination.filePath(written)));
            }
            return RenderGraphDocumentStatus::WriteFailed;
        }
        created.append(file.key());
    }
    // The package root keeps the destination's spelling, so project-relative paths are unchanged.
    mSaved = mDraft;
    mSavedRevision = mHistory.Revision();
    mIsBuiltIn = false;
    outSavedFiles = created;
    return RenderGraphDocumentStatus::Ok;
}
} // namespace ludus::editor
