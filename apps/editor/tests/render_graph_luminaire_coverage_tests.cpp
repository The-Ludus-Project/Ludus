#include "render_graph_template_fixture.h"

#include "internal/render_graph_document.h"
#include "internal/render_graph_luminaire_coverage.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QList>
#include <QPair>
#include <QTemporaryDir>

using namespace ludus;
using namespace ludus::editor;
using foundation::float32;

namespace
{
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString DEFAULT_CONFIGURATION = QStringLiteral("Default");
/// Tolerance of fitted placements, in world units.
constexpr float32 PLACEMENT_TOLERANCE_WORLD = 1e-5F;
constexpr foundation::uint64 CEILING_LUMINAIRE_SOURCE = 7;

/// The renderer tests' Cornell-box luminaire: a 0.7 x 0.7 panel facing down at y = 0.97.
graphics::renderer::PanelLuminaire MakeCornellLuminaire()
{
    graphics::renderer::PanelLuminaire luminaire;
    luminaire.SourceId = CEILING_LUMINAIRE_SOURCE;
    luminaire.CenterWorld = {0, 0.97F, 0};
    luminaire.TangentWorld = {1, 0, 0};
    luminaire.BitangentWorld = {0, 0, 1};
    return luminaire;
}

/// The downward panel FitRenderGraphLuminaire describes, as the World's Transform would place it.
graphics::renderer::PanelLuminaire MakeFittedLuminaire(const WorldLuminairePlacement& placement)
{
    REQUIRE(placement.RotationDegrees.X == WORLD_LUMINAIRE_DOWNWARD_DEGREES);
    REQUIRE(placement.RotationDegrees.Y == 0);
    REQUIRE(placement.RotationDegrees.Z == 0);
    graphics::renderer::PanelLuminaire luminaire = MakeCornellLuminaire();
    luminaire.CenterWorld = placement.PositionWorld;
    luminaire.HalfExtentWorld = placement.HalfExtentWorld;
    return luminaire;
}

RenderGraphLuminaireWorld
MakeWorld(foundation::math::Vector3 minimum, foundation::math::Vector3 maximum, foundation::int32 emissiveObjects)
{
    RenderGraphLuminaireWorld world;
    world.HasBounds = true;
    world.BoundsWorld = { .Min = minimum, .Max = maximum };
    world.EmissiveObjectCount = emissiveObjects;
    world.CanRepair = true;
    return world;
}

void AddLuminaire(RenderGraphLuminaireWorld& inoutWorld, const graphics::renderer::PanelLuminaire& luminaire)
{
    REQUIRE(inoutWorld.Luminaires.Count < graphics::renderer::MAX_PANEL_LUMINAIRES);
    inoutWorld.Luminaires.Luminaires[inoutWorld.Luminaires.Count++] = luminaire;
}

bool IsNear(float32 a, float32 b)
{
    return a - b <= PLACEMENT_TOLERANCE_WORLD && b - a <= PLACEMENT_TOLERANCE_WORLD;
}
} // namespace

TEST_CASE("A World without a luminaire offers to add one fitted to its bounds",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    // The 2 x 2 x 2 room of the renderer's tests, with its ceiling light.
    RenderGraphLuminaireWorld room = MakeWorld({-1, -1, -1}, {1, 1, 1}, 1);
    const RenderGraphLuminaireCoverage missing = DescribeRenderGraphLuminaireCoverage(room);
    INFO(missing.Warning.toStdString());
    CHECK(missing.Warning.contains(QStringLiteral("no enabled luminaire")) == true);
    CHECK(missing.Warning.contains(QStringLiteral("1 emissive object")) == true);
    REQUIRE(missing.Repair == RenderGraphLuminaireRepair::AddLuminaire);
    CHECK(missing.LuminaireSourceId == 0);
    // Centred below the top of the bounds, with a side of FITTED_PANEL_WORLD_FRACTION of the room's width.
    CHECK(IsNear(missing.Placement.PositionWorld.X, 0) == true);
    CHECK(IsNear(missing.Placement.PositionWorld.Z, 0) == true);
    CHECK(missing.Placement.PositionWorld.Y < 1);
    CHECK(missing.Placement.PositionWorld.Y > 0.9F);
    CHECK(IsNear(missing.Placement.HalfExtentWorld, FITTED_PANEL_WORLD_FRACTION) == true);

    // The repaired World is lit.
    AddLuminaire(room, MakeFittedLuminaire(missing.Placement));
    CHECK(DescribeRenderGraphLuminaireCoverage(room).Warning.isEmpty() == true);

    // A project schema without the luminaire preset gets the instruction without a repair.
    RenderGraphLuminaireWorld project = MakeWorld({-1, -1, -1}, {1, 1, 1}, 0);
    project.CanRepair = false;
    const RenderGraphLuminaireCoverage manual = DescribeRenderGraphLuminaireCoverage(project);
    CHECK(manual.Repair == RenderGraphLuminaireRepair::None);
    CHECK(manual.Warning.contains(QStringLiteral("Attach a luminaire component")) == true);
}

TEST_CASE("The Cornell luminaire lights its test room but not a corridor twenty metres long",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    RenderGraphLuminaireWorld room = MakeWorld({-1, -1, -1}, {1, 1, 1}, 1);
    AddLuminaire(room, MakeCornellLuminaire());
    CHECK(DescribeRenderGraphLuminaireCoverage(room).Warning.isEmpty() == true);

    // Bounds of the staircase2 reference scene: the panel sits inside them but spans under 3% of the diagonal.
    RenderGraphLuminaireWorld corridor = MakeWorld({-14.09F, -2.86F, -2.99F}, {6.91F, 11.39F, 4.36F}, 13);
    AddLuminaire(corridor, MakeCornellLuminaire());
    const RenderGraphLuminaireCoverage small = DescribeRenderGraphLuminaireCoverage(corridor);
    INFO(small.Warning.toStdString());
    CHECK(small.Warning.contains(QStringLiteral("lights at most a small patch")) == true);
    CHECK(small.Warning.contains(QStringLiteral("(0.00, 0.97, 0.00)")) == true);
    CHECK(small.Warning.contains(QStringLiteral("13 emissive objects")) == true);
    // Counts of zero are not mentioned.
    CHECK(small.Warning.contains(QStringLiteral("0 lights")) == false);
    // The repair moves and resizes that luminaire, and its fitted placement lights the corridor.
    REQUIRE(small.Repair == RenderGraphLuminaireRepair::MoveLuminaire);
    CHECK(small.LuminaireSourceId == CEILING_LUMINAIRE_SOURCE);
    RenderGraphLuminaireWorld fitted = corridor;
    fitted.Luminaires.Luminaires[0] = MakeFittedLuminaire(small.Placement);
    CHECK(DescribeRenderGraphLuminaireCoverage(fitted).Warning.isEmpty() == true);
}

TEST_CASE("A luminaire outside the World or facing away from it cannot light it",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    RenderGraphLuminaireWorld room = MakeWorld({-1, -1, -1}, {1, 1, 1}, 0);
    graphics::renderer::PanelLuminaire luminaire = MakeCornellLuminaire();
    // On the ceiling, facing up: nothing lies in front of it.
    luminaire.CenterWorld = {0, 1, 0};
    luminaire.BitangentWorld = {0, 0, -1};
    AddLuminaire(room, luminaire);
    const QString away = DescribeRenderGraphLuminaireCoverage(room).Warning;
    CHECK(away.contains(QStringLiteral("away from every World object")) == true);
    // No World emitters or lights: the message does not invent them.
    CHECK(away.contains(QStringLiteral("emissive")) == false);

    room.Luminaires.Luminaires[0] = MakeCornellLuminaire();
    room.Luminaires.Luminaires[0].CenterWorld = {5, 0.5F, 0};
    CHECK(DescribeRenderGraphLuminaireCoverage(room).Warning.contains(QStringLiteral("outside the World's bounds")) ==
          true);
    // One luminaire that can light the World is enough.
    AddLuminaire(room, MakeCornellLuminaire());
    CHECK(DescribeRenderGraphLuminaireCoverage(room).Warning.isEmpty() == true);
}

TEST_CASE("A fitted luminaire hangs under the ceiling above its anchor when the World has one",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    RenderGraphLuminaireWorld world = MakeWorld({-10, 0, -4}, {10, 12, 4}, 0);
    world.HasAnchor = true;
    world.AnchorWorld = {6, 1.5F, 2};
    world.HasCeiling = true;
    world.CeilingHeightWorld = 2;
    const WorldLuminairePlacement placement = FitRenderGraphLuminaire(world);
    CHECK(IsNear(placement.PositionWorld.X, 6) == true);
    CHECK(IsNear(placement.PositionWorld.Z, 2) == true);
    // One percent of the ceiling distance below it.
    CHECK(IsNear(placement.PositionWorld.Y, 3.48F) == true);
    // Without a ceiling above the camera the panel hangs below the top of the bounds.
    world.HasCeiling = false;
    CHECK(FitRenderGraphLuminaire(world).PositionWorld.Y > 11.8F);
    // An anchor outside the bounds falls back to their center.
    world.AnchorWorld = {50, 1.5F, 2};
    CHECK(IsNear(FitRenderGraphLuminaire(world).PositionWorld.X, 0) == true);
}

TEST_CASE("The luminaire check follows the imported template only while it is on the output route",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    tests::SetTemplateAuthoringProfiles(document);
    QString error;
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                                QStringLiteral("Luminaire"),
                                                QStringLiteral("slice-sampling-luminaire"),
                                                error) == RenderGraphDocumentStatus::Ok);
    // Inserted but unconnected, the luminaire does not reach the output.
    CHECK(IsRenderGraphLuminaireRoute(document, DEFAULT_CONFIGURATION) == false);

    const QList<QPair<QString, QString>> inputs{
        {QStringLiteral("scene.scene"), QStringLiteral("Luminaire.scene")},
        {QStringLiteral("camera.view"), QStringLiteral("Luminaire.view")},
        {QStringLiteral("color.texture"), QStringLiteral("Luminaire.color")},
    };
    for (const QPair<QString, QString>& input : inputs)
    {
        REQUIRE(document.Connect(ROOT_GRAPH, DEFAULT_CONFIGURATION, input.first, input.second) ==
                RenderGraphDocumentStatus::Ok);
    }
    REQUIRE(document.ReplaceConnection(ROOT_GRAPH,
                                       DEFAULT_CONFIGURATION,
                                       QStringLiteral("Luminaire.color"),
                                       QStringLiteral("tonemap.color")) == RenderGraphDocumentStatus::Ok);
    CHECK(IsRenderGraphLuminaireRoute(document, DEFAULT_CONFIGURATION) == true);
}
