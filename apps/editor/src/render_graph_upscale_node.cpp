#include "internal/render_graph_upscale_node.h"

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
                          "Output color writes the connected destination texture; its extent sets the output size.")
        .arg(GetUpscaleLabel(node.value(QStringLiteral("algorithm")).toString()));
}
} // namespace ludus::editor
