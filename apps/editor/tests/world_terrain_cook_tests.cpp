// Show cooked pages: the Terrain panel cooks the ready terrain into versioned height pages, reads each page back and
// draws the page borders. The corpus recipe must cook to the manifest the installed SDK consumer cooks and reads
// (tests/sdk_consumer/terrain_readback_corpus.h), and the World preview's mesh must agree with that readback.

#include "viewport_fixture.h"

#include "entropy_fixture.h"
#include "internal/builtin_world.h"
#include "internal/preview_viewport.h"
#include "internal/render_panel.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/viewport_object_interaction.h"
#include "internal/viewport_object_overlay.h"
#include "internal/world_document.h"
#include "internal/world_terrain.h"
#include "internal/world_terrain_cook.h"
#include "internal/world_workspace.h"
#include "terrain_readback_corpus.h"

#include <ludus/content/terrain/terrain_cook.h>
#include <ludus/content/terrain/terrain_noise.h>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/hash/hash.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRectF>
#include <QTemporaryDir>
#include <QTest>
#include <catch2/catch_test_macros.hpp>

namespace ludus::editor
{
namespace
{
namespace terrain = content::terrain;
namespace corpus = content::terrain::testing;
using foundation::float64;
using foundation::uint32;
using foundation::usize;

constexpr foundation::int32 TERRAIN_WAIT_MILLISECONDS = 15000;

[[nodiscard]] WorldDocument MakeWorld()
{
    QByteArray schema;
    QString error;
    INFO(error.toStdString());
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument world;
    REQUIRE(world.LoadSchema(schema, error) == true);
    REQUIRE(world.New(error) == true);
    return world;
}

/// The built-in terrain's defaults with the corpus recipe's values.
[[nodiscard]] QJsonObject MakeCorpusRecipe(const WorldDocument& world, terrain::TerrainNoiseLayout layout)
{
    QJsonObject recipe;
    for (const QJsonValue value :
         world.Component(GetTerrainComponentId()).value(QStringLiteral("properties")).toArray())
    {
        const QJsonObject property = value.toObject();
        recipe.insert(property.value(QStringLiteral("id")).toString(), property.value(QStringLiteral("default")));
    }
    recipe.insert(GetTerrainNoiseLayoutKey(), static_cast<foundation::int32>(layout));
    recipe.insert(QStringLiteral("shape"), QStringLiteral("Rolling Hills"));
    recipe.insert(QStringLiteral("seed"), static_cast<qint64>(corpus::READBACK_SEED));
    recipe.insert(QStringLiteral("width"), corpus::READBACK_EXTENT_METRES);
    recipe.insert(QStringLiteral("depth"), corpus::READBACK_EXTENT_METRES);
    recipe.insert(QStringLiteral("minimumElevation"), corpus::READBACK_MINIMUM_ELEVATION_METRES);
    recipe.insert(QStringLiteral("maximumElevation"), corpus::READBACK_MAXIMUM_ELEVATION_METRES);
    recipe.insert(QStringLiteral("featureSize"), corpus::READBACK_FEATURE_SIZE_METRES);
    recipe.insert(QStringLiteral("roughness"), corpus::READBACK_ROUGHNESS);
    recipe.insert(QStringLiteral("samples"), static_cast<foundation::int32>(corpus::READBACK_SAMPLES_PER_SIDE));
    recipe.insert(QStringLiteral("surface"), QStringLiteral("Plain"));
    recipe.insert(QStringLiteral("rivers"), false);
    return recipe;
}

[[nodiscard]] foundation::Fingerprint128 GetManifestDigest(const terrain::TerrainCookedRevision& revision)
{
    const foundation::BorrowedView<const foundation::uint8> bytes = revision.GetManifestBytes();
    return foundation::StableFingerprint128(foundation::MakeSpan(bytes));
}
} // namespace

TEST_CASE("Terrain inspection pages leave at most four pages per axis", "[editor][world][terrain]")
{
    CHECK(ChooseTerrainInspectionPageCells(63, 63) == corpus::READBACK_CELLS_PER_PAGE);
    CHECK(ChooseTerrainInspectionPageCells(32, 32) == 8);
    CHECK(ChooseTerrainInspectionPageCells(64, 32) == 16);
    CHECK(ChooseTerrainInspectionPageCells(2, 2) == 1);
    // A prime cell count has no smaller divisor, so the whole domain is one page.
    CHECK(ChooseTerrainInspectionPageCells(31, 31) == 31);
    // Coprime counts share only one-cell pages.
    CHECK(ChooseTerrainInspectionPageCells(63, 62) == 1);
}

TEST_CASE("World terrain cooks to the SDK readback corpus and the preview agrees with its pages",
          "[editor][world][terrain]")
{
    for (const terrain::TerrainNoiseLayout layout :
         {terrain::TerrainNoiseLayout::CubicFade, terrain::TerrainNoiseLayout::QuinticFade})
    {
        INFO("noise layout " << static_cast<foundation::int32>(layout));
        WorldDocument world = MakeWorld();
        const QJsonObject recipe = MakeCorpusRecipe(world, layout);
        QString error;
        TerrainCookedInspection inspection;
        {
            INFO(error.toStdString());
            REQUIRE(CookTerrainForInspection(recipe, inspection, error) == true);
        }
        // The Editor writes the bytes the installed SDK consumer cooks and reads.
        CHECK(GetManifestDigest(inspection.Revision) == corpus::GetExpectedReadbackManifestDigest(layout));
        CHECK(inspection.Layout.RecipeVersion == static_cast<uint32>(layout));
        CHECK(inspection.Layout.CellsPerPage == corpus::READBACK_CELLS_PER_PAGE);
        CHECK(inspection.Layout.PagesX == corpus::READBACK_PAGES_PER_AXIS);
        CHECK(inspection.Layout.PagesZ == corpus::READBACK_PAGES_PER_AXIS);
        CHECK(inspection.LargestErrorMetres <= 0.5F * TERRAIN_INSPECTION_QUANTUM_METRES);
        CHECK(DescribeTerrainCookedInspection(inspection).contains(QStringLiteral("3 × 3 pages of 21 cells")) == true);

        corpus::TerrainReadback readback;
        REQUIRE(corpus::TryReadTerrainReadback(inspection.Revision.GetManifestBytes(),
                                               inspection.Revision.GetPagePackBytes(),
                                               readback) == true);
        REQUIRE(static_cast<usize>(inspection.HeightsMetres.size()) == readback.HeightsMetres.GetSize());
        for (usize i = 0; i < readback.HeightsMetres.GetSize(); ++i)
        {
            REQUIRE(inspection.HeightsMetres[static_cast<qsizetype>(i)] == readback.HeightsMetres[i]);
        }

        // The World preview extracts the same terrain; its vertices are the source field and its normals the
        // derivative-derived normals the cooked heights must reproduce within the quantization bound.
        QString key;
        REQUIRE(EditWorldTerrain(world, tests::MakeFixedEntropySource(), {}, recipe, key, error) == true);
        QTemporaryDir root;
        REQUIRE(root.isValid() == true);
        RenderScene scene;
        REQUIRE(RenderScene::Load(root.path(), world, scene, error, false) == true);
        REQUIRE(scene.Objects.size() == 1);
        const PreviewMesh& mesh = scene.Meshes[scene.Objects[0].Mesh];
        REQUIRE(static_cast<usize>(mesh.Positions.size()) == readback.HeightsMetres.GetSize());
        const float64 halfQuantum = 0.5 * static_cast<float64>(readback.QuantumMetres);
        const float64 tolerance =
            corpus::GetReadbackNormalToleranceRadians(readback.QuantumMetres, readback.Grid.SpacingXMetres);
        float64 largestAngle = 0;
        for (usize i = 0; i < readback.HeightsMetres.GetSize(); ++i)
        {
            const qsizetype index = static_cast<qsizetype>(i);
            REQUIRE(foundation::math::Abs(readback.HeightsMetres[i] - static_cast<float64>(mesh.Positions[index].Y)) <=
                    halfQuantum);
            const foundation::math::Vector3 normal = mesh.Normals[index];
            largestAngle = foundation::math::Max(
                largestAngle,
                corpus::GetAngleRadians(
                    {static_cast<float64>(normal.X), static_cast<float64>(normal.Y), static_cast<float64>(normal.Z)},
                    readback.Normals[i]));
        }
        INFO("largest normal angle " << largestAngle << " rad, tolerance " << tolerance);
        CHECK(largestAngle <= tolerance);

        // Page borders run through the decoded heights at the mesh's horizontal positions.
        REQUIRE(inspection.BorderLinesMesh.size() == 2 * (corpus::READBACK_PAGES_PER_AXIS + 1));
        const QList<foundation::math::Vector3>& interiorColumn = inspection.BorderLinesMesh[1];
        REQUIRE(interiorColumn.size() == corpus::READBACK_SAMPLES_PER_SIDE);
        for (uint32 row = 0; row < corpus::READBACK_SAMPLES_PER_SIDE; ++row)
        {
            const qsizetype index =
                (static_cast<qsizetype>(row) * corpus::READBACK_SAMPLES_PER_SIDE) + corpus::READBACK_CELLS_PER_PAGE;
            CHECK(interiorColumn[row].X == mesh.Positions[index].X);
            CHECK(interiorColumn[row].Z == mesh.Positions[index].Z);
            CHECK(interiorColumn[row].Y == static_cast<foundation::float32>(inspection.HeightsMetres[index]));
        }
    }
}

TEST_CASE("Terrain inspection names the square-cell repair for rectangular cells", "[editor][world][terrain]")
{
    WorldDocument world = MakeWorld();
    QJsonObject recipe = MakeCorpusRecipe(world, terrain::TerrainNoiseLayout::QuinticFade);
    recipe.insert(QStringLiteral("depth"), corpus::READBACK_EXTENT_METRES * 0.5);
    TerrainCookedInspection inspection;
    inspection.LargestErrorMetres = 1;
    QString error;
    CHECK(CookTerrainForInspection(recipe, inspection, error) == false);
    INFO(error.toStdString());
    CHECK(error.contains(QStringLiteral("square cells")) == true);
    CHECK(error.contains(QStringLiteral("Set Depth to 31.5 m")) == true);
    CHECK(inspection.LargestErrorMetres == 1);
    CHECK(inspection.Revision.IsEmpty() == true);
}

TEST_CASE("Show cooked pages draws the ready terrain's page borders with Properties hidden", "[editor][world][terrain]")
{
#if defined(Q_OS_WIN)
    const tests::ScopedViewportMode viewportMode(QByteArrayLiteral("native"));
#endif
    QTemporaryDir directory;
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString path = QDir(root).filePath(QStringLiteral("terrain.world.json"));
    WorldWorkspace workspace;
    workspace.SetEntropySource(tests::MakeFixedEntropySource());
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Create(path) == true);
    QWidget properties;
    workspace.TakePropertiesPanel(&properties)->hide();
    workspace.show();

    RenderPanel* renderOrNull = workspace.findChild<RenderPanel*>();
    QWidget* panelOrNull = workspace.findChild<QWidget*>(QStringLiteral("worldTerrainPanel"));
    REQUIRE(renderOrNull != nullptr);
    REQUIRE(panelOrNull != nullptr);
    QPushButton* commitOrNull = panelOrNull->findChild<QPushButton*>(QStringLiteral("worldTerrainCommit"));
    QCheckBox* cookedOrNull = panelOrNull->findChild<QCheckBox*>(QStringLiteral("worldTerrainCookedPages"));
    const QLabel* infoOrNull = panelOrNull->findChild<QLabel*>(QStringLiteral("worldTerrainCookedInfo"));
    QLineEdit* depthOrNull = panelOrNull->findChild<QLineEdit*>(QStringLiteral("worldTerrainField:depth"));
    REQUIRE(commitOrNull != nullptr);
    REQUIRE(cookedOrNull != nullptr);
    REQUIRE(infoOrNull != nullptr);
    REQUIRE(depthOrNull != nullptr);

    PreviewViewport& viewport = renderOrNull->GetViewport();
    viewport.GetAddTerrainAction().trigger();
    REQUIRE(QTest::qWaitFor([commitOrNull]() { return commitOrNull->isEnabled(); }, TERRAIN_WAIT_MILLISECONDS));
    ViewportObjectInteraction& interaction = viewport.GetObjectInteraction();
    interaction.SetActive(true);
    interaction.SetTool(ViewportObjectInteraction::Tool::Select);
    renderOrNull->Preview().SetAxisView(EditorAxisView::Top);
    const QRectF framePixel(0, 0, 800, 600);
    ViewportObjectOverlay withoutPages;
    interaction.BuildOverlay(framePixel, withoutPages);
    CHECK(interaction.GetTerrainCookedPageLineCount() == 0);
    CHECK(infoOrNull->isHidden() == true);

    // The default terrain has 32 cells per side: four pages of eight cells along each axis.
    cookedOrNull->setChecked(true);
    CHECK(infoOrNull->isHidden() == false);
    INFO(infoOrNull->text().toStdString());
    CHECK(infoOrNull->text().contains(QStringLiteral("4 × 4 pages of 8 cells")) == true);
    CHECK(interaction.GetTerrainCookedPageLineCount() == 10);
    ViewportObjectOverlay withPages;
    interaction.BuildOverlay(framePixel, withPages);
    CHECK(withPages.OutlineLinesPixel.GetSize() > withoutPages.OutlineLinesPixel.GetSize());

    // Rectangular cells cannot be cooked; the summary names the repair and the borders disappear.
    depthOrNull->setText(QStringLiteral("16"));
    REQUIRE(QTest::qWaitFor([infoOrNull]() { return infoOrNull->text().contains(QStringLiteral("square cells")); },
                            TERRAIN_WAIT_MILLISECONDS));
    CHECK(infoOrNull->text().contains(QStringLiteral("Set Depth to 32 m")) == true);
    CHECK(interaction.GetTerrainCookedPageLineCount() == 0);

    // The named repair restores the inspection.
    depthOrNull->setText(QStringLiteral("32"));
    REQUIRE(QTest::qWaitFor([infoOrNull]() { return infoOrNull->text().contains(QStringLiteral("4 × 4 pages")); },
                            TERRAIN_WAIT_MILLISECONDS));
    CHECK(interaction.GetTerrainCookedPageLineCount() == 10);

    cookedOrNull->setChecked(false);
    CHECK(infoOrNull->isHidden() == true);
    CHECK(interaction.GetTerrainCookedPageLineCount() == 0);
}
} // namespace ludus::editor
