#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/light_selection.hpp>

#include "internal/builtin_world.h"

#include <QString>

namespace ludus::editor
{
class RenderGraphDocument;
struct RenderScene;

/// The World inputs the luminaire check reads, summarised from the viewport's extracted scene. The
/// slice-sampling-luminaire template lights surfaces with the World's luminaires only (see render-graph-authoring.md,
/// "Sample a directional luminaire with slice sampling"); the World's emitters, lights and environment do not.
struct RenderGraphLuminaireWorld final
{
    /// False when the World has no visible geometry; BoundsWorld is then ignored.
    bool HasBounds = false;
    foundation::math::Aabb3 BoundsWorld{};
    /// Visible objects whose surface emits, and authored analytic lights; neither lights the template's surfaces.
    foundation::int32 EmissiveObjectCount = 0;
    foundation::int32 LightCount = 0;
    /// The World's enabled luminaires (RenderScene::Luminaires).
    graphics::renderer::PanelLuminaireSelection Luminaires{};
    /// Whether the World uses the default schema, whose luminaire presets the repairs create and edit.
    bool CanRepair = false;
    /// The point a repair hangs a luminaire above, when HasAnchor: part of the way from the viewport camera to the
    /// surface it looks at (SummarizeRenderGraphLuminaireWorld), so the luminaire's footprint is in view.
    bool HasAnchor = false;
    foundation::math::Vector3 AnchorWorld{};
    /// Distance from AnchorWorld up to the first surface above it, when HasCeiling; FitRenderGraphLuminaire then
    /// places the panel just below that ceiling instead of below the World's bounds.
    bool HasCeiling = false;
    foundation::float32 CeilingHeightWorld = 0;
};

/// The World edit that resolves a luminaire warning. The editor offers it in a confirmation box, as one undoable edit.
enum class RenderGraphLuminaireRepair : foundation::uint8
{
    None,          ///< Nothing to repair, or the World's schema has no luminaire preset.
    AddLuminaire,  ///< Create a luminaire entity at Placement.
    MoveLuminaire, ///< Move the luminaire whose SourceId is LuminaireSourceId to Placement and set its half extent.
};

/// Why the World's luminaires cannot light it, and how to fix it.
struct RenderGraphLuminaireCoverage final
{
    /// Empty when some luminaire can light the World.
    QString Warning;
    RenderGraphLuminaireRepair Repair = RenderGraphLuminaireRepair::None;
    /// The fitted placement AddLuminaire creates or MoveLuminaire applies.
    WorldLuminairePlacement Placement{};
    /// Source identity (RenderScene::Luminaires) of the luminaire MoveLuminaire edits; zero otherwise.
    foundation::uint64 LuminaireSourceId = 0;
};

/// Whether the retained route of configuration in the document's root graph, including retained Subgraphs, runs a
/// slice-sampling-luminaire shade program. Programs are recognised by their manifest function names and shade.slang
/// source, which both template import paths keep.
/// @note GUI thread only. Reads drafts; no file, shader or GPU access. Bounded by the document's import depth.
[[nodiscard]] bool IsRenderGraphLuminaireRoute(const RenderGraphDocument& document, const QString& configuration);

/// The viewport camera a luminaire repair is fitted around.
struct RenderGraphLuminaireView final
{
    foundation::math::Vector3 EyeWorld{};
    /// Unit viewing direction.
    foundation::math::Vector3 ForwardWorld{0, 0, -1};
};

/// Fraction of the distance from the camera to the surface it looks at where a repair anchors its luminaire.
inline constexpr foundation::float32 LUMINAIRE_ANCHOR_VIEW_FRACTION = 0.7F;

/// Summarise scene for DescribeRenderGraphLuminaireCoverage. With a view and a World that needs a repair, traces the
/// view ray to the surface the camera looks at, anchors the repair LUMINAIRE_ANCHOR_VIEW_FRACTION of the way there
/// (at the eye when the ray hits nothing) and traces up from the anchor to its ceiling. The rays are brute force
/// through the scene's objects, so call it only when the World changes or a repair is requested.
/// @param viewOrNull The viewport camera; null skips the repair's anchor and ceiling.
[[nodiscard]] RenderGraphLuminaireWorld SummarizeRenderGraphLuminaireWorld(const RenderScene& scene,
                                                                           bool canRepair,
                                                                           const RenderGraphLuminaireView* viewOrNull);

/// Describe why world's luminaires cannot light it, or return an empty Warning when one can. A luminaire cannot light
/// the World when no geometry lies in front of it, when its center lies outside the World's bounds, or when its side
/// is below MIN_PANEL_WORLD_FRACTION of the World's bounds diagonal (it then lights at most a patch). The text names
/// the placement, the World's size and its ignored emitters and lights. With CanRepair and bounds, the result offers
/// to add a fitted luminaire to a World without one, or to move and resize the first luminaire otherwise.
[[nodiscard]] RenderGraphLuminaireCoverage DescribeRenderGraphLuminaireCoverage(const RenderGraphLuminaireWorld& world);

/// A downward-facing luminaire for world: below the ceiling above its anchor when known, otherwise just below the
/// top of its bounds above the anchor (or their center when the anchor lies outside them), with a side of
/// FITTED_PANEL_WORLD_FRACTION of the bounds' smaller horizontal extent. The result satisfies
/// DescribeRenderGraphLuminaireCoverage's size rule.
/// @pre world.HasBounds.
[[nodiscard]] WorldLuminairePlacement FitRenderGraphLuminaire(const RenderGraphLuminaireWorld& world);

/// A panel whose side is below this fraction of the World's bounds diagonal lights at most a small patch. The
/// default luminaire fits its 2 x 2 x 2 test room at 0.2; a 20 m corridor with the default panel is near 0.03.
inline constexpr foundation::float32 MIN_PANEL_WORLD_FRACTION = 0.05F;
/// Side of a fitted panel as a fraction of the World bounds' smaller horizontal extent.
inline constexpr foundation::float32 FITTED_PANEL_WORLD_FRACTION = 0.3F;
} // namespace ludus::editor
