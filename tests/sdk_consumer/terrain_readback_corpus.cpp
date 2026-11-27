#include "terrain_readback_corpus.h"

#include <ludus/content/terrain/terrain_tile.h>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::terrain::testing
{
namespace
{
using foundation::Array;
using foundation::BorrowedView;
using foundation::Fingerprint128;
using foundation::math::Vector3d;
namespace math = foundation::math;

/// Binary32 rounding of the Editor's mesh tangents, as an angle in radians; far below one quantum's slope.
constexpr float64 MESH_TANGENT_ROUNDING_RADIANS = 1e-5;

[[nodiscard]] usize GetSampleIndex(const TerrainFieldGrid& grid, uint32 column, uint32 row) noexcept
{
    return (static_cast<usize>(row) * grid.Columns) + column;
}

/// Unit normals from central differences, one-sided on the edge, in the Editor mesh's order: cross(tangentZ,
/// tangentX). Returns false for a degenerate normal or an allocation failure.
[[nodiscard]] bool TryComputeNormals(const TerrainFieldGrid& grid,
                                     const Array<float64>& heightsMetres,
                                     Array<Vector3d>& outNormals) noexcept
{
    Array<Vector3d> normals;
    if (normals.TryResize(heightsMetres.GetSize()) == false)
    {
        return false;
    }
    for (uint32 row = 0; row < grid.Rows; ++row)
    {
        const uint32 previousRow = row == 0 ? row : row - 1;
        const uint32 nextRow = math::Min(row + 1, grid.Rows - 1);
        for (uint32 column = 0; column < grid.Columns; ++column)
        {
            const uint32 previousColumn = column == 0 ? column : column - 1;
            const uint32 nextColumn = math::Min(column + 1, grid.Columns - 1);
            const Vector3d tangentX{static_cast<float64>(nextColumn - previousColumn) *
                                        static_cast<float64>(grid.SpacingXMetres),
                                    heightsMetres[GetSampleIndex(grid, nextColumn, row)] -
                                        heightsMetres[GetSampleIndex(grid, previousColumn, row)],
                                    0};
            const Vector3d tangentZ{0,
                                    heightsMetres[GetSampleIndex(grid, column, nextRow)] -
                                        heightsMetres[GetSampleIndex(grid, column, previousRow)],
                                    static_cast<float64>(nextRow - previousRow) *
                                        static_cast<float64>(grid.SpacingZMetres)};
            const Vector3d normal = math::Cross(tangentZ, tangentX);
            const float64 length = math::Length(normal);
            if ((length > 0) == false)
            {
                return false;
            }
            normals[GetSampleIndex(grid, column, row)] = normal * (1 / length);
        }
    }
    outNormals = foundation::Move(normals);
    return true;
}
} // namespace

Fingerprint128 GetExpectedReadbackManifestDigest(TerrainNoiseLayout layout) noexcept
{
    return layout == TerrainNoiseLayout::CubicFade ? EXPECTED_CUBIC_READBACK_MANIFEST_DIGEST
                                                   : EXPECTED_QUINTIC_READBACK_MANIFEST_DIGEST;
}

bool TryBuildReadbackField(TerrainNoiseLayout layout, TerrainReadbackField& outField) noexcept
{
    const TerrainNoiseShape shape
    {
        .MinimumElevationMetres = READBACK_MINIMUM_ELEVATION_METRES,
        .MaximumElevationMetres = READBACK_MAXIMUM_ELEVATION_METRES,
        .FeatureSizeMetres = READBACK_FEATURE_SIZE_METRES,
        .Roughness = READBACK_ROUGHNESS,
        .Profile = TerrainNoiseProfile::Hills,
        .Layout = layout,
    };
    math::RandomKey key;
    if (CanSampleTerrainNoise(shape, READBACK_EXTENT_METRES * 0.5) == false ||
        math::TryMakeRandomKey(READBACK_SEED, READBACK_RANDOM_DOMAIN, key) != math::MathStatus::Success)
    {
        return false;
    }
    const uint32 cells = READBACK_SAMPLES_PER_SIDE - 1;
    TerrainReadbackField field;
    field.Grid =
    {
        .Columns = READBACK_SAMPLES_PER_SIDE,
        .Rows = READBACK_SAMPLES_PER_SIDE,
        .SpacingXMetres = static_cast<float32>(READBACK_EXTENT_METRES / cells),
        .SpacingZMetres = static_cast<float32>(READBACK_EXTENT_METRES / cells),
    };
    if (field.ElevationsMetres.TryResize(GetTerrainSampleCount(field.Grid)) == false)
    {
        return false;
    }
    // The same expressions as the Editor's terrain mesh (apps/editor/src/world_terrain.cpp), so every sample is
    // bit-identical to its vertex height.
    for (uint32 row = 0; row < READBACK_SAMPLES_PER_SIDE; ++row)
    {
        const float64 v = static_cast<float64>(row) / cells;
        const float64 positionZ = (v - 0.5) * READBACK_EXTENT_METRES;
        for (uint32 column = 0; column < READBACK_SAMPLES_PER_SIDE; ++column)
        {
            const float64 u = static_cast<float64>(column) / cells;
            const float64 positionX = (u - 0.5) * READBACK_EXTENT_METRES;
            field.ElevationsMetres[GetSampleIndex(field.Grid, column, row)] =
                static_cast<float32>(ComputeTerrainElevation(shape, key, positionX, positionZ));
        }
    }
    outField = foundation::Move(field);
    return true;
}

TerrainRevisionLayout MakeReadbackLayout(TerrainNoiseLayout layout, Fingerprint128 sourceDigest) noexcept
{
    TerrainRevisionLayout revision;
    revision.Revision = READBACK_REVISION;
    revision.RecipeVersion = static_cast<uint32>(layout);
    revision.SourceDigest = sourceDigest;
    revision.FirstPage = {0, 0};
    revision.PagesX = READBACK_PAGES_PER_AXIS;
    revision.PagesZ = READBACK_PAGES_PER_AXIS;
    revision.CellsPerPage = READBACK_CELLS_PER_PAGE;
    revision.SpacingMetres = static_cast<float32>(READBACK_EXTENT_METRES / (READBACK_SAMPLES_PER_SIDE - 1));
    revision.Codec =
    {
        .Encoding = TerrainHeightEncoding::Signed32,
        .OriginMetres = 0,
        .QuantumMetres = READBACK_QUANTUM_METRES,
    };
    return revision;
}

bool TryReadTerrainReadback(BorrowedView<const uint8> manifestBytes,
                            BorrowedView<const uint8> pagePackBytes,
                            TerrainReadback& outReadback) noexcept
{
    TerrainManifest manifest;
    if (TryReadTerrainManifest(manifestBytes, manifest) != TerrainCookStatus::Success)
    {
        return false;
    }
    const TerrainRevisionLayout& layout = manifest.GetLayout();
    TerrainReadback readback;
    readback.Grid =
    {
        .Columns = (layout.PagesX * layout.CellsPerPage) + 1,
        .Rows = (layout.PagesZ * layout.CellsPerPage) + 1,
        .SpacingXMetres = layout.SpacingMetres,
        .SpacingZMetres = layout.SpacingMetres,
    };
    readback.RecipeVersion = layout.RecipeVersion;
    readback.QuantumMetres = layout.Codec.QuantumMetres;
    const usize sampleCount = GetTerrainSampleCount(readback.Grid);
    Array<uint8> isWritten;
    if (readback.HeightsMetres.TryResize(sampleCount) == false || isWritten.TryResize(sampleCount) == false)
    {
        return false;
    }
    const BorrowedView<const TerrainPageRecord> records = manifest.GetPageRecords();
    for (usize pageIndex = 0; pageIndex < records.GetSize(); ++pageIndex)
    {
        const TerrainPageRecord& record = records[pageIndex];
        if (record.ByteOffset > pagePackBytes.GetSize() ||
            record.ByteSize > pagePackBytes.GetSize() - record.ByteOffset)
        {
            return false;
        }
        // Only the page's own bytes are visible to the reader.
        const BorrowedView<const uint8> pageBytes{pagePackBytes.GetDataOrNull() + record.ByteOffset, record.ByteSize};
        TerrainPage page;
        if (TryReadTerrainPage(manifest, pageIndex, pageBytes, page) != TerrainCookStatus::Success)
        {
            return false;
        }
        const uint32 firstColumn = static_cast<uint32>(pageIndex % layout.PagesX) * layout.CellsPerPage;
        const uint32 firstRow = static_cast<uint32>(pageIndex / layout.PagesX) * layout.CellsPerPage;
        for (uint32 row = 0; row <= layout.CellsPerPage; ++row)
        {
            for (uint32 column = 0; column <= layout.CellsPerPage; ++column)
            {
                float64 height = 0;
                if (TryGetTerrainPageHeight(page, static_cast<int32>(column), static_cast<int32>(row), height) !=
                    TerrainCookStatus::Success)
                {
                    return false;
                }
                const usize index = GetSampleIndex(readback.Grid, firstColumn + column, firstRow + row);
                if (isWritten[index] != 0 && readback.HeightsMetres[index] != height)
                {
                    return false;
                }
                readback.HeightsMetres[index] = height;
                isWritten[index] = 1;
            }
        }
        ++readback.PagesRead;
    }
    for (usize i = 0; i < sampleCount; ++i)
    {
        if (isWritten[i] == 0)
        {
            return false;
        }
    }
    if (TryComputeNormals(readback.Grid, readback.HeightsMetres, readback.Normals) == false)
    {
        return false;
    }
    outReadback = foundation::Move(readback);
    return true;
}

float64 GetReadbackNormalToleranceRadians(float32 quantumMetres, float32 spacingMetres) noexcept
{
    return (math::Sqrt(2.0) * static_cast<float64>(quantumMetres) / static_cast<float64>(spacingMetres)) +
           MESH_TANGENT_ROUNDING_RADIANS;
}

float64 GetAngleRadians(Vector3d first, Vector3d second) noexcept
{
    return math::Atan2(math::Length(math::Cross(first, second)), math::Dot(first, second));
}

bool TryCookReadbackRevision(TerrainNoiseLayout layout,
                             TerrainCookedRevision& outRevision,
                             Fingerprint128& outDigest) noexcept
{
    TerrainReadbackField field;
    Fingerprint128 sourceDigest;
    if (TryBuildReadbackField(layout, field) == false ||
        TryComputeTerrainFieldDigest(field.Grid, field.ElevationsMetres.AsView(), sourceDigest) !=
            TerrainCookStatus::Success)
    {
        return false;
    }
    TerrainCookedRevision revision;
    if (TryCookTerrainRevision(MakeReadbackLayout(layout, sourceDigest),
                               field.Grid,
                               field.ElevationsMetres.AsView(),
                               revision) != TerrainCookStatus::Success)
    {
        return false;
    }
    const BorrowedView<const uint8> manifestBytes = revision.GetManifestBytes();
    outDigest = foundation::StableFingerprint128(foundation::MakeSpan(manifestBytes));
    outRevision = foundation::Move(revision);
    return true;
}

bool RunTerrainReadbackCorpus() noexcept
{
    for (const TerrainNoiseLayout layout : {TerrainNoiseLayout::CubicFade, TerrainNoiseLayout::QuinticFade})
    {
        TerrainReadbackField field;
        TerrainCookedRevision revision;
        Fingerprint128 digest;
        TerrainReadback readback;
        if (TryBuildReadbackField(layout, field) == false ||
            TryCookReadbackRevision(layout, revision, digest) == false ||
            digest != GetExpectedReadbackManifestDigest(layout) ||
            TryReadTerrainReadback(revision.GetManifestBytes(), revision.GetPagePackBytes(), readback) == false ||
            readback.RecipeVersion != static_cast<uint32>(layout) ||
            readback.PagesRead != static_cast<usize>(READBACK_PAGES_PER_AXIS) * READBACK_PAGES_PER_AXIS)
        {
            return false;
        }
        Array<float64> sourceHeights;
        if (sourceHeights.TryResize(field.ElevationsMetres.GetSize()) == false)
        {
            return false;
        }
        const float64 halfQuantum = 0.5 * static_cast<float64>(readback.QuantumMetres);
        for (usize i = 0; i < sourceHeights.GetSize(); ++i)
        {
            sourceHeights[i] = static_cast<float64>(field.ElevationsMetres[i]);
            if (math::Abs(readback.HeightsMetres[i] - sourceHeights[i]) > halfQuantum)
            {
                return false;
            }
        }
        Array<Vector3d> sourceNormals;
        if (TryComputeNormals(field.Grid, sourceHeights, sourceNormals) == false)
        {
            return false;
        }
        const float64 tolerance = GetReadbackNormalToleranceRadians(readback.QuantumMetres, field.Grid.SpacingXMetres);
        for (usize i = 0; i < sourceNormals.GetSize(); ++i)
        {
            if (GetAngleRadians(readback.Normals[i], sourceNormals[i]) > tolerance)
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace ludus::content::terrain::testing
