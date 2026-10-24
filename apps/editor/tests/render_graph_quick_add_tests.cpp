// The Quick Add palette: ranking, the bounded result list, keyboard use and canvas insertion.
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_document.h"
#include "internal/render_graph_quick_add.h"
#include "internal/render_graph_template_registry.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

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
