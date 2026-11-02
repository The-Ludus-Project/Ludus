#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_scene.h"

#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
// Transform-only scene publication: a placement edit derives the presented scene with the
// cook's own object transforms instead of re-cooking the level. The authoritative cook
// still runs afterwards (LevelWorkspace::PublishPlacement) and confirms the result.

/// How DerivePlacedScene answered.
enum class PlacedSceneStatus : foundation::uint8
{
    /// outScene holds the presented scene with the level's cooked object transforms.
    Derived,
    /// Every cooked object transform already equals the presented one; outScene is untouched.
    Unchanged,
    /// The level changes more than object transforms, the presented scene was not cooked from
    /// presentedLevel, or deriving failed (outError says why); only a full assembly can present it.
    RequiresAssembly,
};

/// Derive the scene the cook produces when level differs from presentedLevel only in object transforms.
/// Object transforms come from content::scene_cook::ComposeLevelTransforms, so they equal the assembled
/// bundle bit for bit; meshes, materials, textures and lights are copied from the presented definition.
/// @param presented A cooked scene (Definition set) presenting presentedLevel; its level objects come first.
/// @param presentedLevel Level JSON the presented scene was assembled or derived from.
/// @param level Candidate level JSON.
/// @param outScene Replaced only when the result is Derived. It owns a new immutable definition and new
/// Identity/PacketIdentity values; AssetsIdentity is kept because no asset changed.
/// @param outError Set when deriving failed (out-of-range transform, singular placement or allocation failure).
/// @return Derived, Unchanged or RequiresAssembly. GUI or worker thread; reads no files.
[[nodiscard]] PlacedSceneStatus DerivePlacedScene(const RenderScene& presented,
                                                  const QJsonObject& presentedLevel,
                                                  const QJsonObject& level,
                                                  RenderScene& outScene,
                                                  QString& outError);

/// Whether a scene cooked from a derived scene's level presents the same content: equal asset
/// identities and equal object/mesh/material/texture/light tables, with bit-identical object transforms.
/// A true result lets the derived scene stay presented without a renderer restart.
[[nodiscard]] bool IsSamePlacedScene(const RenderScene& derived, const RenderScene& cooked) noexcept;
} // namespace ludus::editor
