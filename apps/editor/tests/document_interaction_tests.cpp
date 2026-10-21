#include "internal/code_editor.h"
#include "internal/configuration_workspace.h"
#include "internal/controller.h"
#include "internal/document_well.h"
#include "internal/main_window.h"
#include "internal/renderer_workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTimer>

using namespace ludus::editor;

namespace
{
ToolingPaths MissingTools()
{
    return {QStringLiteral("/missing/python"), QStringLiteral("/missing/editor_tool.py"), QStringLiteral("/missing")};
}

QString Project(QTemporaryDir& directory, const QString& name = QStringLiteral("demo"))
{
    ProjectDescriptor descriptor;
    descriptor.Name = name;
    descriptor.ProviderKind = Provider::Cmake;
    descriptor.SourceDir = QStringLiteral(".");
    descriptor.Preset = QStringLiteral("linux-clang-debug");
    descriptor.Target = QStringLiteral("app");
    descriptor.RunCwd = QStringLiteral(".");
    descriptor.RunArgs = {QStringLiteral("old")};
    const auto path = directory.filePath(QStringLiteral("ludus.project.json"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    const auto bytes = SerializeDescriptor(descriptor);
    REQUIRE(file.write(bytes) == bytes.size());
    return path;
}

QAction* Action(MainWindow& window, const char* name)
{
    auto* action = window.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(action != nullptr);
    return action;
}

void ShowReady(MainWindow& window, EditorController& controller)
{
    window.show();
    window.activateWindow();
    QTest::qWait(100);
    for (ludus::foundation::uint32 attempt = 0; controller.Caps().CanEdit == false && attempt < 100; ++attempt)
    {
        QTest::qWait(50);
    }
    REQUIRE(controller.Caps().CanEdit);
}
} // namespace

TEST_CASE("Project history survives Save, conflicts and failed replacement", "[editor][document]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    const auto path = Project(directory);
    controller.OpenProject(path);
    auto draft = controller.State().Draft;
    draft.Name = QStringLiteral("edited");
    controller.EditDraft(draft);
    const auto revision = controller.ProjectRevision();
    controller.Save();
    CHECK_FALSE(controller.State().Dirty());
    REQUIRE(controller.CanUndoProject());
    controller.UndoProjectEdit();
    CHECK(controller.State().Dirty());
    CHECK(controller.ProjectRevision() > revision);
    CHECK(controller.Store().Load(path).Parse.Descriptor.Name == QStringLiteral("edited"));
    controller.RedoProjectEdit();
    CHECK_FALSE(controller.State().Dirty());

    draft.Name = QStringLiteral("failed");
    controller.EditDraft(draft);
    ProjectStore::FaultHooks hooks;
    hooks.FailCommit = []() { return true; };
    controller.Store().SetFaultHooks(hooks);
    controller.Save();
    CHECK(controller.State().Dirty());
    CHECK(controller.State().Draft.Name == QStringLiteral("failed"));
    CHECK(controller.CanUndoProject());
    controller.UndoProjectEdit();
    CHECK_FALSE(controller.State().Dirty());
    controller.RedoProjectEdit();
    controller.Store().SetFaultHooks({});
    (void)Project(directory, QStringLiteral("external"));
    controller.Save();
    CHECK(controller.State().Result.Code == ResultCode::Conflict);
    CHECK(controller.State().Draft.Name == QStringLiteral("failed"));
    CHECK(controller.CanUndoProject());
}

TEST_CASE("Save acknowledges the captured project snapshot when another edit arrives", "[editor][document]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    const auto path = Project(directory);
    controller.OpenProject(path);
    auto draft = controller.State().Draft;
    draft.Name = QStringLiteral("captured");
    controller.EditDraft(draft);
    ProjectStore::FaultHooks hooks;
    hooks.FailCommit = [&]() {
        auto newer = controller.State().Draft;
        newer.Name = QStringLiteral("newer");
        controller.EditDraft(newer, true);
        controller.Save(); // a nested save is rejected without recursion
        return false;
    };
    controller.Store().SetFaultHooks(hooks);
    controller.Save();
    CHECK(controller.Store().Load(path).Parse.Descriptor.Name == QStringLiteral("captured"));
    CHECK(controller.State().Saved.Name == QStringLiteral("captured"));
    CHECK(controller.State().Draft.Name == QStringLiteral("newer"));
    CHECK(controller.State().Dirty());
    controller.UndoProjectEdit();
    CHECK_FALSE(controller.State().Dirty());
    CHECK(controller.CanUndoProject());
}

TEST_CASE("Invalid saves and rejected loads keep drafts and history; accepted identities reset them",
          "[editor][document]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    const auto path = Project(directory);
    controller.OpenProject(path);
    auto draft = controller.State().Draft;
    draft.Name.clear();
    controller.EditDraft(draft);
    controller.Save();
    CHECK(controller.State().Result.Code == ResultCode::InvalidProject);
    CHECK(controller.State().Draft.Name.isEmpty());
    controller.OpenProject(directory.filePath(QStringLiteral("missing.json")));
    CHECK(controller.CanUndoProject());
    CHECK_FALSE(controller.CloseProject());
    controller.UndoProjectEdit();
    CHECK_FALSE(controller.State().Dirty());
    REQUIRE(controller.CloseProject());
    CHECK_FALSE(controller.CanRedoProject());
    controller.OpenProject(path);
    CHECK_FALSE(controller.CanUndoProject());
}

TEST_CASE("Focused project text keeps selection and text Undo across unrelated notifications",
          "[editor][document][widget]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    MainWindow window(&controller);
    controller.OpenProject(Project(directory));
    ShowReady(window, controller);
    auto* name = window.findChild<QLineEdit*>(QStringLiteral("projectName"));
    REQUIRE(name != nullptr);
    name->setFocus();
    name->setCursorPosition(2);
    QTest::keyClicks(name, QStringLiteral("XY"));
    REQUIRE(name->isUndoAvailable());
    name->setSelection(1, 3);
    const auto position = name->cursorPosition();
    const auto selected = name->selectedText();
    controller.ClearOutput();
    CHECK(name->hasFocus());
    CHECK(name->cursorPosition() == position);
    CHECK(name->selectedText() == selected);
    CHECK(name->isUndoAvailable());
    Action(window, "document.undo")->trigger();
    CHECK(name->text() == QStringLiteral("demo"));
    CHECK_FALSE(controller.State().Dirty());
    Action(window, "document.redo")->trigger();
    CHECK(controller.State().Dirty());
    const auto edited = name->text();
    name->setReadOnly(true);
    Action(window, "document.undo")->trigger();
    CHECK(name->text() == edited);
    CHECK(controller.State().Dirty());
    CHECK_FALSE(Action(window, "document.undo")->isEnabled());
}

TEST_CASE("Pending argument text survives notifications and is included by Save", "[editor][document][widget]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    MainWindow window(&controller);
    const auto path = Project(directory);
    controller.OpenProject(path);
    ShowReady(window, controller);
    auto* advanced = window.findChild<QPushButton*>(QStringLiteral("projectAdvanced"));
    REQUIRE(advanced != nullptr);
    advanced->click();
    auto* arguments = window.findChild<QListWidget*>(QStringLiteral("projectArguments"));
    REQUIRE(arguments != nullptr);
    arguments->setCurrentRow(0);
    arguments->editItem(arguments->item(0));
    auto* edit = arguments->findChild<QLineEdit*>();
    REQUIRE(edit != nullptr);
    edit->setFocus();
    edit->selectAll();
    QTest::keyClicks(edit, QStringLiteral("pending"));
    controller.ClearOutput();
    CHECK(arguments->findChild<QLineEdit*>() == edit);
    CHECK(edit->text() == QStringLiteral("pending"));
    CHECK(edit->hasFocus());
    CHECK(Action(window, "document.undo")->isEnabled());
    auto* save = Action(window, "document.save");
    REQUIRE(save->isEnabled());
    save->trigger();
    CHECK_FALSE(controller.State().Dirty());
    CHECK(controller.Store().Load(path).Parse.Descriptor.RunArgs == QStringList{QStringLiteral("pending")});
}

TEST_CASE("Save in Configuration preserves a different project's unsaved settings", "[editor][document][widget]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    MainWindow window(&controller);
    const auto path = Project(directory);
    controller.OpenProject(path);
    ShowReady(window, controller);
    auto draft = controller.State().Draft;
    draft.Name = QStringLiteral("unsaved project");
    controller.EditDraft(draft);
    auto* configuration =
        static_cast<ConfigurationWorkspace*>(window.findChild<QWidget*>(QStringLiteral("configurationWorkspace")));
    REQUIRE(configuration != nullptr);
    const auto preferences = directory.filePath(QStringLiteral("preferences.json"));
    REQUIRE(configuration->SavePreferences(preferences));
    REQUIRE(configuration->Edit("host.headless", QStringLiteral("true")));
    REQUIRE(window.GetDocumentWell().OpenDocument(configuration) == true);
    Action(window, "document.save")->trigger();
    CHECK_FALSE(configuration->Dirty());
    CHECK(controller.State().Dirty());
    CHECK(controller.Store().Load(path).Parse.Descriptor.Name == QStringLiteral("demo"));
    CHECK_FALSE(Action(window, "document.undo")->isEnabled());
}

TEST_CASE("Quit offers project Save Discard Cancel and Cancel preserves the draft", "[editor][document][widget]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    MainWindow window(&controller);
    controller.OpenProject(Project(directory));
    ShowReady(window, controller);
    auto draft = controller.State().Draft;
    draft.Name = QStringLiteral("unsaved");
    controller.EditDraft(draft);
    bool offeredSave = false;
    QTimer::singleShot(0, &window, [&offeredSave]() {
        auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (prompt != nullptr)
        {
            offeredSave = prompt->standardButtons().testFlag(QMessageBox::Save);
            prompt->button(QMessageBox::Cancel)->click();
        }
    });
    CHECK_FALSE(window.close());
    CHECK(offeredSave);
    CHECK(window.isVisible());
    CHECK(controller.State().Draft.Name == QStringLiteral("unsaved"));
    CHECK(controller.State().Dirty());
}

TEST_CASE("Immediate field edits cannot arrive after opening another project", "[editor][document][widget]")
{
    QTemporaryDir first;
    QTemporaryDir second;
    EditorController controller(MissingTools());
    MainWindow window(&controller);
    controller.OpenProject(Project(first));
    ShowReady(window, controller);
    auto* name = window.findChild<QLineEdit*>(QStringLiteral("projectName"));
    REQUIRE(name != nullptr);
    name->insert(QStringLiteral("stale"));
    REQUIRE(controller.State().Dirty());
    controller.OpenProject(Project(second, QStringLiteral("second")));
    QApplication::processEvents();
    CHECK(controller.State().Draft.Name == QStringLiteral("second"));
    CHECK_FALSE(controller.State().Dirty());
    CHECK_FALSE(controller.CanUndoProject());
}

TEST_CASE("Shader text undo reaches the package through shell keyboard and context menu",
          "[editor][document][widget][text]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    MainWindow window(&controller);
    controller.OpenProject(Project(directory));
    ShowReady(window, controller);
    RendererWorkspace* workspaceOrNull = window.findChild<RendererWorkspace*>();
    REQUIRE(workspaceOrNull != nullptr);
    REQUIRE(window.GetDocumentWell().OpenDocument(workspaceOrNull) == true);
    CodeEditor* editorOrNull = workspaceOrNull->findChild<CodeEditor*>(QStringLiteral("renderGraphSource"));
    REQUIRE(editorOrNull != nullptr);
    QTabWidget* viewsOrNull = workspaceOrNull->findChild<QTabWidget*>(QStringLiteral("renderGraphViews"));
    REQUIRE(viewsOrNull != nullptr);
    viewsOrNull->setCurrentIndex(1);
    editorOrNull->setFocus();
    QApplication::processEvents();
    const QString original = editorOrNull->toPlainText();
    editorOrNull->moveCursor(QTextCursor::End);
    QTest::keyClicks(editorOrNull, QStringLiteral(" "));
    CHECK(workspaceOrNull->IsDirty() == true);
    CHECK(editorOrNull->document()->isUndoAvailable() == false);
    REQUIRE(Action(window, "document.undo")->isEnabled() == true);
    Action(window, "document.undo")->trigger();
    CHECK(editorOrNull->toPlainText() == original);
    REQUIRE(Action(window, "document.redo")->isEnabled() == true);
    Action(window, "document.redo")->trigger();
    CHECK(editorOrNull->toPlainText() == original + QLatin1Char(' '));
    QTest::keySequence(editorOrNull, QKeySequence(QKeySequence::Undo));
    CHECK(editorOrNull->toPlainText() == original);
    QTest::keySequence(editorOrNull, QKeySequence(QKeySequence::Redo));
    CHECK(editorOrNull->toPlainText() == original + QLatin1Char(' '));
    bool hasInvokedContextUndo = false;
    QTimer::singleShot(0, editorOrNull, [&]() {
        QMenu* menuOrNull = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menuOrNull != nullptr)
        {
            for (QAction* action : menuOrNull->actions())
            {
                if (action->text() == QStringLiteral("Undo") && action->isEnabled() == true)
                {
                    action->trigger();
                    hasInvokedContextUndo = true;
                    break;
                }
            }
            menuOrNull->close();
        }
    });
    const QPoint point(20, 20);
    QContextMenuEvent context(QContextMenuEvent::Mouse, point, editorOrNull->viewport()->mapToGlobal(point));
    QApplication::sendEvent(editorOrNull->viewport(), &context);
    CHECK(hasInvokedContextUndo == true);
    CHECK(workspaceOrNull->IsDirty() == false);
}
