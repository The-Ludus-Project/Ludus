#include "internal/world_terrain_cook.h"

#include "internal/render_scene.h"
#include "internal/world_terrain.h"

#include <ludus/content/terrain/terrain_noise.h>
#include <ludus/content/terrain/terrain_tile.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace math = foundation::math;
namespace terrain = content::terrain;

/// Millimetres per metre, for the panel summary.
constexpr float64 MILLIMETRES_PER_METRE = 1000;
/// Decimal places of millimetre values in the panel summary.
constexpr int32 MILLIMETRE_DECIMALS = 2;

[[nodiscard]] uint32 GetGreatestCommonDivisor(uint32 first, uint32 second) noexcept
{
    while (second != 0)
    {
        const uint32 remainder = first % second;
        first = second;
        second = remainder;
    }
    return first;
}

[[nodiscard]] qsizetype GetSampleIndex(const terrain::TerrainFieldGrid& grid, uint32 column, uint32 row) noexcept
{
    return (static_cast<qsizetype>(row) * grid.Columns) + column;
}

/// Decode every page from its own byte range and assemble the cores; shared samples must agree.
[[nodiscard]] bool ReadInspectionPages(const terrain::TerrainManifest& manifest,
                                       BorrowedView<const uint8> pagePackBytes,
                                       const terrain::TerrainFieldGrid& grid,
                                       QList<float64>& outHeightsMetres,
                                       float32& outLargestErrorMetres)
{
    const terrain::TerrainRevisionLayout& layout = manifest.GetLayout();
    const qsizetype sampleCount = static_cast<qsizetype>(terrain::GetTerrainSampleCount(grid));
    QList<float64> heights(sampleCount, 0);
    QList<uint8> isWritten(sampleCount, 0);
    float32 largestError = 0;
    const BorrowedView<const terrain::TerrainPageRecord> records = manifest.GetPageRecords();
    for (usize pageIndex = 0; pageIndex < records.GetSize(); ++pageIndex)
    {
        const terrain::TerrainPageRecord& record = records[pageIndex];
        if (record.ByteOffset > pagePackBytes.GetSize() ||
            record.ByteSize > pagePackBytes.GetSize() - record.ByteOffset)
        {
            return false;
        }
        const BorrowedView<const uint8> pageBytes{pagePackBytes.GetDataOrNull() + record.ByteOffset, record.ByteSize};
        terrain::TerrainPage page;
        if (terrain::TryReadTerrainPage(manifest, pageIndex, pageBytes, page) != terrain::TerrainCookStatus::Success)
        {
            return false;
        }
        largestError = math::Max(largestError, record.MaximumErrorMetres);
        const uint32 firstColumn = static_cast<uint32>(pageIndex % layout.PagesX) * layout.CellsPerPage;
        const uint32 firstRow = static_cast<uint32>(pageIndex / layout.PagesX) * layout.CellsPerPage;
        for (uint32 row = 0; row <= layout.CellsPerPage; ++row)
        {
            for (uint32 column = 0; column <= layout.CellsPerPage; ++column)
            {
                float64 height = 0;
                if (terrain::TryGetTerrainPageHeight(page,
                                                     static_cast<int32>(column),
                                                     static_cast<int32>(row),
                                                     height) != terrain::TerrainCookStatus::Success)
                {
                    return false;
                }
                const qsizetype index = GetSampleIndex(grid, firstColumn + column, firstRow + row);
                if (isWritten[index] != 0 && heights[index] != height)
                {
                    return false;
                }
                heights[index] = height;
                isWritten[index] = 1;
            }
        }
    }
    if (isWritten.contains(0) == true)
    {
        return false;
    }
    outHeightsMetres = Move(heights);
    outLargestErrorMetres = largestError;
    return true;
}

/// Page borders through the decoded heights, at the mesh's horizontal positions.
[[nodiscard]] QList<QList<math::Vector3>> BuildBorderLines(const terrain::TerrainRevisionLayout& layout,
                                                           const terrain::TerrainFieldGrid& grid,
                                                           const PreviewMesh& mesh,
                                                           const QList<float64>& heightsMetres)
{
    const auto pointAt = [&](uint32 column, uint32 row) {
        const qsizetype index = GetSampleIndex(grid, column, row);
        const math::Vector3 positionMesh = mesh.Positions[index];
        return math::Vector3{positionMesh.X, static_cast<float32>(heightsMetres[index]), positionMesh.Z};
    };
    QList<QList<math::Vector3>> lines;
    lines.reserve(static_cast<qsizetype>(layout.PagesX) + layout.PagesZ + 2);
    for (uint32 edge = 0; edge <= layout.PagesX; ++edge)
    {
        const uint32 column = edge * layout.CellsPerPage;
        QList<math::Vector3> line;
        line.reserve(grid.Rows);
        for (uint32 row = 0; row < grid.Rows; ++row)
        {
            line.append(pointAt(column, row));
        }
        lines.append(Move(line));
    }
    for (uint32 edge = 0; edge <= layout.PagesZ; ++edge)
    {
        const uint32 row = edge * layout.CellsPerPage;
        QList<math::Vector3> line;
        line.reserve(grid.Columns);
        for (uint32 column = 0; column < grid.Columns; ++column)
        {
            line.append(pointAt(column, row));
        }
        lines.append(Move(line));
    }
    return lines;
}
} // namespace

uint32 ChooseTerrainInspectionPageCells(uint32 cellsX, uint32 cellsZ) noexcept
{
    const uint32 common = GetGreatestCommonDivisor(cellsX, cellsZ);
    const uint32 longest = math::Max(cellsX, cellsZ);
    for (uint32 cells = 1; cells < common; ++cells)
    {
        if (common % cells == 0 && longest / cells <= MAX_TERRAIN_INSPECTION_PAGES_PER_AXIS)
        {
            return cells;
        }
    }
    return common;
}

bool CookTerrainForInspection(const QJsonObject& recipe, TerrainCookedInspection& outInspection, QString& outError)
{
    PreviewMesh mesh;
    terrain::TerrainFieldGrid grid;
    if (BuildTerrainMesh(recipe, mesh, grid, outError) == false)
    {
        return false;
    }
    const uint32 cellsX = grid.Columns - 1;
    const uint32 cellsZ = grid.Rows - 1;
    if (grid.SpacingXMetres != grid.SpacingZMetres)
    {
        outError = QStringLiteral("Cooked pages need square cells, but these cells are %1 m along X and %2 m along Z. "
                                  "Set Depth to %3 m to match Width, or change Width.")
                       .arg(static_cast<float64>(grid.SpacingXMetres))
                       .arg(static_cast<float64>(grid.SpacingZMetres))
                       .arg(static_cast<float64>(grid.SpacingXMetres) * cellsZ);
        return false;
    }
    QList<float32> elevations;
    elevations.reserve(mesh.Positions.size());
    for (const math::Vector3& positionMesh : mesh.Positions)
    {
        elevations.append(positionMesh.Y);
    }
    const BorrowedView<const float32> elevationView{elevations.constData(), static_cast<usize>(elevations.size())};
    terrain::TerrainRevisionLayout layout;
    layout.Revision = TERRAIN_INSPECTION_REVISION;
    // BuildTerrainMesh accepted the recipe, so its noise layout is one this build implements.
    layout.RecipeVersion = static_cast<uint32>(
        recipe.value(GetTerrainNoiseLayoutKey()).toInt(static_cast<int32>(terrain::TerrainNoiseLayout::CubicFade)));
    layout.FirstPage = {0, 0};
    layout.CellsPerPage = ChooseTerrainInspectionPageCells(cellsX, cellsZ);
    layout.PagesX = cellsX / layout.CellsPerPage;
    layout.PagesZ = cellsZ / layout.CellsPerPage;
    layout.SpacingMetres = grid.SpacingXMetres;
    layout.Codec =
    {
        .Encoding = terrain::TerrainHeightEncoding::Signed32,
        .OriginMetres = 0,
        .QuantumMetres = TERRAIN_INSPECTION_QUANTUM_METRES,
    };
    TerrainCookedInspection inspection;
    terrain::TerrainManifest manifest;
    if (terrain::TryComputeTerrainFieldDigest(grid, elevationView, layout.SourceDigest) !=
            terrain::TerrainCookStatus::Success ||
        terrain::TryCookTerrainRevision(layout, grid, elevationView, inspection.Revision) !=
            terrain::TerrainCookStatus::Success ||
        terrain::TryReadTerrainManifest(inspection.Revision.GetManifestBytes(), manifest) !=
            terrain::TerrainCookStatus::Success ||
        ReadInspectionPages(manifest,
                            inspection.Revision.GetPagePackBytes(),
                            grid,
                            inspection.HeightsMetres,
                            inspection.LargestErrorMetres) == false)
    {
        outError = QStringLiteral("Could not cook or read back the terrain pages. Refresh the draft and try again.");
        return false;
    }
    inspection.Layout = manifest.GetLayout();
    inspection.Grid = grid;
    inspection.BorderLinesMesh = BuildBorderLines(inspection.Layout, grid, mesh, inspection.HeightsMetres);
    outInspection = Move(inspection);
    return true;
}

QString DescribeTerrainCookedInspection(const TerrainCookedInspection& inspection)
{
    const terrain::TerrainRevisionLayout& layout = inspection.Layout;
    return QStringLiteral("Cooked revision %1: %2 × %3 pages of %4 cells, each read back from its own bytes. "
                          "Height step %5 mm; largest error %6 mm. Noise layout %7.")
        .arg(layout.Revision)
        .arg(layout.PagesX)
        .arg(layout.PagesZ)
        .arg(layout.CellsPerPage)
        .arg(static_cast<float64>(layout.Codec.QuantumMetres) * MILLIMETRES_PER_METRE, 0, 'f', MILLIMETRE_DECIMALS)
        .arg(static_cast<float64>(inspection.LargestErrorMetres) * MILLIMETRES_PER_METRE, 0, 'f', MILLIMETRE_DECIMALS)
        .arg(layout.RecipeVersion);
}
} // namespace ludus::editor
