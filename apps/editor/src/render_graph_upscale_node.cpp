#include "internal/render_graph_upscale_node.h"
#include "internal/render_graph_document.h"

namespace ludus::editor
{
// Thanks to NVIDIA, DLSS Programming Guide; AMD, FidelityFX Super Resolution 2.3.3,
// Inputs; Apple, MTLFXTemporalScaler; and Intel, XeSS Developer Guide. Their input
// distinction informs these authoring presets, not an SDK implementation. References
// and departures: docs/development/render-graph-authoring.md#upscaling-presets.
QStringList GetUpscaleAlgorithms()
{
    return {QStringLiteral("dlss-sr"),
            QStringLiteral("fsr-1"),
            QStringLiteral("fsr-2"),
            QStringLiteral("fsr-3"),
            QStringLiteral("metalfx-spatial"),
            QStringLiteral("metalfx-temporal"),
            QStringLiteral("xess-sr")};
}

QString GetUpscaleLabel(const QString& algorithm)
{
    const QStringList algorithms = GetUpscaleAlgorithms();
    const QStringList labels{QStringLiteral("DLSS Super Resolution"),
                             QStringLiteral("FSR 1 spatial"),
                             QStringLiteral("FSR 2 temporal"),
                             QStringLiteral("FSR 3 upscaling"),
                             QStringLiteral("MetalFX Spatial"),
                             QStringLiteral("MetalFX Temporal"),
                             QStringLiteral("XeSS Super Resolution")};
    const qsizetype index = algorithms.indexOf(algorithm);
    return index < 0 ? QString{} : labels.at(index);
}

QJsonObject MakeUpscaleNode(const QString& algorithm, const QString& identity)
{
    if (GetUpscaleAlgorithms().contains(algorithm) == false)
    {
        return {};
    }
    return {{QStringLiteral("id"), identity},
            {QStringLiteral("kind"), QStringLiteral("Upscale")},
            {QStringLiteral("algorithm"), algorithm}};
}

bool AppendUpscalePreset(QJsonObject& inoutConfiguration, const QString& algorithm, const QString& identity)
{
    const QJsonObject upscale = MakeUpscaleNode(algorithm, identity);
    QJsonArray nodes = inoutConfiguration.value(QStringLiteral("nodes")).toArray();
    QJsonArray edges = inoutConfiguration.value(QStringLiteral("edges")).toArray();
    QStringList used;
    for (const QJsonValueConstRef node : nodes)
    {
        used.append(node.toObject().value(QStringLiteral("id")).toString());
    }
    if (upscale.isEmpty() == true || identity.isEmpty() == true || identity == QStringLiteral("input") ||
        identity.size() > RenderGraphDocument::MAX_IDENTIFIER_LENGTH || used.contains(identity) == true)
    {
        return false;
    }
    used.append(identity);
    const auto allocate = [&used, &identity](const QString& suffix) {
        const QString stem =
            identity.left(RenderGraphDocument::MAX_IDENTIFIER_LENGTH - suffix.size() - 1) + QLatin1Char('-') + suffix;
        QString result = stem;
        for (qsizetype index = 2; used.contains(result) == true; ++index)
        {
            const QString ending = QLatin1Char('-') + QString::number(index);
            result = stem.left(RenderGraphDocument::MAX_IDENTIFIER_LENGTH - ending.size()) + ending;
        }
        used.append(result);
        return result;
    };
    const auto connect = [&edges](const QString& source, const QString& destination) {
        edges.append(QJsonObject{{QStringLiteral("from"), source}, {QStringLiteral("to"), destination}});
    };
    const QString outputSize = allocate(QStringLiteral("output-size"));
    const QString renderSize = allocate(QStringLiteral("render-size"));
    nodes.append(QJsonObject{{QStringLiteral("id"), outputSize},
                             {QStringLiteral("kind"), QStringLiteral("OutputSize")},
                             {QStringLiteral("extent"), QStringLiteral("Viewport")}});
    nodes.append(QJsonObject{{QStringLiteral("id"), renderSize},
                             {QStringLiteral("kind"), QStringLiteral("RenderSize")},
                             {QStringLiteral("extent"), QStringLiteral("OutputSize")},
                             {QStringLiteral("scale"), QJsonArray{1, 2}}});
    connect(outputSize + QStringLiteral(".size"), renderSize + QStringLiteral(".output"));
    for (const QJsonValueConstRef input : GetUpscaleInputPorts(upscale))
    {
        const QString port = input.toObject().value(QStringLiteral("id")).toString();
        const QString type = input.toObject().value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("ColorTexture") && type != QStringLiteral("DepthTexture"))
        {
            continue; // Per-frame temporal values must come from authored providers.
        }
        const QString target = allocate(port);
        nodes.append(QJsonObject{
            {QStringLiteral("id"), target},
            {QStringLiteral("kind"), QStringLiteral("Target")},
            {QStringLiteral("type"), type},
            {QStringLiteral("format"),
             type == QStringLiteral("DepthTexture") ? QStringLiteral("Depth32Float") : QStringLiteral("RGBA8Unorm")},
            {QStringLiteral("extent"), QStringLiteral("RenderSize")},
            {QStringLiteral("samples"), 1},
            {QStringLiteral("imported"), false},
            {QStringLiteral("initialized"), false},
            {QStringLiteral("presentation"), false}});
        connect((port == QStringLiteral("destination") ? outputSize : renderSize) + QStringLiteral(".size"),
                target + QStringLiteral(".size"));
        connect(target + QStringLiteral(".texture"), identity + QLatin1Char('.') + port);
    }
    nodes.append(upscale);
    if (nodes.size() > RenderGraphDocument::MAX_NODES || edges.size() > RenderGraphDocument::MAX_EDGES)
    {
        return false;
    }
    inoutConfiguration.insert(QStringLiteral("nodes"), nodes);
    inoutConfiguration.insert(QStringLiteral("edges"), edges);
    return true;
}

QJsonArray GetUpscaleInputPorts(const QJsonObject& node)
{
    const QString algorithm = node.value(QStringLiteral("algorithm")).toString();
    if (GetUpscaleAlgorithms().contains(algorithm) == false)
    {
        return {};
    }
    QJsonArray ports;
    const auto append = [&ports](const QString& identity, const QString& type) {
        ports.append(QJsonObject{{QStringLiteral("id"), identity}, {QStringLiteral("type"), type}});
    };
    append(QStringLiteral("color"), QStringLiteral("ColorTexture"));
    append(QStringLiteral("destination"), QStringLiteral("ColorTexture"));
    if (algorithm != QStringLiteral("fsr-1") && algorithm != QStringLiteral("metalfx-spatial"))
    {
        append(QStringLiteral("depth"), QStringLiteral("DepthTexture"));
        append(QStringLiteral("motion"), QStringLiteral("ColorTexture"));
        append(QStringLiteral("jitter_pixel"), QStringLiteral("Float2"));
        append(QStringLiteral("motion_scale"), QStringLiteral("Float2"));
        append(QStringLiteral("exposure"), QStringLiteral("Float32"));
        append(QStringLiteral("reset"), QStringLiteral("UInt32"));
    }
    return ports;
}

QString GetUpscaleAvailabilityMessage(const QJsonObject& node)
{
    return QStringLiteral("%1 is an authoring preset only. Its backend is unavailable on all current Ludus "
                          "execution profiles. Keep it as an unused draft to retain the current color path. "
                          "Edit the preset's OutputSize node for viewport or fixed output pixels, and its RenderSize "
                          "node for render scale. The color/depth/motion targets need authored producers.")
        .arg(GetUpscaleLabel(node.value(QStringLiteral("algorithm")).toString()));
}
} // namespace ludus::editor
