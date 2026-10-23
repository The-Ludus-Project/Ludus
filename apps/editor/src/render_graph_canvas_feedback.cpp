#include "internal/render_graph_canvas_feedback.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline_node.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 MAX_EFFECT_STEPS = 4096;
constexpr int32 MAX_EFFECT_DEPTH = 64;
enum class EffectState : uint8
{
    None,
    Present,
    Unknown
};
QJsonObject FindConfiguration(const RenderGraphDocument& document, const QString& file, const QString& identity)
{
    QJsonObject graph;
    if (document.GetGraph(file, graph) == true)
    {
        for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
        {
            if (value.toObject().value(QStringLiteral("id")).toString() == identity)
            {
                return value.toObject();
            }
        }
    }
    return {};
}
/// Per-inspection import cache and work bound. No state survives a draft change.
class EffectInspector final
{
public:
    explicit EffectInspector(const RenderGraphDocument& document) : mDocument(document) {}
    EffectState Inspect(const QJsonObject& node, int32 depth = 0)
    {
        if (--mRemainingSteps <= 0 || depth >= MAX_EFFECT_DEPTH)
        {
            return EffectState::Unknown;
        }
        const QString kind = node.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("Export") || kind == QStringLiteral("Present") ||
            (kind == QStringLiteral("Camera") && node.value(QStringLiteral("active")).toBool() == true))
        {
            return EffectState::Present;
        }
        if (kind != QStringLiteral("Subgraph"))
        {
            return EffectState::None;
        }
        const QString file = node.value(QStringLiteral("asset")).toString();
        const QString configuration = node.value(QStringLiteral("configuration")).toString();
        const QString key = file + QLatin1Char('#') + configuration;
        if (mCache.contains(key) == true)
        {
            return mCache.value(key);
        }
        // Publish Unknown before descending so cyclic imports never appear effect-free.
        mCache.insert(key, EffectState::Unknown);
        const QJsonObject record = FindConfiguration(mDocument, file, configuration);
        if (record.isEmpty() == true)
        {
            return EffectState::Unknown;
        }
        EffectState result = EffectState::None;
        for (const QJsonValueConstRef value : record.value(QStringLiteral("nodes")).toArray())
        {
            const EffectState child = Inspect(value.toObject(), depth + 1);
            if (child == EffectState::Unknown)
            {
                return EffectState::Unknown;
            }
            if (child == EffectState::Present)
            {
                result = EffectState::Present;
            }
        }
        mCache.insert(key, result);
        return result;
    }

private:
    const RenderGraphDocument& mDocument;
    QHash<QString, EffectState> mCache;
    int32 mRemainingSteps = MAX_EFFECT_STEPS;
};

bool AreTypesCompatible(const QString& source, const QString& destination)
{
    return source.isEmpty() == false && destination.isEmpty() == false &&
           (source == destination ||
            (destination == QStringLiteral("Texture") &&
             (source == QStringLiteral("ColorTexture") || source == QStringLiteral("DepthTexture"))));
}
} // namespace

QHash<QString, RenderGraphActivityFeedback>
InspectRenderGraphActivity(const RenderGraphDocument& document, const QString& file, const QString& configuration)
{
    const QJsonObject record = FindConfiguration(document, file, configuration);
    const QJsonArray nodes = record.value(QStringLiteral("nodes")).toArray();
    const QJsonArray edges = record.value(QStringLiteral("edges")).toArray();
    if (nodes.size() > RenderGraphDocument::MAX_NODES || edges.size() > RenderGraphDocument::MAX_EDGES)
    {
        return {};
    }
    QHash<QString, QSet<QString>> dependencies;
    QHash<QString, QString> sources;
    QHash<QString, QString> destinations;
    QSet<QString> selected;
    bool hasUnknown = record.isEmpty() == true || nodes.size() > RenderGraphDocument::MAX_NODES ||
                      edges.size() > RenderGraphDocument::MAX_EDGES;
    for (const QJsonValueConstRef value : record.value(QStringLiteral("inputs")).toArray())
    {
        const QJsonObject port = value.toObject();
        sources.insert(QStringLiteral("input.") + port.value(QStringLiteral("id")).toString(),
                       port.value(QStringLiteral("type")).toString());
    }
    EffectInspector effects(document);
    for (const QJsonValueConstRef value : nodes)
    {
        const QJsonObject node = value.toObject();
        const QString identity = node.value(QStringLiteral("id")).toString();
        const QString kind = node.value(QStringLiteral("kind")).toString();
        hasUnknown = hasUnknown == true || (GetRenderGraphDomain(kind) == RenderGraphDomain::Execution &&
                                            kind != QStringLiteral("Raster") && kind != QStringLiteral("Present") &&
                                            kind != QStringLiteral("Accumulate") && kind != QStringLiteral("ToneMap") &&
                                            kind != QStringLiteral("Denoise") && IsPipelineNode(kind) == false);
        hasUnknown = hasUnknown == true || identity.isEmpty() == true || dependencies.contains(identity) == true;
        dependencies.insert(identity, {});
        RenderGraphNodePorts ports;
        if (GetRenderGraphNodePorts(document, node, ports) == false)
        {
            hasUnknown = true;
        }
        for (const RenderGraphPort& port : ports.Inputs)
        {
            destinations.insert(identity + QLatin1Char('.') + port.Id, port.Type);
        }
        for (const RenderGraphPort& port : ports.Outputs)
        {
            sources.insert(identity + QLatin1Char('.') + port.Id, port.Type);
        }
        const EffectState effect = effects.Inspect(node);
        hasUnknown = hasUnknown == true || effect == EffectState::Unknown;
        if (effect == EffectState::Present)
        {
            selected.insert(identity);
        }
    }
    QSet<QString> writers;
    for (const QJsonValueConstRef value : edges)
    {
        const QJsonObject edge = value.toObject();
        const QString from = edge.value(QStringLiteral("from")).toString();
        const QString to = edge.value(QStringLiteral("to")).toString();
        hasUnknown = hasUnknown == true || writers.contains(to) == true ||
                     AreTypesCompatible(sources.value(from), destinations.value(to)) == false;
        writers.insert(to);
        const QString producer = from.section(QLatin1Char('.'), 0, 0);
        if (producer != QStringLiteral("input"))
        {
            dependencies[to.section(QLatin1Char('.'), 0, 0)].insert(producer);
        }
    }
    for (const QJsonValueConstRef value : record.value(QStringLiteral("outputs")).toArray())
    {
        const QJsonObject output = value.toObject();
        const QString source = output.value(QStringLiteral("source")).toString();
        hasUnknown = hasUnknown == true || sources.value(source).isEmpty() == true ||
                     sources.value(source) != output.value(QStringLiteral("type")).toString();
        selected.insert(source.section(QLatin1Char('.'), 0, 0));
    }
    // Structural cycles anywhere remain validation errors, including unused branches.
    QSet<QString> ordered;
    for (qsizetype pass = 0; pass < nodes.size(); ++pass)
    {
        for (QHash<QString, QSet<QString>>::const_iterator node = dependencies.constBegin();
             node != dependencies.constEnd();
             ++node)
        {
            if ((node.value() - ordered).isEmpty() == true)
            {
                ordered.insert(node.key());
            }
        }
    }
    hasUnknown = hasUnknown == true || ordered.size() != dependencies.size();
    for (qsizetype pass = 0; pass < nodes.size(); ++pass)
    {
        const QSet<QString> previous = selected;
        for (const QString& identity : previous)
        {
            selected.unite(dependencies.value(identity));
        }
        if (previous == selected)
        {
            break;
        }
    }
    QHash<QString, RenderGraphActivityFeedback> feedback;
    const QString scope =
        file != document.GetRootGraph()
            ? QStringLiteral(
                  " Library inspection assumes all exposed outputs are requested; a caller instance may use fewer.")
            : QString();
    for (const QJsonValueConstRef value : nodes)
    {
        const QString identity = value.toObject().value(QStringLiteral("id")).toString();
        RenderGraphActivityFeedback annotation;
        if (IsRenderGraphNodeEnabled(value.toObject()) == false)
        {
            // An authored fact, so it holds even where dependency inspection is unknown.
            annotation.Activity = RenderGraphActivity::Disabled;
            annotation.Label = QStringLiteral("Disabled · bypassed");
            annotation.Explanation = QStringLiteral(
                "Disabled: the compiler connects this node's color input directly to its consumers and emits no "
                "operation for it. Its settings and wires are kept; enable it from the context menu or Properties.");
        }
        else if (hasUnknown == true)
        {
            annotation.Label = QStringLiteral("Activity unknown");
            annotation.Explanation = QStringLiteral("Draft structure or imports are invalid, unresolved or exceed the "
                                                    "inspection bound. Validate the graph to locate the problem.");
        }
        else if (selected.contains(identity) == true)
        {
            annotation.Activity = RenderGraphActivity::Retained;
            annotation.Label = QStringLiteral("On draft route");
            annotation.Explanation =
                QStringLiteral("Required by an exposed output, Export/Present effect or active-camera obligation. This "
                               "is dependency feedback; Validate still checks execution support and required inputs.");
        }
        else
        {
            annotation.Activity = RenderGraphActivity::Inactive;
            annotation.Label = QStringLiteral("Inactive branch");
            annotation.Explanation = QStringLiteral(
                "No route to an exposed output, Export/Present effect or active camera in this configuration. The "
                "compiler skips this branch. Structural validation still checks its declarations and wires.");
        }
        annotation.Explanation += scope;
        feedback.insert(identity, annotation);
    }
    return feedback;
}
} // namespace ludus::editor
