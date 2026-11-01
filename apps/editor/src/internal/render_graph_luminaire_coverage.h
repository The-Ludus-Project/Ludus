#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QByteArray>
#include <QString>

namespace ludus::editor
{
class RenderGraphDocument;

/// The analytic panel of a slice-sampling-luminaire template copy, as its project-owned luminaire.slang places it.
/// The template lights surfaces with this panel only (see render-graph-authoring.md, "Sample a directional luminaire
/// with slice sampling"); the World's emitters, lights and environment do not light them.
struct RenderGraphLuminairePanel final
{
    /// Package-relative luminaire.slang beside the route's shade.slang.
    QString Source;
    /// False when Source is missing or its placement constants are not plain literals; the others are then zero.
    bool IsPlacementKnown = false;
    foundation::math::Vector3 CenterWorld{};
    /// Half the panel's side length, in world units.
    foundation::float32 HalfExtentWorld = 0;
    /// Unit emitting normal; the panel lights the half space it points into.
    foundation::math::Vector3 NormalWorld{};
};

/// The World inputs the panel check reads, summarised from the viewport's extracted scene.
struct RenderGraphLuminaireWorld final
{
    /// False when the World has no visible geometry; BoundsWorld is then ignored.
    bool HasBounds = false;
    foundation::math::Aabb3 BoundsWorld{};
    /// Visible objects whose surface emits, and authored analytic lights; neither lights the template's surfaces.
    foundation::int32 EmissiveObjectCount = 0;
    foundation::int32 LightCount = 0;
};

/// Find the slice-sampling-luminaire shade program on the retained route of configuration in the document's root
/// graph, including retained Subgraphs, and read its panel placement from the draft luminaire.slang. Programs are
/// recognised by their manifest function names and shade.slang source, which both template import paths keep.
/// @return False when no retained node runs that program; outPanel is then unchanged.
/// @note GUI thread only. Reads drafts; no file, shader or GPU access. Bounded by the document's import depth.
[[nodiscard]] bool FindRenderGraphLuminairePanel(const RenderGraphDocument& document,
                                                 const QString& configuration,
                                                 RenderGraphLuminairePanel& outPanel);

/// Read LUMINAIRE_CENTER_WORLD, LUMINAIRE_HALF_EXTENT_WORLD and LUMINAIRE_NORMAL_WORLD from luminaire.slang source.
/// Each must be a `static const` declaration of decimal literals (an `f` suffix is accepted) outside comments, with a
/// positive half extent and a nonzero normal, which is normalised.
/// @return False for any other spelling; outPanel's placement is then unchanged.
[[nodiscard]] bool ReadRenderGraphLuminairePlacement(const QByteArray& source, RenderGraphLuminairePanel& outPanel);

/// Describe why panel cannot light world, or return an empty string when it can. The panel cannot light the World
/// when no geometry lies in front of it, when its center lies outside the World's bounds, or when its side is below
/// MIN_PANEL_WORLD_FRACTION of the World's bounds diagonal (it then lights at most a patch). The text names the
/// placement, the World's size and its ignored emitters and lights, and the next step: edit the named constants or
/// route the output from a template that samples the World's emitters. Unknown placements are reported as such.
[[nodiscard]] QString DescribeRenderGraphLuminaireCoverage(const RenderGraphLuminairePanel& panel,
                                                           const RenderGraphLuminaireWorld& world);

/// A panel whose side is below this fraction of the World's bounds diagonal lights at most a small patch. The
/// template's default fits its 2 x 2 x 2 test room at 0.2; a 20 m corridor with the default panel is near 0.03.
inline constexpr foundation::float32 MIN_PANEL_WORLD_FRACTION = 0.05F;
} // namespace ludus::editor
