#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ludus::editor
{
/// True for one of the three authored GPU pipeline kinds; legacy Raster is separate.
[[nodiscard]] bool IsPipelineNode(const QString& kind) noexcept;
/// Shader stage names accepted for this authored pipeline kind; empty for other kinds.
[[nodiscard]] QStringList GetPipelineShaderStages(const QString& kind);
/// Make an empty-interface draft; ray limits are automatic until explicitly overridden.
/// The shared backend validates program/interface compatibility before execution.
[[nodiscard]] QJsonObject MakePipelineNode(const QString& kind, const QString& identity);
/// True for a ComputePipeline that declares a Scene input: a scene compute pass, which the builder
/// lowers as a member of the ordered ray sequence. Other compute drafts stay generic.
[[nodiscard]] bool IsSceneComputeNode(const QJsonObject& node);
/// True when the node's invocation is a ray launch: a RayTracingPipeline or a scene compute pass.
/// Its dimensions are "ColorTarget" or an explicit [width, height, 1].
[[nodiscard]] bool UsesRayLaunch(const QJsonObject& node);
/// Default scene compute workgroup used when no pass in the shader bundle overrides it.
[[nodiscard]] QJsonArray GetSceneComputeGroupSize();
/// Admitted explicit shapes, imported from the compiler policy; every explicit override in a bundle must agree.
[[nodiscard]] QJsonArray GetSupportedSceneComputeGroupSizes();
/// Reconcile state and invocation when an edit adds or removes a ComputePipeline's Scene input.
/// A new scene compute pass gets automatic state ({}) and Match color target. A pass that loses its
/// Scene input gets the generic [1, 1, 1] group size and dimensions where they are missing.
/// Returns node unchanged for other kinds and for edits that keep the profile, so drafts round-trip.
[[nodiscard]] QJsonObject ApplyComputeProfile(const QJsonObject& previousNode, const QJsonObject& node);
/// Make a generic ComputePipeline a scene compute pass: add the missing Scene and View inputs with
/// unique IDs, then apply the scene compute defaults. Wires are not changed.
/// Returns false and preserves outNode for other kinds and for nodes that already have a Scene input.
[[nodiscard]] bool MakeSceneComputeNode(const QJsonObject& node, QJsonObject& outNode);
/// Convert a legacy Raster using its explicitly selected manifest technique.
/// Requires exactly one recipe. Failure preserves outNode and never changes sources.
[[nodiscard]] bool MigrateRasterNode(const QJsonObject& node, const QJsonObject& manifest, QJsonObject& outNode);

} // namespace ludus::editor
