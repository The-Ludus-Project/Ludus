#include "internal/render_graph_scene_bundle.h"
#include "internal/render_graph_node_ports.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace ludus::editor
{
namespace
{
constexpr qsizetype RAY_CALLBACK_COUNT = 3;
} // namespace

void AppendSceneBundleInputs(QVector<RenderGraphPort>& inoutPorts)
{
    const QVector<RenderGraphPort> original = inoutPorts;
    for (const RenderGraphPort& port : original)
    {
        if (port.Type != QStringLiteral("Scene"))
        {
            continue;
        }
        for (const QString& part : {QStringLiteral("geometry"), QStringLiteral("materials"), QStringLiteral("lights")})
        {
            QString type = part;
            type[0] = type.at(0).toUpper();
            inoutPorts.append({port.Id + QLatin1Char('_') + part, QStringLiteral("Scene") + type});
        }
    }
}

QString GetSceneBundlePortType(const QJsonObject& node, const QString& port)
{
    for (const QJsonValueConstRef value : node.value(QStringLiteral("inputs")).toArray())
    {
        const QJsonObject input = value.toObject();
        if (input.value(QStringLiteral("type")).toString() != QStringLiteral("Scene"))
        {
            continue;
        }
        for (const QString& part : {QStringLiteral("geometry"), QStringLiteral("materials"), QStringLiteral("lights")})
        {
            if (port == input.value(QStringLiteral("id")).toString() + QLatin1Char('_') + part)
            {
                QString type = part;
                type[0] = type.at(0).toUpper();
                return QStringLiteral("Scene") + type;
            }
        }
    }
    return {};
}

bool HasSceneBundle(const QJsonObject& node)
{
    if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Scene"))
    {
        return true;
    }
    for (const QJsonValueConstRef port : node.value(QStringLiteral("inputs")).toArray())
    {
        if (port.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("Scene"))
        {
            return true;
        }
    }
    return false;
}

bool IsSceneBundleMember(const RenderGraphPort& port)
{
    return port.Type == QStringLiteral("SceneGeometry") || port.Type == QStringLiteral("SceneMaterials") ||
           port.Type == QStringLiteral("SceneLights");
}

RenderGraphDocumentStatus PrepareConnectedSceneShaders(RenderGraphSnapshot& inoutSnapshot,
                                                       const QString& rootGraph,
                                                       const QString& packageRoot,
                                                       QString* outErrorOrNull)
{
    const QString manifest =
        rootGraph.chopped(QStringLiteral(".lurender.json").size()) + QStringLiteral(".lushader.json");
    const QJsonObject build = QJsonDocument::fromJson(inoutSnapshot.Files.value(manifest)).object();
    if (build.isEmpty() == true || HasSceneRayInterface(build) == true)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    bool hasConnectedRay = false;
    QStringList files{rootGraph};
    for (qsizetype index = 0; index < files.size(); ++index)
    {
        const QString file = files.at(index);
        const QJsonObject graph = QJsonDocument::fromJson(inoutSnapshot.Files.value(file)).object();
        for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
        {
            const QJsonObject configuration = value.toObject();
            for (const QJsonValueConstRef item : configuration.value(QStringLiteral("nodes")).toArray())
            {
                const QJsonObject node = item.toObject();
                if (node.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"))
                {
                    const QString asset = node.value(QStringLiteral("asset")).toString();
                    if (inoutSnapshot.Files.contains(asset) == true && files.contains(asset) == false)
                    {
                        files.append(asset);
                    }
                }
                if (node.value(QStringLiteral("kind")).toString() != QStringLiteral("RayTracingPipeline") ||
                    node.value(QStringLiteral("shaders")).toArray().size() != RAY_CALLBACK_COUNT)
                {
                    continue;
                }
                QStringList stages;
                for (const QJsonValueConstRef shaderValue : node.value(QStringLiteral("shaders")).toArray())
                {
                    const QJsonObject shader = shaderValue.toObject();
                    const QString stage = shader.value(QStringLiteral("stage")).toString();
                    for (const QJsonValueConstRef programValue : build.value(QStringLiteral("programs")).toArray())
                    {
                        const QJsonObject program = programValue.toObject();
                        if (program.value(QStringLiteral("id")) == shader.value(QStringLiteral("program")) &&
                            program.value(QStringLiteral("stage")).toString() == stage &&
                            stages.contains(stage) == false)
                        {
                            stages.append(stage);
                        }
                    }
                }
                if (stages.contains(QStringLiteral("ray_generation")) == false ||
                    stages.contains(QStringLiteral("miss")) == false ||
                    stages.contains(QStringLiteral("closest_hit")) == false)
                {
                    continue;
                }
                for (const QJsonValueConstRef portValue : node.value(QStringLiteral("inputs")).toArray())
                {
                    const QJsonObject port = portValue.toObject();
                    if (port.value(QStringLiteral("type")).toString() != QStringLiteral("Scene"))
                    {
                        continue;
                    }
                    const QString endpoint = node.value(QStringLiteral("id")).toString() + QLatin1Char('.') +
                                             port.value(QStringLiteral("id")).toString();
                    for (const QJsonValueConstRef edge : configuration.value(QStringLiteral("edges")).toArray())
                    {
                        const QString target = edge.toObject().value(QStringLiteral("to")).toString();
                        hasConnectedRay = hasConnectedRay == true || target == endpoint ||
                                          target == endpoint + QStringLiteral("_geometry");
                    }
                }
            }
        }
    }
    if (hasConnectedRay == false)
    {
        return RenderGraphDocumentStatus::Ok;
    }
    QString error;
    const RenderGraphDocumentStatus status = PrepareRenderGraphTechniques(inoutSnapshot, rootGraph, error);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        if (outErrorOrNull != nullptr)
        {
            *outErrorOrNull = error;
        }
        return status;
    }
    const RenderGraphDocumentStatus converted = ConvertSceneRayInterface(inoutSnapshot, manifest, packageRoot, error);
    if (outErrorOrNull != nullptr)
    {
        *outErrorOrNull = error;
    }
    return converted;
}
} // namespace ludus::editor
