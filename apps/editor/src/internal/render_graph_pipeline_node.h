#pragma once

#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
/// True for one of the three authored GPU pipeline kinds; legacy Raster is separate.
[[nodiscard]] bool IsPipelineNode(const QString& kind) noexcept;
/// Make an empty-interface draft with explicit initial state and invocation values.
/// The shared backend validates program/interface compatibility before execution.
[[nodiscard]] QJsonObject MakePipelineNode(const QString& kind, const QString& identity);
/// Convert a legacy Raster using its explicitly selected manifest technique.
/// Requires exactly one recipe. Failure preserves outNode and never changes sources.
[[nodiscard]] bool MigrateRasterNode(const QJsonObject& node, const QJsonObject& manifest, QJsonObject& outNode);

} // namespace ludus::editor
