#include "internal/controller.h"
#include "internal/main_window.h"
#include "internal/path_text.h"
#include "internal/recent_project_delegate.h"
#include "internal/recent_projects.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace ludus::editor;

namespace
{
// Upper bound for a recent-project open to publish its descriptor (QTRY_COMPARE's
// former default); the wait returns as soon as the state changes.
constexpr ludus::foundation::int32 OPEN_WAIT_MILLISECONDS = 5000;

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
    // A QtTest QTRY_* macro returns silently on failure inside a Catch2 case;
    // report the wait through Catch2 instead.
    REQUIRE(QTest::qWaitFor([&restarted, &first] { return restarted.State().DescriptorPath == first; },
                            OPEN_WAIT_MILLISECONDS));
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
    CHECK(status->text().contains(QStringLiteral("Last result:")));
    WriteProject(directory, QStringLiteral("game.json"), QByteArrayLiteral("Restored"));
    clear->trigger();
    CHECK(list->count() == 0);
    CHECK_FALSE(open->isEnabled());
    CHECK(QFile::exists(path));
    RecentProjectStore restored(history);
    REQUIRE(restored.Load());
    CHECK(restored.Entries().isEmpty());
}

TEST_CASE("A missing recent project is dimmed and can be removed without touching other entries", "[editor][recent]")
{
    QTemporaryDir directory;
    const QString history = directory.filePath(QStringLiteral("recent.json"));
    const QString kept = WriteProject(directory, QStringLiteral("kept.json"), QByteArrayLiteral("Kept"));
    const QString gone = WriteProject(directory, QStringLiteral("gone.json"), QByteArrayLiteral("Gone"));
    EditorController previous(NoTooling(), nullptr, history);
    previous.OpenProject(kept);
    previous.OpenProject(gone);
    REQUIRE(QFile::remove(gone));
    EditorController controller(NoTooling(), nullptr, history);
    MainWindow window(&controller);
    QListWidget* list = window.findChild<QListWidget*>(QStringLiteral("recentProjectsList"));
    QPushButton* remove = window.findChild<QPushButton*>(QStringLiteral("removeRecentProjectButton"));
    REQUIRE(list != nullptr);
    REQUIRE(remove != nullptr);
    REQUIRE(list->count() == 2);
    // The missing project is the most recent, yet it follows the one that can open, in its own section.
    const QListWidgetItem* present = list->item(0);
    const QListWidgetItem* missing = list->item(1);
    CHECK(missing->text().contains(QStringLiteral("(missing)")));
    CHECK(missing->foreground().color() == window.palette().color(QPalette::Disabled, QPalette::Text));
    CHECK(present->foreground().style() == Qt::NoBrush);
    list->setCurrentRow(1);
    REQUIRE(remove->isEnabled() == true);
    remove->click();
    REQUIRE(list->count() == 1);
    CHECK(list->item(0)->data(Qt::UserRole).toString() == QDir::cleanPath(kept));
    CHECK(QFile::exists(kept));
    RecentProjectStore restored(history);
    REQUIRE(restored.Load());
    REQUIRE(restored.Entries().size() == 1);
    CHECK(restored.Entries().first().DescriptorPath == QDir::cleanPath(kept));
}

TEST_CASE("Recent projects lead with their names and group missing ones under a section that clears at once",
          "[editor][recent]")
{
    QTemporaryDir directory;
    const QString history = directory.filePath(QStringLiteral("recent.json"));
    const QString kept = WriteProject(directory, QStringLiteral("kept.json"), QByteArrayLiteral("Kept"));
    const QString firstGone =
        WriteProject(directory, QStringLiteral("first-gone.json"), QByteArrayLiteral("First Gone"));
    const QString secondGone =
        WriteProject(directory, QStringLiteral("second-gone.json"), QByteArrayLiteral("Second Gone"));
    {
        EditorController previous(NoTooling(), nullptr, history);
        previous.OpenProject(firstGone);
        previous.OpenProject(kept);
        previous.OpenProject(secondGone);
    }
    REQUIRE(QFile::remove(firstGone));
    REQUIRE(QFile::remove(secondGone));
    EditorController controller(NoTooling(), nullptr, history);
    MainWindow window(&controller);
    QListWidget* list = window.findChild<QListWidget*>(QStringLiteral("recentProjectsList"));
    QPushButton* removeMissing = window.findChild<QPushButton*>(QStringLiteral("removeMissingRecentProjectsButton"));
    REQUIRE(list != nullptr);
    REQUIRE(removeMissing != nullptr);
    CHECK(qobject_cast<RecentProjectDelegate*>(list->itemDelegate()) != nullptr);
    REQUIRE(list->count() == 3);

    // The project that opens leads; the missing ones keep their most-recent order after it.
    CHECK(list->item(0)->data(RECENT_PROJECT_NAME_ROLE).toString() == QStringLiteral("Kept"));
    CHECK(list->item(0)->data(RECENT_PROJECT_IS_MISSING_ROLE).toBool() == false);
    CHECK(list->item(0)->data(RECENT_PROJECT_STARTS_MISSING_SECTION_ROLE).toBool() == false);
    CHECK(list->item(1)->data(RECENT_PROJECT_NAME_ROLE).toString() == QStringLiteral("Second Gone"));
    CHECK(list->item(1)->data(RECENT_PROJECT_IS_MISSING_ROLE).toBool() == true);
    CHECK(list->item(1)->data(RECENT_PROJECT_STARTS_MISSING_SECTION_ROLE).toBool() == true);
    CHECK(list->item(2)->data(RECENT_PROJECT_NAME_ROLE).toString() == QStringLiteral("First Gone"));
    CHECK(list->item(2)->data(RECENT_PROJECT_STARTS_MISSING_SECTION_ROLE).toBool() == false);

    // The heading rides on the first missing row, which is taller by it; the rows keep one line for the name and one
    // for the path.
    QStyleOptionViewItem option;
    option.initFrom(list);
    const QSize presentSize = list->itemDelegate()->sizeHint(option, list->model()->index(0, 0));
    const QSize headingSize = list->itemDelegate()->sizeHint(option, list->model()->index(1, 0));
    const QSize missingSize = list->itemDelegate()->sizeHint(option, list->model()->index(2, 0));
    CHECK(presentSize.height() == missingSize.height());
    CHECK(headingSize.height() > missingSize.height());
    CHECK(presentSize.height() >= QFontMetrics(list->font()).height() * 2);

    // The File menu keeps the plain most-recent order.
    QMenu* recentMenu = window.findChild<QMenu*>(QStringLiteral("recentProjectsMenu"));
    REQUIRE(recentMenu != nullptr);
    REQUIRE(recentMenu->actions().size() == 3);
    CHECK(recentMenu->actions().first()->data().toString() == QDir::cleanPath(secondGone));

    REQUIRE(removeMissing->isHidden() == false);
    CHECK(removeMissing->text() == QStringLiteral("Remove Missing (2)"));
    REQUIRE(removeMissing->isEnabled() == true);
    removeMissing->click();
    REQUIRE(list->count() == 1);
    CHECK(list->item(0)->data(Qt::UserRole).toString() == QDir::cleanPath(kept));
    CHECK(removeMissing->isHidden() == true);
    CHECK(QFile::exists(kept));
    RecentProjectStore restored(history);
    REQUIRE(restored.Load());
    REQUIRE(restored.Entries().size() == 1);
    CHECK(restored.Entries().first().DescriptorPath == QDir::cleanPath(kept));
}

TEST_CASE("With no project open, Project Explorer and Properties say so and offer the way in", "[editor][recent]")
{
    QTemporaryDir directory;
    const QString path = WriteProject(directory, QStringLiteral("game.json"), QByteArrayLiteral("Game"));
    EditorController controller(NoTooling(), nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.show();
    QApplication::processEvents();
    QStackedWidget* explorerPages = window.findChild<QStackedWidget*>(QStringLiteral("projectExplorerPages"));
    QWidget* explorer = window.findChild<QWidget*>(QStringLiteral("projectExplorer"));
    QWidget* noProject = window.findChild<QWidget*>(QStringLiteral("projectExplorerNoProject"));
    QWidget* noProperties = window.findChild<QWidget*>(QStringLiteral("propertiesEmpty"));
    REQUIRE(explorerPages != nullptr);
    REQUIRE(explorer != nullptr);
    REQUIRE(noProject != nullptr);
    REQUIRE(noProperties != nullptr);
    QLabel* explorerMessage = noProject->findChild<QLabel*>(QStringLiteral("stateMessage"));
    QPushButton* openAction = noProject->findChild<QPushButton*>(QStringLiteral("stateAction"));
    QLabel* propertiesMessage = noProperties->findChild<QLabel*>(QStringLiteral("stateMessage"));
    REQUIRE(explorerMessage != nullptr);
    REQUIRE(openAction != nullptr);
    REQUIRE(propertiesMessage != nullptr);

    CHECK(explorerPages->currentWidget()->isAncestorOf(noProject) == true);
    CHECK(noProject->isVisible() == true);
    CHECK(explorerMessage->text() == QStringLiteral("No project is open"));
    CHECK(openAction->isHidden() == false);
    CHECK(openAction->text() == QStringLiteral("Open Project..."));
    CHECK(noProperties->isVisible() == true);
    CHECK(propertiesMessage->text() == QStringLiteral("No project is open"));

    controller.OpenProject(path);
    REQUIRE(controller.State().Document == DocumentState::ProjectLoaded);
    QApplication::processEvents();
    CHECK(explorerPages->currentWidget() == explorer);
    CHECK(explorer->isVisible() == true);
    CHECK(propertiesMessage->text() == QStringLiteral("Nothing selected"));

    REQUIRE(controller.CloseProject() == true);
    QApplication::processEvents();
    CHECK(explorerPages->currentWidget()->isAncestorOf(noProject) == true);
    CHECK(propertiesMessage->text() == QStringLiteral("No project is open"));
}

TEST_CASE("The Start page says why New Project is unavailable", "[editor][recent]")
{
    QTemporaryDir directory;
    const QString path = WriteProject(directory, QStringLiteral("game.json"), QByteArrayLiteral("Game"));
    EditorController controller(NoTooling(), nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller);
    QPushButton* button = window.findChild<QPushButton*>(QStringLiteral("welcomeNewProject"));
    QLabel* reason = window.findChild<QLabel*>(QStringLiteral("welcomeNewProjectReason"));
    REQUIRE(button != nullptr);
    REQUIRE(reason != nullptr);
    CHECK(button->isEnabled() == true);
    CHECK(reason->isHidden() == true);
    CHECK(button->toolTip().isEmpty() == true);

    controller.OpenProject(path);
    ProjectDescriptor draft = controller.State().Draft;
    draft.Name = QStringLiteral("Unsaved edit");
    controller.EditDraft(draft);
    CHECK(button->isEnabled() == false);
    CHECK(reason->isHidden() == false);
    CHECK(reason->text().contains(QStringLiteral("unsaved changes")));
    CHECK(button->toolTip() == reason->text());
}

TEST_CASE("Recent projects that share a name show the folder that tells them apart", "[editor][recent]")
{
    // Worktrees of one repository: the project folder is the same, the worktree folder differs.
    const QStringList names = {QStringLiteral("Dolomites Survival"),
                               QStringLiteral("Dolomites Survival"),
                               QStringLiteral("dolomites survival"),
                               QStringLiteral("Knife Edge")};
    const QStringList paths = {QStringLiteral("/work/Ludus/Samples/Dolomites/project.json"),
                               QStringLiteral("/work/milestone-2/Samples/Dolomites/project.json"),
                               QStringLiteral("/work/milestone-1/Samples/Dolomites/project.json"),
                               QStringLiteral("/work/Ludus/Samples/KnifeEdge/project.json")};
    const QStringList qualifiers = GetRecentProjectQualifiers(names, paths);
    REQUIRE(qualifiers.size() == names.size());
    CHECK(qualifiers.at(0) == QStringLiteral("Ludus"));
    CHECK(qualifiers.at(1) == QStringLiteral("milestone-2"));
    CHECK(qualifiers.at(2) == QStringLiteral("milestone-1"));
    // A name nobody else uses needs no qualifier.
    CHECK(qualifiers.at(3).isEmpty());

    // One folder is enough when the project folders already differ.
    const QStringList siblings = GetRecentProjectQualifiers(
        {QStringLiteral("Demo"), QStringLiteral("Demo")},
        {QStringLiteral("/work/a/Demo/project.json"), QStringLiteral("/work/a/Demo copy/project.json")});
    CHECK(siblings == QStringList({QStringLiteral("Demo"), QStringLiteral("Demo copy")}));

    // When no one folder differs by itself, the trailing folders down from the first that differs do.
    const QStringList crossed =
        GetRecentProjectQualifiers({QStringLiteral("Sample"), QStringLiteral("Sample"), QStringLiteral("Sample")},
                                   {QStringLiteral("/a/repro/Sample/project.json"),
                                    QStringLiteral("/b/repro/Sample/project.json"),
                                    QStringLiteral("/a/other/Sample/project.json")});
    CHECK(crossed ==
          QStringList(
              {QStringLiteral("a/repro/Sample"), QStringLiteral("b/repro/Sample"), QStringLiteral("a/other/Sample")}));
}

TEST_CASE("The start page qualifies recent projects that share a name", "[editor][recent]")
{
    QTemporaryDir directory;
    const QString history = directory.filePath(QStringLiteral("recent.json"));
    QStringList paths;
    for (const QString& worktree : {QStringLiteral("milestone-1"), QStringLiteral("milestone-2")})
    {
        REQUIRE(QDir(directory.path()).mkpath(worktree + QStringLiteral("/Dolomites")));
        paths.append(WriteProject(directory,
                                  worktree + QStringLiteral("/Dolomites/project.json"),
                                  QByteArrayLiteral("Dolomites Survival")));
    }
    const QString unique = WriteProject(directory, QStringLiteral("kept.json"), QByteArrayLiteral("Kept"));
    {
        EditorController previous(NoTooling(), nullptr, history);
        previous.OpenProject(unique);
        previous.OpenProject(paths.at(0));
        previous.OpenProject(paths.at(1));
    }
    EditorController controller(NoTooling(), nullptr, history);
    MainWindow window(&controller);
    QListWidget* list = window.findChild<QListWidget*>(QStringLiteral("recentProjectsList"));
    REQUIRE(list != nullptr);
    REQUIRE(list->count() == 3);
    // Most recent first: milestone-2, milestone-1, then the project nobody shares a name with.
    CHECK(list->item(0)->data(RECENT_PROJECT_QUALIFIER_ROLE).toString() == QStringLiteral("milestone-2"));
    CHECK(list->item(1)->data(RECENT_PROJECT_QUALIFIER_ROLE).toString() == QStringLiteral("milestone-1"));
    CHECK(list->item(2)->data(RECENT_PROJECT_QUALIFIER_ROLE).toString().isEmpty());
    // Assistive technology reads the qualifier too, so the rows do not sound alike either.
    CHECK(list->item(0)->text().startsWith(QStringLiteral("Dolomites Survival") + GetDocumentQualifierSeparator() +
                                           QStringLiteral("milestone-2")));
    CHECK(list->item(2)->text().startsWith(QStringLiteral("Kept\n")));
}
