#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ludus::editor
{
/// Stable upscaling preset identities; all are authoring-only on current execution profiles.
[[nodiscard]] QStringList GetUpscaleAlgorithms();
/// Human-readable name for a stable algorithm identity; empty for unknown identities.
[[nodiscard]] QString GetUpscaleLabel(const QString& algorithm);
/// Create a node draft without shaders or disk writes. Returns empty for an unknown algorithm.
[[nodiscard]] QJsonObject MakeUpscaleNode(const QString& algorithm, const QString& identity);
/// Fixed typed inputs, including destination storage. Unknown algorithms return no ports.
[[nodiscard]] QJsonArray GetUpscaleInputPorts(const QJsonObject& node);
/// Explain availability beside editable fields on both canvas and Properties surfaces.
[[nodiscard]] QString GetUpscaleAvailabilityMessage(const QJsonObject& node);
} // namespace ludus::editor
