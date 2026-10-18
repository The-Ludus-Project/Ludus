#include "internal/configuration_workspace.h"
#include "internal/controller.h"
#include "internal/main_window.h"
#include "internal/project_creation_dialog.h"
#include "internal/workspace_style.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace ludus::editor;

namespace
{
QString Project(QTemporaryDir& directory)
{
    QFile file(directory.filePath(QStringLiteral("ludus.project.json")));
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral(R"json({"version":1,"name":"Demo","provider":"cmake",
"source_dir":".","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json"));
    return file.fileName();
}
} // namespace

TEST_CASE("Close Project retires identity and returns to Welcome with recent history", "[editor][lifecycle]")
{
    QTemporaryDir directory;
    EditorController controller({}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.show();
    auto* tabs = window.findChild<QTabWidget*>();
    auto* welcome = window.findChild<QWidget*>(QStringLiteral("projectWelcome"));
    auto* close = window.findChild<QAction*>(QStringLiteral("closeProjectAction"));
    auto* configuration = window.findChild<ConfigurationWorkspace*>(QStringLiteral("configurationWorkspace"));
    REQUIRE(tabs != nullptr);
    REQUIRE(welcome != nullptr);
    REQUIRE(close != nullptr);
    REQUIRE(configuration != nullptr);
    CHECK(tabs->isTabVisible(tabs->indexOf(configuration)));
    CHECK(tabs->currentWidget() == welcome);
    CHECK_FALSE(close->isEnabled());
    const auto path = Project(directory);
    controller.OpenProject(path);
    QApplication::processEvents();
    CHECK_FALSE(welcome->isVisible());
    CHECK(close->isEnabled());
    REQUIRE(configuration->Edit("host.headless", QStringLiteral("true")));
    tabs->setCurrentWidget(configuration);
    const auto epoch = controller.State().ProjectEpoch;
    const auto nextJob = controller.State().NextJob;
    QTest::keyClick(&window, Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::processEvents();
    CHECK(controller.State().Document == DocumentState::NoProject);
    CHECK(controller.State().DescriptorPath.isEmpty());
    CHECK(controller.State().ProjectEpoch == epoch + 1);
    CHECK(controller.State().NextJob == nextJob);
    CHECK(controller.State().SetupStatus.isEmpty());
    CHECK(controller.RecentProjects().first().DescriptorPath == path);
    CHECK(welcome->isVisible());
    CHECK(tabs->currentWidget() == welcome);
    CHECK(tabs->isTabVisible(tabs->indexOf(configuration)));
    CHECK(configuration->Dirty()); // The separately loaded preview remains accessible.
    controller.OpenProject(path);
    CHECK(controller.State().Saved.Name == QStringLiteral("Demo"));
}

TEST_CASE("Close Project confirms dirty settings and preserves them on Cancel or failed Save", "[editor][lifecycle]")
{
    QTemporaryDir directory;
    EditorController controller({}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    const auto path = Project(directory);
    controller.OpenProject(path);
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.show();
    auto draft = controller.State().Draft;
    draft.Name = QStringLiteral("Changed");
    controller.EditDraft(draft);
    CHECK_FALSE(controller.CloseProject()); // API never silently discards.
    auto choice = QMessageBox::Cancel;
    bool closed = false;
    SECTION("Cancel") {}
    SECTION("Discard")
    {
        choice = QMessageBox::Discard;
        closed = true;
    }
    SECTION("Save")
    {
        choice = QMessageBox::Save;
        closed = true;
    }
    SECTION("Failed Save")
    {
        choice = QMessageBox::Save;
        ProjectStore::FaultHooks hooks;
        hooks.FailCommit = []() { return true; };
        controller.Store().SetFaultHooks(hooks);
    }
    bool prompted = false;
    QTimer::singleShot(0, &window, [&prompted, choice]() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
        {
            prompted = true;
            box->button(choice)->click();
        }
    });
    window.findChild<QAction*>(QStringLiteral("closeProjectAction"))->trigger();
    CHECK(prompted);
    CHECK((controller.State().Document == DocumentState::NoProject) == closed);
    if (closed == false)
    {
        CHECK(controller.State().Draft.Name == QStringLiteral("Changed"));
        CHECK(controller.State().Dirty());
    }
    ProjectStore store;
    CHECK(store.Load(path).Parse.Descriptor.Name ==
          (closed && choice == QMessageBox::Save ? QStringLiteral("Changed") : QStringLiteral("Demo")));
}

TEST_CASE("Project switching is unavailable while an operation owns the workspace", "[editor][lifecycle]")
{
    WorkspaceState state;
    state.Document = DocumentState::ProjectLoaded;
    state.HasSaved = true;
    CHECK(ComputeCapabilities(state).CanCloseProject);
    for (const auto phase : {Phase::Building, Phase::Running, Phase::Stopping, Phase::CleanupUnknown})
    {
        state.OperationPhase = phase;
        CHECK_FALSE(ComputeCapabilities(state).CanCloseProject);
    }
}

TEST_CASE("New shortcut opens the creation dialog and Escape cancels without a job", "[editor][lifecycle]")
{
    QTemporaryDir directory;
    EditorController controller({}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.show();
    QApplication::processEvents();
    QTest::keyClick(&window, Qt::Key_N, Qt::ControlModifier);
    QApplication::processEvents();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("newProjectDialog"));
    REQUIRE(dialog != nullptr);
    REQUIRE(dialog->isVisible());
    QTest::keyClick(dialog, Qt::Key_Escape);
    QApplication::processEvents();
    CHECK(controller.State().Document == DocumentState::NoProject);
    CHECK(controller.State().ActiveJob == 0);
}

TEST_CASE("Project creation dark and light captures support visual review", "[editor][visual]")
{
    const auto capture = qEnvironmentVariable("LUDUS_EDITOR_CREATION_CAPTURE");
    if (capture.isEmpty())
    {
        return;
    }
    const auto original = QApplication::palette();
    for (const bool dark : {false, true})
    {
        auto palette = original;
        if (dark)
        {
            palette.setColor(QPalette::Window, QColor(QStringLiteral("#282c30")));
            palette.setColor(QPalette::Base, QColor(QStringLiteral("#202428")));
            palette.setColor(QPalette::Button, QColor(QStringLiteral("#343a40")));
            for (const auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
            {
                palette.setColor(role, QColor(QStringLiteral("#edf1f5")));
            }
            palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#167b83")));
        }
        QApplication::setPalette(palette);
        QTemporaryDir directory;
        EditorController controller({}, nullptr, directory.filePath(QStringLiteral("recent.json")));
        MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
        window.resize(900, 740);
        window.show();
        ProjectCreationDialog dialog(&window);
        dialog.show();
        QApplication::processEvents();
        REQUIRE(window.grab().save(
            capture + (dark ? QStringLiteral("-dark-workspace.png") : QStringLiteral("-light-workspace.png"))));
        REQUIRE(dialog.grab().save(capture + (dark ? QStringLiteral("-dark.png") : QStringLiteral("-light.png"))));
    }
    QApplication::setPalette(original);
}

TEST_CASE("Workspace appearance follows application palette changes", "[editor][appearance]")
{
    const auto original = QApplication::palette();
    QMainWindow workspace;
    ApplyWorkspaceStyle(&workspace);
    workspace.show();
    // ApplicationPaletteChange is dispatched while the application event loop runs.
    QTimer::singleShot(0, &workspace, [&]() {
        auto dark = original;
        dark.setColor(QPalette::Window, QColor(QStringLiteral("#202428")));
        QApplication::setPalette(dark);
        QApplication::processEvents();
        CHECK(workspace.palette().color(QPalette::Window).lightness() < 128);
        CHECK(workspace.palette().color(QPalette::Text).lightness() > 128);
        auto light = original;
        light.setColor(QPalette::Window, QColor(QStringLiteral("#ffffff")));
        QApplication::setPalette(light);
        QApplication::processEvents();
        CHECK(workspace.palette().color(QPalette::Window).lightness() > 128);
        CHECK(workspace.palette().color(QPalette::Text).lightness() < 128);
        QApplication::setPalette(original);
        QApplication::quit();
    });
    QApplication::exec();
}

TEST_CASE("Project overview explains setup and reveals build details without editing", "[editor][lifecycle][overview]")
{
    QTemporaryDir directory;
    EditorController controller({}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.resize(800, 650);
    window.show();
    controller.OpenProject(Project(directory));
    QApplication::processEvents();
    auto* identity = window.findChild<QLabel*>(QStringLiteral("projectIdentity"));
    auto* setup = window.findChild<QLabel*>(QStringLiteral("projectSetupStatus"));
    auto* next = window.findChild<QLabel*>(QStringLiteral("projectNextStep"));
    auto* advanced = window.findChild<QPushButton*>(QStringLiteral("projectAdvanced"));
    auto* details = window.findChild<QWidget*>(QStringLiteral("projectBuildDetails"));
    auto* profile = window.findChild<QComboBox*>(QStringLiteral("projectProfile"));
    REQUIRE(identity != nullptr);
    REQUIRE(setup != nullptr);
    REQUIRE(next != nullptr);
    REQUIRE(advanced != nullptr);
    REQUIRE(details != nullptr);
    REQUIRE(profile != nullptr);
    CHECK(identity->textFormat() == Qt::PlainText);
    CHECK(identity->text().contains(directory.path()));
    CHECK(identity->text().contains(QStringLiteral("linux-clang-debug")));
    CHECK(setup->text() == controller.State().SetupStatus);
    CHECK_FALSE(details->isVisible());
    CHECK(profile->isVisible());
    const auto release = profile->findData(QStringLiteral("linux-clang-release"));
    REQUIRE(release >= 0);
    CHECK_FALSE(profile->model()->flags(profile->model()->index(release, 0)).testFlag(Qt::ItemIsEnabled));
    CHECK(next->text().contains(QStringLiteral("This project owns its engine and build setup")));
    const auto original = controller.State().Draft;
    QTest::mouseClick(advanced, Qt::LeftButton);
    CHECK(details->isVisible());
    QTest::mouseClick(advanced, Qt::LeftButton);
    CHECK_FALSE(details->isVisible());
    CHECK(controller.State().Draft == original);
    CHECK_FALSE(controller.State().Dirty());
    CHECK(controller.State().ActiveJob == 0);
    const auto capture = qEnvironmentVariable("LUDUS_PROJECT_OVERVIEW_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(window.grab().save(capture));
    }
}

TEST_CASE("Friendly build profiles preserve preset identity and require explicit save", "[editor][lifecycle][overview]")
{
    QTemporaryDir directory;
    const auto path = Project(directory);
    // Exercise v2 profiles without claiming setup exists on this computer.
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    auto object = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    object.insert(QStringLiteral("version"), 2);
    object.insert(QStringLiteral("engine"),
                  QJsonObject{{QStringLiteral("version"), QStringLiteral("0.1.0")},
                              {QStringLiteral("components"), QJsonArray{QStringLiteral("FoundationBase")}},
                              {QStringLiteral("features"), QJsonArray{}}});
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(object).toJson());
    file.close();
    EditorController controller({}, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.show();
    controller.OpenProject(path);
    auto* profile = window.findChild<QComboBox*>(QStringLiteral("projectProfile"));
    auto* identity = window.findChild<QLabel*>(QStringLiteral("projectIdentity"));
    auto* next = window.findChild<QLabel*>(QStringLiteral("projectNextStep"));
    auto* save = window.findChild<QPushButton*>(QStringLiteral("projectSaveSettings"));
    auto* run = window.findChild<QPushButton*>(QStringLiteral("projectBuildRun"));
    auto* check = window.findChild<QPushButton*>(QStringLiteral("projectCheckSetup"));
    auto* repair = window.findChild<QPushButton*>(QStringLiteral("projectRepairSetup"));
    REQUIRE(profile != nullptr);
    REQUIRE(identity != nullptr);
    REQUIRE(next != nullptr);
    REQUIRE(save != nullptr);
    REQUIRE(run != nullptr);
    REQUIRE(check != nullptr);
    REQUIRE(repair != nullptr);
    REQUIRE(controller.State().Saved.HasEngine);
    CHECK(identity->text().contains(QStringLiteral("Required engine: Ludus 0.1.0")));
    CHECK(identity->text().contains(QStringLiteral("FoundationBase")));
    for (ludus::foundation::int32 i = 0; i < profile->count(); ++i)
    {
        profile->setCurrentIndex(i);
        CHECK(controller.State().Draft.Preset == profile->itemData(i).toString());
        CHECK(controller.State().Saved.Preset == QStringLiteral("linux-clang-debug"));
    }
    REQUIRE(controller.State().Dirty());
    CHECK(next->text().contains(QStringLiteral("Save your project settings")));
    CHECK_FALSE(run->isEnabled());
    CHECK_FALSE(check->isEnabled());
    CHECK_FALSE(repair->isEnabled());
    CHECK(controller.State().ActiveJob == 0);
    save->click();
    CHECK_FALSE(controller.State().Dirty());
    CHECK(controller.Store().Load(path).Parse.Descriptor.Preset == QStringLiteral("macos-clang-release"));
    CHECK(identity->text().contains(QStringLiteral("macos-clang-release")));
    CHECK(check->isEnabled() == controller.Caps().CanProjectCheck);
    CHECK(repair->isEnabled() == controller.Caps().CanProjectSetup);
    CHECK(run->isEnabled() == controller.Caps().CanBuildRun);
}
