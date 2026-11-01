#include "internal/render_graph_document.h"
#include "internal/render_graph_luminaire_coverage.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QFile>
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
const QString IMPORTED_PLACEMENT = QStringLiteral("slice_sampling_luminaire/main_shaders/luminaire.slang");
/// Tolerance of placement literals read back from source text, in world units.
constexpr float32 PLACEMENT_TOLERANCE_WORLD = 1e-6F;

QByteArray ReadShippedPlacement()
{
    QFile file(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT) +
               QStringLiteral("/modules/graphics/renderer/render_graphs/slice_sampling_luminaire/main_shaders/"
                              "luminaire.slang"));
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

RenderGraphLuminairePanel MakeDefaultPanel()
{
    RenderGraphLuminairePanel panel;
    panel.Source = IMPORTED_PLACEMENT;
    panel.IsPlacementKnown = true;
    panel.CenterWorld = {0, 0.97F, 0};
    panel.HalfExtentWorld = 0.35F;
    panel.NormalWorld = {0, -1, 0};
    return panel;
}

RenderGraphLuminaireWorld
MakeWorld(foundation::math::Vector3 minimum, foundation::math::Vector3 maximum, foundation::int32 emissiveObjects)
{
    RenderGraphLuminaireWorld world;
    world.HasBounds = true;
    world.BoundsWorld = { .Min = minimum, .Max = maximum };
    world.EmissiveObjectCount = emissiveObjects;
    return world;
}

bool IsNear(float32 a, float32 b)
{
    return a - b <= PLACEMENT_TOLERANCE_WORLD && b - a <= PLACEMENT_TOLERANCE_WORLD;
}
} // namespace

TEST_CASE("The shipped slice-sampling luminaire placement stays readable by the editor's panel check",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    RenderGraphLuminairePanel panel;
    REQUIRE(ReadRenderGraphLuminairePlacement(ReadShippedPlacement(), panel) == true);
    CHECK(IsNear(panel.CenterWorld.X, 0) == true);
    CHECK(IsNear(panel.CenterWorld.Y, 0.97F) == true);
    CHECK(IsNear(panel.CenterWorld.Z, 0) == true);
    CHECK(IsNear(panel.HalfExtentWorld, 0.35F) == true);
    CHECK(IsNear(panel.NormalWorld.Y, -1) == true);
}

TEST_CASE("Luminaire placement accepts Slang literals and rejects expressions and comments",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    RenderGraphLuminairePanel panel;
    const QByteArray literals = "static const float3 LUMINAIRE_CENTER_WORLD = float3(-2.5f, .5, 3.);\n"
                                "static const float LUMINAIRE_HALF_EXTENT_WORLD = 1.25F;\n"
                                "static const float3 LUMINAIRE_NORMAL_WORLD = float3(0, 0, -2);\n";
    REQUIRE(ReadRenderGraphLuminairePlacement(literals, panel) == true);
    CHECK(IsNear(panel.CenterWorld.X, -2.5F) == true);
    CHECK(IsNear(panel.CenterWorld.Y, 0.5F) == true);
    CHECK(IsNear(panel.CenterWorld.Z, 3) == true);
    CHECK(IsNear(panel.HalfExtentWorld, 1.25F) == true);
    // The normal is normalised.
    CHECK(IsNear(panel.NormalWorld.Z, -1) == true);

    const RenderGraphLuminairePanel before = panel;
    const QByteArray expression = "static const float3 LUMINAIRE_CENTER_WORLD = float3(0, CEILING - 0.03, 0);\n"
                                  "static const float LUMINAIRE_HALF_EXTENT_WORLD = 0.35;\n"
                                  "static const float3 LUMINAIRE_NORMAL_WORLD = float3(0, -1, 0);\n";
    CHECK(ReadRenderGraphLuminairePlacement(expression, panel) == false);
    const QByteArray commented = "// static const float3 LUMINAIRE_CENTER_WORLD = float3(0, 1, 0);\n"
                                 "static const float LUMINAIRE_HALF_EXTENT_WORLD = 0.35;\n"
                                 "static const float3 LUMINAIRE_NORMAL_WORLD = float3(0, -1, 0);\n";
    CHECK(ReadRenderGraphLuminairePlacement(commented, panel) == false);
    const QByteArray degenerate = "static const float3 LUMINAIRE_CENTER_WORLD = float3(0, 1, 0);\n"
                                  "static const float LUMINAIRE_HALF_EXTENT_WORLD = 0;\n"
                                  "static const float3 LUMINAIRE_NORMAL_WORLD = float3(0, -1, 0);\n";
    CHECK(ReadRenderGraphLuminairePlacement(degenerate, panel) == false);
    // Failure preserves the previous placement.
    CHECK(IsNear(panel.CenterWorld.X, before.CenterWorld.X) == true);
    CHECK(IsNear(panel.HalfExtentWorld, before.HalfExtentWorld) == true);
}

TEST_CASE("The default panel lights its test room but not a corridor twenty metres long",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    const RenderGraphLuminairePanel panel = MakeDefaultPanel();
    // The 2 x 2 x 2 room of the renderer's tests, with its ceiling light.
    CHECK(DescribeRenderGraphLuminaireCoverage(panel, MakeWorld({-1, -1, -1}, {1, 1, 1}, 1)).isEmpty() == true);

    // Bounds of the staircase2 reference scene: the panel sits inside them but spans under 3% of the diagonal.
    const QString corridor =
        DescribeRenderGraphLuminaireCoverage(panel, MakeWorld({-14.09F, -2.86F, -2.99F}, {6.91F, 11.39F, 4.36F}, 13));
    INFO(corridor.toStdString());
    CHECK(corridor.contains(QStringLiteral("lights at most a small patch")) == true);
    CHECK(corridor.contains(QStringLiteral("(0.00, 0.97, 0.00)")) == true);
    CHECK(corridor.contains(QStringLiteral("13 emissive objects")) == true);
    // Counts of zero are not mentioned.
    CHECK(corridor.contains(QStringLiteral("0 lights")) == false);
    // The message names the file and constants to edit.
    CHECK(corridor.contains(IMPORTED_PLACEMENT) == true);
    CHECK(corridor.contains(QStringLiteral("LUMINAIRE_CENTER_WORLD")) == true);
}

TEST_CASE("A panel outside the World or facing away from it cannot light it",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    RenderGraphLuminairePanel panel = MakeDefaultPanel();
    const RenderGraphLuminaireWorld room = MakeWorld({-1, -1, -1}, {1, 1, 1}, 0);
    // On the ceiling, facing up: nothing lies in front of it.
    panel.CenterWorld = {0, 1, 0};
    panel.NormalWorld = {0, 1, 0};
    const QString away = DescribeRenderGraphLuminaireCoverage(panel, room);
    CHECK(away.contains(QStringLiteral("away from every World object")) == true);
    // No World emitters or lights: the message does not invent them.
    CHECK(away.contains(QStringLiteral("emissive")) == false);

    panel.NormalWorld = {0, -1, 0};
    panel.CenterWorld = {5, 0.5F, 0};
    const QString outside = DescribeRenderGraphLuminaireCoverage(panel, room);
    CHECK(outside.contains(QStringLiteral("outside the World's bounds")) == true);
}

TEST_CASE("An unreadable placement is reported only when the World brings lighting the template ignores",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    RenderGraphLuminairePanel panel;
    panel.Source = IMPORTED_PLACEMENT;
    CHECK(DescribeRenderGraphLuminaireCoverage(panel, MakeWorld({-1, -1, -1}, {1, 1, 1}, 0)).isEmpty() == true);
    RenderGraphLuminaireWorld lit = MakeWorld({-1, -1, -1}, {1, 1, 1}, 0);
    lit.LightCount = 1;
    const QString unknown = DescribeRenderGraphLuminaireCoverage(panel, lit);
    CHECK(unknown.contains(QStringLiteral("cannot check it")) == true);
    CHECK(unknown.contains(QStringLiteral("World's 1 light.")) == true);
    CHECK(unknown.contains(QStringLiteral("emissive")) == false);
}

TEST_CASE("The panel check follows the imported luminaire only while it is on the output route",
          "[editor][render-graph][slice-sampling-luminaire-coverage]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    workspace.SetLivePreview(false);
    RenderGraphDocument& document = workspace.GetDocument();
    QString error;
    REQUIRE(document.ApplyConfigurationTemplate(ROOT_GRAPH,
                                                QStringLiteral("Luminaire"),
                                                QStringLiteral("slice-sampling-luminaire"),
                                                error) == RenderGraphDocumentStatus::Ok);
    RenderGraphLuminairePanel panel;
    // Inserted but unconnected, the luminaire does not reach the output.
    CHECK(FindRenderGraphLuminairePanel(document, DEFAULT_CONFIGURATION, panel) == false);

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
    REQUIRE(FindRenderGraphLuminairePanel(document, DEFAULT_CONFIGURATION, panel) == true);
    CHECK(panel.Source == IMPORTED_PLACEMENT);
    CHECK(panel.IsPlacementKnown == true);
    CHECK(IsNear(panel.CenterWorld.Y, 0.97F) == true);
}
