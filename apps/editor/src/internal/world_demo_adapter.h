#pragma once

#include <ludus/foundation/identity/uuid.hpp>

#include "internal/level.h"
#include "internal/render_scene.h"
#include "internal/world_document.h"

namespace ludus::editor
{
// World owns all durable authoring state. This adapter only converts the sample's
// legacy files and projects snapshots into its trusted renderer/player codec.
[[nodiscard]] bool IsWorldDemo(const WorldDocument& document) noexcept;
// A colliding settings-entity ID is replaced by one minted from `entropy`; when
// it is unavailable the import fails with `outError` and `inoutDocument` unchanged.
[[nodiscard]] bool ImportWorldDemo(WorldDocument& inoutDocument,
                                   const foundation::identity::EntropySource& entropy,
                                   const QByteArray& source,
                                   QString& outError);
[[nodiscard]] bool ProjectWorldDemo(const WorldDocument& document, world_demo::Level& output, QString& error);
/// Build the World viewport's scene from the game's own first-frame extraction.
///
/// Builds the World-demo GameWorld from the projected level without ticking it, extracts
/// one frame and converts it with the player's render_items builders. Edit and play
/// therefore draw the same boxes, scatter included. Objects keep their World entity keys
/// (scatter has none), and the game camera becomes the default camera. Runs on the load
/// worker; it executes only the trusted in-tree World-demo code.
/// @param document World-demo World to extract.
/// @param outScene Unlit flat-colour scene over one unit quad, written only on success.
/// @param outError Reason for a rejected World, written only on failure.
/// @return Whether the World projected, built, extracted and converted.
[[nodiscard]] bool LoadWorldDemoScene(const WorldDocument& document, RenderScene& outScene, QString& outError);
} // namespace ludus::editor
