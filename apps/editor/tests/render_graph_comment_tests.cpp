#include "internal/render_graph_comments.h"
#include "internal/render_graph_diagnostics.h"
#include "internal/render_graph_history.h"
#include "internal/render_graph_live_build.h"
#include "internal/render_graph_navigation.h"
#include "internal/render_graph_reroutes.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <limits>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
QString GetRoot()
{
    return QStringLiteral("main.lurender.json");
}
QString GetDefault()
{
    return QStringLiteral("Default");
}

QJsonObject MakeFrame(const QString& identity = QStringLiteral("stage"))
{
    QJsonObject frame{{QStringLiteral("id"), identity},
                      {QStringLiteral("title"), QStringLiteral("Visibility")},
                      {QStringLiteral("notes"), QStringLiteral("<b>Plain text</b>\nG-buffer stage")},
                      {QStringLiteral("colour"), QStringLiteral("#547FA3")}};
    return SetRenderGraphCommentBounds(frame, QRectF(-40, -110, 600, 400));
}
QJsonArray GetFrames(const RenderGraphDocument& document)
{
    return QJsonDocument::fromJson(document.GetDraft(GetRoot()))
        .object()
        .value(QStringLiteral("configurations"))
        .toArray()
        .first()
        .toObject()
        .value(QStringLiteral("layout"))
        .toObject()
        .value(QStringLiteral("frames"))
        .toArray();
}
void OpenWorkspace(RendererWorkspace& inoutWorkspace, const QTemporaryDir& project)
{
    const QByteArray bytes = QByteArrayLiteral(R"JSON({"version":2,"id":"Main","default":"Default","configurations":[{
      "id":"Default","inputs":[{"id":"color","type":"ColorTexture"}],
      "outputs":[{"id":"color","type":"ColorTexture","source":"third.color","required":false}],
      "nodes":[{"id":"first","kind":"ToneMap","exposure":1},
               {"id":"second","kind":"ToneMap","exposure":1},{"id":"third","kind":"ToneMap","exposure":1}],
      "edges":[{"from":"input.color","to":"first.color"},{"from":"first.color","to":"second.color"},
               {"from":"second.color","to":"third.color"}],
      "layout":{"positions":[{"node":"first","x":0,"y":0},{"node":"second","x":300,"y":50},
                                 {"node":"third","x":900,"y":500}]}}]})JSON");
    QFile file(QDir(project.path()).filePath(GetRoot()));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    file.close();
    inoutWorkspace.SetProject(project.path(), 1);
    REQUIRE(inoutWorkspace.OpenGraph(file.fileName()) == true);
    inoutWorkspace.SetLivePreview(false);
    inoutWorkspace.resize(1200, 850);
    inoutWorkspace.show();
    QCoreApplication::processEvents();
    inoutWorkspace.SetChromeVisible(false);
}
void SeedFrame(RendererWorkspace& inoutWorkspace)
{
    REQUIRE(inoutWorkspace.GetDocument().ReplaceCommentFrames(GetRoot(), GetDefault(), {MakeFrame()}) ==
            RenderGraphDocumentStatus::Ok);
    inoutWorkspace.GetCanvas().Rebuild();
}
RenderGraphViewState SelectTrace(RenderGraphCanvas& inoutCanvas)
{
    REQUIRE(inoutCanvas.RestoreViewState({{QStringLiteral("first"), QStringLiteral("third")},
                                          QPointF(400, 200),
                                          0.8,
                                          RenderGraphTraceDirection::Downstream,
                                          {}}) == true);
    return inoutCanvas.CaptureViewState();
}
void CheckSelectionTrace(const RenderGraphCanvas& canvas, const RenderGraphViewState& expected)
{
    CHECK(canvas.GetSelectedNodes() == expected.Selection);
    CHECK(canvas.CaptureViewState().Trace == expected.Trace);
    const RenderGraphNavigation* navigationOrNull = canvas.findChild<RenderGraphNavigation*>();
    REQUIRE(navigationOrNull != nullptr);
    CHECK(navigationOrNull->GetTrace().Nodes.contains(QStringLiteral("second")) == true);
}
void ScheduleDialog(bool shouldAccept, const QString& title = QStringLiteral("Lighting"))
{
    QTimer::singleShot(0, [shouldAccept, title]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        REQUIRE(dialogOrNull->objectName() == QStringLiteral("renderGraphCommentDialog"));
        dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphCommentTitle"))->setText(title);
        dialogOrNull->findChild<QPlainTextEdit*>(QStringLiteral("renderGraphCommentNotes"))
            ->setPlainText(QStringLiteral("Authored notes\n<b>literal</b>"));
        dialogOrNull->findChild<QLineEdit*>(QStringLiteral("renderGraphCommentColour"))
            ->setText(QStringLiteral("#A36F54"));
        QDialogButtonBox* buttonsOrNull = dialogOrNull->findChild<QDialogButtonBox*>();
        REQUIRE(buttonsOrNull != nullptr);
        buttonsOrNull->button(shouldAccept == true ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)->click();
    });
}
} // namespace

TEST_CASE("comment metadata validates bounds, Unicode text, colours and complete arrays",
          "[editor][render-graph][comments]")
{
    CHECK(AreRenderGraphCommentFramesValid({MakeFrame()}) == true);
    QJsonObject unicode = MakeFrame();
    const QString emoji = QString::fromUtf8("🌟");
    unicode.insert(QStringLiteral("title"), emoji.repeated(layout_policy::MAX_COMMENT_TITLE_CODEPOINTS));
    CHECK(AreRenderGraphCommentFramesValid({unicode}) == true);
    unicode.insert(QStringLiteral("title"), emoji.repeated(layout_policy::MAX_COMMENT_TITLE_CODEPOINTS + 1));
    CHECK(AreRenderGraphCommentFramesValid({unicode}) == false);
    for (const QString& field : MakeFrame().keys())
    {
        QJsonObject frame = MakeFrame();
        frame.remove(field);
        CHECK(AreRenderGraphCommentFramesValid({frame}) == false);
    }
    CHECK(AreRenderGraphCommentFramesValid({MakeFrame(), MakeFrame()}) == false);
    const QVector<QJsonObject> invalid{
        {{QStringLiteral("title"), QStringLiteral(" \n ")}},
        {{QStringLiteral("title"), QStringLiteral("first\nsecond")}},
        {{QStringLiteral("notes"), QString(layout_policy::MAX_COMMENT_NOTES_CODEPOINTS + 1, QLatin1Char('a'))}},
        {{QStringLiteral("colour"), QStringLiteral("red")}},
        {{QStringLiteral("colour"), QStringLiteral("#FF000080")}},
        {{QStringLiteral("x"), 0.5}},
        {{QStringLiteral("x"), true}},
        {{QStringLiteral("width"), layout_policy::MIN_COMMENT_WIDTH - 1}},
        {{QStringLiteral("height"), layout_policy::MIN_COMMENT_HEIGHT - 1}},
        {{QStringLiteral("x"), layout_policy::MAX_COORDINATE}},
        {{QStringLiteral("id"), QStringLiteral("bad.id")}},
        {{QStringLiteral("unknown"), 1}}};
    for (const QJsonObject& change : invalid)
    {
        QJsonObject frame = MakeFrame();
        for (QJsonObject::const_iterator item = change.constBegin(); item != change.constEnd(); ++item)
        {
            frame.insert(item.key(), item.value());
        }
        CHECK(AreRenderGraphCommentFramesValid({frame}) == false);
    }
    QJsonArray frames;
    for (int32 index = 0; index < layout_policy::MAX_COMMENT_FRAMES; ++index)
    {
        frames.append(MakeFrame(QStringLiteral("stage%1").arg(index)));
    }
    CHECK(AreRenderGraphCommentFramesValid(frames) == true);
    frames.append(MakeFrame(QStringLiteral("excess")));
    CHECK(AreRenderGraphCommentFramesValid(frames) == false);
}

TEST_CASE("comment metadata and grouped positions publish atomically and save without a build identity change",
          "[editor][render-graph][comments]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphDocument& document = workspace.GetDocument();
    const QByteArray before = document.GetDraft(GetRoot());
    const QByteArray identity = ComputeRenderGraphBuildIdentity(document, GetDefault(), RenderGraphSource::Draft);
    const uint64 revision = document.GetRevision();
    CHECK(document.ReplaceCommentFrames(GetRoot(), GetDefault(), {MakeFrame()}, {{QStringLiteral("missing"), 1, 2}}) ==
          RenderGraphDocumentStatus::InvalidEdit);
    CHECK(document.GetDraft(GetRoot()) == before);
    CHECK(document.GetRevision() == revision);
    REQUIRE(document.ReplaceCommentFrames(GetRoot(),
                                          GetDefault(),
                                          {MakeFrame()},
                                          {{QStringLiteral("first"), 20, 30}, {QStringLiteral("second"), 320, 80}}) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(document.GetRevision() == revision + 1);
    CHECK(ComputeRenderGraphBuildIdentity(document, GetDefault(), RenderGraphSource::Draft) == identity);
    const QByteArray edited = document.GetDraft(GetRoot());
    workspace.Undo();
    CHECK(document.GetDraft(GetRoot()) == before);
    workspace.Redo();
    CHECK(document.GetDraft(GetRoot()) == edited);
    QStringList saved;
    REQUIRE(document.Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(project.path(), GetRoot()) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(GetRoot()) == edited);
    CHECK(GetFrames(reopened) == QJsonArray{MakeFrame()});
}

TEST_CASE("comment header dragging moves enclosed nodes in one undo step while preserving selected trace roots",
          "[editor][render-graph][comments]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphComments& comments = canvas.GetComments();
    const RenderGraphViewState selected = SelectTrace(canvas);
    const QByteArray before = workspace.GetDocument().GetDraft(GetRoot());
    const uint64 revision = workspace.GetDocument().GetRevision();
    QSignalSpy requests(&comments, &RenderGraphComments::EditRequested);
    const QPointF press(-20, -95);
    REQUIRE(comments.BeginGesture(press) == true);
    REQUIRE(comments.UpdateGesture(press + QPointF(70, 90)) == true);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == QPointF(70, 90));
    CHECK(canvas.GetNodePosition(QStringLiteral("second")) == QPointF(370, 140));
    CHECK(canvas.GetNodePosition(QStringLiteral("third")) == QPointF(900, 500));
    CHECK(workspace.GetDocument().GetRevision() == revision);
    CheckSelectionTrace(canvas, selected);
    REQUIRE(comments.EndGesture() == true);
    CHECK(requests.count() == 1);
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    CHECK(GetRenderGraphCommentBounds(GetFrames(workspace.GetDocument()).first().toObject()) ==
          QRectF(30, -20, 600, 400));
    CheckSelectionTrace(canvas, selected);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
    CheckSelectionTrace(canvas, selected);
    workspace.Redo();
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == QPointF(70, 90));
    CheckSelectionTrace(canvas, selected);
    const uint64 beforeNoop = workspace.GetDocument().GetRevision();
    REQUIRE(comments.BeginGesture(press + QPointF(70, 90)) == true);
    REQUIRE(comments.UpdateGesture(press + QPointF(170, 190)) == true);
    REQUIRE(comments.CancelGesture() == true);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == QPointF(70, 90));
    CHECK(workspace.GetDocument().GetRevision() == beforeNoop);
    REQUIRE(comments.BeginGesture(press + QPointF(70, 90)) == true);
    REQUIRE(comments.EndGesture() == true);
    CHECK(workspace.GetDocument().GetRevision() == beforeNoop);
}

TEST_CASE("comment resizing changes only grouping bounds and clamps gestures rigidly",
          "[editor][render-graph][comments]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    RenderGraphComments& comments = canvas.GetComments();
    const RenderGraphViewState selected = SelectTrace(canvas);
    const QPointF resize(550, 280);
    REQUIRE(comments.BeginGesture(resize) == true);
    REQUIRE(comments.UpdateGesture(QPointF(std::numeric_limits<float64>::quiet_NaN(), 0)) == true);
    REQUIRE(comments.UpdateGesture(QPointF(0, std::numeric_limits<float64>::infinity())) == true);
    CHECK(comments.GetFrameBounds(QStringLiteral("stage")) == GetRenderGraphCommentBounds(MakeFrame()));
    REQUIRE(comments.UpdateGesture(resize + QPointF(600, 600)) == true);
    REQUIRE(comments.EndGesture() == true);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == QPointF(0, 0));
    CHECK(canvas.GetNodePosition(QStringLiteral("third")) == QPointF(900, 500));
    CHECK(comments.GetFrameBounds(QStringLiteral("stage")).size() == QSizeF(1200, 1000));
    // The larger frame now fully encloses the third node, independent of its selected state.
    REQUIRE(comments.BeginGesture(QPointF(-20, -95)) == true);
    REQUIRE(comments.UpdateGesture(QPointF(1e100, 1e100)) == true);
    REQUIRE(comments.EndGesture() == true);
    const QRectF moved = comments.GetFrameBounds(QStringLiteral("stage"));
    CHECK(moved.right() == layout_policy::MAX_COORDINATE);
    CHECK(moved.bottom() == layout_policy::MAX_COORDINATE);
    CHECK(canvas.GetNodePosition(QStringLiteral("third")) - canvas.GetNodePosition(QStringLiteral("first")) ==
          QPointF(900, 500));
    CheckSelectionTrace(canvas, selected);
    workspace.Undo();
    workspace.Undo();
    CHECK(comments.GetFrameBounds(QStringLiteral("stage")) == GetRenderGraphCommentBounds(MakeFrame()));
}

TEST_CASE("comment canvas controls edit plain text metadata with Properties hidden and cancellation preserves history",
          "[editor][render-graph][comments]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState selected = SelectTrace(canvas);
    const uint64 revision = workspace.GetDocument().GetRevision();
    ScheduleDialog(false);
    canvas.GetComments().CreateFrame(QPointF(0, 0));
    CHECK(workspace.GetDocument().GetRevision() == revision);
    QTimer::singleShot(0, &workspace, [&workspace]() {
        QMenu* menuOrNull = workspace.findChild<QMenu*>(QStringLiteral("renderGraphBackgroundMenu"));
        REQUIRE(menuOrNull != nullptr);
        QAction* actionOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphAddComment"));
        REQUIRE(actionOrNull != nullptr);
        menuOrNull->close();
        ScheduleDialog(true, QStringLiteral("Post-processing"));
        actionOrNull->trigger();
    });
    const QPoint point = canvas.mapFromScene(QPointF(-500, -500));
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    REQUIRE(GetFrames(workspace.GetDocument()).size() == 1);
    QJsonObject frame = GetFrames(workspace.GetDocument()).first().toObject();
    CHECK(frame.value(QStringLiteral("title")).toString() == QStringLiteral("Post-processing"));
    CHECK(frame.value(QStringLiteral("notes")).toString() == QStringLiteral("Authored notes\n<b>literal</b>"));
    CHECK(frame.value(QStringLiteral("colour")).toString() == QStringLiteral("#A36F54"));
    CheckSelectionTrace(canvas, selected);
    ScheduleDialog(true, QStringLiteral("Lighting"));
    canvas.GetComments().EditFrame(frame.value(QStringLiteral("id")).toString());
    CHECK(GetFrames(workspace.GetDocument()).first().toObject().value(QStringLiteral("title")).toString() ==
          QStringLiteral("Lighting"));
    const QString captures = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_COMMENTS_CAPTURE");
    if (captures.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(captures) == true);
        canvas.FitGraph();
        REQUIRE(canvas.grab().save(QDir(captures).filePath(QStringLiteral("comment-frame.png"))) == true);
    }
    const QPointF first = canvas.GetNodePosition(QStringLiteral("first"));
    canvas.GetComments().RemoveFrame(frame.value(QStringLiteral("id")).toString());
    CHECK(GetFrames(workspace.GetDocument()).isEmpty() == true);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == first);
    CheckSelectionTrace(canvas, selected);
    workspace.Undo();
    CHECK(GetFrames(workspace.GetDocument()).size() == 1);
}

TEST_CASE("comment gestures cancel on Escape and configuration changes without clearing dependency tracing",
          "[editor][render-graph][comments]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState selected = SelectTrace(canvas);
    const uint64 revision = workspace.GetDocument().GetRevision();
    canvas.FitGraph();
    const QPoint press = canvas.mapFromScene(QPointF(-20, -95));
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, press);
    QTest::mouseMove(canvas.viewport(), press + QPoint(40, 40));
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) != QPointF(0, 0));
    QTest::keyClick(&canvas, Qt::Key_Escape);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == QPointF(0, 0));
    CHECK(workspace.GetDocument().GetRevision() == revision);
    CheckSelectionTrace(canvas, selected);
    REQUIRE(canvas.GetComments().BeginGesture(QPointF(-20, -95)) == true);
    REQUIRE(canvas.GetComments().UpdateGesture(QPointF(40, 40)) == true);
    canvas.ShowConfiguration({}, {});
    CHECK(canvas.GetComments().EndGesture() == false);
    CHECK(workspace.GetDocument().GetRevision() == revision);
    canvas.ShowConfiguration(GetRoot(), GetDefault());
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == QPointF(0, 0));
}

TEST_CASE("stale comment dialogs and malformed source metadata preserve authored state",
          "[editor][render-graph][comments]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState selected = SelectTrace(canvas);
    const QJsonArray original = GetFrames(workspace.GetDocument());
    QTimer::singleShot(0, &workspace, [&workspace]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        REQUIRE(workspace.GetDocument().MoveNodes(GetRoot(), GetDefault(), {{QStringLiteral("third"), 1000, 500}}) ==
                RenderGraphDocumentStatus::Ok);
        QDialogButtonBox* buttonsOrNull = dialogOrNull->findChild<QDialogButtonBox*>();
        REQUIRE(buttonsOrNull != nullptr);
        buttonsOrNull->button(QDialogButtonBox::Ok)->click();
        CHECK(dialogOrNull->isVisible() == true);
        CHECK(dialogOrNull->findChild<QLabel*>(QStringLiteral("renderGraphCommentFeedback"))
                  ->text()
                  .contains(QStringLiteral("graph changed")) == true);
        dialogOrNull->reject();
    });
    canvas.GetComments().EditFrame(QStringLiteral("stage"));
    CHECK(GetFrames(workspace.GetDocument()) == original);
    canvas.Rebuild();
    CheckSelectionTrace(canvas, selected);
    QJsonObject graph = QJsonDocument::fromJson(workspace.GetDocument().GetDraft(GetRoot())).object();
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject configuration = configurations.first().toObject();
    QJsonObject layout = configuration.value(QStringLiteral("layout")).toObject();
    layout.insert(QStringLiteral("frames"), QStringLiteral("invalid"));
    configuration.insert(QStringLiteral("layout"), layout);
    configurations.replace(0, configuration);
    graph.insert(QStringLiteral("configurations"), configurations);
    REQUIRE(workspace.GetDocument().SetText(GetRoot(), EncodeCanonicalJson(graph)) == RenderGraphDocumentStatus::Ok);
    canvas.Rebuild();
    const QByteArray malformed = workspace.GetDocument().GetDraft(GetRoot());
    QSignalSpy rejected(&canvas.GetComments(), &RenderGraphComments::Rejected);
    canvas.GetComments().CreateFrame(QPointF(0, 0));
    CHECK(rejected.count() == 1);
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == malformed);
    CHECK(canvas.GetComments().GetBounds().isEmpty() == true);
}

TEST_CASE("comment frame double-click, context removal and mouse release use canvas authoring with Properties hidden",
          "[editor][render-graph][comments]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState selected = SelectTrace(canvas);
    canvas.FitGraph();
    QPoint header = canvas.mapFromScene(QPointF(-20, -95));
    ScheduleDialog(true, QStringLiteral("Deferred lighting"));
    QTest::mouseDClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, header);
    CHECK(GetFrames(workspace.GetDocument()).first().toObject().value(QStringLiteral("title")).toString() ==
          QStringLiteral("Deferred lighting"));
    CheckSelectionTrace(canvas, selected);
    header = canvas.mapFromScene(QPointF(-20, -95));
    const QPointF before = canvas.GetNodePosition(QStringLiteral("first"));
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, header);
    QTest::mouseMove(canvas.viewport(), header + QPoint(40, 40));
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, header + QPoint(40, 40));
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) != before);
    CheckSelectionTrace(canvas, selected);
    const QRectF frame = canvas.GetComments().GetFrameBounds(QStringLiteral("stage"));
    QTimer::singleShot(0, &workspace, []() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        CHECK(menuOrNull->objectName() == QStringLiteral("renderGraphCommentMenu"));
        QAction* removeOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphRemoveComment"));
        REQUIRE(removeOrNull != nullptr);
        QTest::mouseClick(menuOrNull,
                          Qt::LeftButton,
                          Qt::NoModifier,
                          menuOrNull->actionGeometry(removeOrNull).center());
    });
    const QPoint point = canvas.mapFromScene(frame.topLeft() + QPointF(20, 15));
    const QPointF moved = canvas.GetNodePosition(QStringLiteral("first"));
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    CHECK(GetFrames(workspace.GetDocument()).isEmpty() == true);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == moved);
    CheckSelectionTrace(canvas, selected);
}

TEST_CASE("partially enclosed cards stay fixed and oversized comment publication preserves history",
          "[editor][render-graph][comments]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    QJsonObject frame = SetRenderGraphCommentBounds(MakeFrame(), QRectF(-40, -110, 400, 400));
    REQUIRE(workspace.GetDocument().ReplaceCommentFrames(GetRoot(), GetDefault(), {frame}) ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    const RenderGraphViewState selected = SelectTrace(canvas);
    REQUIRE(canvas.GetComments().BeginGesture(QPointF(-20, -95)) == true);
    REQUIRE(canvas.GetComments().UpdateGesture(QPointF(50, -5)) == true);
    REQUIRE(canvas.GetComments().EndGesture() == true);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == QPointF(70, 90));
    CHECK(canvas.GetNodePosition(QStringLiteral("second")) == QPointF(300, 50));
    CheckSelectionTrace(canvas, selected);
    const QByteArray before = workspace.GetDocument().GetDraft(GetRoot());
    const uint64 revision = workspace.GetDocument().GetRevision();
    QJsonArray frames;
    for (int32 index = 0; index < layout_policy::MAX_COMMENT_FRAMES; ++index)
    {
        QJsonObject large = MakeFrame(QStringLiteral("large%1").arg(index));
        large.insert(QStringLiteral("notes"), QString(layout_policy::MAX_COMMENT_NOTES_CODEPOINTS, QLatin1Char('x')));
        frames.append(large);
    }
    REQUIRE(AreRenderGraphCommentFramesValid(frames) == true);
    CHECK(workspace.GetDocument().ReplaceCommentFrames(GetRoot(),
                                                       GetDefault(),
                                                       frames,
                                                       {{QStringLiteral("first"), 500, 500}}) ==
          RenderGraphDocumentStatus::Capacity);
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
    CHECK(workspace.GetDocument().GetRevision() == revision);
}

TEST_CASE("failed grouped frame transactions restore the drawable preview and selection",
          "[editor][render-graph][comments]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    RenderGraphDocument& document = workspace.GetDocument();
    QJsonObject graph = QJsonDocument::fromJson(document.GetDraft(GetRoot())).object();
    QJsonArray configurations = graph.value(QStringLiteral("configurations")).toArray();
    QJsonObject configuration = configurations.first().toObject();
    QJsonObject layout = configuration.value(QStringLiteral("layout")).toObject();
    QJsonArray frames;
    for (int32 index = 0; index < layout_policy::MAX_COMMENT_FRAMES; ++index)
    {
        QJsonObject frame = MakeFrame(QStringLiteral("fill%1").arg(index));
        frame.insert(QStringLiteral("notes"), QString());
        frames.append(frame);
    }
    const auto encode = [&graph, &configurations, &configuration, &layout, &frames]() {
        layout.insert(QStringLiteral("frames"), frames);
        configuration.insert(QStringLiteral("layout"), layout);
        configurations.replace(0, configuration);
        graph.insert(QStringLiteral("configurations"), configurations);
        return EncodeCanonicalJson(graph);
    };
    QByteArray bytes = encode();
    for (qsizetype index = 0; index < frames.size() && bytes.size() < RenderGraphDocument::MAX_FILE_BYTES; ++index)
    {
        QJsonObject frame = frames.at(index).toObject();
        const qsizetype count = qMin(static_cast<qsizetype>(layout_policy::MAX_COMMENT_NOTES_CODEPOINTS),
                                     RenderGraphDocument::MAX_FILE_BYTES - bytes.size());
        frame.insert(QStringLiteral("notes"), QString(count, QLatin1Char('x')));
        frames.replace(index, frame);
        bytes = encode();
    }
    REQUIRE(bytes.size() == RenderGraphDocument::MAX_FILE_BYTES);
    REQUIRE(document.SetText(GetRoot(), bytes) == RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    const RenderGraphViewState selected = SelectTrace(canvas);
    const uint64 revision = document.GetRevision();
    REQUIRE(canvas.GetComments().BeginGesture(QPointF(-20, -95)) == true);
    REQUIRE(canvas.GetComments().UpdateGesture(QPointF(3980, 3905)) == true);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == QPointF(4000, 4000));
    REQUIRE(canvas.GetComments().EndGesture() == true);
    CHECK(document.GetRevision() == revision);
    CHECK(document.GetDraft(GetRoot()) == bytes);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == QPointF(0, 0));
    CHECK(canvas.GetComments().GetFrameBounds(QStringLiteral("fill%1").arg(layout_policy::MAX_COMMENT_FRAMES - 1)) ==
          GetRenderGraphCommentBounds(MakeFrame()));
    CheckSelectionTrace(canvas, selected);
}

namespace
{
QGraphicsItem* FindCardOrNull(const RenderGraphCanvas& canvas, const QString& identity)
{
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        if (item->data(0).toString() == identity)
        {
            return item;
        }
    }
    return nullptr;
}
QJsonObject MakeStageLocation(const QString& node)
{
    return {{QStringLiteral("source"), GetRoot()},
            {QStringLiteral("configuration"), GetDefault()},
            {QStringLiteral("node"), node},
            {QStringLiteral("field"), QStringLiteral("exposure")}};
}
void AcceptStageErrors(RendererWorkspace& inoutWorkspace)
{
    RenderGraphJobResult result;
    result.Request.ProjectEpoch = 1;
    result.Request.Revision = inoutWorkspace.GetDocument().GetRevision();
    const QComboBox* configurationOrNull =
        inoutWorkspace.findChild<QComboBox*>(QStringLiteral("renderGraphConfiguration"));
    REQUIRE(configurationOrNull != nullptr);
    result.Request.InputIdentity = ComputeRenderGraphBuildIdentity(inoutWorkspace.GetDocument(),
                                                                   configurationOrNull->currentText(),
                                                                   RenderGraphSource::Draft);
    result.Request.Action = QStringLiteral("validate");
    for (const QString& node : {QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third")})
    {
        result.Diagnostics.append(QJsonObject{{QStringLiteral("code"), QStringLiteral("Field")},
                                              {QStringLiteral("message"), QStringLiteral("Repair exposure")},
                                              {QStringLiteral("severity"), QStringLiteral("error")},
                                              {QStringLiteral("location"), MakeStageLocation(node)}});
    }
    inoutWorkspace.AcceptResult(result);
}
} // namespace

TEST_CASE("stage collapse schema is optional boolean and remains layout-only through save and undo",
          "[editor][render-graph][stages]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState view = SelectTrace(canvas);
    const QByteArray before = workspace.GetDocument().GetDraft(GetRoot());
    const QByteArray identity =
        ComputeRenderGraphBuildIdentity(workspace.GetDocument(), GetDefault(), RenderGraphSource::Draft);
    const uint64 revision = workspace.GetDocument().GetRevision();
    for (const QJsonValue& value : {QJsonValue(1), QJsonValue(QStringLiteral("true")), QJsonValue(QJsonValue::Null)})
    {
        QJsonObject invalid = MakeFrame();
        invalid.insert(QStringLiteral("collapsed"), value);
        CHECK(AreRenderGraphCommentFramesValid({invalid}) == false);
        CHECK(workspace.GetDocument().ReplaceCommentFrames(GetRoot(), GetDefault(), {invalid}) ==
              RenderGraphDocumentStatus::InvalidEdit);
    }
    canvas.GetComments().SetCollapsed(QStringLiteral("stage"), true);
    const QByteArray collapsed = workspace.GetDocument().GetDraft(GetRoot());
    CHECK(workspace.GetDocument().GetRevision() == revision + 1);
    CHECK(GetFrames(workspace.GetDocument()).first().toObject().value(QStringLiteral("collapsed")).toBool() == true);
    CHECK(ComputeRenderGraphBuildIdentity(workspace.GetDocument(), GetDefault(), RenderGraphSource::Draft) == identity);
    CheckSelectionTrace(canvas, view);
    CHECK(FindCardOrNull(canvas, QStringLiteral("first"))->isVisible() == false);
    CHECK(FindCardOrNull(canvas, QStringLiteral("second"))->isVisible() == false);
    CHECK(FindCardOrNull(canvas, QStringLiteral("third"))->isVisible() == true);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
    CHECK(FindCardOrNull(canvas, QStringLiteral("first"))->isVisible() == true);
    CheckSelectionTrace(canvas, view);
    workspace.Redo();
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == collapsed);
    CHECK(FindCardOrNull(canvas, QStringLiteral("first"))->isVisible() == false);
    CheckSelectionTrace(canvas, view);
    QStringList saved;
    REQUIRE(workspace.GetDocument().Save(saved) == RenderGraphDocumentStatus::Ok);
    RenderGraphDocument reopened;
    REQUIRE(reopened.Load(project.path(), GetRoot()) == RenderGraphDocumentStatus::Ok);
    CHECK(reopened.GetDraft(GetRoot()) == collapsed);
    RenderGraphCanvas other(reopened);
    other.ShowConfiguration(GetRoot(), GetDefault());
    CHECK(other.GetComments().GetCollapsedFrame(QStringLiteral("second")) == QStringLiteral("stage"));
}

TEST_CASE("collapsed stage wires expose original crossing endpoints and preserve saved reroutes and trace",
          "[editor][render-graph][stages]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState view = SelectTrace(canvas);
    const QPointF first = canvas.GetNodePosition(QStringLiteral("first"));
    const QPointF second = canvas.GetNodePosition(QStringLiteral("second"));
    canvas.GetReroutes().AddPoint(QStringLiteral("second.color"), QStringLiteral("third.color"), 0, QPointF(700, 500));
    const QVector<QPointF> route =
        canvas.GetReroutes().GetPoints(QStringLiteral("second.color"), QStringLiteral("third.color"));
    canvas.GetComments().SetCollapsed(QStringLiteral("stage"), true);
    CHECK(canvas.GetNodeCount() == 3);
    CHECK(canvas.GetEdgeCount() == 4);
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("first.color"), false) != nullptr);
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("second.color"), true) != nullptr);
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("first.color"), true) == nullptr);
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("second.color"), false) == nullptr);
    CHECK(canvas.GetReroutes().GetBounds().isEmpty() == true);
    CHECK(canvas.GetReroutes().GetPoints(QStringLiteral("second.color"), QStringLiteral("third.color")) == route);
    QGraphicsEllipseItem* boundary = canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("second.color"), true);
    REQUIRE(boundary != nullptr);
    CHECK(canvas.GetPortPosition(QStringLiteral("second.color"), true) ==
          boundary->mapToScene(boundary->rect().center()));
    bool isOutput = false;
    CHECK(canvas.GetComments().GetPortAt(canvas.GetPortPosition(QStringLiteral("second.color"), true), isOutput) ==
          QStringLiteral("second.color"));
    CHECK(isOutput == true);
    CHECK(boundary->pen().widthF() == 2);
    QSignalSpy replaced(&canvas, &RenderGraphCanvas::ReplaceConnectionRequested);
    canvas.FitGraph();
    const QPoint source = canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("input.color"), true));
    const QPoint destination = canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("first.color"), false));
    QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, source);
    QTest::mouseMove(canvas.viewport(), destination);
    QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, destination);
    REQUIRE(replaced.size() == 1);
    CHECK(replaced.first().at(0).toString() == QStringLiteral("input.color"));
    CHECK(replaced.first().at(1).toString() == QStringLiteral("first.color"));
    CheckSelectionTrace(canvas, view);
    QSignalSpy inspected(&canvas, &RenderGraphCanvas::ResourceInspectionRequested);
    canvas.FitGraph();
    QTimer::singleShot(0, [&canvas]() {
        QMenu* menuOrNull = canvas.findChild<QMenu*>(QStringLiteral("renderGraphConnectionMenu"));
        REQUIRE(menuOrNull != nullptr);
        QAction* inspectOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphInspectResource"));
        REQUIRE(inspectOrNull != nullptr);
        inspectOrNull->trigger();
        menuOrNull->close();
    });
    const QPoint port = canvas.mapFromScene(canvas.GetPortPosition(QStringLiteral("second.color"), true));
    QContextMenuEvent context(QContextMenuEvent::Mouse, port, canvas.viewport()->mapToGlobal(port));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    REQUIRE(inspected.size() == 1);
    CHECK(inspected.first().at(0).toString() == QStringLiteral("second.color"));
    const uint64 revision = workspace.GetDocument().GetRevision();
    canvas.GetReroutes().AddPoint(QStringLiteral("second.color"), QStringLiteral("third.color"), 0, QPointF(600, 400));
    CHECK(workspace.GetDocument().GetRevision() == revision);
    canvas.GetComments().SetCollapsed(QStringLiteral("stage"), false);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == first);
    CHECK(canvas.GetNodePosition(QStringLiteral("second")) == second);
    CHECK(canvas.GetReroutes().GetPoints(QStringLiteral("second.color"), QStringLiteral("third.color")) == route);
    CHECK(canvas.GetReroutes().GetBounds().isEmpty() == false);
    CheckSelectionTrace(canvas, view);
}

TEST_CASE("empty overlapping and nested frames stay expanded even with saved collapsed metadata",
          "[editor][render-graph][stages]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    QJsonObject stage = MakeFrame();
    stage.insert(QStringLiteral("collapsed"), true);
    for (const QRectF& bounds : {QRectF(-40, -110, 600, 400), QRectF(-100, -150, 750, 500), QRectF(400, 0, 600, 400)})
    {
        const QJsonObject other = SetRenderGraphCommentBounds(MakeFrame(QStringLiteral("other")), bounds);
        REQUIRE(workspace.GetDocument().ReplaceCommentFrames(GetRoot(), GetDefault(), {stage, other}) ==
                RenderGraphDocumentStatus::Ok);
        workspace.GetCanvas().Rebuild();
        CHECK(workspace.GetCanvas().GetComments().CanCollapse(QStringLiteral("stage")) == false);
        CHECK(FindCardOrNull(workspace.GetCanvas(), QStringLiteral("first"))->isVisible() == true);
        CHECK(workspace.GetCanvas().GetComments().GetCollapsedFrame(QStringLiteral("first")).isEmpty() == true);
    }
    stage = SetRenderGraphCommentBounds(stage, QRectF(-2000, -2000, 600, 400));
    REQUIRE(workspace.GetDocument().ReplaceCommentFrames(GetRoot(), GetDefault(), {stage}) ==
            RenderGraphDocumentStatus::Ok);
    workspace.GetCanvas().Rebuild();
    CHECK(workspace.GetCanvas().GetComments().CanCollapse(QStringLiteral("stage")) == false);
}

TEST_CASE("moving collapsed stages preserves expanded geometry membership selection and undo",
          "[editor][render-graph][stages]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState view = SelectTrace(canvas);
    canvas.GetComments().SetCollapsed(QStringLiteral("stage"), true);
    const QByteArray before = workspace.GetDocument().GetDraft(GetRoot());
    const QRectF expanded = GetRenderGraphCommentBounds(GetFrames(workspace.GetDocument()).first().toObject());
    const QPointF first = canvas.GetNodePosition(QStringLiteral("first"));
    const QPointF third = canvas.GetNodePosition(QStringLiteral("third"));
    const QPointF press = canvas.GetComments().GetFrameBounds(QStringLiteral("stage")).topLeft() + QPointF(20, 15);
    REQUIRE(canvas.GetComments().BeginGesture(press) == true);
    REQUIRE(canvas.GetComments().UpdateGesture(press + QPointF(100, 150)) == true);
    REQUIRE(canvas.GetComments().EndGesture() == true);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == first + QPointF(100, 150));
    CHECK(canvas.GetNodePosition(QStringLiteral("third")) == third);
    CHECK(GetRenderGraphCommentBounds(GetFrames(workspace.GetDocument()).first().toObject()) ==
          expanded.translated(100, 150));
    CHECK(canvas.GetComments().GetCollapsedFrame(QStringLiteral("first")) == QStringLiteral("stage"));
    CheckSelectionTrace(canvas, view);
    workspace.Undo();
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == first);
    CheckSelectionTrace(canvas, view);
    REQUIRE(canvas.GetComments().BeginGesture(press) == true);
    REQUIRE(canvas.GetComments().UpdateGesture(press + QPointF(50, 50)) == true);
    REQUIRE(canvas.GetComments().CancelGesture() == true);
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
    CHECK(canvas.GetNodePosition(QStringLiteral("first")) == first);
}

TEST_CASE("Find Node temporarily reveals a collapsed stage and browser snapshots restore reveal preferences",
          "[editor][render-graph][stages]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.GetComments().SetCollapsed(QStringLiteral("stage"), true);
    const RenderGraphViewState before = SelectTrace(canvas);
    const QByteArray bytes = workspace.GetDocument().GetDraft(GetRoot());
    const uint64 revision = workspace.GetDocument().GetRevision();
    RenderGraphNavigation* navigationOrNull = canvas.findChild<RenderGraphNavigation*>();
    REQUIRE(navigationOrNull != nullptr);
    navigationOrNull->NodeRequested(QStringLiteral("second"));
    CHECK(FindCardOrNull(canvas, QStringLiteral("second"))->isVisible() == true);
    CHECK(canvas.GetSelectedNodes() == QStringList{QStringLiteral("second")});
    CHECK(canvas.CaptureViewState().RevealedFrames == QStringList{QStringLiteral("stage")});
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == bytes);
    CHECK(workspace.GetDocument().GetRevision() == revision);
    const RenderGraphViewState revealed = canvas.CaptureViewState();
    REQUIRE(canvas.RestoreViewState(before) == true);
    CHECK(FindCardOrNull(canvas, QStringLiteral("second"))->isVisible() == false);
    CheckSelectionTrace(canvas, before);
    REQUIRE(canvas.RestoreViewState(revealed) == true);
    CHECK(FindCardOrNull(canvas, QStringLiteral("second"))->isVisible() == true);
    canvas.GetComments().SetCollapsed(QStringLiteral("stage"), true);
    CHECK(FindCardOrNull(canvas, QStringLiteral("second"))->isVisible() == false);
    CHECK(workspace.GetDocument().GetRevision() == revision);
}

TEST_CASE("diagnostics count stage errors and reveal exact fields with Back Forward and stale labels",
          "[editor][render-graph][stages]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.GetComments().SetCollapsed(QStringLiteral("stage"), true);
    const RenderGraphViewState before = SelectTrace(canvas);
    AcceptStageErrors(workspace);
    CHECK(canvas.GetDiagnostics().CountNodeErrors(GetRoot(),
                                                  GetDefault(),
                                                  {QStringLiteral("first"), QStringLiteral("second")}) == 2);
    bool hasCounts = false;
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        const QGraphicsSimpleTextItem* labelOrNull = qgraphicsitem_cast<QGraphicsSimpleTextItem*>(item);
        if (labelOrNull != nullptr && labelOrNull->text() == QStringLiteral("2 nodes · 2 errors"))
        {
            hasCounts = true;
        }
    }
    CHECK(hasCounts == true);
    const QByteArray bytes = workspace.GetDocument().GetDraft(GetRoot());
    QTimer::singleShot(0, []() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        dialogOrNull->reject();
    });
    REQUIRE(canvas.GetDiagnostics().Next() == true);
    CHECK(FindCardOrNull(canvas, QStringLiteral("first"))->isVisible() == true);
    CHECK(canvas.GetDiagnosticLocation().value(QStringLiteral("field")).toString() == QStringLiteral("exposure"));
    REQUIRE(canvas.GetHistory().Back() == true);
    CHECK(FindCardOrNull(canvas, QStringLiteral("first"))->isVisible() == false);
    CheckSelectionTrace(canvas, before);
    REQUIRE(canvas.GetHistory().Forward() == true);
    CHECK(FindCardOrNull(canvas, QStringLiteral("first"))->isVisible() == true);
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == bytes);
    canvas.GetComments().SetCollapsed(QStringLiteral("stage"), true);
    canvas.GetDiagnostics().Refresh(RenderGraphDiagnosticFreshness::Stale);
    bool hasStaleCounts = false;
    for (QGraphicsItem* item : canvas.scene()->items())
    {
        const QGraphicsSimpleTextItem* labelOrNull = qgraphicsitem_cast<QGraphicsSimpleTextItem*>(item);
        if (labelOrNull != nullptr && labelOrNull->text() == QStringLiteral("2 nodes · 2 errors · stale"))
        {
            hasStaleCounts = true;
        }
    }
    CHECK(hasStaleCounts == true);
    const QString captures = qEnvironmentVariable("LUDUS_EDITOR_GRAPH_STAGES_CAPTURE");
    if (captures.isEmpty() == false)
    {
        REQUIRE(QDir().mkpath(captures) == true);
        canvas.FitGraph();
        QCoreApplication::processEvents();
        REQUIRE(canvas.grab().save(QDir(captures).filePath(QStringLiteral("collapsed-stage.png"))) == true);
    }
}

TEST_CASE("stage context collapse and double-click expansion work with Properties hidden",
          "[editor][render-graph][stages]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    SeedFrame(workspace);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    const RenderGraphViewState view = SelectTrace(canvas);
    canvas.FitGraph();
    QTimer::singleShot(0, []() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        REQUIRE(menuOrNull != nullptr);
        QAction* toggleOrNull = menuOrNull->findChild<QAction*>(QStringLiteral("renderGraphToggleStage"));
        REQUIRE(toggleOrNull != nullptr);
        CHECK(toggleOrNull->isEnabled() == true);
        QTest::mouseClick(menuOrNull,
                          Qt::LeftButton,
                          Qt::NoModifier,
                          menuOrNull->actionGeometry(toggleOrNull).center());
    });
    QPoint point = canvas.mapFromScene(QPointF(-20, -95));
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, canvas.viewport()->mapToGlobal(point));
    QCoreApplication::sendEvent(canvas.viewport(), &context);
    CHECK(FindCardOrNull(canvas, QStringLiteral("first"))->isVisible() == false);
    CheckSelectionTrace(canvas, view);
    point = canvas.mapFromScene(QPointF(-20, -95));
    QTest::mouseDClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, point);
    CHECK(FindCardOrNull(canvas, QStringLiteral("first"))->isVisible() == true);
    CheckSelectionTrace(canvas, view);
    workspace.Undo();
    CHECK(FindCardOrNull(canvas, QStringLiteral("first"))->isVisible() == false);
    QTest::keyClick(&canvas, Qt::Key_A, Qt::ControlModifier);
    CHECK(canvas.GetSelectedNodes() ==
          QStringList{QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third")});
}

TEST_CASE("boundary ports between collapsed stages and interfaces follow rename disconnect delete and undo",
          "[editor][render-graph][stages]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    QJsonObject other = SetRenderGraphCommentBounds(MakeFrame(QStringLiteral("other")), QRectF(860, 390, 500, 400));
    REQUIRE(workspace.GetDocument().ReplaceCommentFrames(GetRoot(), GetDefault(), {MakeFrame(), other}) ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    canvas.GetComments().SetCollapsed(QStringLiteral("stage"), true);
    canvas.GetComments().SetCollapsed(QStringLiteral("other"), true);
    CHECK(canvas.GetComments().GetCollapsedFrame(QStringLiteral("third")) == QStringLiteral("other"));
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("second.color"), true) != nullptr);
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("third.color"), false) != nullptr);
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("third.color"), true) != nullptr);
    QString error;
    REQUIRE(workspace.GetDocument().RenameNode(GetRoot(),
                                               GetDefault(),
                                               QStringLiteral("second"),
                                               QStringLiteral("toned"),
                                               error) == RenderGraphDocumentStatus::Ok);
    canvas.Rebuild();
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("second.color"), true) == nullptr);
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("toned.color"), true) != nullptr);
    REQUIRE(workspace.GetDocument().Disconnect(GetRoot(),
                                               GetDefault(),
                                               QStringLiteral("toned.color"),
                                               QStringLiteral("third.color")) == RenderGraphDocumentStatus::Ok);
    canvas.Rebuild();
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("toned.color"), true) == nullptr);
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("third.color"), false) == nullptr);
    workspace.Undo();
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("toned.color"), true) != nullptr);
    REQUIRE(workspace.GetDocument().RemoveNodes(GetRoot(),
                                                GetDefault(),
                                                {QStringLiteral("first"), QStringLiteral("toned")}) ==
            RenderGraphDocumentStatus::Ok);
    canvas.Rebuild();
    CHECK(canvas.GetComments().CanCollapse(QStringLiteral("stage")) == false);
    CHECK(canvas.GetComments().GetBoundaryPortOrNull(QStringLiteral("first.color"), false) == nullptr);
    workspace.Undo();
    CHECK(canvas.GetComments().GetCollapsedFrame(QStringLiteral("toned")) == QStringLiteral("stage"));
}

TEST_CASE("temporary stage reveal extends the scroll range to distant hidden nodes without editing layout",
          "[editor][render-graph][stages]")
{
    QTemporaryDir project;
    RendererWorkspace workspace({});
    OpenWorkspace(workspace, project);
    REQUIRE(workspace.GetDocument().MoveNodes(GetRoot(), GetDefault(), {{QStringLiteral("third"), 10000, 5000}}) ==
            RenderGraphDocumentStatus::Ok);
    const QJsonObject frame = SetRenderGraphCommentBounds(MakeFrame(), QRectF(-40, -110, 10500, 5500));
    REQUIRE(workspace.GetDocument().ReplaceCommentFrames(GetRoot(), GetDefault(), {frame}) ==
            RenderGraphDocumentStatus::Ok);
    RenderGraphCanvas& canvas = workspace.GetCanvas();
    canvas.Rebuild();
    canvas.GetComments().SetCollapsed(QStringLiteral("stage"), true);
    canvas.FitGraph();
    canvas.ResetZoom();
    const QByteArray before = workspace.GetDocument().GetDraft(GetRoot());
    const RenderGraphViewState view = canvas.CaptureViewState();
    const QRectF target = FindCardOrNull(canvas, QStringLiteral("third"))->sceneBoundingRect();
    REQUIRE(canvas.sceneRect().contains(target) == false);
    canvas.GetComments().RevealNode(QStringLiteral("third"));
    CHECK(canvas.sceneRect().contains(target) == true);
    CHECK(canvas.CaptureViewState().Zoom == view.Zoom);
    CHECK(qAbs(canvas.CaptureViewState().CentreScene.x() - view.CentreScene.x()) < 1);
    CHECK(qAbs(canvas.CaptureViewState().CentreScene.y() - view.CentreScene.y()) < 1);
    canvas.RevealNode(QStringLiteral("third"));
    CHECK(canvas.mapToScene(canvas.viewport()->rect()).boundingRect().contains(target.center()) == true);
    CHECK(workspace.GetDocument().GetDraft(GetRoot()) == before);
}
