#pragma once

#include "render_graph_layout_policy.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRectF>

namespace ludus::editor
{
/// Validate the complete bounded frame array, including IDs, Unicode text, RGB colours and
/// integer geometry. Matches the offline schema; no document edits or output side effects.
[[nodiscard]] bool AreRenderGraphCommentFramesValid(const QJsonArray& frames);
/// Decode a previously validated frame's saved scene rectangle.
[[nodiscard]] QRectF GetRenderGraphCommentBounds(const QJsonObject& frame);
/// Replace only integer geometry on a copied frame; validation belongs to the caller.
/// @pre All rectangle components are finite and round into the int32 range.
[[nodiscard]] QJsonObject SetRenderGraphCommentBounds(QJsonObject frame, const QRectF& bounds);
} // namespace ludus::editor
