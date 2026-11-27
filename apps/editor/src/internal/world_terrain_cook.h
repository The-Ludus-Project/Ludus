#pragma once

#include <ludus/content/terrain/terrain_cook.h>
#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/math/vector.hpp>

#include <QJsonObject>
#include <QList>
#include <QString>

// Cooked page inspection of World terrain: cook a terrain's final heights into a versioned page revision with
// Ludus::ContentTerrain, read every page back from its own byte range, and describe the result for the Terrain
// panel and its page-border overlay. The revision exists only for inspection; World files keep their recipe. See
// docs/modules/content/terrain.md#inspect-and-read-a-cooked-revision.

namespace ludus::editor
{
/// Most pages along one axis of an inspection revision, so the page borders stay readable in the viewport.
inline constexpr foundation::uint32 MAX_TERRAIN_INSPECTION_PAGES_PER_AXIS = 4;
/// Height step of the inspection codec in metres: signed 32-bit codes around a zero origin. A power of two keeps
/// every decoded height exact in binary64; the range far exceeds any World terrain.
inline constexpr foundation::float32 TERRAIN_INSPECTION_QUANTUM_METRES = 1.0F / 1024.0F;
/// Revision an inspection publishes. It has no earlier revision to recook from.
inline constexpr foundation::uint64 TERRAIN_INSPECTION_REVISION = 1;

/// A terrain's cooked revision and the heights decoded from it.
struct TerrainCookedInspection final
{
    /// The manifest and page pack, as an SDK consumer receives them.
    content::terrain::TerrainCookedRevision Revision;
    /// The layout the manifest records.
    content::terrain::TerrainRevisionLayout Layout{};
    /// Sample grid of the cooked domain, equal to the terrain mesh's.
    content::terrain::TerrainFieldGrid Grid{};
    /// Decoded height of every sample in local metres, row-major like the terrain mesh.
    QList<foundation::float64> HeightsMetres;
    /// Largest |decoded - source| over every page core, in metres.
    foundation::float32 LargestErrorMetres = 0;
    /// Page borders in the terrain's local space: one polyline per page column edge along +Z, then one per page
    /// row edge along +X, through the decoded heights of the samples on the edge.
    QList<QList<foundation::math::Vector3>> BorderLinesMesh;
};

/// Choose the cells per page edge for a domain: the smallest common divisor of both cell counts that leaves at most
/// MAX_TERRAIN_INSPECTION_PAGES_PER_AXIS pages along each axis, or their greatest common divisor when none does.
[[nodiscard]] foundation::uint32 ChooseTerrainInspectionPageCells(foundation::uint32 cellsX,
                                                                  foundation::uint32 cellsZ) noexcept;

/// Cook a terrain recipe's final heights, after rivers and sculpt strokes, and decode them page by page.
///
/// The field is the terrain mesh's: its positions are the source elevations and its grid the cooked domain. The
/// manifest records the recipe's noise layout as its RecipeVersion and TryComputeTerrainFieldDigest of the field
/// as its SourceDigest, so a consumer that regenerates the field reproduces the manifest bytes.
/// @param recipe A terrain recipe that BuildTerrainMesh accepts.
/// @param outInspection Written only on success.
/// @param outError The cause and the next step on failure.
[[nodiscard]] bool
CookTerrainForInspection(const QJsonObject& recipe, TerrainCookedInspection& outInspection, QString& outError);

/// One-paragraph summary for the Terrain panel: revision, pages, height step, largest error and noise layout.
[[nodiscard]] QString DescribeTerrainCookedInspection(const TerrainCookedInspection& inspection);
} // namespace ludus::editor
