#include "internal/render_graph_availability.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_resources.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/core.h>

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
const QString ROOT_GRAPH = QStringLiteral("main.lurender.json");
const QString LIBRARY_GRAPH = QStringLiteral("library.lurender.json");
const QString DEFAULT_CONFIGURATION = QStringLiteral("Default");
/// Opacity the canvas gives dimmed cards; mirrors the canvas's inactive-branch constant.
constexpr float64 DIMMED_OPACITY = 0.55;

QJsonObject MakeShader(const QString& stage)
{
    return {{QStringLiteral("stage"), stage}, {QStringLiteral("program"), stage}};
}

QJsonObject MakeRayNode(const QString& identity, const QJsonObject& invocation)
{
    QJsonObject node = MakePipelineNode(QStringLiteral("RayTracingPipeline"), identity);
    node.insert(QStringLiteral("invocation"), invocation);
    return node;
}

QJsonObject MakeSceneRayComputeNode(const QString& identity, bool hasKernel)
{
    QJsonObject node = MakePipelineNode(QStringLiteral("ComputePipeline"), identity);
    node.insert(QStringLiteral("inputs"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("scene")},
                                       {QStringLiteral("type"), QStringLiteral("Scene")}}});
    if (hasKernel == true)
    {
        node.insert(QStringLiteral("shaders"), QJsonArray{MakeShader(QStringLiteral("compute"))});
    }
    return node;
}

RenderGraphDeviceProfile MakeNativeProfile(bool hasRayHardware, bool hasDenoiser = true)
{
    RenderGraphDeviceProfile profile;
    profile.Kind = RenderGraphDeviceKind::Native;
    profile.DeviceName = QStringLiteral("Metal");
    profile.HasCompute = true;
    profile.HasNativeRayQueries = hasRayHardware;
    profile.HasRayPipelines = hasRayHardware;
    profile.HasDenoiser = hasDenoiser;
    return profile;
}

QByteArray MakeGraph(const QString& identity, const QJsonArray& nodes)
{
    return EncodeCanonicalJson(QJsonObject{
        {QStringLiteral("version"), 2},
        {QStringLiteral("id"), identity},
        {QStringLiteral("default"), DEFAULT_CONFIGURATION},
        {QStringLiteral("configurations"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), DEFAULT_CONFIGURATION},
                                {QStringLiteral("inputs"), QJsonArray{}},
                                {QStringLiteral("outputs"), QJsonArray{}},
                                {QStringLiteral("nodes"), nodes},
                                {QStringLiteral("edges"), QJsonArray{}}}}},
    });
}

void WriteFile(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()) == true);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
    REQUIRE(file.write(bytes) == bytes.size());
}

/// A root graph with one native-query ray pass, one compute-BVH pass, a disabled Denoise node and a
/// Subgraph whose library holds another native pass; the library imports itself to exercise cycles.
QString MakeAvailabilityPackage(const QTemporaryDir& project)
{
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    // Only bypassable kinds can be disabled; the compiler ignores "enabled" on pipelines.
    const QJsonObject disabled{{QStringLiteral("id"), QStringLiteral("disabled")},
                               {QStringLiteral("kind"), QStringLiteral("Denoise")},
                               {QStringLiteral("quality"), QStringLiteral("Fast")},
                               {QStringLiteral("enabled"), false}};
    const QJsonObject library{{QStringLiteral("id"), QStringLiteral("nested")},
                              {QStringLiteral("kind"), QStringLiteral("Subgraph")},
                              {QStringLiteral("asset"), LIBRARY_GRAPH},
                              {QStringLiteral("configuration"), DEFAULT_CONFIGURATION}};
    QJsonObject cycle = library;
    cycle.insert(QStringLiteral("id"), QStringLiteral("again"));
    WriteFile(QDir(package).filePath(ROOT_GRAPH),
              MakeGraph(QStringLiteral("Main"),
                        {MakeRayNode(QStringLiteral("native"), {}),
                         MakeRayNode(QStringLiteral("software"),
                                     {{QStringLiteral("type"), QStringLiteral("Compute")},
                                      {QStringLiteral("traversal"), QStringLiteral("ComputeBvh")}}),
                         disabled,
                         library}));
    WriteFile(QDir(package).filePath(LIBRARY_GRAPH),
              MakeGraph(QStringLiteral("Library"), {MakeRayNode(QStringLiteral("inner"), {}), cycle}));
    return package;
}

QGraphicsRectItem* FindItem(const RenderGraphCanvas& canvas, const QString& key)
{
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        if (item->data(0).toString() == key)
        {
            return qgraphicsitem_cast<QGraphicsRectItem*>(item);
        }
    }
    return nullptr;
}

QJsonObject FindNode(const RenderGraphDocument& document, const QString& identity)
{
    QJsonObject graph;
    REQUIRE(document.GetGraph(ROOT_GRAPH, graph) == true);
    for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations"))
                                              .toArray()
                                              .at(0)
                                              .toObject()
                                              .value(QStringLiteral("nodes"))
                                              .toArray())
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return value.toObject();
        }
    }
    return {};
}
} // namespace

TEST_CASE("node requirements resolve invocation and traversal like the graph compiler",
          "[editor][render-graph][availability]")
{
    // Legacy ray pipelines (no invocation type) keep native queries; explicit Compute defaults to the loop.
    CHECK(GetRenderGraphNodeRequirement(MakeRayNode(QStringLiteral("a"), {})) ==
          RenderGraphRequirement::NativeRayQueries);
    CHECK(GetRenderGraphNodeRequirement(
              MakeRayNode(QStringLiteral("a"), {{QStringLiteral("type"), QStringLiteral("RayTracing")}})) ==
          RenderGraphRequirement::RayPipelines);
    CHECK(GetRenderGraphNodeRequirement(
              MakeRayNode(QStringLiteral("a"), {{QStringLiteral("type"), QStringLiteral("Compute")}})) ==
          RenderGraphRequirement::Compute);
    CHECK(GetRenderGraphNodeRequirement(MakeRayNode(QStringLiteral("a"),
                                                    {{QStringLiteral("type"), QStringLiteral("Compute")},
                                                     {QStringLiteral("traversal"), QStringLiteral("NativeQuery")}})) ==
          RenderGraphRequirement::NativeRayQueries);
    // Legacy scene compute: a pass with its own kernel keeps native queries, shared callbacks use the loop.
    CHECK(GetRenderGraphNodeRequirement(MakeSceneRayComputeNode(QStringLiteral("a"), true)) ==
          RenderGraphRequirement::NativeRayQueries);
    CHECK(GetRenderGraphNodeRequirement(MakeSceneRayComputeNode(QStringLiteral("a"), false)) ==
          RenderGraphRequirement::Compute);
    CHECK(GetRenderGraphNodeRequirement(MakePipelineNode(QStringLiteral("ComputePipeline"), QStringLiteral("a"))) ==
          RenderGraphRequirement::BeyondProfile);
    QJsonObject graphics = MakePipelineNode(QStringLiteral("GraphicsPipeline"), QStringLiteral("a"));
    graphics.insert(QStringLiteral("shaders"),
                    QJsonArray{MakeShader(QStringLiteral("vertex")), MakeShader(QStringLiteral("fragment"))});
    CHECK(GetRenderGraphNodeRequirement(graphics) == RenderGraphRequirement::None);
    graphics.insert(QStringLiteral("shaders"),
                    QJsonArray{MakeShader(QStringLiteral("vertex")),
                               MakeShader(QStringLiteral("geometry")),
                               MakeShader(QStringLiteral("fragment"))});
    CHECK(GetRenderGraphNodeRequirement(graphics) == RenderGraphRequirement::BeyondProfile);
    CHECK(GetRenderGraphNodeRequirement({{QStringLiteral("kind"), QStringLiteral("Denoise")}}) ==
          RenderGraphRequirement::Denoiser);
    CHECK(GetRenderGraphNodeRequirement({{QStringLiteral("kind"), QStringLiteral("Raster")}}) ==
          RenderGraphRequirement::None);
}

TEST_CASE("availability compares enabled nodes and nested imports with the device profile",
          "[editor][render-graph][availability]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeAvailabilityPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const QByteArray draft = document.GetDraft(ROOT_GRAPH);
    const uint64 revision = document.GetRevision();

    SECTION("a GPU without ray hardware greys native passes and offers Compute BVH")
    {
        const RenderGraphConfigurationAvailability availability =
            InspectRenderGraphAvailability(document,
                                           ROOT_GRAPH,
                                           DEFAULT_CONFIGURATION,
                                           MakeNativeProfile(false, false));
        REQUIRE(availability.Nodes.contains(QStringLiteral("native")) == true);
        const RenderGraphAvailabilityFeedback native = availability.Nodes.value(QStringLiteral("native"));
        CHECK(native.Availability == RenderGraphAvailability::Unavailable);
        CHECK(native.Requirement == RenderGraphRequirement::NativeRayQueries);
        CHECK(native.Repair == RenderGraphAvailabilityRepair::UseComputeBvh);
        CHECK(native.Label == QStringLiteral("Needs RT hardware"));
        CHECK(native.Explanation.contains(QStringLiteral("Metal")) == true);
        CHECK(native.Explanation.contains(QStringLiteral("Use Compute BVH Traversal")) == true);
        // Ordinary compute traversal runs here; a disabled Denoise is bypassed, so a missing denoiser does not matter.
        CHECK(availability.Nodes.contains(QStringLiteral("software")) == false);
        CHECK(availability.Nodes.contains(QStringLiteral("disabled")) == false);
        // The cyclic library import terminates and reports its one native pass on the instance.
        const RenderGraphAvailabilityFeedback nested = availability.Nodes.value(QStringLiteral("nested"));
        CHECK(nested.Availability == RenderGraphAvailability::Unavailable);
        CHECK(nested.Label == QStringLiteral("1 nested node can't run"));
        CHECK(nested.Explanation.contains(QStringLiteral("nested/inner")) == true);
        CHECK(nested.Repair == RenderGraphAvailabilityRepair::None);
        CHECK(nested.Explanation.contains(QStringLiteral("again")) == false);
        CHECK(availability.UnavailableCount == 2);
        CHECK(availability.Summary.startsWith(QStringLiteral("2 nodes cannot run on this Metal GPU.")) == true);
        CHECK(availability.Summary.contains(QStringLiteral("raster graph")) == true);
    }
    SECTION("ray hardware runs every node and shows no notice")
    {
        const RenderGraphConfigurationAvailability availability =
            InspectRenderGraphAvailability(document, ROOT_GRAPH, DEFAULT_CONFIGURATION, MakeNativeProfile(true));
        CHECK(availability.Nodes.isEmpty() == true);
        CHECK(availability.UnavailableCount == 0);
        CHECK(availability.Summary.isEmpty() == true);
    }
    SECTION("an unknown device makes no device claim")
    {
        const RenderGraphConfigurationAvailability availability =
            InspectRenderGraphAvailability(document, ROOT_GRAPH, DEFAULT_CONFIGURATION, {});
        CHECK(availability.Nodes.isEmpty() == true);
        CHECK(availability.Summary.isEmpty() == true);
    }
    SECTION("the browser editor runs no graph and has no device repair")
    {
        RenderGraphDeviceProfile browser;
        browser.Kind = RenderGraphDeviceKind::Browser;
        const RenderGraphConfigurationAvailability availability =
            InspectRenderGraphAvailability(document, ROOT_GRAPH, DEFAULT_CONFIGURATION, browser);
        CHECK(availability.Nodes.value(QStringLiteral("native")).Label == QStringLiteral("Desktop editor only"));
        CHECK(availability.Nodes.value(QStringLiteral("software")).Availability ==
              RenderGraphAvailability::Unavailable);
        CHECK(availability.Nodes.value(QStringLiteral("native")).Repair == RenderGraphAvailabilityRepair::None);
        CHECK(availability.Summary.contains(QStringLiteral("desktop editor")) == true);
    }
    // Inspection is read-only.
    CHECK(document.GetDraft(ROOT_GRAPH) == draft);
    CHECK(document.GetRevision() == revision);
}

TEST_CASE("engine-profile limits are reported on every device with their repair",
          "[editor][render-graph][availability]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    WriteFile(QDir(package).filePath(ROOT_GRAPH),
              MakeGraph(QStringLiteral("Main"),
                        {MakePipelineNode(QStringLiteral("ComputePipeline"), QStringLiteral("generic")),
                         QJsonObject{{QStringLiteral("id"), QStringLiteral("denoise")},
                                     {QStringLiteral("kind"), QStringLiteral("Denoise")},
                                     {QStringLiteral("quality"), QStringLiteral("Fast")}}}));
    RenderGraphDocument document;
    REQUIRE(document.Load(package, ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    const RenderGraphConfigurationAvailability unknown =
        InspectRenderGraphAvailability(document, ROOT_GRAPH, DEFAULT_CONFIGURATION, {});
    const RenderGraphAvailabilityFeedback generic = unknown.Nodes.value(QStringLiteral("generic"));
    CHECK(generic.Availability == RenderGraphAvailability::Unsupported);
    CHECK(generic.Repair == RenderGraphAvailabilityRepair::AddSceneInputs);
    CHECK(generic.Explanation.contains(QStringLiteral("Add Scene and View Inputs")) == true);
    // No device claim about the denoiser before a device is known.
    CHECK(unknown.Nodes.contains(QStringLiteral("denoise")) == false);
    CHECK(unknown.Summary.contains(QStringLiteral("does not support yet")) == true);
    RenderGraphDeviceProfile withoutDenoiser = MakeNativeProfile(true);
    withoutDenoiser.HasDenoiser = false;
    const RenderGraphConfigurationAvailability native =
        InspectRenderGraphAvailability(document, ROOT_GRAPH, DEFAULT_CONFIGURATION, withoutDenoiser);
    CHECK(native.Nodes.value(QStringLiteral("generic")).Availability == RenderGraphAvailability::Unsupported);
    CHECK(native.Nodes.value(QStringLiteral("denoise")).Label == QStringLiteral("Bypassed · no denoiser"));
    CHECK(native.Nodes.value(QStringLiteral("denoise")).Explanation.contains(QStringLiteral("--with-denoiser")) ==
          true);
    CHECK(native.Summary.contains(QStringLiteral("skips Denoise nodes")) == true);
    CHECK(native.Summary.contains(QStringLiteral("capability error")) == true);
}

TEST_CASE("the Compute BVH repair changes only invocation type and traversal", "[editor][render-graph][availability]")
{
    QJsonObject node = MakeRayNode(QStringLiteral("hardware"),
                                   {{QStringLiteral("type"), QStringLiteral("RayTracing")},
                                    {QStringLiteral("dimensions"), QStringLiteral("ColorTarget")}});
    QJsonObject repaired;
    REQUIRE(MakeComputeBvhRayNode(node, repaired) == true);
    QJsonObject expected = node;
    expected.insert(QStringLiteral("invocation"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("Compute")},
                                {QStringLiteral("traversal"), QStringLiteral("ComputeBvh")},
                                {QStringLiteral("dimensions"), QStringLiteral("ColorTarget")}});
    CHECK(repaired == expected);
    CHECK(GetRenderGraphNodeRequirement(repaired) == RenderGraphRequirement::Compute);
    // Nodes that already run in compute, and non-ray nodes, are rejected and keep the output.
    QJsonObject preserved{{QStringLiteral("kept"), true}};
    CHECK(MakeComputeBvhRayNode(repaired, preserved) == false);
    CHECK(MakeComputeBvhRayNode(MakePipelineNode(QStringLiteral("GraphicsPipeline"), QStringLiteral("g")), preserved) ==
          false);
    CHECK(preserved == QJsonObject{{QStringLiteral("kept"), true}});
}

TEST_CASE("unavailable nodes are greyed on the canvas with a badge and configuration notice",
          "[editor][render-graph][availability][canvas]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeAvailabilityPackage(project), ROOT_GRAPH) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas canvas(document);
    canvas.resize(1000, 700);
    canvas.show();
    canvas.ShowConfiguration(ROOT_GRAPH, DEFAULT_CONFIGURATION);
    QCoreApplication::processEvents();
    QLabel* noticeOrNull = canvas.findChild<QLabel*>(QStringLiteral("renderGraphAvailabilityNotice"));
    REQUIRE(noticeOrNull != nullptr);
    QGraphicsRectItem* badgeOrNull = FindItem(canvas, QStringLiteral("#availability-badge:native"));
    REQUIRE(badgeOrNull != nullptr);
    QGraphicsRectItem* frameOrNull = FindItem(canvas, QStringLiteral("native"));
    REQUIRE(frameOrNull != nullptr);
    // Before a device is known nothing is greyed (these unconnected cards are already dimmed as inactive).
    CHECK(noticeOrNull->isHidden() == true);
    CHECK(badgeOrNull->isVisible() == false);
    CHECK(frameOrNull->pen().style() == Qt::SolidLine);
    const uint64 revision = document.GetRevision();

    canvas.SetDeviceProfile(MakeNativeProfile(false));
    CHECK(noticeOrNull->isHidden() == false);
    CHECK(noticeOrNull->text() == canvas.GetAvailabilitySummary());
    CHECK(badgeOrNull->isVisible() == true);
    CHECK(badgeOrNull->toolTip().contains(QStringLiteral("native ray queries")) == true);
    QGraphicsSimpleTextItem* labelOrNull = nullptr;
    for (QGraphicsItem* child : badgeOrNull->childItems())
    {
        labelOrNull = labelOrNull != nullptr ? labelOrNull : qgraphicsitem_cast<QGraphicsSimpleTextItem*>(child);
    }
    REQUIRE(labelOrNull != nullptr);
    CHECK(labelOrNull->text() == QStringLiteral("Needs RT hardware"));
    CHECK(frameOrNull->opacity() == DIMMED_OPACITY);
    CHECK(frameOrNull->pen().style() == Qt::DashLine);
    CHECK(frameOrNull->toolTip().contains(QStringLiteral("Use Compute BVH Traversal")) == true);
    QGraphicsRectItem* softwareOrNull = FindItem(canvas, QStringLiteral("software"));
    REQUIRE(softwareOrNull != nullptr);
    CHECK(softwareOrNull->pen().style() == Qt::SolidLine);
    CHECK(canvas.GetNodeAvailability(QStringLiteral("software")).Availability == RenderGraphAvailability::Available);
    // A view-only change: drafts and history are untouched.
    CHECK(document.GetRevision() == revision);

    // A device with ray hardware clears every mark.
    canvas.SetDeviceProfile(MakeNativeProfile(true));
    CHECK(noticeOrNull->isHidden() == true);
    CHECK(badgeOrNull->isVisible() == false);
    CHECK(frameOrNull->pen().style() == Qt::SolidLine);
}

TEST_CASE("the canvas repairs an unavailable ray pass after confirmation with Properties hidden",
          "[editor][render-graph][availability][canvas]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QString package = MakeAvailabilityPackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(ROOT_GRAPH)) == true);
    workspace.SetLivePreview(false);
    workspace.resize(1200, 800);
    workspace.show();
    QWidget* propertiesOrNull = workspace.findChild<QWidget*>(QStringLiteral("renderGraphProperties"));
    REQUIRE(propertiesOrNull != nullptr);
    propertiesOrNull->hide();
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.SetDeviceProfile(MakeNativeProfile(false));
    REQUIRE(workspace.SelectNode(ROOT_GRAPH, DEFAULT_CONFIGURATION, QStringLiteral("native")) == true);
    QCoreApplication::processEvents();
    // The outline greys the configuration and the node and keeps their cause.
    QTreeWidget* treeOrNull = workspace.findChild<QTreeWidget*>(QStringLiteral("renderGraphTree"));
    REQUIRE(treeOrNull != nullptr);
    bool hasGreyedNode = false;
    for (QTreeWidgetItemIterator item(treeOrNull); *item != nullptr; ++item)
    {
        if ((*item)->text(0) == QStringLiteral("native : RayTracingPipeline"))
        {
            hasGreyedNode = (*item)->toolTip(0).contains(QStringLiteral("Needs RT hardware"));
        }
    }
    CHECK(hasGreyedNode == true);
    const QJsonObject original = FindNode(workspace.GetDocument(), QStringLiteral("native"));
    const uint64 revision = workspace.GetDocument().GetRevision();
    const auto chooseRepair = [&canvas](const QString& button) {
        QTimer::singleShot(0, &canvas, [&canvas, button]() {
            QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            REQUIRE(menuOrNull != nullptr);
            QAction* causeOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphAvailabilityCause"));
            REQUIRE(causeOrNull != nullptr);
            CHECK(causeOrNull->isEnabled() == false);
            CHECK(causeOrNull->toolTip().contains(QStringLiteral("native ray queries")) == true);
            QAction* repairOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphUseComputeBvhAction"));
            REQUIRE(repairOrNull != nullptr);
            REQUIRE(repairOrNull->isEnabled() == true);
            QTimer::singleShot(0, &canvas, [button]() {
                QMessageBox* boxOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                REQUIRE(boxOrNull != nullptr);
                CHECK(boxOrNull->objectName() == QStringLiteral("renderGraphComputeBvhConfirmation"));
                CHECK(boxOrNull->text().contains(QStringLiteral("native ray query traversal")) == true);
                // Cancel is the default; the action button names the change.
                CHECK(boxOrNull->defaultButton() == boxOrNull->button(QMessageBox::Cancel));
                QAbstractButton* chosenOrNull = button.isEmpty() == true ? boxOrNull->button(QMessageBox::Cancel)
                                                                         : boxOrNull->findChild<QPushButton*>(button);
                REQUIRE(chosenOrNull != nullptr);
                chosenOrNull->click();
            });
            menuOrNull->close();
            repairOrNull->trigger();
        });
        const QPoint point = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("native")) + QPointF(20, 10));
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
        QApplication::sendEvent(canvas.viewport(), &event);
    };
    chooseRepair({});
    CHECK(workspace.GetDocument().GetRevision() == revision);
    CHECK(FindNode(workspace.GetDocument(), QStringLiteral("native")) == original);

    chooseRepair(QStringLiteral("renderGraphComputeBvhApply"));
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    const QJsonObject invocation =
        FindNode(workspace.GetDocument(), QStringLiteral("native")).value(QStringLiteral("invocation")).toObject();
    CHECK(invocation.value(QStringLiteral("type")) == QStringLiteral("Compute"));
    CHECK(invocation.value(QStringLiteral("traversal")) == QStringLiteral("ComputeBvh"));
    // The repaired pass is no longer greyed; one undo restores the authored invocation and its mark.
    CHECK(canvas.GetNodeAvailability(QStringLiteral("native")).Availability == RenderGraphAvailability::Available);
    workspace.Undo();
    CHECK(FindNode(workspace.GetDocument(), QStringLiteral("native")) == original);
    CHECK(canvas.GetNodeAvailability(QStringLiteral("native")).Availability == RenderGraphAvailability::Unavailable);
}
