#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/graphics/renderer/lighting.hpp>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
[[nodiscard]] QString GetGiProbeVolumeComponentId();
[[nodiscard]] QString GetGiProbeReceiverComponentId();

/// Whether the entity owns only the volume and its minimum-corner Transform.
[[nodiscard]] bool IsStandaloneWorldGiProbeVolume(const QJsonObject& entity, const QJsonObject& components);

/// Add the built-in components without changing the bundled schema's recovery fingerprint.
void AppendGiProbeComponents(QJsonArray& inoutComponents);

/// Read an attached volume's grid. A disabled volume produces zero counts unless requested for authoring.
[[nodiscard]] bool ReadWorldGiProbeGrid(const QJsonObject& components,
                                        graphics::renderer::GiProbeGrid& outGrid,
                                        QString& outError,
                                        bool shouldIncludeDisabled = false);

/// The first and last probe positions of a nonempty grid.
[[nodiscard]] foundation::math::Aabb3 ComputeWorldGiProbeBounds(const graphics::renderer::GiProbeGrid& grid) noexcept;

/// Read and write the authored minimum corner in X/Y/Z order, preserving JSON precision.
[[nodiscard]] QJsonArray ReadWorldGiProbeOrigin(const QJsonObject& components);
void WriteWorldGiProbeOrigin(QJsonObject& inoutComponents, const QJsonArray& origin);

/// Change one axis count, shifting its minimum corner if the maximum face stays fixed.
void ResizeWorldGiProbeAxis(QJsonObject& inoutComponents,
                            foundation::usize axis,
                            foundation::uint32 count,
                            bool isMinimumFace);
} // namespace ludus::editor
