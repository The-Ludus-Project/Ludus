#include "internal/error_list.h"
#include "internal/preview_viewport.h"
#include "internal/render_graph_diagnostics.h"
#include "internal/render_graph_history.h"
#include "internal/render_graph_live_build.h"
#include "internal/render_graph_navigation.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <ludus/graphics/renderer/world_feature.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QGraphicsEllipseItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
QString GetRoot()
{
    return QStringLiteral("main.lurender.json");
}
QString GetChild()
{
    return QStringLiteral("child.lurender.json");
}
QString GetDefault()
{
    return QStringLiteral("Default");
}

void OpenWorkspace(RendererWorkspace& inoutWorkspace, const QTemporaryDir& project)
{
    const QByteArray root = QByteArrayLiteral(R"JSON({"version":2,"id":"Main","default":"Default","configurations":[{
      "id":"Default","inputs":[{"id":"color","type":"ColorTexture"}],
      "outputs":[{"id":"color","type":"ColorTexture","source":"left.color","required":false}],
      "nodes":[{"id":"first","kind":"ToneMap","exposure":1},
               {"id":"second","kind":"ToneMap","exposure":1},
               {"id":"left","kind":"Subgraph","asset":"child.lurender.json","configuration":"Default"}],
      "edges":[{"from":"input.color","to":"first.color"},{"from":"first.color","to":"second.color"},
               {"from":"second.color","to":"left.color"}]}]})JSON");
    const QByteArray child = QByteArrayLiteral(R"JSON({"version":2,"id":"Child","default":"Default","configurations":[{
      "id":"Default","inputs":[{"id":"color","type":"ColorTexture"}],
      "outputs":[{"id":"color","type":"ColorTexture","source":"tone.color","required":false}],
      "nodes":[{"id":"tone","kind":"ToneMap","exposure":1}],
      "edges":[{"from":"input.color","to":"tone.color"}]}]})JSON");
    const QStringList files{GetRoot(), GetChild(), QStringLiteral("test.slang")};
    const QVector<QByteArray> bytes{root, child, QByteArrayLiteral("first line\nsecond line\n")};
    for (qsizetype index = 0; index < files.size(); ++index)
    {
        QFile file(QDir(project.path()).filePath(files.at(index)));
        REQUIRE(file.open(QIODevice::WriteOnly) == true);
        REQUIRE(file.write(bytes.at(index)) == bytes.at(index).size());
    }
    inoutWorkspace.SetProject(project.path(), 1);
    REQUIRE(inoutWorkspace.OpenGraph(QDir(project.path()).filePath(GetRoot())) == true);
    inoutWorkspace.SetLivePreview(false);
    inoutWorkspace.resize(1450, 850);
    inoutWorkspace.show();
    inoutWorkspace.activateWindow();
    QCoreApplication::processEvents();
    inoutWorkspace.SetChromeVisible(false);
}

QJsonObject MakeLocation(const QString& node, const QString& file = GetRoot())
{
    return {{QStringLiteral("source"), file},
            {QStringLiteral("configuration"), GetDefault()},
            {QStringLiteral("node"), node}};
}
QJsonObject MakeIssue(const QString& code, const QJsonObject& location, bool isWarning = false)
{
    return {{QStringLiteral("code"), code},
            {QStringLiteral("message"), QStringLiteral("Repair the authored input")},
            {QStringLiteral("severity"), isWarning == true ? QStringLiteral("warning") : QStringLiteral("error")},
            {QStringLiteral("location"), location}};
}
QByteArray GetIdentity(RendererWorkspace& inoutWorkspace, RenderGraphSource source = RenderGraphSource::Draft)
{
    QComboBox* configurationOrNull = inoutWorkspace.findChild<QComboBox*>(QStringLiteral("renderGraphConfiguration"));
    REQUIRE(configurationOrNull != nullptr);
    return ComputeRenderGraphBuildIdentity(inoutWorkspace.GetDocument(), configurationOrNull->currentText(), source);
}
RenderGraphJobResult MakeResult(RendererWorkspace& inoutWorkspace, const QJsonArray& issues)
{
    RenderGraphJobResult result;
    result.Request.ProjectEpoch = 1;
    result.Request.Revision = inoutWorkspace.GetDocument().GetRevision();
    result.Request.InputIdentity = GetIdentity(inoutWorkspace);
    result.Request.Action = QStringLiteral("validate");
    result.Code = issues.first().toObject().value(QStringLiteral("code")).toString();
    result.Message = QStringLiteral("Validation found problems");
    result.Diagnostics = issues;
    return result;
}
RenderGraphViewState SetView(RenderGraphCanvas& inoutCanvas)
{
    REQUIRE(inoutCanvas.RestoreViewState({{QStringLiteral("first"), QStringLiteral("second")},
                                          QPointF(200, 170),
                                          0.7,
                                          RenderGraphTraceDirection::Upstream,
                                          {}}) == true);
    return inoutCanvas.CaptureViewState();
}
void CheckView(const RenderGraphCanvas& canvas, const RenderGraphViewState& expected)
{
    const RenderGraphViewState actual = canvas.CaptureViewState();
    CHECK(actual.Selection == expected.Selection);
    CHECK(actual.Trace == expected.Trace);
    CHECK(qAbs(actual.Zoom - expected.Zoom) < 0.000001);
    CHECK((actual.CentreScene - expected.CentreScene).manhattanLength() * expected.Zoom < 2);
}
QAction& GetAction(RenderGraphCanvas& inoutCanvas, const QString& name)
{
    QAction* actionOrNull = inoutCanvas.findChild<QAction*>(name);
    REQUIRE(actionOrNull != nullptr);
    return *actionOrNull;
}
} // namespace

TEST_CASE("canvas diagnostic counts exclude summaries and navigation wraps over available targets",
          "[editor][render-graph][diagnostic-navigation]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphDiagnostics& diagnostics = canvas.GetDiagnostics();
    CHECK(diagnostics.GetFreshness() == RenderGraphDiagnosticFreshness::None);
    CHECK(diagnostics.Next() == false);
    QJsonObject first = MakeLocation(QStringLiteral("first"));
    first.insert(QStringLiteral("port"), QStringLiteral("color"));
    QJsonObject source{{QStringLiteral("source"), QStringLiteral("test.slang")}, {QStringLiteral("line"), 2}};
    RenderGraphJobResult result =
        MakeResult(workspace,
                   {MakeIssue(QStringLiteral("A"), first),
                    MakeIssue(QStringLiteral("B"), MakeLocation(QStringLiteral("tone"), GetChild()), true),
                    MakeIssue(QStringLiteral("C"), MakeLocation(QStringLiteral("second"))),
                    MakeIssue(QStringLiteral("D"), MakeLocation(QStringLiteral("deleted"))),
                    MakeIssue(QStringLiteral("Shader"), source, true)});
    workspace.AcceptResult(result);
    CHECK(diagnostics.GetErrorCount() == 3);
    CHECK(diagnostics.GetWarningCount() == 2);
    CHECK(diagnostics.GetAvailableCount() == 3);
    CHECK(diagnostics.GetFreshness() == RenderGraphDiagnosticFreshness::Current);
    const uint64 revision = workspace.GetDocument().GetRevision();
    REQUIRE(diagnostics.Next() == true);
    CHECK(diagnostics.GetCurrentRow() == 0);
    CHECK(canvas.GetDiagnosticLocation() == first);
    REQUIRE(diagnostics.Next() == true);
    CHECK(diagnostics.GetCurrentRow() == 1);
    CHECK(canvas.GetFile() == GetChild());
    bool hasWarningLabel = false;
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        const QGraphicsSimpleTextItem* labelOrNull = qgraphicsitem_cast<QGraphicsSimpleTextItem*>(item);
        if (labelOrNull != nullptr && labelOrNull->text().startsWith(QStringLiteral("Warning:")) == true)
        {
            hasWarningLabel = true;
        }
    }
    CHECK(hasWarningLabel == true);
    REQUIRE(diagnostics.Next() == true);
    CHECK(diagnostics.GetCurrentRow() == 2);
    REQUIRE(diagnostics.Next() == true);
    CHECK(diagnostics.GetCurrentRow() == 0);
    REQUIRE(diagnostics.Previous() == true);
    CHECK(diagnostics.GetCurrentRow() == 2);
    CHECK(workspace.GetDocument().GetRevision() == revision);
    // Shader summary rows are not a second problem when compiler diagnostics follow them.
    result.Code = QStringLiteral("BuildSummary");
    workspace.AcceptResult(result);
    CHECK(workspace.GetDiagnosticCount() == 6);
    CHECK(diagnostics.GetErrorCount() == 3);
    CHECK(diagnostics.GetWarningCount() == 2);
    REQUIRE(diagnostics.Next() == true);
    CHECK(diagnostics.GetCurrentRow() == 1);
}

TEST_CASE("same-configuration diagnostic visits restore prior selection pan zoom and trace with Back",
          "[editor][render-graph][diagnostic-navigation]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState before = SetView(canvas);
    const QByteArray bytes = workspace.GetDocument().GetDraft(GetRoot());
    const uint64 revision = workspace.GetDocument().GetRevision();
    workspace.AcceptResult(
        MakeResult(workspace, {MakeIssue(QStringLiteral("A"), MakeLocation(QStringLiteral("left")), true)}));
    const int32 visits = canvas.GetHistory().GetEntryCount();
    REQUIRE(canvas.GetDiagnostics().Next() == true);
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("left")});
    CHECK(canvas.GetHistory().GetEntryCount() == visits + 1);
    REQUIRE(canvas.GetHistory().Back() == true);
    CheckView(canvas, before);
    REQUIRE(canvas.GetHistory().Forward() == true);
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("left")});
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == bytes);
    CHECK(workspace.GetDocument().GetRevision() == revision);
    const QString captures = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_DIAGNOSTICS_CAPTURE");
    if (captures.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(captures) == true);
        REQUIRE(canvas.GetDiagnostics().Next() == true);
        QCoreApplication::processEvents();
        REQUIRE(canvas.grab().save(QDir(captures).filePath(QStringLiteral("diagnostic-navigation.png"))) == true);
    }
}

TEST_CASE("nested diagnostic visits preserve same-configuration instance paths and cross-graph Back views",
          "[editor][render-graph][diagnostic-navigation]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState root = SetView(canvas);
    REQUIRE(canvas.ActivateNode(QStringLiteral("left")) == true);
    const QVector<RenderGraphHistoryLocation> path = canvas.GetHistory().GetPath();
    REQUIRE(path.size() == 2);
    REQUIRE(canvas.RestoreViewState(
                {{QStringLiteral("tone")}, QPointF(100, 80), 1.2, RenderGraphTraceDirection::Downstream, {}}) == true);
    const RenderGraphViewState child = canvas.CaptureViewState();
    workspace.AcceptResult(MakeResult(workspace,
                                      {MakeIssue(QStringLiteral("A"), MakeLocation(QStringLiteral("tone"), GetChild())),
                                       MakeIssue(QStringLiteral("B"), MakeLocation(QStringLiteral("second")))}));
    REQUIRE(canvas.GetDiagnostics().Next() == true);
    CHECK(canvas.GetHistory().GetPath().size() == 2);
    CHECK(canvas.GetHistory().GetPath().last().Instance == QStringLiteral("left"));
    REQUIRE(canvas.GetHistory().Back() == true);
    CheckView(canvas, child);
    REQUIRE(canvas.GetDiagnostics().Next() == true);
    CHECK(canvas.GetFile() == GetRoot());
    REQUIRE(canvas.GetHistory().Back() == true);
    CHECK(canvas.GetFile() == GetChild());
    CheckView(canvas, child);
    REQUIRE(canvas.GetHistory().Back() == true);
    CHECK(canvas.GetFile() == GetRoot());
    CheckView(canvas, root);
}

TEST_CASE("diagnostic freshness follows semantic edits undo saved inputs and result replacement",
          "[editor][render-graph][diagnostic-navigation]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphJobResult result =
        MakeResult(workspace, {MakeIssue(QStringLiteral("A"), MakeLocation(QStringLiteral("first")))});
    result.Request.Source = RenderGraphSource::Saved;
    workspace.AcceptResult(result);
    CHECK(canvas.GetDiagnostics().GetFreshness() == RenderGraphDiagnosticFreshness::Current);
    canvas.BeginDrag({QStringLiteral("first")});
    canvas.UpdateDrag(QPointF(30, 40));
    canvas.EndDrag();
    CHECK(canvas.GetDiagnostics().GetFreshness() == RenderGraphDiagnosticFreshness::Current);
    QJsonObject graph;
    REQUIRE(workspace.GetDocument().GetGraph(GetRoot(), graph) == true);
    QJsonObject node = graph.value(QStringLiteral("configurations"))
                           .toArray()
                           .first()
                           .toObject()
                           .value(QStringLiteral("nodes"))
                           .toArray()
                           .first()
                           .toObject();
    node.insert(QStringLiteral("exposure"), 2);
    REQUIRE(workspace.GetDocument().ReplaceNode(GetRoot(), GetDefault(), node) == RenderGraphDocumentStatus::Ok);
    REQUIRE(canvas.GetDiagnostics().Next() == true);
    CHECK(canvas.GetDiagnostics().GetFreshness() == RenderGraphDiagnosticFreshness::Stale);
    CHECK(canvas.accessibleDescription().contains(QStringLiteral("earlier source version")) == true);
    workspace.Undo();
    CHECK(canvas.GetDiagnostics().GetFreshness() == RenderGraphDiagnosticFreshness::Current);
    result.Request.InputIdentity.clear();
    workspace.AcceptResult(result);
    CHECK(canvas.GetDiagnostics().GetFreshness() == RenderGraphDiagnosticFreshness::Unverified);
    result.IsSuccessful = true;
    workspace.AcceptResult(result);
    CHECK(canvas.GetDiagnostics().GetErrorCount() == 0);
    CHECK(canvas.GetDiagnostics().Next() == false);
    CHECK(canvas.GetDiagnosticLocation().isEmpty() == true);
    result.IsSuccessful = false;
    result.IsCancelled = true;
    workspace.AcceptResult(result);
    CHECK(canvas.GetDiagnostics().GetWarningCount() == 0);
    CHECK(canvas.GetDiagnostics().GetAvailableCount() == 0);
}

TEST_CASE("deleted diagnostic targets are skipped and invalid Error List jumps preserve view and history",
          "[editor][render-graph][diagnostic-navigation]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    workspace.AcceptResult(MakeResult(workspace,
                                      {MakeIssue(QStringLiteral("A"), MakeLocation(QStringLiteral("first"))),
                                       MakeIssue(QStringLiteral("B"), MakeLocation(QStringLiteral("second"))),
                                       MakeIssue(QStringLiteral("C"), MakeLocation(QStringLiteral("left")))}));
    REQUIRE(canvas.GetDiagnostics().Next() == true);
    REQUIRE(canvas.GetDiagnostics().Next() == true);
    REQUIRE(workspace.GetDocument().RemoveNode(GetRoot(), GetDefault(), QStringLiteral("second")) ==
            RenderGraphDocumentStatus::Ok);
    REQUIRE(canvas.GetDiagnostics().Next() == true);
    CHECK(canvas.GetDiagnostics().GetCurrentRow() == 2);
    CHECK(canvas.GetDiagnostics().GetErrorCount() == 3);
    CHECK(canvas.GetDiagnostics().GetAvailableCount() == 2);
    const RenderGraphViewState before = canvas.CaptureViewState();
    const int32 visits = canvas.GetHistory().GetEntryCount();
    CHECK(workspace.ActivateDiagnostic(1) == false);
    CHECK(canvas.GetHistory().GetEntryCount() == visits);
    CheckView(canvas, before);
    QTemporaryDir replacement;
    workspace.SetProject(replacement.path(), 2);
    CHECK(canvas.GetDiagnostics().GetFreshness() == RenderGraphDiagnosticFreshness::None);
    CHECK(canvas.GetDiagnostics().Next() == false);
}

TEST_CASE("canvas F8 shortcuts reveal field controls without stealing source or modal editing",
          "[editor][render-graph][diagnostic-navigation]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    QJsonObject field = MakeLocation(QStringLiteral("first"));
    field.insert(QStringLiteral("field"), QStringLiteral("exposure"));
    workspace.AcceptResult(MakeResult(workspace,
                                      {MakeIssue(QStringLiteral("A"), field),
                                       MakeIssue(QStringLiteral("B"), MakeLocation(QStringLiteral("second")), true)}));
    QAction& next = GetAction(canvas, QStringLiteral("renderGraphNextDiagnostic"));
    CHECK(next.shortcutContext() == Qt::WidgetWithChildrenShortcut);
    bool hasField = false;
    canvas.setFocus();
    QCoreApplication::processEvents();
    QTimer::singleShot(0, &workspace, [&hasField, &canvas]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        for (QWidget* control : dialogOrNull->findChildren<QWidget*>())
        {
            hasField = hasField == true || control->property("isRenderGraphDiagnosticTarget").toBool() == true;
        }
        QTest::keyClick(dialogOrNull, Qt::Key_F8);
        CHECK(canvas.GetDiagnostics().GetCurrentRow() == 0);
        dialogOrNull->reject();
    });
    QTest::keyClick(&canvas, Qt::Key_F8);
    CHECK(hasField == true);
    CHECK(canvas.GetDiagnostics().GetCurrentRow() == 0);
    workspace.activateWindow();
    canvas.setFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(&canvas, Qt::Key_F8);
    CHECK(canvas.GetDiagnostics().GetCurrentRow() == 1);
    QPlainTextEdit* sourceOrNull = workspace.findChild<QPlainTextEdit*>(QStringLiteral("renderGraphSource"));
    REQUIRE(sourceOrNull != nullptr);
    QTabWidget* viewsOrNull = workspace.findChild<QTabWidget*>(QStringLiteral("renderGraphViews"));
    REQUIRE(viewsOrNull != nullptr);
    viewsOrNull->setCurrentIndex(1);
    sourceOrNull->setFocus();
    QCoreApplication::processEvents();
    QTimer::singleShot(0, &workspace, []() {
        QDialog* unexpectedOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (unexpectedOrNull != nullptr)
        {
            unexpectedOrNull->reject();
        }
    });
    QTest::keyClick(sourceOrNull, Qt::Key_F8);
    CHECK(canvas.GetDiagnostics().GetCurrentRow() == 1);
    CHECK(GetAction(canvas, QStringLiteral("renderGraphPreviousDiagnostic")).shortcut() ==
          QKeySequence(Qt::SHIFT | Qt::Key_F8));
}

TEST_CASE("the World features table names each feature's scene function and the feature warning reveals it",
          "[editor][render-graph][diagnostic-navigation][world-features]")
{
    namespace renderer = ludus::graphics::renderer;
    RendererWorkspace workspace({});
    QTableWidget* tableOrNull = workspace.findChild<QTableWidget*>(QStringLiteral("renderGraphWorldFeatures"));
    REQUIRE(tableOrNull != nullptr);
    REQUIRE(tableOrNull->rowCount() == static_cast<int>(renderer::WORLD_FEATURE_COUNT));
    for (usize index = 0; index < renderer::WORLD_FEATURE_COUNT; ++index)
    {
        const int row = static_cast<int>(index);
        const renderer::WorldFeatureDescription description =
            renderer::DescribeWorldFeature(static_cast<renderer::WorldFeature>(index));
        REQUIRE(tableOrNull->item(row, 0) != nullptr);
        CHECK(tableOrNull->item(row, 0)->text() == QString::fromUtf8(description.Name));
        // Without a drawing graph the table says so instead of guessing a support level.
        CHECK(tableOrNull->item(row, 1)->text() == QStringLiteral("No drawing graph"));
        CHECK(tableOrNull->item(row, 2)->text() == QString::fromUtf8(description.Function));
        CHECK(tableOrNull->item(row, 3)->text() == QString::fromUtf8(description.Callback));
    }
    // Activating the warning lands on the table; with no missing feature the first row is selected.
    workspace.RevealWorldFeatures();
    CHECK(tableOrNull->currentRow() == 0);
    CHECK(tableOrNull->selectionModel()->isRowSelected(0, {}) == true);
    // The current row's function and where to call it read under the list.
    QLabel* detailOrNull = workspace.findChild<QLabel*>(QStringLiteral("renderGraphWorldFeatureDetail"));
    REQUIRE(detailOrNull != nullptr);
    const renderer::WorldFeatureDescription first =
        renderer::DescribeWorldFeature(renderer::WorldFeature::AtmosphereSky);
    CHECK(detailOrNull->text().contains(QString::fromUtf8(first.Function)) == true);
    CHECK(detailOrNull->text().contains(QString::fromUtf8(first.Callback)) == true);
    tableOrNull->setCurrentCell(1, 0);
    CHECK(detailOrNull->text().contains(QString::fromUtf8(
              renderer::DescribeWorldFeature(renderer::WorldFeature::AerialPerspective).Function)) == true);
    const QList<ErrorListEntry> rows =
        MakeRenderGraphFeatureWarningEntries(QStringLiteral("World features not rendered: Atmosphere sky."),
                                             QStringLiteral("main.lurender.json"));
    REQUIRE(rows.size() == 1);
    CHECK(rows.first().Code == QString::fromLatin1(renderer::WORLD_FEATURE_WARNING_CODE));
}

TEST_CASE("Render Preview fits a narrow dock: a compact World features list and no empty preview box",
          "[editor][render-graph][diagnostic-navigation][world-features][layout]")
{
    namespace renderer = ludus::graphics::renderer;
    RendererWorkspace workspace({});
    QWidget* panelOrNull = workspace.findChild<QWidget*>(QStringLiteral("renderGraphPreviewPanel"));
    QTableWidget* tableOrNull = workspace.findChild<QTableWidget*>(QStringLiteral("renderGraphWorldFeatures"));
    QWidget* imageOrNull = workspace.findChild<QWidget*>(QStringLiteral("renderGraphPreviewImage"));
    QLabel* detailOrNull = workspace.findChild<QLabel*>(QStringLiteral("renderGraphWorldFeatureDetail"));
    REQUIRE(panelOrNull != nullptr);
    REQUIRE(tableOrNull != nullptr);
    REQUIRE(imageOrNull != nullptr);
    REQUIRE(detailOrNull != nullptr);
    // The Render Preview tool window hosts the panel; a MacBook Air's default left dock is about this wide.
    QWidget dock;
    QVBoxLayout* layout = new QVBoxLayout(&dock);
    layout->setContentsMargins(0, 0, 0, 0);
    panelOrNull->setParent(&dock);
    layout->addWidget(panelOrNull);
    panelOrNull->show();
    constexpr int DOCK_WIDTH = 240;
    constexpr int DOCK_HEIGHT = 640;
    dock.resize(DOCK_WIDTH, DOCK_HEIGHT);
    dock.show();
    QApplication::processEvents();
    // Two columns: feature and support. The long function and callback text stay in the model only.
    CHECK(tableOrNull->isColumnHidden(0) == false);
    CHECK(tableOrNull->isColumnHidden(1) == false);
    CHECK(tableOrNull->isColumnHidden(2) == true);
    CHECK(tableOrNull->isColumnHidden(3) == true);
    // It fits the dock: no sideways scrolling, every row one line, and the whole list shown without scrolling.
    CHECK(tableOrNull->horizontalScrollBar()->isVisible() == false);
    CHECK(tableOrNull->width() <= dock.width());
    const int lineHeight = tableOrNull->fontMetrics().height();
    for (int row = 0; row < tableOrNull->rowCount(); ++row)
    {
        CHECK(tableOrNull->rowHeight(row) < 2 * lineHeight);
    }
    CHECK(tableOrNull->verticalScrollBar()->maximum() == 0);
    CHECK(detailOrNull->isVisible() == true);
    CHECK(detailOrNull->width() <= dock.width());
    // Without a frame the preview is one caption line, not a black box taking the dock's height.
    CHECK(imageOrNull->isVisible() == true);
    CHECK(imageOrNull->height() < 2 * lineHeight);
    CHECK(tableOrNull->mapTo(&dock, QPoint(0, 0)).y() < DOCK_HEIGHT / 2);
}

TEST_CASE("a link to the World features warning selects its Error List row",
          "[editor][render-graph][diagnostic-navigation][world-features]")
{
    namespace renderer = ludus::graphics::renderer;
    ErrorList list;
    QTableWidget* tableOrNull = list.findChild<QTableWidget*>();
    REQUIRE(tableOrNull != nullptr);
    ErrorListEntry error;
    error.Severity = ErrorListSeverity::Error;
    error.Producer = ErrorListProducer::Build;
    error.Code = QStringLiteral("BuildError");
    error.Description = QStringLiteral("main.cpp:1: error: broken");
    list.ReplaceEntries(ErrorListProducer::Build, {error});
    list.ReplaceEntries(
        ErrorListProducer::RenderPreview,
        MakeRenderGraphFeatureWarningEntries(QStringLiteral("World features not rendered: Aerial perspective."),
                                             QStringLiteral("main.lurender.json")));
    const QString code = QString::fromLatin1(renderer::WORLD_FEATURE_WARNING_CODE);
    CHECK(list.SelectEntry(QStringLiteral("NoSuchCode")) == false);
    REQUIRE(list.SelectEntry(code) == true);
    const auto selectedCode = [&list, tableOrNull]() {
        const QModelIndexList rows = tableOrNull->selectionModel()->selectedRows();
        REQUIRE(rows.size() == 1);
        const int entry = tableOrNull->item(rows.first().row(), 0)->data(Qt::UserRole).toInt();
        return list.GetEntries().at(entry).Code;
    };
    CHECK(selectedCode() == code);
    // A filtered-out warning is shown again rather than missed.
    QToolButton* warningsOrNull = list.findChild<QToolButton*>(QStringLiteral("errorListWarnings"));
    REQUIRE(warningsOrNull != nullptr);
    warningsOrNull->setChecked(false);
    REQUIRE(list.SelectEntry(code) == true);
    CHECK(warningsOrNull->isChecked() == true);
    CHECK(selectedCode() == code);
}

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
TEST_CASE("Viewport renderer recovery opens the exact current shader or graph node",
          "[editor][render-graph][diagnostic-navigation][viewport-recovery]")
{
    QTemporaryDir project;
    QTemporaryDir tooling;
    REQUIRE(project.isValid() == true);
    REQUIRE(tooling.isValid() == true);
    const QString package = QDir(tooling.path()).filePath(QStringLiteral("scripts/python/ludus_tools"));
    REQUIRE(QDir().mkpath(package) == true);
    const auto writeFile = [](const QString& path, const QByteArray& bytes) {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
        REQUIRE(file.write(bytes) == bytes.size());
    };
    writeFile(QDir(package).filePath(QStringLiteral("render_graph.py")), {});
    writeFile(QDir(project.path()).filePath(QStringLiteral("main.lushader.json")),
              QByteArrayLiteral(R"JSON({"version":1,"programs":[]})JSON"));
    RendererWorkspace workspace({QString::fromUtf8(LUDUS_TEST_PYTHON), {}, tooling.path()});
    OpenWorkspace(workspace, project);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(800, 600);
    viewport.UseProjectGraph(workspace);
    viewport.show();

    QJsonObject location;
    SECTION("Shader diagnostic")
    {
        location = {{QStringLiteral("source"), QStringLiteral("test.slang")},
                    {QStringLiteral("line"), 2},
                    {QStringLiteral("column"), 3}};
    }
    SECTION("Node diagnostic in an included graph")
    {
        location = MakeLocation(QStringLiteral("tone"), GetChild());
    }
    REQUIRE(location.isEmpty() == false);
    const QJsonObject error{
        {QStringLiteral("code"), QStringLiteral("RenderGraphBuildShader")},
        {QStringLiteral("message"), QStringLiteral("Preview build failed")},
        {QStringLiteral("diagnostics"), QJsonArray{MakeIssue(QStringLiteral("TestError"), location)}}};
    const QJsonObject report{{QStringLiteral("schema"), 1}, {QStringLiteral("error"), error}};
    const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Compact);
    writeFile(QDir(package).filePath(QStringLiteral("__main__.py")),
              QByteArrayLiteral("print(r'''") + json + QByteArrayLiteral("''')\n"));
    QSignalSpy finished(&workspace, &RendererWorkspace::JobFinished);
    INFO(workspace.GetStatusText().toStdString());
    REQUIRE(workspace.BuildAndPreview() == true);
    REQUIRE(finished.wait() == true);
    QPushButton* primaryOrNull = viewport.findChild<QPushButton*>(QStringLiteral("viewportRenderRecoveryPrimary"));
    REQUIRE(primaryOrNull != nullptr);
    REQUIRE(primaryOrNull->isVisible() == true);
    QWidget* graphRecoveryOrNull = workspace.findChild<QWidget*>(QStringLiteral("renderGraphPreviewRecovery"));
    REQUIRE(graphRecoveryOrNull != nullptr);
    QPushButton* graphActionOrNull =
        graphRecoveryOrNull->findChild<QPushButton*>(QStringLiteral("viewportRenderRecoveryPrimary"));
    REQUIRE(graphActionOrNull != nullptr);
    REQUIRE(graphActionOrNull->isVisible() == true);
    CHECK(graphActionOrNull->text() == primaryOrNull->text());
    if (location.contains(QStringLiteral("line")) == true)
    {
        REQUIRE(primaryOrNull->text() == QStringLiteral("Open shader"));
        primaryOrNull->click();
        CHECK(workspace.GetSourceFile() == QStringLiteral("test.slang"));
        CHECK(workspace.GetSourceLine() == 2);
        CHECK(workspace.GetSourceColumn() == 3);
    }
    else
    {
        REQUIRE(graphActionOrNull->text() == QStringLiteral("Show graph node"));
        graphActionOrNull->click();
        CHECK(workspace.GetCanvas().GetFile() == GetChild());
        CHECK(workspace.GetCanvas().GetSelectedNodes() == QStringList{QStringLiteral("tone")});
    }
}
#endif
