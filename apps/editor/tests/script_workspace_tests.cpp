// Explicit S5 journey uses a real Qt workspace, installed SDK, compiler and
// out-of-process GameHost. Offscreen rendering proves wiring, not GPU output.
#include "internal/controller.h"
#include "internal/main_window.h"
#include "internal/script_workspace.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTimer>

using namespace ludus::editor;
namespace
{
template <typename Predicate>
bool Await(Predicate&& predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (predicate() == false && timer.elapsed() < 90000)
    {
        QTest::qWait(20);
    }
    return predicate();
}
struct PlayCleanup
{
    EditorController& Controller;
    QString Directory;
    ~PlayCleanup()
    {
        Controller.Stop();
        if (Await([&]() { return Controller.PlayState().Phase == PlayPhase::Stopped; }))
        {
            // Generation directories are intentionally sealed while leased.
            // Restore only this test-owned tree's directory permissions after Stop.
            QDirIterator directories(Directory,
                                     QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                                     QDirIterator::Subdirectories);
            while (directories.hasNext())
            {
                (void)QFile::setPermissions(directories.next(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            }
        }
    }
};
void WriteScript(const QString& path, const QByteArray& bytes)
{
    QSaveFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
    REQUIRE(file.commit());
}
QByteArray ReadScript(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
QPushButton* Button(ScriptWorkspace& workspace, const QString& text)
{
    for (auto* button : workspace.findChildren<QPushButton*>())
    {
        if (button->text() == text)
        {
            return button;
        }
    }
    return nullptr;
}
} // namespace
TEST_CASE("script assets retain drafts and reject external overwrite", "[editor][scripts]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid());
    REQUIRE(QDir(project.path()).mkdir(QStringLiteral("behaviors")));
    const auto root = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    const QDir sample(QDir(root).filePath(QStringLiteral("examples/scripted-game")));
    for (const auto& name : {QStringLiteral("ludus.scripts.json"),
                             QStringLiteral("behaviors/package.json"),
                             QStringLiteral("behaviors/contract.json"),
                             QStringLiteral("behaviors/encounter.json"),
                             QStringLiteral("behaviors/encounter.luau")})
    {
        REQUIRE(QFile::copy(sample.filePath(name), project.filePath(name)));
    }
    EditorController controller({});
    ScriptWorkspace workspace(&controller);
    workspace.SetProject(project.path(), project.path(), QStringLiteral("linux-clang-development"));
    auto* nodes = workspace.findChild<QTableWidget*>(QStringLiteral("scriptNodes"));
    REQUIRE(nodes != nullptr);
    ludus::foundation::int32 row = -1;
    for (ludus::foundation::int32 i = 0; i < nodes->rowCount(); ++i)
    {
        if (nodes->item(i, 1)->text() == QStringLiteral("Increment"))
        {
            row = i;
            break;
        }
    }
    REQUIRE(row >= 0);
    nodes->item(row, 2)->setText(QStringLiteral("2"));
    REQUIRE(workspace.Dirty());
    Button(workspace, QStringLiteral("Undo"))->click();
    REQUIRE_FALSE(workspace.Dirty());
    Button(workspace, QStringLiteral("Redo"))->click();
    REQUIRE(workspace.Dirty());
    REQUIRE(workspace.Save());
    nodes->item(row, 2)->setText(QStringLiteral("3"));
    REQUIRE(workspace.Dirty());
    workspace.SetProject(project.path(), project.path(), QStringLiteral("linux-clang-debug"));
    REQUIRE(workspace.Dirty());
    REQUIRE(nodes->item(row, 2)->text() == QStringLiteral("3"));
    const auto file = project.filePath(QStringLiteral("behaviors/encounter.json"));
    const auto external = ReadScript(file) + '\n';
    WriteScript(file, external);
    REQUIRE_FALSE(workspace.Save());
    REQUIRE(workspace.Dirty());
    REQUIRE(ReadScript(file) == external);
    auto* assets = workspace.findChild<QComboBox*>(QStringLiteral("scriptAssets"));
    REQUIRE(assets != nullptr);
    QTimer::singleShot(0, []() {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(dialog != nullptr);
        dialog->button(QMessageBox::Cancel)->click();
    });
    assets->setCurrentIndex(1);
    REQUIRE(assets->currentIndex() == 0);
    REQUIRE(workspace.Dirty());
    QTimer::singleShot(0, []() {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(dialog != nullptr);
        dialog->button(QMessageBox::Discard)->click();
    });
    assets->setCurrentIndex(1);
    REQUIRE_FALSE(workspace.Dirty());
    auto* text = workspace.findChild<QPlainTextEdit*>(QStringLiteral("scriptText"));
    REQUIRE(text != nullptr);
    text->appendPlainText(QStringLiteral("-- draft"));
    REQUIRE(workspace.Dirty());
    Button(workspace, QStringLiteral("Undo"))->click();
    REQUIRE_FALSE(workspace.Dirty());
    Button(workspace, QStringLiteral("Redo"))->click();
    REQUIRE(workspace.Dirty());
}
TEST_CASE("script document actions save pending graph cells and route history", "[editor][scripts]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid());
    REQUIRE(QDir(project.path()).mkdir(QStringLiteral("behaviors")));
    const QDir sample(
        QDir(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)).filePath(QStringLiteral("examples/scripted-game")));
    for (const auto& name : {QStringLiteral("ludus.scripts.json"),
                             QStringLiteral("behaviors/package.json"),
                             QStringLiteral("behaviors/contract.json"),
                             QStringLiteral("behaviors/encounter.json"),
                             QStringLiteral("behaviors/encounter.luau")})
    {
        REQUIRE(QFile::copy(sample.filePath(name), project.filePath(name)));
    }
    ProjectDescriptor descriptor;
    descriptor.Name = QStringLiteral("scripts");
    descriptor.ProviderKind = Provider::Cmake;
    descriptor.SourceDir = QStringLiteral(".");
    descriptor.Preset = QStringLiteral("linux-clang-development");
    descriptor.Target = QStringLiteral("game");
    descriptor.RunCwd = QStringLiteral(".");
    const auto path = project.filePath(QStringLiteral("ludus.project.json"));
    WriteScript(path, SerializeDescriptor(descriptor));
    EditorController controller({});
    MainWindow window(&controller, nullptr, project.filePath(QStringLiteral("layout.ini")));
    controller.OpenProject(path);
    window.show();
    window.activateWindow();
    REQUIRE(Await([&]() { return controller.Caps().CanEdit; }));
    auto* workspace = window.findChild<ScriptWorkspace*>();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("workspaceTabs"));
    REQUIRE(workspace != nullptr);
    REQUIRE(tabs != nullptr);
    tabs->setCurrentWidget(workspace);
    auto* nodes = workspace->findChild<QTableWidget*>(QStringLiteral("scriptNodes"));
    REQUIRE(nodes != nullptr);
    ludus::foundation::int32 row = -1;
    for (ludus::foundation::int32 i = 0; i < nodes->rowCount(); ++i)
    {
        if (nodes->item(i, 1)->text() == QStringLiteral("Increment"))
        {
            row = i;
            break;
        }
    }
    REQUIRE(row >= 0);
    nodes->setCurrentCell(row, 2);
    nodes->setFocus();
    nodes->editItem(nodes->item(row, 2));
    QTest::qWait(20);
    auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
    REQUIRE(edit != nullptr);
    edit->selectAll();
    QTest::keyClicks(edit, QStringLiteral("4"));
    auto* save = window.findChild<QAction*>(QStringLiteral("document.save"));
    REQUIRE(save != nullptr);
    REQUIRE(save->isEnabled());
    save->trigger();
    REQUIRE_FALSE(workspace->Dirty());
    REQUIRE(nodes->item(row, 2)->text() == QStringLiteral("4"));
    nodes->setFocus();
    QTest::qWait(20);
    auto* undo = window.findChild<QAction*>(QStringLiteral("document.undo"));
    auto* redo = window.findChild<QAction*>(QStringLiteral("document.redo"));
    REQUIRE(undo != nullptr);
    REQUIRE(redo != nullptr);
    REQUIRE(undo->isEnabled());
    undo->trigger();
    REQUIRE(workspace->Dirty());
    REQUIRE(nodes->item(row, 2)->text() == QStringLiteral("event"));
    REQUIRE(redo->isEnabled());
    redo->trigger();
    REQUIRE_FALSE(workspace->Dirty());
    REQUIRE(nodes->item(row, 2)->text() == QStringLiteral("4"));
}
TEST_CASE("editor cooks debugs and reloads installed SDK behavior generations", "[.script-journey]")
{
    REQUIRE(qEnvironmentVariable("LUDUS_SDK_PREFIX").isEmpty() == false);
    QTemporaryDir project;
    REQUIRE(project.isValid());
    REQUIRE(QDir(project.path()).mkdir(QStringLiteral("src")));
    REQUIRE(QDir(project.path()).mkdir(QStringLiteral("behaviors")));
    const auto root = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    const QDir sample(QDir(root).filePath(QStringLiteral("examples/scripted-game")));
    for (const auto& name : {QStringLiteral("CMakeLists.txt"),
                             QStringLiteral("CMakePresets.json"),
                             QStringLiteral("ludus.project.json"),
                             QStringLiteral("ludus.lock.json"),
                             QStringLiteral("ludus.play.json"),
                             QStringLiteral("ludus.scripts.json"),
                             QStringLiteral("src/game.cpp"),
                             QStringLiteral("behaviors/contract.json"),
                             QStringLiteral("behaviors/package.json"),
                             QStringLiteral("behaviors/encounter.json"),
                             QStringLiteral("behaviors/encounter.luau")})
    {
        REQUIRE(QFile::copy(sample.filePath(name), project.filePath(name)));
    }
    const auto descriptor = project.filePath(QStringLiteral("ludus.project.json"));
    auto settings = QJsonDocument::fromJson(ReadScript(descriptor)).object();
    settings.insert(QStringLiteral("run"),
                    QJsonObject{{QStringLiteral("cwd"), QStringLiteral(".")},
                                {QStringLiteral("args"), QJsonArray{QStringLiteral("--headless")}}});
    WriteScript(descriptor, QJsonDocument(settings).toJson());
    EditorController controller({QDir(root).filePath(QStringLiteral("out/host-tools/venv/bin/python")),
                                 QDir(root).filePath(QStringLiteral("scripts/python/editor_tool.py")),
                                 root});
    PlayCleanup cleanup{controller, project.path()};
    ScriptWorkspace workspace(&controller);
    workspace.resize(1100, 850);
    workspace.show();
    controller.OpenProject(descriptor);
    REQUIRE(Await([&]() { return controller.State().Busy() == false && controller.Caps().CanProjectSetup; }));
    REQUIRE_FALSE(QFile::exists(project.filePath(QStringLiteral("CMakeUserPresets.json"))));
    controller.SetupProject(qEnvironmentVariable("LUDUS_SDK_PREFIX"), {}, false);
    REQUIRE(Await([&]() { return controller.State().Busy() == false && controller.Caps().CanProjectSetup; }));
    INFO(controller.JobDetails().toStdString());
    INFO(controller.Log().Text().toStdString());
    REQUIRE(controller.State().Result.Kind == Outcome::Success);
    workspace.SetProject(project.path(), project.path(), QStringLiteral("linux-clang-development"));
    controller.CookScripts();
    REQUIRE(Await([&]() { return controller.State().Busy() == false && controller.CanPlay(); }));
    INFO(controller.State().Result.Message.toStdString());
    INFO(controller.Log().Text().toStdString());
    REQUIRE(controller.State().Result.Kind == Outcome::Success);
    controller.Play();
    const bool running = Await([&]() {
        return controller.PlayState().Phase == PlayPhase::Running ||
               (controller.PlayState().Phase == PlayPhase::Stopped &&
                controller.State().Result.Kind == Outcome::Failed && controller.State().Busy() == false);
    });
    INFO(controller.JobDetails().toStdString());
    INFO(controller.Log().Text().toStdString());
    REQUIRE(running);
    REQUIRE(controller.PlayState().Phase == PlayPhase::Running);
    workspace.Send(QStringLiteral("inspect"));
    REQUIRE(Await([&]() { return controller.PlayState().ScriptStatus.isEmpty() == false; }));
    auto* nodes = workspace.findChild<QTableWidget*>(QStringLiteral("scriptNodes"));
    REQUIRE(nodes != nullptr);
    ludus::foundation::int32 row = -1;
    for (ludus::foundation::int32 i = 0; i < nodes->rowCount(); ++i)
    {
        if (nodes->item(i, 1)->text() == QStringLiteral("Increment"))
        {
            row = i;
            break;
        }
    }
    REQUIRE(row >= 0);
    nodes->setCurrentCell(row, 2);
    REQUIRE(Button(workspace, QStringLiteral("Set breakpoint"))->isEnabled());
    Button(workspace, QStringLiteral("Set breakpoint"))->click();
    REQUIRE(Await([&]() { return controller.PlayState().ScriptBusy == false; }));
    workspace.Send(QStringLiteral("interact"));
    REQUIRE(Await([&]() { return controller.PlayState().ScriptPaused; }));
    REQUIRE(controller.PlayState().ScriptStatus.value(QStringLiteral("interactions")).toInt() == 0);
    REQUIRE(controller.PlayState().ScriptStatus.value(QStringLiteral("applied")).toInt() == 0);
    REQUIRE_FALSE(controller.CanBuildReload());
    const auto oldStop = controller.PlayState().ScriptStatus.value(QStringLiteral("stop"));
    workspace.Send(QStringLiteral("into"));
    REQUIRE(Await([&]() { return controller.PlayState().ScriptStatus.value(QStringLiteral("stop")) != oldStop; }));
    REQUIRE(controller.PlayState().ScriptPaused);
    REQUIRE(controller.PlayState().ScriptStatus.value(QStringLiteral("frames")).toArray().isEmpty() == false);
    REQUIRE(workspace.grab().save(QDir(root).filePath(QStringLiteral("out/s5-workspace.png"))));
    nodes->setCurrentCell(row, 2);
    Button(workspace, QStringLiteral("Clear breakpoint"))->click();
    REQUIRE(Await([&]() { return controller.PlayState().ScriptBusy == false; }));
    workspace.Send(QStringLiteral("continue"));
    REQUIRE(Await([&]() { return controller.PlayState().ScriptPaused == false; }));
    REQUIRE(controller.PlayState().ScriptStatus.value(QStringLiteral("interactions")).toInt() == 1);
    const auto beforeRestart = controller.PlayState().ScriptStatus.value(QStringLiteral("execution"));
    workspace.Send(QStringLiteral("restart"));
    REQUIRE(Await(
        [&]() { return controller.PlayState().ScriptStatus.value(QStringLiteral("execution")) != beforeRestart; }));
    REQUIRE(controller.PlayState().ScriptStatus.value(QStringLiteral("interactions")).toInt() == 0);
    workspace.Send(QStringLiteral("interact"));
    REQUIRE(Await(
        [&]() { return controller.PlayState().ScriptStatus.value(QStringLiteral("interactions")).toInt() == 1; }));
    const auto generation = controller.PlayState().Generation;
    const auto execution = controller.PlayState().ScriptStatus.value(QStringLiteral("execution"));
    nodes->item(row, 2)->setText(QStringLiteral("2"));
    REQUIRE(workspace.Dirty());
    REQUIRE(workspace.Save());
    REQUIRE_FALSE(Button(workspace, QStringLiteral("Set breakpoint"))->isEnabled());
    controller.CookScripts();
    REQUIRE(Await([&]() { return controller.State().Busy() == false && controller.CanBuildReload(); }));
    REQUIRE(controller.State().Result.Kind == Outcome::Success);
    REQUIRE_FALSE(Button(workspace, QStringLiteral("Set breakpoint"))->isEnabled());
    const auto textPath = project.filePath(QStringLiteral("behaviors/encounter.luau"));
    const auto text = ReadScript(textPath);
    WriteScript(textPath, QByteArray("--!strict\nreturn broken syntax\n"));
    controller.BuildReload();
    REQUIRE(Await([&]() { return controller.State().Busy() == false && controller.CanBuildReload(); }));
    REQUIRE(controller.State().Result.Kind == Outcome::Failed);
    REQUIRE(controller.PlayState().Generation == generation);
    WriteScript(textPath, text);
    controller.BuildReload();
    const bool reloaded =
        Await([&]() { return controller.PlayState().Generation != generation && controller.CanBuildReload(); });
    INFO(controller.JobDetails().toStdString());
    INFO(controller.Log().Text().toStdString());
    REQUIRE(reloaded);
    workspace.Send(QStringLiteral("inspect"));
    REQUIRE(Await([&]() { return controller.PlayState().ScriptStatus.isEmpty() == false; }));
    REQUIRE(controller.PlayState().ScriptStatus.value(QStringLiteral("execution")) != execution);
    REQUIRE(controller.PlayState().ScriptStatus.value(QStringLiteral("interactions")).toInt() == 1);
    REQUIRE(Button(workspace, QStringLiteral("Set breakpoint"))->isEnabled());
    workspace.Send(QStringLiteral("interact"));
    REQUIRE(Await(
        [&]() { return controller.PlayState().ScriptStatus.value(QStringLiteral("interactions")).toInt() == 3; }));
    auto stale = controller.PlayState().ScriptStatus;
    QJsonObject command{{QStringLiteral("version"), 1},
                        {QStringLiteral("action"), QStringLiteral("continue")},
                        {QStringLiteral("session"), stale.value(QStringLiteral("session"))},
                        {QStringLiteral("execution"), execution},
                        {QStringLiteral("stop"), oldStop}};
    controller.ScriptCommand(command);
    REQUIRE(Await([&]() { return controller.PlayState().ScriptMessage.startsWith(QStringLiteral("InvalidRequest")); }));
    REQUIRE(controller.PlayState().ScriptStatus.value(QStringLiteral("interactions")).toInt() == 3);
    auto* assets = workspace.findChild<QComboBox*>(QStringLiteral("scriptAssets"));
    auto* source = workspace.findChild<QPlainTextEdit*>(QStringLiteral("scriptText"));
    REQUIRE(assets != nullptr);
    REQUIRE(source != nullptr);
    assets->setCurrentIndex(1);
    auto cursor = source->textCursor();
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, 3);
    source->setTextCursor(cursor);
    REQUIRE(Button(workspace, QStringLiteral("Set breakpoint"))->isEnabled());
    Button(workspace, QStringLiteral("Set breakpoint"))->click();
    REQUIRE(Await([&]() { return controller.PlayState().ScriptBusy == false; }));
    workspace.Send(QStringLiteral("interact"));
    REQUIRE(Await([&]() { return controller.PlayState().ScriptPaused; }));
    REQUIRE(controller.PlayState().ScriptStatus.value(QStringLiteral("interactions")).toInt() == 3);
    REQUIRE_FALSE(source->extraSelections().isEmpty());
    source->appendPlainText(QStringLiteral("-- draft while paused"));
    REQUIRE(source->extraSelections().isEmpty());
    Button(workspace, QStringLiteral("Undo"))->click();
    REQUIRE_FALSE(workspace.Dirty());
    REQUIRE_FALSE(source->extraSelections().isEmpty());
    workspace.Send(QStringLiteral("restart"));
    REQUIRE(Await([&]() { return controller.PlayState().ScriptPaused == false; }));
    REQUIRE(controller.PlayState().ScriptStatus.value(QStringLiteral("interactions")).toInt() == 0);
    controller.Stop();
    REQUIRE(Await([&]() { return controller.PlayState().Phase == PlayPhase::Stopped && controller.CanPlay(); }));
    REQUIRE(controller.PlayState().ScriptStatus.isEmpty());
    REQUIRE(
        controller.Log().Text().contains(QStringLiteral("S5 behavior VM retired before native module lease release")));
}
