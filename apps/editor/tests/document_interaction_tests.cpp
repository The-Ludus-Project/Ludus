#include "internal/code_editor.h"
#include "internal/configuration_workspace.h"
#include "internal/controller.h"
#include "internal/document_well.h"
#include "internal/main_window.h"
#include "internal/renderer_workspace.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
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

TEST_CASE("Project Settings inspects external saves before choosing recovery", "[editor][document][widget]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.ini")));
    const QString path = Project(directory);
    controller.OpenProject(path);
    ShowReady(window, controller);
    QLineEdit* nameOrNull = window.findChild<QLineEdit*>(QStringLiteral("projectName"));
    REQUIRE(nameOrNull != nullptr);
    nameOrNull->setFocus();
    nameOrNull->selectAll();
    QTest::keyClicks(nameOrNull, QStringLiteral("my draft"));
    REQUIRE(controller.State().Draft.Name == QStringLiteral("my draft"));
    REQUIRE(controller.CanUndoProject() == true);

    QByteArray inspectedBytes = controller.Store().Load(path).Bytes;
    inspectedBytes.replace("\"demo\"", "\"external one\"");
    inspectedBytes.replace("\"old\"", "\"new argument\"");
    bool hasValidInspection = true;
    bool shouldCancel = false;
    bool shouldReloadInvalid = false;
    QByteArray latestBytes = inspectedBytes;
    latestBytes.replace("\"external one\"", "\"external two\"");

    SECTION("Cancel keeps a draft and its undo history")
    {
        shouldCancel = true;
        latestBytes = inspectedBytes;
    }
    SECTION("Reload reads a newer revision than the inspected one") {}
    SECTION("Reload retries validation when the inspected bytes are invalid")
    {
        hasValidInspection = false;
        inspectedBytes = QByteArrayLiteral("invalid descriptor");
    }
    SECTION("Reload rejects a second change to invalid bytes and keeps the draft")
    {
        shouldReloadInvalid = true;
        latestBytes = QByteArrayLiteral("invalid second revision");
    }

    const auto writeBytes = [&path](const QByteArray& bytes) {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == true);
        REQUIRE(file.write(bytes) == bytes.size());
    };
    writeBytes(inspectedBytes);
    for (QDockWidget* dock : window.findChildren<QDockWidget*>())
    {
        if (dock->windowTitle() == QStringLiteral("Error List"))
        {
            dock->hide();
        }
    }

    bool hasAttemptedCommit = false;
    ProjectStore::FaultHooks hooks;
    hooks.FailCommit = [&hasAttemptedCommit]() {
        hasAttemptedCommit = true;
        return false;
    };
    controller.Store().SetFaultHooks(hooks);

    bool hasOpenedInspection = false;
    bool hasShownReloadError = false;
    QTimer::singleShot(0, &window, [&]() {
        QDialog* dialogOrNull = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(dialogOrNull != nullptr);
        CHECK(dialogOrNull->objectName() == QStringLiteral("projectSaveConflict"));
        hasOpenedInspection = true;
        QPlainTextEdit* fieldsOrNull =
            dialogOrNull->findChild<QPlainTextEdit*>(QStringLiteral("projectSaveConflictFields"));
        REQUIRE(fieldsOrNull != nullptr);
        CHECK(fieldsOrNull->isReadOnly() == true);
        const QString fields = fieldsOrNull->toPlainText();
        CHECK(fields.contains(QStringLiteral("Saved baseline  \"demo\"")) == true);
        CHECK(fields.contains(QStringLiteral("Pending draft   \"my draft\"")) == true);
        if (hasValidInspection == true)
        {
            CHECK(fields.contains(QStringLiteral("name (changed on disk and in draft)")) == true);
            CHECK(fields.contains(QStringLiteral("On disk         \"external one\"")) == true);
            CHECK(fields.contains(QStringLiteral("run.args")) == true);
            CHECK(fields.contains(QStringLiteral("On disk         [\"new argument\"]")) == true);
            CHECK(fields.contains(QStringLiteral("source_dir")) == false);
        }
        else
        {
            CHECK(fields.contains(QStringLiteral("On disk         (unavailable)")) == true);
        }
        CHECK(controller.Store().Load(path).Bytes == inspectedBytes);
        QDialogButtonBox* buttonsOrNull = dialogOrNull->findChild<QDialogButtonBox*>();
        REQUIRE(buttonsOrNull != nullptr);
        QPushButton* cancelOrNull = buttonsOrNull->button(QDialogButtonBox::Cancel);
        REQUIRE(cancelOrNull != nullptr);
        CHECK(cancelOrNull->isDefault() == true);
        if (shouldCancel == true)
        {
            cancelOrNull->click();
            return;
        }

        writeBytes(latestBytes);
        if (shouldReloadInvalid == true)
        {
            QTimer::singleShot(0, &window, [&hasShownReloadError]() {
                QMessageBox* warningOrNull = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                REQUIRE(warningOrNull != nullptr);
                CHECK(warningOrNull->text().contains(QStringLiteral("Repair or restore the descriptor")) == true);
                CHECK(warningOrNull->text().contains(QStringLiteral("undo history")) == true);
                hasShownReloadError = true;
                warningOrNull->accept();
            });
        }
        QPushButton* reloadOrNull = dialogOrNull->findChild<QPushButton*>(QStringLiteral("projectSaveConflictReload"));
        REQUIRE(reloadOrNull != nullptr);
        reloadOrNull->click();
    });

    QAction* save = Action(window, "document.save");
    REQUIRE(save->isEnabled() == true);
    save->trigger();
    CHECK(hasOpenedInspection == true);
    CHECK(hasAttemptedCommit == false);
    CHECK(hasShownReloadError == shouldReloadInvalid);
    CHECK(controller.Store().Load(path).Bytes == latestBytes);

    if (shouldCancel == true || shouldReloadInvalid == true)
    {
        CHECK(controller.State().Dirty() == true);
        CHECK(controller.State().Saved.Name == QStringLiteral("demo"));
        CHECK(controller.State().Draft.Name == QStringLiteral("my draft"));
        REQUIRE(controller.CanUndoProject() == true);
        controller.UndoProjectEdit();
        CHECK(controller.State().Draft.Name == QStringLiteral("demo"));
        controller.RedoProjectEdit();
        CHECK(controller.State().Draft.Name == QStringLiteral("my draft"));
    }
    else
    {
        CHECK(controller.State().Dirty() == false);
        CHECK(controller.State().Saved.Name == QStringLiteral("external two"));
        CHECK(controller.State().Draft.Name == QStringLiteral("external two"));
        CHECK(controller.CanUndoProject() == false);
    }
}

TEST_CASE("Project Settings Undo restores an unsaved value after exploring an alternative",
          "[editor][document][widget]")
{
    QTemporaryDir directory;
    EditorController controller(MissingTools());
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.ini")));
    const QString path = Project(directory);
    controller.OpenProject(path);
    ShowReady(window, controller);
    const QByteArray savedBytes = controller.Store().Load(path).Bytes;
    QLineEdit* nameOrNull = window.findChild<QLineEdit*>(QStringLiteral("projectName"));
    REQUIRE(nameOrNull != nullptr);
    nameOrNull->setFocus();
    nameOrNull->selectAll();
    QTest::keyClicks(nameOrNull, QStringLiteral("first draft"));
    REQUIRE(controller.State().Draft.Name == QStringLiteral("first draft"));

    // Leave the field to establish a separate project history entry.
    QTest::keyClick(nameOrNull, Qt::Key_Tab);
    REQUIRE(nameOrNull->hasFocus() == false);
    nameOrNull->setFocus();
    nameOrNull->selectAll();
    QTest::keyClicks(nameOrNull, QStringLiteral("alternate draft"));
    REQUIRE(controller.State().Draft.Name == QStringLiteral("alternate draft"));

    QTest::keyClick(nameOrNull, Qt::Key_Tab);
    REQUIRE(nameOrNull->hasFocus() == false);
    QAction* undo = Action(window, "document.undo");
    REQUIRE(undo->isEnabled() == true);
    undo->trigger();
    CHECK(nameOrNull->text() == QStringLiteral("first draft"));
    CHECK(controller.State().Draft.Name == QStringLiteral("first draft"));
    CHECK(controller.State().Dirty() == true);
    CHECK(controller.Store().Load(path).Bytes == savedBytes);
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
    const QByteArray savedBytes = controller.Store().Load(path).Bytes;
    auto draft = controller.State().Draft;
    draft.Name.clear();
    controller.EditDraft(draft);
    controller.Save();
    CHECK(controller.State().Result.Code == ResultCode::InvalidProject);
    CHECK(controller.Store().Load(path).Bytes == savedBytes);
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
    QAbstractButton* advanced = window.findChild<QAbstractButton*>(QStringLiteral("projectAdvanced"));
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
