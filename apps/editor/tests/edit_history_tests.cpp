#include "internal/controller.h"
#include "internal/document_history.h"
#include "internal/document_well.h"
#include "internal/edit_history.h"
#include "internal/main_window.h"
#include "internal/render_graph_document.h"
#include "internal/world_document.h"

#include <ludus/foundation/base/types.h>

#include <ludus/platform/entropy.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListView>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>

using namespace ludus::editor;
using ludus::foundation::int32;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;

namespace
{
bool IsNamed(const char* nameOrNull, const char* expected)
{
    return nameOrNull != nullptr && std::strcmp(nameOrNull, expected) == 0;
}

QByteArray ReadSource(const char* relativePath)
{
    QFile file(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT) + QLatin1Char('/') + QLatin1String(relativePath));
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

WorldDocument OpenStarterWorld()
{
    WorldDocument document;
    QString error;
    REQUIRE(document.LoadSchema(ReadSource("examples/live-edit-game/authoring/world.schema.json"), error) == true);
    REQUIRE(document.Load(ReadSource("examples/live-edit-game/authoring/starter.world.json"), error) == true);
    return document;
}

const QString GRAPH_ROOT = QStringLiteral("main.lurender.json");
const QString GRAPH_CONFIGURATION = QStringLiteral("Default");

QString MakeGraphPackage(const QTemporaryDir& project)
{
    const QString package = QDir(project.path()).filePath(QStringLiteral("graphs"));
    REQUIRE(QDir().mkpath(package) == true);
    const QByteArray bytes = R"({"version":2,"id":"Main","default":"Default","configurations":[
        {"id":"Default","inputs":[],"outputs":[{"id":"result","type":"ColorTexture","source":"color.texture"}],
         "nodes":[
            {"id":"size","kind":"RenderSize","extent":[640,360]},
            {"id":"color","kind":"Target","type":"ColorTexture","format":"RGBA8Unorm",
             "extent":"RenderSize","samples":1,"imported":false,"initialized":false,"presentation":false},
            {"id":"output","kind":"Export","type":"ColorTexture"}],
         "edges":[{"from":"size.size","to":"color.size"},{"from":"color.texture","to":"output.texture"}],
         "layout":{"positions":[{"node":"color","x":80,"y":120},{"node":"output","x":300,"y":0}]}}]})";
    const QByteArray canonical = EncodeCanonicalJson(QJsonDocument::fromJson(bytes).object());
    QFile file(QDir(package).filePath(GRAPH_ROOT));
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(canonical) == canonical.size());
    return package;
}

ToolingPaths MissingTools()
{
    return {QStringLiteral("/missing/python"), QStringLiteral("/missing/editor_tool.py"), QStringLiteral("/missing")};
}

QString WriteProject(const QTemporaryDir& directory)
{
    ProjectDescriptor descriptor;
    descriptor.Name = QStringLiteral("demo");
    descriptor.ProviderKind = Provider::Cmake;
    descriptor.SourceDir = QStringLiteral(".");
    descriptor.Preset = QStringLiteral("linux-clang-debug");
    descriptor.Target = QStringLiteral("app");
    descriptor.RunCwd = QStringLiteral(".");
    const QString path = directory.filePath(QStringLiteral("ludus.project.json"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    const QByteArray bytes = SerializeDescriptor(descriptor);
    REQUIRE(file.write(bytes) == bytes.size());
    return path;
}

QAction* FindAction(const MainWindow& window, const QString& name)
{
    QAction* actionOrNull = window.findChild<QAction*>(name);
    REQUIRE(actionOrNull != nullptr);
    return actionOrNull;
}
} // namespace

TEST_CASE("Document history keeps a readable name for every entry", "[editor][document][history]")
{
    DocumentHistory<uint32, 3> history;
    CHECK(history.GetUndoNameOrNull() == nullptr);
    CHECK(history.GetRedoNameOrNull() == nullptr);
    REQUIRE(history.Commit(0, 1, false, "Create Entity") == true);
    REQUIRE(history.Commit(1, 2, false, "Rename Entity") == true);
    REQUIRE(history.Commit(2, 3) == true);
    CHECK(history.GetCount() == 3);
    CHECK(history.GetCursor() == 3);
    CHECK(IsNamed(history.GetName(0), "Create Entity") == true);
    CHECK(IsNamed(history.GetName(1), "Rename Entity") == true);
    CHECK(IsNamed(history.GetName(2), UNNAMED_EDIT_NAME) == true);
    CHECK(IsNamed(history.GetName(3), UNNAMED_EDIT_NAME) == true);
    CHECK(IsNamed(history.GetUndoNameOrNull(), UNNAMED_EDIT_NAME) == true);

    uint32 current = 3;
    REQUIRE(history.Undo(current) == true);
    CHECK(IsNamed(history.GetUndoNameOrNull(), "Rename Entity") == true);
    CHECK(IsNamed(history.GetRedoNameOrNull(), UNNAMED_EDIT_NAME) == true);

    // Eviction drops the oldest name with its entry, so names stay aligned with their edits.
    REQUIRE(history.Commit(current, 4, false, "Move Entity") == true);
    current = 4;
    REQUIRE(history.Commit(current, 5, false, "Delete Entity") == true);
    current = 5;
    CHECK(history.GetCount() == 3);
    CHECK(IsNamed(history.GetName(0), "Rename Entity") == true);
    CHECK(IsNamed(history.GetName(1), "Move Entity") == true);
    CHECK(IsNamed(history.GetName(2), "Delete Entity") == true);

    // A merged edit keeps the name of the group it joins.
    REQUIRE(history.Commit(current, 6, true, "Typing") == true);
    CHECK(history.GetCount() == 3);
    CHECK(IsNamed(history.GetName(2), "Delete Entity") == true);
}

TEST_CASE("Document history jumps several steps at once and rejects stale heads whole", "[editor][document][history]")
{
    DocumentHistory<uint32> history;
    uint32 current = 0;
    for (uint32 next = 1; next <= 4; ++next)
    {
        REQUIRE(history.Commit(current, next, false, "Step") == true);
        current = next;
    }
    const uint64 revision = history.Revision();
    REQUIRE(history.JumpTo(current, 1) == true);
    CHECK(current == 1);
    CHECK(history.GetCursor() == 1);
    CHECK(history.Revision() == revision + 1);
    CHECK(history.CanRedo() == true);
    REQUIRE(history.JumpTo(current, 4) == true);
    CHECK(current == 4);
    REQUIRE(history.JumpTo(current, 0) == true);
    CHECK(current == 0);
    CHECK(history.CanUndo() == false);

    // Jumping to the current position changes nothing.
    const uint64 settled = history.Revision();
    REQUIRE(history.JumpTo(current, 0) == true);
    CHECK(history.Revision() == settled);

    // Out of range and stale heads leave the value and the history alone.
    CHECK(history.JumpTo(current, 5) == false);
    uint32 stale = 9;
    CHECK(history.JumpTo(stale, 3) == false);
    CHECK(stale == 9);
    CHECK(history.GetCursor() == 0);
    CHECK(history.Revision() == settled);

    // A jump ends the edit group, so the next edit never merges into an older entry.
    REQUIRE(history.JumpTo(current, 2) == true);
    REQUIRE(history.Commit(current, 7, true, "Branch") == true);
    CHECK(history.GetCount() == 3);
    CHECK(IsNamed(history.GetName(2), "Branch") == true);
}

TEST_CASE("Undo feedback names the command and the state a jump reaches", "[editor][history]")
{
    CHECK(MakeUndoMenuText(QString()) == QStringLiteral("&Undo"));
    CHECK(MakeUndoMenuText(QStringLiteral("Rename Entity")) == QStringLiteral("&Undo Rename Entity"));
    CHECK(MakeRedoMenuText(QString()) == QStringLiteral("&Redo"));
    CHECK(MakeRedoMenuText(QStringLiteral("Add Node")) == QStringLiteral("&Redo Add Node"));
    CHECK(MakeUndoFeedbackText(QStringLiteral("Rename Entity")) == QStringLiteral("Undid: Rename Entity"));
    CHECK(MakeRedoFeedbackText(QStringLiteral("Rename Entity")) == QStringLiteral("Redid: Rename Entity"));

    const EditHistoryState state
    {
        .DocumentName = QStringLiteral("World"),
        .Names = {QStringLiteral("Create Entity"), QStringLiteral("Move Entity"), QStringLiteral("Rename Entity")},
        .Cursor = 3,
        .CanJump = true,
    };
    CHECK(MakeHistoryJumpFeedbackText(state, 2) == QStringLiteral("Undid: Rename Entity"));
    CHECK(MakeHistoryJumpFeedbackText(state, 1) == QStringLiteral("Undid 2 edits, back to: Create Entity"));
    CHECK(MakeHistoryJumpFeedbackText(state, 0) == QStringLiteral("Undid 3 edits, back to: initial state"));
    EditHistoryState undone = state;
    undone.Cursor = 0;
    CHECK(MakeHistoryJumpFeedbackText(undone, 1) == QStringLiteral("Redid: Create Entity"));
    CHECK(MakeHistoryJumpFeedbackText(undone, 3) == QStringLiteral("Redid 3 edits, up to: Rename Entity"));
    CHECK(MakeHistoryJumpFeedbackText(state, 3).isEmpty() == true);
    CHECK(MakeHistoryJumpFeedbackText(state, 4).isEmpty() == true);
    CHECK(MakeHistoryJumpFeedbackText(state, -1).isEmpty() == true);
}

TEST_CASE("History model rows are history positions with the current one marked", "[editor][history]")
{
    EditHistoryModel model;
    CHECK(model.rowCount() == 0);
    DocumentHistory<uint32> history;
    REQUIRE(history.Commit(0, 1, false, "Create Entity") == true);
    REQUIRE(history.Commit(1, 2, false, "Move Entity") == true);
    uint32 current = 2;
    REQUIRE(history.Undo(current) == true);
    model.SetState(MakeEditHistoryState(QStringLiteral("World"), history));
    REQUIRE(model.rowCount() == 3);
    CHECK(model.data(model.index(0, 0), Qt::DisplayRole).toString() ==
          QString::fromUtf8(EditHistoryModel::INITIAL_STATE_NAME));
    CHECK(model.data(model.index(1, 0), Qt::DisplayRole).toString() == QStringLiteral("Create Entity"));
    CHECK(model.data(model.index(2, 0), Qt::DisplayRole).toString() == QStringLiteral("Move Entity"));
    CHECK(model.data(model.index(1, 0), EditHistoryModel::IsCurrentRole).toBool() == true);
    CHECK(model.data(model.index(2, 0), EditHistoryModel::IsUndoneRole).toBool() == true);
    CHECK(model.data(model.index(0, 0), EditHistoryModel::IsUndoneRole).toBool() == false);
    CHECK(model.data(model.index(1, 0), Qt::AccessibleTextRole).toString() ==
          QStringLiteral("Create Entity, current state"));
    CHECK(model.data(model.index(2, 0), Qt::AccessibleTextRole).toString() == QStringLiteral("Move Entity, undone"));
    CHECK((model.flags(model.index(2, 0)) & Qt::ItemIsEnabled) != 0);

    // Text fields keep their own history: rows are not offered as jump targets.
    EditHistoryState text = model.GetState();
    text.CanJump = false;
    model.SetState(text);
    CHECK((model.flags(model.index(2, 0)) & Qt::ItemIsEnabled) == 0);
}

TEST_CASE("History panel asks to jump only to another available state", "[editor][history][widget]")
{
    EditHistoryPanel panel;
    panel.resize(240, 320);
    panel.show();
    QSignalSpy jumps(&panel, &EditHistoryPanel::JumpRequested);
    const EditHistoryState state
    {
        .DocumentName = QStringLiteral("World"),
        .Names = {QStringLiteral("Create Entity"), QStringLiteral("Move Entity")},
        .Cursor = 2,
        .CanJump = true,
    };
    panel.SetState(state);
    QListView& list = panel.GetList();
    REQUIRE(list.isVisible() == true);
    CHECK(list.currentIndex().row() == 2);
    const QRect current = list.visualRect(panel.GetModel().index(2, 0));
    QTest::mouseClick(list.viewport(), Qt::LeftButton, {}, current.center());
    CHECK(jumps.isEmpty() == true);
    const QRect initial = list.visualRect(panel.GetModel().index(0, 0));
    QTest::mouseClick(list.viewport(), Qt::LeftButton, {}, initial.center());
    REQUIRE(jumps.size() == 1);
    CHECK(jumps.takeFirst().at(0).toInt() == 0);

    // Keyboard: choose a row and press Enter.
    list.setFocus();
    list.setCurrentIndex(panel.GetModel().index(1, 0));
    QTest::keyClick(&list, Qt::Key_Return);
    REQUIRE(jumps.size() == 1);
    CHECK(jumps.takeFirst().at(0).toInt() == 1);

    // A document with no edits shows a hint instead of a lone initial row.
    panel.SetState(
        EditHistoryState{ .DocumentName = QStringLiteral("World"), .Names = {}, .Cursor = 0, .CanJump = true });
    CHECK(list.isVisible() == false);
}

TEST_CASE("World edits carry names that describe what changed", "[editor][world][history]")
{
    WorldDocument document = OpenStarterWorld();
    QString error;
    QString id;
    REQUIRE(document.CreateEntity(ludus::platform::GetSystemEntropySource(), error, QStringLiteral("Lamp"), id) ==
            true);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Create Entity") == true);
    REQUIRE(document.Attach(id, error, QStringLiteral("presentation")) == true);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Add Component") == true);
    REQUIRE(document.RenameEntity(id, error, QStringLiteral("Desk lamp")) == true);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Rename Entity") == true);

    // An inspector transaction that only changes the display name reads as a rename.
    const QJsonObject components = document.Entity(id).value(QStringLiteral("components")).toObject();
    REQUIRE(document.Edit(id, components, QStringLiteral("Reading lamp"), error) == true);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Rename Entity") == true);
    QJsonObject moved = components;
    QJsonObject transform = moved.value(QStringLiteral("transform")).toObject();
    transform.insert(QStringLiteral("x"), 2.0);
    moved.insert(QStringLiteral("transform"), transform);
    REQUIRE(document.Edit(id, moved, QStringLiteral("Reading lamp"), error) == true);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Edit Properties") == true);
    transform.insert(QStringLiteral("x"), 3.0);
    moved.insert(QStringLiteral("transform"), transform);
    REQUIRE(document.Edit(id, moved, QStringLiteral("Reading lamp"), error, nullptr, "Move Entity") == true);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Move Entity") == true);
    REQUIRE(document.Detach(id, error, QStringLiteral("presentation")) == true);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Remove Component") == true);
    REQUIRE(document.RemoveEntity(id, error) == true);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Delete Entity") == true);

    // Jumping back to the start restores the opened World in one step.
    const QJsonObject opened = OpenStarterWorld().Draft();
    REQUIRE(document.JumpToHistory(0) == true);
    CHECK(document.Draft() == opened);
    CHECK(IsNamed(document.GetHistory().GetRedoNameOrNull(), "Create Entity") == true);
    REQUIRE(document.JumpToHistory(document.GetHistory().GetCount()) == true);
    CHECK(document.Entity(id).isEmpty() == true);
}

TEST_CASE("Render graph edits carry names that describe what changed", "[editor][render-graph][history]")
{
    QTemporaryDir project;
    RenderGraphDocument document;
    REQUIRE(document.Load(MakeGraphPackage(project), GRAPH_ROOT) == RenderGraphDocumentStatus::Ok);
    const QByteArray opened = document.GetDraft(GRAPH_ROOT);
    constexpr int32 MOVED_X = 120;
    constexpr int32 MOVED_Y = 160;
    REQUIRE(document.MoveNode(GRAPH_ROOT, GRAPH_CONFIGURATION, QStringLiteral("color"), MOVED_X, MOVED_Y) ==
            RenderGraphDocumentStatus::Ok);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Move Node") == true);
    REQUIRE(document.Disconnect(GRAPH_ROOT,
                                GRAPH_CONFIGURATION,
                                QStringLiteral("color.texture"),
                                QStringLiteral("output.texture")) == RenderGraphDocumentStatus::Ok);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Disconnect") == true);
    QString error;
    REQUIRE(document.RenameNode(GRAPH_ROOT,
                                GRAPH_CONFIGURATION,
                                QStringLiteral("output"),
                                QStringLiteral("final"),
                                error) == RenderGraphDocumentStatus::Ok);
    CHECK(IsNamed(document.GetHistory().GetUndoNameOrNull(), "Rename Node") == true);
    CHECK(document.GetHistory().GetCount() == 3);
    REQUIRE(document.JumpToHistory(0) == true);
    CHECK(document.GetDraft(GRAPH_ROOT) == opened);
    CHECK(IsNamed(document.GetHistory().GetRedoNameOrNull(), "Move Node") == true);
}

TEST_CASE("Project setting edits are named after the field that changed", "[editor][document][history]")
{
    ProjectDescriptor before;
    before.Name = QStringLiteral("demo");
    before.Preset = QStringLiteral("linux-clang-debug");
    ProjectDescriptor after = before;
    CHECK(std::strcmp(GetProjectEditName(before, after), "Edit Project Settings") == 0);
    after.Name = QStringLiteral("game");
    CHECK(std::strcmp(GetProjectEditName(before, after), "Rename Project") == 0);
    after = before;
    after.Preset = QStringLiteral("linux-clang-development");
    CHECK(std::strcmp(GetProjectEditName(before, after), "Change Build Preset") == 0);
    after = before;
    after.RunArgs = {QStringLiteral("--fast")};
    CHECK(std::strcmp(GetProjectEditName(before, after), "Edit Run Arguments") == 0);
    after.Name = QStringLiteral("game");
    CHECK(std::strcmp(GetProjectEditName(before, after), "Edit Project Settings") == 0);
}

TEST_CASE("Undo and Redo name their command, confirm it and jump from History", "[editor][history][widget]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    MainWindow window(&controller);
    controller.OpenProject(WriteProject(directory));
    window.show();
    window.activateWindow();
    QTest::qWait(100);
    for (uint32 attempt = 0; controller.Caps().CanEdit == false && attempt < 100; ++attempt)
    {
        QTest::qWait(50);
    }
    REQUIRE(controller.Caps().CanEdit == true);
    QWidget* settings = window.findChild<QWidget*>(QStringLiteral("projectSettingsScroll"));
    REQUIRE(settings != nullptr);
    REQUIRE(window.GetDocumentWell().OpenDocument(settings) == true);
    if (QApplication::focusWidget() != nullptr)
    {
        QApplication::focusWidget()->clearFocus();
    }
    QAction* undo = FindAction(window, QStringLiteral("document.undo"));
    QAction* redo = FindAction(window, QStringLiteral("document.redo"));
    QAction* history = FindAction(window, QStringLiteral("edit.history"));
    CHECK(undo->isEnabled() == false);
    CHECK(undo->text() == QStringLiteral("&Undo"));
    CHECK(undo->toolTip().startsWith(QStringLiteral("Nothing to undo")) == true);

    ProjectDescriptor draft = controller.State().Draft;
    draft.Name = QStringLiteral("renamed");
    controller.EditDraft(draft);
    draft.Preset = QStringLiteral("linux-clang-development");
    controller.EditDraft(draft);
    REQUIRE(undo->isEnabled() == true);
    CHECK(undo->text() == QStringLiteral("&Undo Change Build Preset"));
    CHECK(undo->toolTip().startsWith(QStringLiteral("Undo Change Build Preset")) == true);
    CHECK(redo->isEnabled() == false);
    CHECK(redo->text() == QStringLiteral("&Redo"));

    undo->trigger();
    CHECK(controller.State().Draft.Preset == QStringLiteral("linux-clang-debug"));
    CHECK(undo->text() == QStringLiteral("&Undo Rename Project"));
    CHECK(redo->text() == QStringLiteral("&Redo Change Build Preset"));
    UndoFeedbackToast* toast = window.findChild<UndoFeedbackToast*>(QStringLiteral("undoFeedbackToast"));
    REQUIRE(toast != nullptr);
    CHECK(toast->isVisible() == true);
    CHECK(toast->GetText() == QStringLiteral("Undid: Change Build Preset"));
    redo->trigger();
    CHECK(toast->GetText() == QStringLiteral("Redid: Change Build Preset"));

    history->trigger();
    QDockWidget* dock = window.findChild<QDockWidget*>(QStringLiteral("historyDock"));
    REQUIRE(dock != nullptr);
    CHECK(dock->isVisible() == true);
    EditHistoryPanel* panel = window.findChild<EditHistoryPanel*>(QStringLiteral("editHistoryPanel"));
    REQUIRE(panel != nullptr);
    REQUIRE(panel->GetModel().rowCount() == 3);
    CHECK(panel->GetModel().GetState().DocumentName == QStringLiteral("Project Settings"));
    CHECK(panel->GetList().currentIndex().row() == 2);
    QListView& list = panel->GetList();
    const QRect initial = list.visualRect(panel->GetModel().index(0, 0));
    QTest::mouseClick(list.viewport(), Qt::LeftButton, {}, initial.center());
    CHECK(controller.State().Draft.Name == QStringLiteral("demo"));
    CHECK(toast->GetText() == QStringLiteral("Undid 2 edits, back to: initial state"));
    CHECK(panel->GetList().currentIndex().row() == 0);
    CHECK(undo->isEnabled() == false);
    CHECK(redo->text() == QStringLiteral("&Redo Rename Project"));
    toast->Dismiss();
}
