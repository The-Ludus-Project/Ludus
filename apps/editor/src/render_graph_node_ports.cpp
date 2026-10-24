#include "internal/render_graph_node_ports.h"

#include <ludus/foundation/base/core.h>

#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_scene_bundle.h"
#include "internal/render_graph_upscale_node.h"

namespace ludus::editor
{
QVector<RenderGraphPort> ReadRenderGraphInterfacePorts(const QJsonArray& ports)
{
    QVector<RenderGraphPort> result;
    for (const QJsonValueConstRef port : ports)
    {
        result.append({port.toObject().value(QStringLiteral("id")).toString(),
                       port.toObject().value(QStringLiteral("type")).toString()});
    }
    return result;
}

bool GetRenderGraphNodePorts(const RenderGraphDocument& document,
                             const QJsonObject& node,
                             RenderGraphNodePorts& outPorts)
{
    const QString kind = node.value(QStringLiteral("kind")).toString();
    RenderGraphNodePorts ports;
    if (kind == QStringLiteral("Upscale"))
    {
        ports.Inputs = ReadRenderGraphInterfacePorts(GetUpscaleInputPorts(node));
        ports.Outputs = {{QStringLiteral("color"), QStringLiteral("ColorTexture")}};
    }
    else if (IsPipelineNode(kind) == true)
    {
        ports.Inputs = ReadRenderGraphInterfacePorts(node.value(QStringLiteral("inputs")).toArray());
        AppendSceneBundleInputs(ports.Inputs);
        ports.Outputs = ReadRenderGraphInterfacePorts(node.value(QStringLiteral("outputs")).toArray());
    }
    else if (kind == QStringLiteral("Accumulate") || kind == QStringLiteral("ToneMap") ||
             kind == QStringLiteral("Denoise"))
    {
        ports.Inputs = {{QStringLiteral("color"), QStringLiteral("ColorTexture")}};
        ports.Outputs = {{QStringLiteral("color"), QStringLiteral("ColorTexture")}};
    }
    else if (kind == QStringLiteral("Scene"))
    {
        ports.Outputs = {{QStringLiteral("scene"), QStringLiteral("Scene")},
                         {QStringLiteral("geometry"), QStringLiteral("SceneGeometry")},
                         {QStringLiteral("materials"), QStringLiteral("SceneMaterials")},
                         {QStringLiteral("lights"), QStringLiteral("SceneLights")}};
    }
    else if (kind == QStringLiteral("Camera"))
    {
        ports.Inputs = {{QStringLiteral("target"), node.value(QStringLiteral("target_type")).toString()}};
        ports.Outputs = {{QStringLiteral("view"), QStringLiteral("View")}};
    }
    else if (kind == QStringLiteral("Buffer") || kind == QStringLiteral("StructuredBuffer"))
    {
        if (node.contains(QStringLiteral("bytes_per_pixel")) == true)
        {
            ports.Inputs = {{QStringLiteral("size"), QStringLiteral("RenderSize")}};
        }
        ports.Outputs = {{QStringLiteral("buffer"), QStringLiteral("Buffer")}};
    }
    else if (kind == QStringLiteral("Target"))
    {
        if (node.value(QStringLiteral("extent")).toString() == QStringLiteral("RenderSize"))
        {
            ports.Inputs = {{QStringLiteral("size"), QStringLiteral("RenderSize")}};
        }
        ports.Outputs = {{QStringLiteral("texture"), node.value(QStringLiteral("type")).toString()}};
    }
    else if (kind == QStringLiteral("RenderSize") || kind == QStringLiteral("OutputSize"))
    {
        if (node.value(QStringLiteral("extent")).toString() == QStringLiteral("Texture"))
        {
            ports.Inputs = {{QStringLiteral("texture"), QStringLiteral("Texture")}};
        }
        if (node.value(QStringLiteral("extent")).toString() == QStringLiteral("OutputSize"))
        {
            ports.Inputs = {{QStringLiteral("output"), QStringLiteral("RenderSize")}};
        }
        ports.Outputs = {{QStringLiteral("size"), QStringLiteral("RenderSize")}};
    }
    else if (kind == QStringLiteral("Raster"))
    {
        ports.Inputs = {{QStringLiteral("scene"), QStringLiteral("Scene")},
                        {QStringLiteral("view"), QStringLiteral("View")},
                        {QStringLiteral("depth"), QStringLiteral("DepthTexture")}};
        ports.Outputs = {{QStringLiteral("depth"), QStringLiteral("DepthTexture")}};
        if (node.value(QStringLiteral("mode")).toString() == QStringLiteral("ColorAndDepth"))
        {
            ports.Inputs.append({QStringLiteral("color"), QStringLiteral("ColorTexture")});
            ports.Outputs.append({QStringLiteral("color"), QStringLiteral("ColorTexture")});
        }
    }
    else if (kind == QStringLiteral("Subgraph"))
    {
        // Assets are package-relative, exactly as the shared validator resolves them.
        QJsonObject graph;
        if (document.GetGraph(node.value(QStringLiteral("asset")).toString(), graph) == false)
        {
            return false;
        }
        QJsonObject selected;
        for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
        {
            if (value.toObject().value(QStringLiteral("id")) == node.value(QStringLiteral("configuration")))
            {
                selected = value.toObject();
                break;
            }
        }
        if (selected.isEmpty() == true)
        {
            return false;
        }
        ports.Inputs = ReadRenderGraphInterfacePorts(selected.value(QStringLiteral("inputs")).toArray());
        ports.Outputs = ReadRenderGraphInterfacePorts(selected.value(QStringLiteral("outputs")).toArray());
    }
    else if (kind == QStringLiteral("Export"))
    {
        ports.Inputs = {{QStringLiteral("texture"), node.value(QStringLiteral("type")).toString()}};
    }
    else if (kind == QStringLiteral("Present"))
    {
        ports.Inputs = {{QStringLiteral("color"), QStringLiteral("ColorTexture")}};
    }
    outPorts = foundation::Move(ports);
    return true;
}

} // namespace ludus::editor
