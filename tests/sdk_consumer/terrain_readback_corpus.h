#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/content/terrain/terrain_cook.h>
#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/content/terrain/terrain_noise.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/vector.hpp>

// Terrain readback corpus shared by ludus_content_terrain_tests, the Editor tests and the installed SDK consumer,
// so every build checks one cooked revision. The corpus field is the Editor's built-in World terrain for the recipe
// below, generated with each noise layout. The Editor cooks that terrain for its Show cooked pages inspection
// (apps/editor/src/world_terrain_cook.cpp) and its tests check that it writes the manifest pinned here; the SDK
// consumer cooks the same field with the installed Ludus::ContentTerrain and reads it back page by page. See
// docs/modules/content/terrain.md#inspect-and-read-a-cooked-revision. Exception-free; Catch2 stays in the tests.
namespace ludus::content::terrain::testing
{
/// Samples per side of the corpus terrain, the Editor recipe's `samples`.
inline constexpr uint32 READBACK_SAMPLES_PER_SIDE = 64;
/// Width and depth of the corpus terrain in metres, so each of its 63 cells is half a metre square.
inline constexpr float64 READBACK_EXTENT_METRES = 31.5;
/// The recipe's `minimumElevation` in metres.
inline constexpr float64 READBACK_MINIMUM_ELEVATION_METRES = -2;
/// The recipe's `maximumElevation` in metres.
inline constexpr float64 READBACK_MAXIMUM_ELEVATION_METRES = 6;
/// The recipe's `featureSize` in metres.
inline constexpr float64 READBACK_FEATURE_SIZE_METRES = 12;
/// The recipe's `roughness`.
inline constexpr float64 READBACK_ROUGHNESS = 0.45;
/// The recipe's `seed`.
inline constexpr uint64 READBACK_SEED = 1198;
/// Random domain of the Editor's terrain elevation noise.
inline constexpr uint32 READBACK_RANDOM_DOMAIN = 1;

/// Cells per page edge the Editor chooses for 63 cells: the smallest divisor that leaves at most four pages per
/// axis.
inline constexpr uint32 READBACK_CELLS_PER_PAGE = 21;
/// Pages along each axis of the corpus revision.
inline constexpr uint32 READBACK_PAGES_PER_AXIS = 3;
/// Height step of the Editor's inspection codec: signed 32-bit codes of 1/1024 m around a zero origin. A power of
/// two keeps every decoded height exact in binary64.
inline constexpr float32 READBACK_QUANTUM_METRES = 1.0F / 1024.0F;
/// Revision the Editor's inspection publishes.
inline constexpr uint64 READBACK_REVISION = 1;

/// XXH3-128 of the corpus manifest for the cubic layout, recorded from the native reference build. The manifest
/// carries every page's payload digest, so it pins the whole revision. A change to the noise, the codec or the
/// manifest layout changes it; update it only with an explained behaviour change.
inline constexpr foundation::Fingerprint128 EXPECTED_CUBIC_READBACK_MANIFEST_DIGEST
{
    .Low = 0xbeafdb93e0fd5a6eULL,
    .High = 0xf449e81a8fa794f2ULL,
};
/// XXH3-128 of the corpus manifest for the quintic layout, under the same rules.
inline constexpr foundation::Fingerprint128 EXPECTED_QUINTIC_READBACK_MANIFEST_DIGEST
{
    .Low = 0x43e3edb0ae83d473ULL,
    .High = 0x7bd4fdfd5e5d1c99ULL,
};

/// The pinned manifest digest of a layout's corpus revision.
[[nodiscard]] foundation::Fingerprint128 GetExpectedReadbackManifestDigest(TerrainNoiseLayout layout) noexcept;

/// One complete elevation field.
struct TerrainReadbackField final
{
    /// The field's grid: columns along +X, rows along +Z.
    TerrainFieldGrid Grid{};
    /// One elevation per sample in metres, row-major.
    foundation::Array<float32> ElevationsMetres;
};

/// Generate the corpus field exactly as the Editor's terrain mesh samples it: local X and Z run from minus to plus
/// half the extent, and each elevation is rounded to binary32. Failure preserves outField.
[[nodiscard]] bool TryBuildReadbackField(TerrainNoiseLayout layout, TerrainReadbackField& outField) noexcept;

/// The revision layout the Editor's inspection uses for the corpus field.
/// @param layout The noise layout the field was generated with; recorded as the RecipeVersion.
/// @param sourceDigest TryComputeTerrainFieldDigest of the field.
[[nodiscard]] TerrainRevisionLayout MakeReadbackLayout(TerrainNoiseLayout layout,
                                                       foundation::Fingerprint128 sourceDigest) noexcept;

/// A revision decoded the way an SDK consumer reads it.
struct TerrainReadback final
{
    /// The domain field's grid.
    TerrainFieldGrid Grid{};
    /// The manifest's RecipeVersion.
    uint32 RecipeVersion = 0;
    /// The manifest codec's quantum in metres.
    float32 QuantumMetres = 0;
    /// Pages decoded, each from its own byte range.
    usize PagesRead = 0;
    /// Decoded height of every domain sample in metres, row-major.
    foundation::Array<float64> HeightsMetres;
    /// Unit normal of every domain sample, from central differences of the decoded heights; one-sided on the
    /// domain edge, like the Editor's terrain mesh normals.
    foundation::Array<foundation::math::Vector3d> Normals;
};

/// The two byte sequences a consumer receives for one revision.
struct TerrainRevisionBytes final
{
    /// The manifest bytes.
    foundation::BorrowedView<const uint8> Manifest;
    /// The page pack the manifest indexes.
    foundation::BorrowedView<const uint8> PagePack;
};

/// Borrow both byte sequences of a cooked revision for as long as it stays alive and unchanged.
[[nodiscard]] TerrainRevisionBytes GetRevisionBytes(const TerrainCookedRevision& revision) noexcept;

/// Read a revision as a consumer does: validate the manifest, decode every page from exactly its record's byte
/// range of the page pack and nothing else, then assemble the page cores into the domain field. A sample that
/// several pages share must decode to the same height in each. Failure preserves outReadback.
[[nodiscard]] bool TryReadTerrainReadback(const TerrainRevisionBytes& bytes, TerrainReadback& outReadback) noexcept;

/// Largest angle in radians between a normal derived from source heights and one derived from heights decoded
/// within half a quantum of them. Each slope component then differs by at most quantum / spacing (a one-sided
/// difference on the edge), so the gradients differ by at most sqrt(2) quantum / spacing. The remainder covers
/// binary32 tangents of the Editor's mesh.
[[nodiscard]] float64 GetReadbackNormalToleranceRadians(float32 quantumMetres, float32 spacingMetres) noexcept;

/// Angle in radians between two unit vectors.
[[nodiscard]] float64 GetAngleRadians(foundation::math::Vector3d first, foundation::math::Vector3d second) noexcept;

/// Cook the corpus field of a layout with the inspection layout, then compute its manifest digest.
/// @param layout The noise layout.
/// @param outRevision Receives the cooked revision.
/// @param outDigest Receives StableFingerprint128 of the manifest bytes.
[[nodiscard]] bool TryCookReadbackRevision(TerrainNoiseLayout layout,
                                           TerrainCookedRevision& outRevision,
                                           foundation::Fingerprint128& outDigest) noexcept;

/// For each noise layout, cook the corpus revision, check its pinned manifest digest, read it back page by page,
/// and check every decoded height within half a quantum of the source and every normal within
/// GetReadbackNormalToleranceRadians of the source field's.
[[nodiscard]] bool RunTerrainReadbackCorpus() noexcept;
} // namespace ludus::content::terrain::testing
