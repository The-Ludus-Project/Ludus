#pragma once

#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <QVector>

namespace ludus::editor
{
class RenderGraphDocument;
/// Stable identities for one output/render provider pair in the current renderer document.
struct RenderGraphResolutionSelection final
{
    QString File;
    QString Configuration;
    QString OutputNode;
    QString RenderNode;
};
/// Find directly connected provider pairs in loaded configurations; reads only, no defaults are written.
[[nodiscard]] QVector<RenderGraphResolutionSelection>
FindRenderGraphResolutionSettings(const RenderGraphDocument& document);
/// Read a still-connected pair. Failure preserves outputs; values remain owned by the graph.
[[nodiscard]] bool ReadRenderGraphResolutionSettings(const RenderGraphDocument& document,
                                                     const RenderGraphResolutionSelection& selection,
                                                     QJsonValue& outExtent,
                                                     QJsonArray& outScale);
/// Edit both providers in one document transaction. Invalid/stale selections preserve draft/history.
[[nodiscard]] bool WriteRenderGraphResolutionSettings(RenderGraphDocument& inoutDocument,
                                                      const RenderGraphResolutionSelection& selection,
                                                      const QJsonValue& extent,
                                                      const QJsonArray& scale,
                                                      QString& outError);
/// Expose the root's ordinary raster resolution without changing its current dimensions.
/// Supports one color target using fixed dimensions or a directly connected legacy fixed/view size.
/// Ambiguous/custom layouts reject without mutation. The caller still owns explicit Save.
[[nodiscard]] bool EnableRenderGraphResolutionSettings(RenderGraphDocument& inoutDocument, QString& outError);
} // namespace ludus::editor
