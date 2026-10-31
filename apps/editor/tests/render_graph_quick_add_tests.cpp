// The Quick Add palette: ranking, the bounded result list, keyboard use, canvas insertion and port filtering.
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_quick_add.h"
#include "internal/render_graph_quick_add_graphs.h"
#include "internal/render_graph_template_registry.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
QString GetRoot()
{
    return QStringLiteral("main.lurender.json");
}

QString MakeQuickAddTestPackage(const QTemporaryDir& project)
{
    const QDir source(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/tests/sdk_consumer/rg2/offscreen"));
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    REQUIRE(QDir().mkpath(package) == true);
    for (const QString& file : {GetRoot(), QStringLiteral("rasterize.lurender.json")})
    {
        REQUIRE(QFile::copy(source.filePath(file), QDir(package).filePath(file)) == true);
    }
    return package;
}

QStringList GetKeys(const QVector<RenderGraphTemplate>& templates)
{
    QStringList keys;
    for (const RenderGraphTemplate& entry : templates)
    {
        keys.append(GetRenderGraphTemplateKey(entry));
    }
    return keys;
}

QVector<RenderGraphTemplate> GetInsertable()
{
    const RenderGraphTemplateRegistry& registry = RenderGraphTemplateRegistry::GetBuiltIn();
    return registry.GetTemplates(RenderGraphTemplateKind::Snippet) +
           registry.GetTemplates(RenderGraphTemplateKind::Node);
}

/// The first configuration's layout position for a node, or a null point when it has none.
QPoint GetLayoutPosition(const RenderGraphDocument& document, const QString& node)
{
    QJsonObject graph;
    REQUIRE(document.GetGraph(GetRoot(), graph) == true);
    const QJsonArray positions = graph.value(QStringLiteral("configurations"))
                                     .toArray()
                                     .at(0)
                                     .toObject()
                                     .value(QStringLiteral("layout"))
                                     .toObject()
                                     .value(QStringLiteral("positions"))
                                     .toArray();
    for (const QJsonValueConstRef position : positions)
    {
        if (position.toObject().value(QStringLiteral("node")).toString() == node)
        {
            return {position.toObject().value(QStringLiteral("x")).toInt(),
                    position.toObject().value(QStringLiteral("y")).toInt()};
        }
    }
    return {};
}

/// The first configuration's record in the root graph draft.
QJsonObject GetConfiguration(const RenderGraphDocument& document)
{
    QJsonObject graph;
    REQUIRE(document.GetGraph(GetRoot(), graph) == true);
    return graph.value(QStringLiteral("configurations")).toArray().at(0).toObject();
}

bool HasEdge(const RenderGraphDocument& document, const QString& from, const QString& to)
{
    const QJsonArray edges = GetConfiguration(document).value(QStringLiteral("edges")).toArray();
    for (const QJsonValueConstRef edge : edges)
    {
        if (edge.toObject().value(QStringLiteral("from")).toString() == from &&
            edge.toObject().value(QStringLiteral("to")).toString() == to)
        {
            return true;
        }
    }
    return false;
}

bool HasNode(const RenderGraphDocument& document, const QString& node)
{
    const QJsonArray nodes = GetConfiguration(document).value(QStringLiteral("nodes")).toArray();
    for (const QJsonValueConstRef value : nodes)
    {
        if (value.toObject().value(QStringLiteral("id")).toString() == node)
        {
            return true;
        }
    }
    return false;
}

/// A shown workspace on the offscreen test package with its whole graph in view and no Quick Add history.
struct QuickAddCanvasFixture final
{
    QTemporaryDir Project;
    RendererWorkspace Workspace{{}};
    RenderGraphQuickAdd* PaletteOrNull = nullptr;

    QuickAddCanvasFixture()
    {
        const QString package = MakeQuickAddTestPackage(Project);
        Workspace.SetProject(Project.path(), 1);
        REQUIRE(Workspace.OpenGraph(QDir(package).filePath(GetRoot())) == true);
        Workspace.SetLivePreview(false);
        Workspace.resize(1450, 850);
        Workspace.show();
        QCoreApplication::processEvents();
        RenderGraphCanvas& canvas = Workspace.GetCanvas();
        canvas.fitInView(canvas.scene()->itemsBoundingRect().adjusted(-200, -200, 200, 200), Qt::KeepAspectRatio);
        PaletteOrNull = Workspace.findChild<RenderGraphQuickAdd*>(QStringLiteral("renderGraphQuickAdd"));
        REQUIRE(PaletteOrNull != nullptr);
        PaletteOrNull->SetRecent({});
        PaletteOrNull->SetPinned({});
    }

    /// A viewport point over empty canvas, well away from avoid.
    [[nodiscard]] QPoint FindEmptyPoint(QPoint avoid)
    {
        RenderGraphCanvas& canvas = Workspace.GetCanvas();
        constexpr int32 STEP = 24;
        constexpr int32 MINIMUM_DISTANCE = 120;
        for (int32 y = STEP; y < canvas.viewport()->height() - STEP; y += STEP)
        {
            for (int32 x = STEP; x < canvas.viewport()->width() - STEP; x += STEP)
            {
                const QPoint point(x, y);
                if ((point - avoid).manhattanLength() >= MINIMUM_DISTANCE && canvas.itemAt(point) == nullptr)
                {
                    return point;
                }
            }
        }
        FAIL("the canvas has no empty point in view");
        return {};
    }

    /// Drag a wire from an output and release it on the viewport point end.
    void DragWire(const QString& from, QPoint end)
    {
        RenderGraphCanvas& canvas = Workspace.GetCanvas();
        const QPoint start = canvas.mapFromScene(canvas.GetPortPosition(from, true));
        QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(canvas.viewport(), (start + end) / 2);
        QTest::mouseMove(canvas.viewport(), end);
        QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, end);
    }
};
} // namespace

TEST_CASE("Quick Add ranks label words first and accepts synonyms", "[editor][render-graph][quick-add]")
{
    const QVector<RenderGraphTemplate> templates = GetInsertable();
    CHECK(GetKeys(SearchRenderGraphTemplates(QStringLiteral("tone"), templates)).first() ==
          QStringLiteral("node:ToneMap"));
    // Camel-case identities split into words, so "graphics pipe" finds GraphicsPipeline.
    CHECK(GetKeys(SearchRenderGraphTemplates(QStringLiteral("graphics pipe"), templates)).first() ==
          QStringLiteral("node:GraphicsPipeline"));
    // Keywords hold the words people type: "hdr" for ToneMap, "fsr" for the FSR presets.
    CHECK(GetKeys(SearchRenderGraphTemplates(QStringLiteral("hdr"), templates))
              .contains(QStringLiteral("node:ToneMap")) == true);
    const QStringList upscalers = GetKeys(SearchRenderGraphTemplates(QStringLiteral("fsr"), templates));
    CHECK(upscalers == QStringList{QStringLiteral("node:Upscale/fsr-1"),
                                   QStringLiteral("node:Upscale/fsr-2"),
                                   QStringLiteral("node:Upscale/fsr-3")});
    CHECK(GetKeys(SearchRenderGraphTemplates(QStringLiteral("restir"), templates)).first() ==
          QStringLiteral("snippet:restir-di"));
    CHECK(GetKeys(SearchRenderGraphTemplates(QStringLiteral("tiled"), templates)).first() ==
          QStringLiteral("snippet:tiled-vbuffer-nee"));
    CHECK(GetKeys(SearchRenderGraphTemplates(QStringLiteral("wavefront"), templates)).first() ==
          QStringLiteral("snippet:compute-ray-tracing"));
    CHECK(SearchRenderGraphTemplates(QStringLiteral("tone zzz"), templates).isEmpty() == true);

    // An empty query keeps everything, grouped by category in vocabulary order.
    const QVector<RenderGraphTemplate> all = SearchRenderGraphTemplates({}, templates);
    REQUIRE(all.size() == templates.size());
    const QStringList categories = GetRenderGraphTemplateCategories(RenderGraphTemplateKind::Node);
    for (qsizetype i = 1; i < all.size(); ++i)
    {
        CAPTURE(GetRenderGraphTemplateKey(all.at(i)).toStdString());
        CHECK(categories.indexOf(all.at(i - 1).Category) <= categories.indexOf(all.at(i).Category));
    }
}

TEST_CASE("Quick Add never lists more rows than fit", "[editor][render-graph][quick-add]")
{
    RenderGraphQuickAdd palette(RenderGraphTemplateRegistry::GetBuiltIn());
    palette.SetRecent({QStringLiteral("node:ToneMap"), QStringLiteral("snippet:visibility-buffer")});
    palette.SetPinned({QStringLiteral("node:Present"), QStringLiteral("node:Retired")});
    palette.Open(QPoint(40, 40));
    REQUIRE(palette.isVisible() == true);
    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);

    // Set LUDUS_EDITOR_QUICK_ADD_CAPTURE to a path prefix to save review screenshots.
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_QUICK_ADD_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(palette.grab().save(capture + QStringLiteral("-suggested.png")) == true);
    }
    // Opening shows recent, then pinned templates; unknown keys are kept but not listed.
    CHECK(palette.GetCategory() == QStringLiteral("suggested"));
    CHECK(palette.GetListedKeys() == QStringList{QStringLiteral("node:ToneMap"),
                                                 QStringLiteral("snippet:visibility-buffer"),
                                                 QStringLiteral("node:Present")});
    CHECK(palette.GetPinned().contains(QStringLiteral("node:Retired")) == true);

    // Everything is far more than eight rows: the rest is counted, not scrolled to.
    palette.SetCategory(QStringLiteral("all"));
    CHECK(palette.GetListedKeys().size() == RenderGraphQuickAdd::MAX_ROWS);
    CHECK(palette.GetHiddenCount() == GetInsertable().size() - RenderGraphQuickAdd::MAX_ROWS);
    QLabel* moreOrNull = palette.findChild<QLabel*>(QStringLiteral("renderGraphQuickAddMore"));
    REQUIRE(moreOrNull != nullptr);
    CHECK(moreOrNull->isVisible() == true);
    CHECK(moreOrNull->text().startsWith(QStringLiteral("+%1 more").arg(palette.GetHiddenCount())) == true);

    // A category narrows the list: all eight upscalers fit exactly.
    palette.SetCategory(QStringLiteral("upscaling"));
    qsizetype upscalers = 0;
    for (const RenderGraphTemplate& entry : GetInsertable())
    {
        upscalers += entry.Category == QStringLiteral("upscaling") ? 1 : 0;
    }
    REQUIRE(upscalers == RenderGraphQuickAdd::MAX_ROWS);
    CHECK(palette.GetListedKeys().size() == upscalers);
    CHECK(palette.GetHiddenCount() == 0);

    // Typing searches every category and resets the category to All matches.
    QTest::keyClicks(searchOrNull, QStringLiteral("map"));
    CHECK(palette.GetCategory() == QStringLiteral("all"));
    CHECK(palette.GetListedKeys().first() == QStringLiteral("node:ToneMap"));
    if (capture.isEmpty() == false)
    {
        REQUIRE(palette.grab().save(capture + QStringLiteral("-search.png")) == true);
    }

    // Tab cycles the kind filter instead of leaving the search field.
    QTest::keyClick(searchOrNull, Qt::Key_Tab);
    CHECK(palette.GetKind() == RenderGraphQuickAddKind::Snippets);
    CHECK(searchOrNull->hasFocus() == true);
    QTest::keyClick(searchOrNull, Qt::Key_Tab);
    CHECK(palette.GetKind() == RenderGraphQuickAddKind::Nodes);
    QTest::keyClick(searchOrNull, Qt::Key_Backtab);
    CHECK(palette.GetKind() == RenderGraphQuickAddKind::Snippets);
    searchOrNull->clear();
    palette.SetCategory(QStringLiteral("all"));
    for (const QString& key : palette.GetListedKeys())
    {
        CHECK(key.startsWith(QStringLiteral("snippet:")) == true);
    }
    palette.close();
}

TEST_CASE("Quick Add inserts with the keyboard and remembers recent and pinned templates",
          "[editor][render-graph][quick-add]")
{
    RenderGraphQuickAdd palette(RenderGraphTemplateRegistry::GetBuiltIn());
    QSignalSpy inserted(&palette, &RenderGraphQuickAdd::InsertRequested);
    QSignalSpy history(&palette, &RenderGraphQuickAdd::HistoryChanged);
    QSignalSpy newGraph(&palette, &RenderGraphQuickAdd::NewGraphRequested);
    palette.Open(QPoint(40, 40));
    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    QTest::keyClicks(searchOrNull, QStringLiteral("upscale"));
    CHECK(palette.GetHighlightedKey() == QStringLiteral("node:Upscale/bilinear"));
    QTest::keyClick(searchOrNull, Qt::Key_Down);
    CHECK(palette.GetHighlightedKey() == QStringLiteral("node:Upscale/dlss-sr"));
    QTest::keyClick(searchOrNull, Qt::Key_Up);
    QTest::keyClick(searchOrNull, Qt::Key_Up);
    CHECK(palette.GetHighlightedKey() == QStringLiteral("node:Upscale/bilinear"));
    QTest::keyClick(searchOrNull, Qt::Key_Return);
    REQUIRE(inserted.size() == 1);
    CHECK(GetRenderGraphTemplateKey(inserted.at(0).at(0).value<RenderGraphTemplate>()) ==
          QStringLiteral("node:Upscale/bilinear"));
    CHECK(palette.isVisible() == false);
    CHECK(palette.GetRecent() == QStringList{QStringLiteral("node:Upscale/bilinear")});
    CHECK(history.size() == 1);

    // Reopening suggests it; pinning keeps a template there after it leaves Recent.
    palette.Open(QPoint(40, 40));
    CHECK(palette.GetListedKeys() == QStringList{QStringLiteral("node:Upscale/bilinear")});
    palette.TogglePinned(QStringLiteral("node:Denoise"));
    CHECK(palette.GetPinned() == QStringList{QStringLiteral("node:Denoise")});
    CHECK(palette.GetListedKeys() ==
          QStringList{QStringLiteral("node:Upscale/bilinear"), QStringLiteral("node:Denoise")});
    palette.TogglePinned(QStringLiteral("node:Denoise"));
    CHECK(palette.GetPinned().isEmpty() == true);
    CHECK(history.size() == 3);

    // Recent keeps the newest three, most recent first.
    for (const QString& query : {QStringLiteral("present"), QStringLiteral("export"), QStringLiteral("camera")})
    {
        searchOrNull->setText(query);
        REQUIRE(palette.InsertHighlighted() == true);
        palette.Open(QPoint(40, 40));
    }
    CHECK(palette.GetRecent() ==
          QStringList{QStringLiteral("node:Camera"), QStringLiteral("node:Export"), QStringLiteral("node:Present")});

    // Complete graphs are offered through New Render Graph, never inserted.
    QPushButton* graphHintOrNull = palette.findChild<QPushButton*>(QStringLiteral("renderGraphQuickAddGraphHint"));
    REQUIRE(graphHintOrNull != nullptr);
    searchOrNull->setText(QStringLiteral("forward"));
    CHECK(graphHintOrNull->isVisible() == true);
    for (const QString& key : palette.GetListedKeys())
    {
        CHECK(key.startsWith(QStringLiteral("graph:")) == false);
    }
    graphHintOrNull->click();
    REQUIRE(newGraph.size() == 1);
    CHECK(newGraph.at(0).at(0).toString() == QStringLiteral("forward-lit"));
    CHECK(palette.isVisible() == false);
}

TEST_CASE("Quick Add discovers unused graphs and searches their interfaces and configurations",
          "[editor][render-graph][quick-add]")
{
    QTemporaryDir project;
    const QString package = MakeQuickAddTestPackage(project);
    const QString asset = QStringLiteral("sampling/StratumSampling.lurender.json");
    REQUIRE(QDir(package).mkpath(QStringLiteral("sampling")) == true);
    REQUIRE(QFile::copy(QDir(package).filePath(QStringLiteral("rasterize.lurender.json")),
                        QDir(package).filePath(asset)) == true);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, GetRoot()) == RenderGraphDocumentStatus::Ok);
    QJsonObject graph;
    REQUIRE(document.GetGraph(asset, graph) == true);
    graph.insert(QStringLiteral("id"), QStringLiteral("StratumSampling"));
    graph.insert(QStringLiteral("default"), QStringLiteral("DepthOnly"));
    REQUIRE(document.SetText(asset, EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);

    RenderGraphQuickAdd palette(RenderGraphTemplateRegistry::GetBuiltIn());
    palette.SetGraphs(BuildRenderGraphQuickAddGraphs(document, GetRoot(), QStringLiteral("Default")));
    palette.Open(QPoint(40, 40));
    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    const QString key = QStringLiteral("graph:") + asset;
    searchOrNull->setText(QStringLiteral("stratumsampl"));
    CHECK(palette.GetListedKeys() == QStringList{key});
    palette.SetKind(RenderGraphQuickAddKind::Graphs);
    for (const QString& query : {QStringLiteral("DepthOnly"),
                                 QStringLiteral("Scene"),
                                 QStringLiteral("reference"),
                                 QStringLiteral("sampling/StratumSampling")})
    {
        searchOrNull->setText(query);
        CHECK(palette.GetListedKeys().contains(key) == true);
    }
    searchOrNull->setText(QStringLiteral("stratumsampl"));
    QLabel* portsOrNull = palette.findChild<QLabel*>(QStringLiteral("renderGraphQuickAddPorts"));
    REQUIRE(portsOrNull != nullptr);
    CHECK(portsOrNull->text().contains(asset) == true);
    CHECK(portsOrNull->text().contains(QStringLiteral("Default configuration: DepthOnly")) == true);
    CHECK(portsOrNull->text().contains(QStringLiteral("depth (DepthTexture)")) == true);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_QUICK_ADD_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(palette.grab().save(capture + QStringLiteral("-graphs.png")) == true);
    }
    QSignalSpy inserted(&palette, &RenderGraphQuickAdd::InsertRequested);
    REQUIRE(palette.InsertHighlighted() == true);
    REQUIRE(inserted.size() == 1);
    const RenderGraphTemplate entry = inserted.at(0).at(0).value<RenderGraphTemplate>();
    CHECK(entry.Identity == asset);
    CHECK(entry.DefaultConfiguration == QStringLiteral("DepthOnly"));
    palette.Open(QPoint(40, 40));
    CHECK(palette.GetListedKeys().contains(key) == true);
    palette.TogglePinned(key);
    CHECK(palette.GetPinned().contains(key) == true);
    palette.close();

    // Recheck the selected default before insertion instead of silently using a new interface.
    graph.insert(QStringLiteral("default"), QStringLiteral("ColorAndDepth"));
    REQUIRE(document.SetText(asset, EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
    CHECK(CheckRenderGraphQuickAddGraph(document, GetRoot(), QStringLiteral("Default"), entry)
              .contains(QStringLiteral("default configuration changed")) == true);
}

TEST_CASE("Quick Add graph references preserve layout and undo with Properties hidden",
          "[editor][render-graph][quick-add]")
{
    QuickAddCanvasFixture fixture;
    RendererWorkspace& workspace = fixture.Workspace;
    RenderGraphQuickAdd& palette = *fixture.PaletteOrNull;
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray original = document.GetDraft(GetRoot());
    const QByteArray library = document.GetDraft(QStringLiteral("rasterize.lurender.json"));
    workspace.TakeInspectorPanel(&workspace)->hide();
    REQUIRE(workspace.ShowQuickAdd(QPointF(320.0, 240.0)) == true);
    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    searchOrNull->setText(QStringLiteral("RasterizeGraph"));
    REQUIRE(palette.InsertHighlighted() == true);
    const QJsonArray nodes = GetConfiguration(document).value(QStringLiteral("nodes")).toArray();
    const QJsonObject inserted = nodes.last().toObject();
    CHECK(inserted.value(QStringLiteral("kind")).toString() == QStringLiteral("Subgraph"));
    CHECK(inserted.value(QStringLiteral("asset")).toString() == QStringLiteral("rasterize.lurender.json"));
    CHECK(inserted.value(QStringLiteral("configuration")).toString() == QStringLiteral("ColorAndDepth"));
    CHECK(GetLayoutPosition(document, QStringLiteral("subgraph")) == QPoint(320, 240));
    CHECK(document.GetDraft(QStringLiteral("rasterize.lurender.json")) == library);
    workspace.Undo();
    CHECK(document.GetDraft(GetRoot()) == original);
    workspace.Redo();
    CHECK(HasNode(document, QStringLiteral("subgraph")) == true);
    REQUIRE(workspace.ShowQuickAdd(QPointF(400.0, 300.0)) == true);
    searchOrNull->setText(QStringLiteral("RasterizeGraph"));
    REQUIRE(palette.InsertHighlighted() == true);
    CHECK(HasNode(document, QStringLiteral("subgraph2")) == true);
}

TEST_CASE("Quick Add keeps recursive and invalid graphs inspectable but blocks insertion",
          "[editor][render-graph][quick-add]")
{
    QuickAddCanvasFixture fixture;
    RendererWorkspace& workspace = fixture.Workspace;
    RenderGraphQuickAdd& palette = *fixture.PaletteOrNull;
    REQUIRE(workspace.SelectNode(QStringLiteral("rasterize.lurender.json"), QStringLiteral("ColorAndDepth"), {}) ==
            true);
    REQUIRE(workspace.ShowQuickAdd(QPointF(0.0, 0.0)) == true);
    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    QLabel* warningOrNull = palette.findChild<QLabel*>(QStringLiteral("renderGraphQuickAddWarning"));
    QPushButton* insertOrNull = palette.findChild<QPushButton*>(QStringLiteral("renderGraphQuickAddInsert"));
    REQUIRE(searchOrNull != nullptr);
    REQUIRE(warningOrNull != nullptr);
    REQUIRE(insertOrNull != nullptr);
    palette.SetKind(RenderGraphQuickAddKind::Graphs);
    QSignalSpy inserted(&palette, &RenderGraphQuickAdd::InsertRequested);
    const uint64 revision = workspace.GetDocument().GetRevision();
    for (const QString& query : {QStringLiteral("RasterizeGraph"), QStringLiteral("Main")})
    {
        searchOrNull->setText(query);
        REQUIRE(palette.GetListedKeys().size() == 1);
        CHECK(warningOrNull->text().contains(QStringLiteral("cycle")) == true);
        CHECK(insertOrNull->isEnabled() == false);
        CHECK(palette.InsertHighlighted() == false);
    }
    CHECK(inserted.isEmpty() == true);
    CHECK(workspace.GetDocument().GetRevision() == revision);
    palette.close();
    REQUIRE(workspace.SelectNode(QStringLiteral("rasterize.lurender.json"), QStringLiteral("DepthOnly"), {}) == true);
    REQUIRE(workspace.ShowQuickAdd(QPointF(0.0, 0.0)) == true);
    searchOrNull->setText(QStringLiteral("Main"));
    palette.SetKind(RenderGraphQuickAddKind::Graphs);
    // Imports are recursive across files even when the selected configuration is different.
    CHECK(warningOrNull->text().contains(QStringLiteral("cycle")) == true);
    CHECK(palette.InsertHighlighted() == false);
    palette.close();

    QJsonObject graph;
    REQUIRE(workspace.GetDocument().GetGraph(QStringLiteral("rasterize.lurender.json"), graph) == true);
    graph.insert(QStringLiteral("default"), QStringLiteral("Missing"));
    REQUIRE(workspace.GetDocument().SetText(QStringLiteral("rasterize.lurender.json"), EncodeCanonicalJson(graph)) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.SelectNode(GetRoot(), QStringLiteral("Default"), {}) == true);
    REQUIRE(workspace.ShowQuickAdd(QPointF(0.0, 0.0)) == true);
    searchOrNull->setText(QStringLiteral("RasterizeGraph"));
    CHECK(warningOrNull->text().contains(QStringLiteral("no valid interface")) == true);
    CHECK(palette.InsertHighlighted() == false);
    palette.close();
}

TEST_CASE("Quick Add rechecks graph defaults before editing the draft", "[editor][render-graph][quick-add]")
{
    QuickAddCanvasFixture fixture;
    RendererWorkspace& workspace = fixture.Workspace;
    RenderGraphQuickAdd& palette = *fixture.PaletteOrNull;
    RenderGraphDocument& document = workspace.GetDocument();
    REQUIRE(workspace.ShowQuickAdd(QPointF(100.0, 200.0)) == true);
    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    searchOrNull->setText(QStringLiteral("RasterizeGraph"));
    QJsonObject graph;
    REQUIRE(document.GetGraph(QStringLiteral("rasterize.lurender.json"), graph) == true);
    graph.insert(QStringLiteral("default"), QStringLiteral("DepthOnly"));
    REQUIRE(document.SetText(QStringLiteral("rasterize.lurender.json"), EncodeCanonicalJson(graph)) ==
            RenderGraphDocumentStatus::Ok);
    const uint64 revision = document.GetRevision();
    const QByteArray root = document.GetDraft(GetRoot());
    REQUIRE(palette.InsertHighlighted() == true);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(GetRoot()) == root);
    REQUIRE(workspace.ShowQuickAdd(QPointF(100.0, 200.0)) == true);
    searchOrNull->setText(QStringLiteral("RasterizeGraph"));
    REQUIRE(palette.InsertHighlighted() == true);
    const QJsonArray nodes = GetConfiguration(document).value(QStringLiteral("nodes")).toArray();
    CHECK(nodes.last().toObject().value(QStringLiteral("configuration")).toString() == QStringLiteral("DepthOnly"));
}

TEST_CASE("Quick Add connects graph interfaces in one undo step and ranks compatible graphs first",
          "[editor][render-graph][quick-add]")
{
    QuickAddCanvasFixture fixture;
    RendererWorkspace& workspace = fixture.Workspace;
    RenderGraphQuickAdd& palette = *fixture.PaletteOrNull;
    const QByteArray original = workspace.GetDocument().GetDraft(GetRoot());
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const QPoint drop =
        fixture.FindEmptyPoint(canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("scene.scene"), true)));
    fixture.DragWire(QStringLiteral("scene.scene"), drop);
    REQUIRE(palette.isVisible() == true);
    palette.SetKind(RenderGraphQuickAddKind::Graphs);
    REQUIRE(palette.GetListedKeys().size() == 2);
    CHECK(palette.GetListedKeys().first() == QStringLiteral("graph:rasterize.lurender.json"));
    CHECK(palette.GetPortFilter().Inputs.value(palette.GetHighlightedKey()) == QStringLiteral("scene"));
    REQUIRE(palette.InsertHighlighted() == true);
    CHECK(HasEdge(workspace.GetDocument(), QStringLiteral("scene.scene"), QStringLiteral("subgraph.scene")) == true);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == original);
    workspace.Redo();
    CHECK(HasEdge(workspace.GetDocument(), QStringLiteral("scene.scene"), QStringLiteral("subgraph.scene")) == true);

    QJsonObject graph;
    REQUIRE(workspace.GetDocument().GetGraph(QStringLiteral("rasterize.lurender.json"), graph) == true);
    graph.insert(QStringLiteral("default"), QStringLiteral("DepthOnly"));
    REQUIRE(workspace.GetDocument().SetText(QStringLiteral("rasterize.lurender.json"), EncodeCanonicalJson(graph)) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(workspace.ShowConnectQuickAdd(QStringLiteral("color.texture"), QPointF(300.0, 200.0)) == true);
    palette.SetKind(RenderGraphQuickAddKind::Graphs);
    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    searchOrNull->setText(QStringLiteral("RasterizeGraph"));
    QLabel* warningOrNull = palette.findChild<QLabel*>(QStringLiteral("renderGraphQuickAddWarning"));
    REQUIRE(warningOrNull != nullptr);
    CHECK(warningOrNull->text().contains(QStringLiteral("No input accepts")) == true);
    CHECK(palette.InsertHighlighted() == false);
    palette.ClearPortFilter();
    searchOrNull->setText(QStringLiteral("RasterizeGraph"));
    const QJsonArray edgesBefore = GetConfiguration(workspace.GetDocument()).value(QStringLiteral("edges")).toArray();
    REQUIRE(palette.InsertHighlighted() == true);
    CHECK(HasNode(workspace.GetDocument(), QStringLiteral("subgraph2")) == true);
    CHECK(GetConfiguration(workspace.GetDocument()).value(QStringLiteral("edges")).toArray() == edgesBefore);
}

TEST_CASE("Quick Add places nodes where it opened in one undo step", "[editor][render-graph][quick-add]")
{
    QTemporaryDir project;
    const QString package = MakeQuickAddTestPackage(project);
    RendererWorkspace workspace({});
    workspace.SetProject(project.path(), 1);
    REQUIRE(workspace.OpenGraph(QDir(package).filePath(GetRoot())) == true);
    workspace.SetLivePreview(false);
    workspace.show();
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray original = document.GetDraft(GetRoot());
    RenderGraphQuickAdd* paletteOrNull =
        workspace.findChild<RenderGraphQuickAdd*>(QStringLiteral("renderGraphQuickAdd"));
    REQUIRE(paletteOrNull != nullptr);
    paletteOrNull->SetRecent({});
    paletteOrNull->SetPinned({});

    REQUIRE(workspace.ShowQuickAdd(QPointF(320.0, 240.0)) == true);
    REQUIRE(paletteOrNull->isVisible() == true);
    QLineEdit* searchOrNull = paletteOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    QTest::keyClicks(searchOrNull, QStringLiteral("tonemap"));
    QTest::keyClick(searchOrNull, Qt::Key_Return);
    CHECK(workspace.GetCanvas().HasNode(QStringLiteral("tonemap")) == true);
    CHECK(GetLayoutPosition(document, QStringLiteral("tonemap")) == QPoint(320, 240));
    workspace.Undo();
    CHECK(document.GetDraft(GetRoot()) == original);
    workspace.Redo();
    CHECK(GetLayoutPosition(document, QStringLiteral("tonemap")) == QPoint(320, 240));

    // Upscale presets place the Upscale node there; their helper nodes keep automatic placement.
    REQUIRE(workspace.ShowQuickAdd(QPointF(-40.0, 600.0)) == true);
    searchOrNull->setText(QStringLiteral("bilinear"));
    REQUIRE(paletteOrNull->InsertHighlighted() == true);
    CHECK(GetLayoutPosition(document, QStringLiteral("bilinear")) == QPoint(-40, 600));

    // Snippets open Apply Configuration Template with that snippet selected.
    QString offered;
    QTimer::singleShot(0, &workspace, [&offered]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        QComboBox* choiceOrNull =
            dialogOrNull->findChild<QComboBox*>(QStringLiteral("renderGraphConfigurationTemplate"));
        REQUIRE(choiceOrNull != nullptr);
        offered = choiceOrNull->currentData().toString();
        dialogOrNull->reject();
    });
    const QByteArray beforeSnippet = document.GetDraft(GetRoot());
    REQUIRE(workspace.ShowQuickAdd(QPointF(0.0, 0.0)) == true);
    searchOrNull->setText(QStringLiteral("visibility buffer"));
    paletteOrNull->SetKind(RenderGraphQuickAddKind::Snippets);
    REQUIRE(paletteOrNull->GetHighlightedKey() == QStringLiteral("snippet:visibility-buffer"));
    REQUIRE(paletteOrNull->InsertHighlighted() == true);
    CHECK(offered == QStringLiteral("visibility-buffer"));
    CHECK(document.GetDraft(GetRoot()) == beforeSnippet);

    // Shift+A opens Quick Add from the canvas without editing the draft.
    paletteOrNull->close();
    workspace.GetCanvas().setFocus();
    QTest::keyClick(&workspace.GetCanvas(), Qt::Key_A, Qt::ShiftModifier);
    CHECK(paletteOrNull->isVisible() == true);
    paletteOrNull->close();
    CHECK(document.GetDraft(GetRoot()) == beforeSnippet);
}

TEST_CASE("Quick Add needs an open graph", "[editor][render-graph][quick-add]")
{
    RendererWorkspace workspace({});
    RenderGraphQuickAdd* paletteOrNull =
        workspace.findChild<RenderGraphQuickAdd*>(QStringLiteral("renderGraphQuickAdd"));
    REQUIRE(paletteOrNull != nullptr);
    CHECK(workspace.ShowQuickAdd(QPointF(0.0, 0.0)) == false);
    CHECK(paletteOrNull->isVisible() == false);
}

TEST_CASE("Quick Add lists only nodes that accept a port filter, and can clear it", "[editor][render-graph][quick-add]")
{
    RenderGraphQuickAdd palette(RenderGraphTemplateRegistry::GetBuiltIn());
    QSignalSpy inserted(&palette, &RenderGraphQuickAdd::InsertRequested);
    palette.SetRecent({QStringLiteral("snippet:visibility-buffer"), QStringLiteral("node:Scene")});
    const RenderGraphQuickAddPortFilter filter{QStringLiteral("tonemap.color"),
                                               QStringLiteral("ColorTexture"),
                                               {{QStringLiteral("node:Export"), QStringLiteral("texture")},
                                                {QStringLiteral("node:Present"), QStringLiteral("color")}}};
    palette.Open(QPoint(40, 40), filter);
    QWidget* contextOrNull = palette.findChild<QWidget*>(QStringLiteral("renderGraphQuickAddContext"));
    QLabel* contextLabelOrNull = palette.findChild<QLabel*>(QStringLiteral("renderGraphQuickAddContextLabel"));
    QToolButton* clearOrNull = palette.findChild<QToolButton*>(QStringLiteral("renderGraphQuickAddContextClear"));
    QToolButton* snippetsOrNull = palette.findChild<QToolButton*>(QStringLiteral("renderGraphQuickAddKindSnippets"));
    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(contextOrNull != nullptr);
    REQUIRE(contextLabelOrNull != nullptr);
    REQUIRE(clearOrNull != nullptr);
    REQUIRE(snippetsOrNull != nullptr);
    REQUIRE(searchOrNull != nullptr);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_QUICK_ADD_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(palette.grab().save(capture + QStringLiteral("-filtered.png")) == true);
    }

    // The filter opens on everything compatible: recent incompatible templates and snippets stay out.
    CHECK(contextOrNull->isVisible() == true);
    CHECK(contextLabelOrNull->text() == QStringLiteral("Connect from tonemap.color · ColorTexture"));
    CHECK(palette.GetCategory() == QStringLiteral("all"));
    CHECK(palette.GetListedKeys() == QStringList{QStringLiteral("node:Export"), QStringLiteral("node:Present")});
    CHECK(snippetsOrNull->isEnabled() == false);
    QTest::keyClick(searchOrNull, Qt::Key_Tab);
    CHECK(palette.GetKind() == RenderGraphQuickAddKind::Nodes);
    QTest::keyClick(searchOrNull, Qt::Key_Tab);
    CHECK(palette.GetKind() == RenderGraphQuickAddKind::Graphs);
    QTest::keyClick(searchOrNull, Qt::Key_Tab);
    CHECK(palette.GetKind() == RenderGraphQuickAddKind::All);
    QTest::keyClicks(searchOrNull, QStringLiteral("scene"));
    CHECK(palette.GetListedKeys().isEmpty() == true);
    searchOrNull->setText(QStringLiteral("present"));
    REQUIRE(palette.InsertHighlighted() == true);
    REQUIRE(inserted.size() == 1);
    CHECK(GetRenderGraphTemplateKey(inserted.at(0).at(0).value<RenderGraphTemplate>()) ==
          QStringLiteral("node:Present"));
    CHECK(inserted.at(0).at(1).toString() == QStringLiteral("color"));

    // The clear button lists every template again and the next insert is unconnected.
    palette.Open(QPoint(40, 40), filter);
    clearOrNull->click();
    CHECK(palette.GetPortFilter().Source.isEmpty() == true);
    CHECK(contextOrNull->isVisible() == false);
    CHECK(snippetsOrNull->isEnabled() == true);
    CHECK(palette.GetCategory() == QStringLiteral("suggested"));
    CHECK(palette.GetListedKeys().contains(QStringLiteral("snippet:visibility-buffer")) == true);
    searchOrNull->setText(QStringLiteral("export"));
    REQUIRE(palette.InsertHighlighted() == true);
    REQUIRE(inserted.size() == 2);
    CHECK(inserted.at(1).at(1).toString().isEmpty() == true);

    // Backspace in an empty search clears the filter too; reopening without one shows no context.
    palette.Open(QPoint(40, 40), filter);
    QTest::keyClick(searchOrNull, Qt::Key_Backspace);
    CHECK(palette.GetPortFilter().Source.isEmpty() == true);
    CHECK(contextOrNull->isVisible() == false);
    palette.Open(QPoint(40, 40), filter);
    palette.close();
    palette.Open(QPoint(40, 40));
    CHECK(contextOrNull->isVisible() == false);
    palette.close();
}

TEST_CASE("Dropping a wire on empty canvas adds a connected node in one undo step", "[editor][render-graph][quick-add]")
{
    QuickAddCanvasFixture fixture;
    RenderGraphCanvas& canvas = fixture.Workspace.GetCanvas();
    RenderGraphDocument& document = fixture.Workspace.GetDocument();
    RenderGraphQuickAdd& palette = *fixture.PaletteOrNull;
    const QByteArray original = document.GetDraft(GetRoot());
    const QString source = QStringLiteral("color.texture");
    const QPoint drop = fixture.FindEmptyPoint(canvas.mapFromScene(canvas.GetPortPosition(source, true)));
    const QPointF dropScene = canvas.mapToScene(drop);

    fixture.DragWire(source, drop);
    REQUIRE(palette.isVisible() == true);
    CHECK(palette.GetPortFilter().Source == source);
    CHECK(palette.GetPortFilter().Type == QStringLiteral("ColorTexture"));
    CHECK(palette.GetPortFilter().Inputs.value(QStringLiteral("node:ToneMap")) == QStringLiteral("color"));
    QLabel* contextLabelOrNull = palette.findChild<QLabel*>(QStringLiteral("renderGraphQuickAddContextLabel"));
    REQUIRE(contextLabelOrNull != nullptr);
    CHECK(contextLabelOrNull->text() == QStringLiteral("Connect from color.texture · ColorTexture"));
    // Every row passed the link pre-check; nodes without such an input are never offered.
    for (const QString& key : palette.GetListedKeys())
    {
        CHECK(palette.GetPortFilter().Inputs.contains(key) == true);
    }
    CHECK(palette.GetPortFilter().Inputs.contains(QStringLiteral("node:Scene")) == false);
    CHECK(document.GetDraft(GetRoot()) == original);

    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    QTest::keyClicks(searchOrNull, QStringLiteral("tonemap"));
    QTest::keyClick(searchOrNull, Qt::Key_Return);
    REQUIRE(HasNode(document, QStringLiteral("tonemap")) == true);
    CHECK(HasEdge(document, source, QStringLiteral("tonemap.color")) == true);
    // The matching input sits on the drop point (layout positions are whole canvas units).
    const QPointF input = canvas.GetPortPosition(QStringLiteral("tonemap.color"), false);
    CHECK(qAbs(input.x() - dropScene.x()) <= 1.0);
    CHECK(qAbs(input.y() - dropScene.y()) <= 1.0);

    // Node, layout position and connection are one edit.
    fixture.Workspace.Undo();
    CHECK(document.GetDraft(GetRoot()) == original);
    fixture.Workspace.Redo();
    CHECK(HasEdge(document, source, QStringLiteral("tonemap.color")) == true);
    CHECK(GetLayoutPosition(document, QStringLiteral("tonemap")).isNull() == false);
}

TEST_CASE("Clicking an output offers connected nodes to its right", "[editor][render-graph][quick-add]")
{
    QuickAddCanvasFixture fixture;
    RenderGraphCanvas& canvas = fixture.Workspace.GetCanvas();
    RenderGraphDocument& document = fixture.Workspace.GetDocument();
    RenderGraphQuickAdd& palette = *fixture.PaletteOrNull;
    const QString source = QStringLiteral("raster.color");
    const QPointF port = canvas.GetPortPosition(source, true);
    const QPoint point = canvas.mapFromScene(port);
    QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, point);
    REQUIRE(palette.isVisible() == true);
    CHECK(palette.GetPortFilter().Source == source);

    // An Upscale preset connects the wire instead of creating a helper target for that input.
    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    searchOrNull->setText(QStringLiteral("bilinear"));
    REQUIRE(palette.GetHighlightedKey() == QStringLiteral("node:Upscale/bilinear"));
    REQUIRE(palette.InsertHighlighted() == true);
    CHECK(HasEdge(document, source, QStringLiteral("bilinear.color")) == true);
    CHECK(HasNode(document, QStringLiteral("bilinear-color")) == false);
    CHECK(HasNode(document, QStringLiteral("bilinear-destination")) == true);
    const QPointF input = canvas.GetPortPosition(QStringLiteral("bilinear.color"), false);
    CHECK(input.x() > port.x());
    CHECK(qAbs(input.y() - port.y()) <= 1.0);
}

TEST_CASE("Port-filtered Quick Add never offers incompatible templates and cancels cleanly",
          "[editor][render-graph][quick-add]")
{
    QuickAddCanvasFixture fixture;
    RenderGraphCanvas& canvas = fixture.Workspace.GetCanvas();
    RenderGraphDocument& document = fixture.Workspace.GetDocument();
    RenderGraphQuickAdd& palette = *fixture.PaletteOrNull;
    const QByteArray original = document.GetDraft(GetRoot());
    const uint64 revision = document.GetRevision();
    const QString source = QStringLiteral("depth.texture");
    fixture.DragWire(source, fixture.FindEmptyPoint(canvas.mapFromScene(canvas.GetPortPosition(source, true))));
    REQUIRE(palette.isVisible() == true);
    CHECK(palette.GetPortFilter().Type == QStringLiteral("DepthTexture"));

    // ToneMap only takes color: it is not offered for depth, even by name, and nor are snippets.
    CHECK(palette.GetPortFilter().Inputs.contains(QStringLiteral("node:ToneMap")) == false);
    QLineEdit* searchOrNull = palette.findChild<QLineEdit*>(QStringLiteral("renderGraphQuickAddSearch"));
    REQUIRE(searchOrNull != nullptr);
    QTest::keyClicks(searchOrNull, QStringLiteral("tone"));
    CHECK(palette.GetListedKeys().contains(QStringLiteral("node:ToneMap")) == false);
    searchOrNull->setText(QStringLiteral("visibility"));
    for (const QString& key : palette.GetListedKeys())
    {
        CHECK(key.startsWith(QStringLiteral("snippet:")) == false);
    }

    // Escape closes the palette without touching the draft or its history.
    QTest::keyClick(searchOrNull, Qt::Key_Escape);
    CHECK(palette.isVisible() == false);
    CHECK(document.GetDraft(GetRoot()) == original);
    CHECK(document.GetRevision() == revision);

    // Releasing on a node body (not a port or empty canvas) opens nothing.
    const QPoint body = canvas.mapFromScene(canvas.GetNodePosition(QStringLiteral("scene")) + QPointF(60.0, 8.0));
    fixture.DragWire(source, body);
    CHECK(palette.isVisible() == false);
    CHECK(document.GetRevision() == revision);
}

TEST_CASE("AddNode connects the new node in the same edit", "[editor][render-graph][quick-add]")
{
    QTemporaryDir project;
    const QString package = MakeQuickAddTestPackage(project);
    RenderGraphDocument document;
    REQUIRE(document.Load(package, GetRoot()) == RenderGraphDocumentStatus::Ok);
    const QByteArray original = document.GetDraft(GetRoot());
    const QJsonObject node{{QStringLiteral("id"), QStringLiteral("present")},
                           {QStringLiteral("kind"), QStringLiteral("Present")}};
    const QString from = QStringLiteral("raster.color");

    // The shared pre-check sees the node as added: a matching type passes, the wrong type and a used
    // identity do not.
    CHECK(CheckRenderGraphLinkToNewNode(document,
                                        GetRoot(),
                                        QStringLiteral("Default"),
                                        node,
                                        from,
                                        QStringLiteral("present.color"))
              .IsLegal() == true);
    CHECK(CheckRenderGraphLinkToNewNode(document,
                                        GetRoot(),
                                        QStringLiteral("Default"),
                                        node,
                                        QStringLiteral("depth.texture"),
                                        QStringLiteral("present.color"))
              .Code == QStringLiteral("TypeMismatch"));
    const QJsonObject used{{QStringLiteral("id"), QStringLiteral("camera")},
                           {QStringLiteral("kind"), QStringLiteral("Present")}};
    CHECK(CheckRenderGraphLinkToNewNode(document,
                                        GetRoot(),
                                        QStringLiteral("Default"),
                                        used,
                                        from,
                                        QStringLiteral("camera.color"))
              .IsLegal() == false);

    // A connection whose destination is not on the new node is rejected and leaves the draft.
    const RenderGraphNodeConnection elsewhere{from, QStringLiteral("output.texture")};
    CHECK(document.AddNode(GetRoot(), QStringLiteral("Default"), node, nullptr, &elsewhere) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.GetDraft(GetRoot()) == original);

    const QPoint position(100, -20);
    const RenderGraphNodeConnection connection{from, QStringLiteral("present.color")};
    REQUIRE(document.AddNode(GetRoot(), QStringLiteral("Default"), node, &position, &connection) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(HasEdge(document, from, QStringLiteral("present.color")) == true);
    CHECK(GetLayoutPosition(document, QStringLiteral("present")) == position);
    REQUIRE(document.Undo() == true);
    CHECK(document.GetDraft(GetRoot()) == original);
}
