#pragma once

#include <ludus/graphics/renderer/lighting.hpp>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
[[nodiscard]] QString GetGiProbeVolumeComponentId();
[[nodiscard]] QString GetGiProbeReceiverComponentId();

/// Add the built-in components without changing the bundled schema's recovery fingerprint.
void AppendGiProbeComponents(QJsonArray& inoutComponents);

/// Read an attached volume's grid. A disabled volume validates but produces zero counts.
[[nodiscard]] bool
ReadWorldGiProbeGrid(const QJsonObject& components, graphics::renderer::GiProbeGrid& outGrid, QString& outError);
} // namespace ludus::editor
