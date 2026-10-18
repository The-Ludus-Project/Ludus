#include "internal/controller.h"
#include "internal/main_window.h"
#include "internal/recent_projects.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace ludus::editor;

namespace
{
QString WriteProject(QTemporaryDir& directory, const QString& filename, const QByteArray& name)
{
    const QString path = directory.filePath(filename);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    QByteArray bytes = QByteArrayLiteral(R"json({"version":1,"name":"demo","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json");
    bytes.replace("demo", name);
    REQUIRE(file.write(bytes) == bytes.size());
    return path;
}

ToolingPaths NoTooling()
{
    return {};
}
} // namespace

TEST_CASE("Editor branding is embedded and decodes without external artwork files", "[editor][brand]")
{
    QTemporaryDir directory;
    EditorController controller(NoTooling(), nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    CHECK_FALSE(window.windowIcon().pixmap(32, 32).isNull());
    const auto* logo = window.findChild<QLabel*>(QStringLiteral("welcomeLogo"));
    REQUIRE(logo != nullptr);
    CHECK_FALSE(logo->pixmap().isNull());
}

TEST_CASE("Recent projects persist in bounded most-recent order without duplicate paths", "[editor][recent]")
{
    QTemporaryDir directory;
    const QString history = directory.filePath(QStringLiteral("preferences/recent.json"));
    RecentProjectStore store(history);
    REQUIRE(store.Load());
    for (int index = 0; index < 12; ++index)
    {
        REQUIRE(store.Remember(
        {
            .DescriptorPath = directory.filePath(QStringLiteral("%1.json").arg(index)),
            .Name = QStringLiteral("Game %1").arg(index),
        }));
    }
    REQUIRE(store.Entries().size() == 10);
    CHECK(store.Entries().first().Name == QStringLiteral("Game 11"));
    CHECK(store.Entries().last().Name == QStringLiteral("Game 2"));
    const QString reopened = directory.filePath(QStringLiteral("./5.json"));
    REQUIRE(store.Remember({ .DescriptorPath = reopened, .Name = QStringLiteral("Renamed & game 🎮") }));
    RecentProjectStore restored(history);
    REQUIRE(restored.Load());
    REQUIRE(restored.Entries().size() == 10);
    CHECK(restored.Entries().first().DescriptorPath == directory.filePath(QStringLiteral("5.json")));
    CHECK(restored.Entries().first().Name == QStringLiteral("Renamed & game 🎮"));
    REQUIRE(restored.Clear());
    REQUIRE(store.Load());
    CHECK(store.Entries().isEmpty());
}

TEST_CASE("Malformed or oversized recent history is recoverable and invalid entries are skipped", "[editor][recent]")
{
    QTemporaryDir directory;
    const QString history = directory.filePath(QStringLiteral("recent.json"));
    QFile file(history);
    REQUIRE(file.open(QIODevice::WriteOnly));
    SECTION("Malformed JSON")
    {
        file.write("{broken");
    }
    SECTION("Oversized history")
    {
        file.write(QByteArray(256 * 1024 + 1, ' '));
    }
    SECTION("Wrong root type")
    {
        file.write("{}");
    }
    file.close();
    RecentProjectStore store(history);
    CHECK_FALSE(store.Load());
    CHECK(store.Entries().isEmpty());
    const QString project = directory.filePath(QStringLiteral("project.json"));
    REQUIRE(store.Remember({ .DescriptorPath = project, .Name = QStringLiteral("Recovery") }));
    REQUIRE(store.Load());
    CHECK(store.Entries().size() == 1);

    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QByteArrayLiteral("[null,{\"path\":\"relative.json\",\"name\":\"bad\"},"
                                 "{\"path\":\"/absolute.json\",\"name\":\"good\"},"
                                 "{\"path\":\"/./absolute.json\",\"name\":\"duplicate\"}]"));
    file.close();
    REQUIRE(store.Load());
    REQUIRE(store.Entries().size() == 1);
    CHECK(store.Entries().first().Name == QStringLiteral("good"));
}

TEST_CASE("History write failures preserve session history and do not prevent opening a project", "[editor][recent]")
{
    QTemporaryDir directory;
    const QString blocked = directory.filePath(QStringLiteral("history-is-a-directory"));
    REQUIRE(QDir().mkpath(blocked));
    RecentProjectStore store(blocked);
    CHECK_FALSE(store.Remember(
    {
        .DescriptorPath = directory.filePath(QStringLiteral("game.json")),
        .Name = QStringLiteral("Game"),
    }));
    CHECK(store.Entries().size() == 1);
    CHECK_FALSE(store.Clear());
    CHECK(store.Entries().isEmpty());
    CHECK(QDir(blocked).exists());

    EditorController controller(NoTooling(), nullptr, blocked);
    const QString path = WriteProject(directory, QStringLiteral("game.json"), QByteArrayLiteral("Game"));
    controller.OpenProject(path);
    CHECK(controller.State().Document == DocumentState::ProjectLoaded);
    CHECK(controller.State().Result.Kind == Outcome::None);
    REQUIRE(controller.RecentProjects().size() == 1);
    CHECK(controller.RecentProjects().first().DescriptorPath == path);
}

TEST_CASE("Only successful project opens enter history and saved names refresh it", "[editor][recent]")
{
    QTemporaryDir directory;
    const QString history = directory.filePath(QStringLiteral("recent.json"));
    const QString first = WriteProject(directory, QStringLiteral("first.json"), QByteArrayLiteral("First"));
    const QString second = WriteProject(directory, QStringLiteral("second.json"), QByteArrayLiteral("Second"));
    EditorController controller(NoTooling(), nullptr, history);
    controller.OpenProject(first);
    controller.OpenProject(second);
    controller.OpenProject(directory.filePath(QStringLiteral("missing.json")));
    REQUIRE(controller.RecentProjects().size() == 2);
    CHECK(controller.RecentProjects().first().DescriptorPath == second);
    CHECK(controller.State().DescriptorPath == second);
    ProjectDescriptor draft = controller.State().Draft;
    draft.Name = QStringLiteral("Renamed");
    controller.EditDraft(draft);
    CHECK(controller.RecentProjects().first().Name == QStringLiteral("Second"));
    controller.Save();
    CHECK(controller.RecentProjects().first().Name == QStringLiteral("Renamed"));

    EditorController restarted(NoTooling(), nullptr, history);
    CHECK(restarted.State().Document == DocumentState::NoProject);
    REQUIRE(restarted.RecentProjects().size() == 2);
    MainWindow window(&restarted);
    auto* menu = window.findChild<QMenu*>(QStringLiteral("recentProjectsMenu"));
    REQUIRE(menu != nullptr);
    REQUIRE(menu->actions().size() == 2);
    menu->actions().last()->trigger();
    QTRY_COMPARE(restarted.State().DescriptorPath, first);
    CHECK(restarted.RecentProjects().first().DescriptorPath == first);
    CHECK(restarted.State().SetupStatus.contains(QStringLiteral("tooling is missing")));
}

TEST_CASE("Recent project opening honors Save Discard Cancel and failed Save", "[editor][recent][widgets]")
{
    QTemporaryDir directory;
    const QString first = WriteProject(directory, QStringLiteral("first.json"), QByteArrayLiteral("First"));
    const QString second = WriteProject(directory, QStringLiteral("second.json"), QByteArrayLiteral("Second"));
    EditorController controller(NoTooling(), nullptr, directory.filePath(QStringLiteral("recent.json")));
    controller.OpenProject(first);
    controller.OpenProject(second);
    ProjectDescriptor draft = controller.State().Draft;
    draft.Name = QStringLiteral("Unsaved edit");
    controller.EditDraft(draft);
    MainWindow window(&controller);
    auto* list = window.findChild<QListWidget*>(QStringLiteral("recentProjectsList"));
    auto* open = window.findChild<QPushButton*>(QStringLiteral("openRecentProjectButton"));
    REQUIRE(list != nullptr);
    REQUIRE(open != nullptr);
    REQUIRE(list->count() == 2);
    list->setCurrentRow(1);

    auto choice = QMessageBox::Cancel;
    bool shouldOpen = false;
    SECTION("Cancel keeps draft and workspace") {}
    SECTION("Discard opens the selected project")
    {
        choice = QMessageBox::Discard;
        shouldOpen = true;
    }
    SECTION("Save commits the old project before opening the selected project")
    {
        choice = QMessageBox::Save;
        shouldOpen = true;
    }
    SECTION("Failed Save aborts opening and retains the draft")
    {
        choice = QMessageBox::Save;
        ProjectStore::FaultHooks hooks;
        hooks.FailCommit = []() { return true; };
        controller.Store().SetFaultHooks(hooks);
    }
    bool promptShown = false;
    QTimer::singleShot(0, &window, [&promptShown, choice]() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
        {
            promptShown = true;
            box->button(choice)->click();
        }
    });
    open->click();
    CHECK(promptShown);
    CHECK(controller.State().DescriptorPath == (shouldOpen ? first : second));
    CHECK(controller.State().Dirty() == (shouldOpen == false));
    CHECK(controller.RecentProjects().first().DescriptorPath == (shouldOpen ? first : second));
    ProjectStore store;
    const auto saved = store.Load(second);
    REQUIRE(saved.Ok());
    CHECK(saved.Parse.Descriptor.Name ==
          (shouldOpen && choice == QMessageBox::Save ? QStringLiteral("Unsaved edit") : QStringLiteral("Second")));
}

TEST_CASE("Missing recent projects report an error and clearing history leaves projects intact", "[editor][recent]")
{
    QTemporaryDir directory;
    const QString history = directory.filePath(QStringLiteral("recent.json"));
    const QString path = WriteProject(directory, QStringLiteral("game.json"), QByteArrayLiteral("Game & Tools"));
    EditorController previous(NoTooling(), nullptr, history);
    previous.OpenProject(path);
    REQUIRE(QFile::remove(path));
    EditorController controller(NoTooling(), nullptr, history);
    MainWindow window(&controller);
    auto* list = window.findChild<QListWidget*>(QStringLiteral("recentProjectsList"));
    auto* open = window.findChild<QPushButton*>(QStringLiteral("openRecentProjectButton"));
    auto* clear = window.findChild<QAction*>(QStringLiteral("clearRecentProjectsAction"));
    REQUIRE(list != nullptr);
    REQUIRE(open != nullptr);
    REQUIRE(clear != nullptr);
    REQUIRE(list->count() == 1);
    CHECK(list->item(0)->text().contains(QStringLiteral("(missing)")));
    open->click();
    CHECK(controller.State().Document == DocumentState::NoProject);
    CHECK(controller.State().Result.Kind == Outcome::Failed);
    CHECK(controller.RecentProjects().size() == 1);
    auto* status = window.findChild<QLabel*>(QStringLiteral("workspaceStatus"));
    REQUIRE(status != nullptr);
    CHECK(status->text().contains(QStringLiteral("last result:")));
    WriteProject(directory, QStringLiteral("game.json"), QByteArrayLiteral("Restored"));
    clear->trigger();
    CHECK(list->count() == 0);
    CHECK_FALSE(open->isEnabled());
    CHECK(QFile::exists(path));
    RecentProjectStore restored(history);
    REQUIRE(restored.Load());
    CHECK(restored.Entries().isEmpty());
}
