#include "internal/configuration_workspace.h"
#include "internal/controller.h"
#include "internal/document_well.h"
#include "internal/main_window.h"
#include "internal/project_creation_dialog.h"
#include "internal/settings_page.h"
#include "internal/workspace_style.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
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
    DocumentWell* well = window.findChild<DocumentWell*>();
    auto* welcome = window.findChild<QWidget*>(QStringLiteral("projectWelcome"));
    auto* close = window.findChild<QAction*>(QStringLiteral("closeProjectAction"));
    auto* configuration = window.findChild<ConfigurationWorkspace*>(QStringLiteral("configurationWorkspace"));
    REQUIRE(well != nullptr);
    REQUIRE(welcome != nullptr);
    REQUIRE(close != nullptr);
    REQUIRE(configuration != nullptr);
    CHECK(well->IsDocumentAvailable(configuration));
    CHECK(well->GetActiveDocumentOrNull() == welcome);
    CHECK_FALSE(close->isEnabled());
    const auto path = Project(directory);
    controller.OpenProject(path);
    QApplication::processEvents();
    CHECK_FALSE(welcome->isVisible());
    CHECK(close->isEnabled());
    REQUIRE(configuration->Edit("host.headless", QStringLiteral("true")));
    REQUIRE(well->OpenDocument(configuration));
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
    CHECK(well->GetActiveDocumentOrNull() == welcome);
    CHECK(well->IsDocumentAvailable(configuration));
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
    // A project opens with Project Settings as a background document tab.
    QWidget* settings = window.findChild<QWidget*>(QStringLiteral("projectSettingsScroll"));
    REQUIRE(window.GetDocumentWell().IsDocumentOpen(settings) == true);
    REQUIRE(window.GetDocumentWell().OpenDocument(settings) == true);
    QLabel* folder = window.findChild<QLabel*>(QStringLiteral("projectFolder"));
    QLabel* savedProfile = window.findChild<QLabel*>(QStringLiteral("projectSavedProfile"));
    auto* setup = window.findChild<QLabel*>(QStringLiteral("projectSetupStatus"));
    StatusChip* chip = window.findChild<StatusChip*>(QStringLiteral("projectSetupChip"));
    auto* next = window.findChild<QLabel*>(QStringLiteral("projectNextStep"));
    QAbstractButton* advanced = window.findChild<QAbstractButton*>(QStringLiteral("projectAdvanced"));
    auto* details = window.findChild<QWidget*>(QStringLiteral("projectBuildDetails"));
    auto* profile = window.findChild<QComboBox*>(QStringLiteral("projectProfile"));
    REQUIRE(folder != nullptr);
    REQUIRE(savedProfile != nullptr);
    REQUIRE(setup != nullptr);
    REQUIRE(chip != nullptr);
    REQUIRE(next != nullptr);
    REQUIRE(advanced != nullptr);
    REQUIRE(details != nullptr);
    REQUIRE(profile != nullptr);
    CHECK(folder->textFormat() == Qt::PlainText);
    CHECK(QDir::fromNativeSeparators(folder->text()).contains(directory.path()));
    CHECK(savedProfile->text() == QStringLiteral("linux-clang-debug"));
    // The chip names the state; the sentence after it is the rest of the controller's status.
    const ProjectSetupSummary summary = DescribeProjectSetup(controller.State().SetupStatus, {});
    CHECK(setup->text() == summary.Summary);
    CHECK(chip->text() == summary.Chip);
    CHECK(controller.State().SetupStatus.endsWith(setup->text(), Qt::CaseInsensitive) == true);
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
    QLabel* engine = window.findChild<QLabel*>(QStringLiteral("projectEngine"));
    QLabel* components = window.findChild<QLabel*>(QStringLiteral("projectComponents"));
    QLabel* savedProfile = window.findChild<QLabel*>(QStringLiteral("projectSavedProfile"));
    auto* next = window.findChild<QLabel*>(QStringLiteral("projectNextStep"));
    auto* save = window.findChild<QPushButton*>(QStringLiteral("projectSaveSettings"));
    auto* run = window.findChild<QPushButton*>(QStringLiteral("projectBuildRun"));
    auto* check = window.findChild<QPushButton*>(QStringLiteral("projectCheckSetup"));
    auto* repair = window.findChild<QPushButton*>(QStringLiteral("projectRepairSetup"));
    REQUIRE(profile != nullptr);
    REQUIRE(engine != nullptr);
    REQUIRE(components != nullptr);
    REQUIRE(savedProfile != nullptr);
    REQUIRE(next != nullptr);
    REQUIRE(save != nullptr);
    REQUIRE(run != nullptr);
    REQUIRE(check != nullptr);
    REQUIRE(repair != nullptr);
    REQUIRE(controller.State().Saved.HasEngine);
    CHECK(engine->text() == QStringLiteral("Ludus 0.1.0"));
    CHECK(components->text().contains(QStringLiteral("FoundationBase")));
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
    const QString selectedPreset = profile->currentData().toString();
    REQUIRE(selectedPreset.isEmpty() == false);
    save->click();
    CHECK_FALSE(controller.State().Dirty());
    CHECK(controller.Store().Load(path).Parse.Descriptor.Preset == selectedPreset);
    CHECK(savedProfile->text() == selectedPreset);
    CHECK(check->isEnabled() == controller.Caps().CanProjectCheck);
    CHECK(repair->isEnabled() == controller.Caps().CanProjectSetup);
    CHECK(run->isEnabled() == controller.Caps().CanBuildRun);
}

TEST_CASE("Project setup status reads as a chip, one sentence and detail rows", "[editor][lifecycle][overview]")
{
    const ProjectSetupSummary unchecked = DescribeProjectSetup({}, {});
    CHECK(unchecked.Tone == StatusTone::Neutral);
    CHECK(unchecked.Chip == QStringLiteral("Not checked"));
    CHECK(unchecked.Details.isEmpty() == true);
    const ProjectSetupSummary ready =
        DescribeProjectSetup(QStringLiteral("CMake setup ready: Setup checked: selectable presets, SDKs\n"
                                            "Selected engine: Ludus 0.1.0 - Development\n"
                                            "Installation: /Users/someone/Ludus/out/install/macos-clang-development"),
                             {});
    CHECK(ready.Tone == StatusTone::Success);
    CHECK(ready.Chip == QStringLiteral("Ready"));
    CHECK(ready.Summary == QStringLiteral("Setup checked: selectable presets, SDKs"));
    REQUIRE(ready.Details.size() == 2);
    CHECK(ready.Details.at(0).first == QStringLiteral("Selected engine"));
    CHECK(ready.Details.at(0).second == QStringLiteral("Ludus 0.1.0 - Development"));
    CHECK(ready.Details.at(1).first == QStringLiteral("Installation"));
    const ProjectSetupSummary attention =
        DescribeProjectSetup(QStringLiteral("CMake setup needs attention: run repair"), {});
    CHECK(attention.Tone == StatusTone::Warning);
    CHECK(attention.Chip == QStringLiteral("Needs attention"));
    CHECK(attention.Summary == QStringLiteral("Run repair"));
    const ProjectSetupSummary outOfDate =
        DescribeProjectSetup(QStringLiteral("CMake setup needs attention: This project's build setup is out of date: "
                                            "SDK or tool inputs changed; run repair to refresh presets and caches"),
                             {});
    CHECK(outOfDate.Tone == StatusTone::Warning);
    CHECK(outOfDate.Chip == QStringLiteral("Out of date"));
    const ProjectSetupSummary unavailable =
        DescribeProjectSetup(QStringLiteral("Setup validation unavailable: managed editor tooling is missing"), {});
    CHECK(unavailable.Chip == QStringLiteral("Unavailable"));
    CHECK(DescribeProjectSetup(QStringLiteral("Checking selectable CMake presets and local toolchain…"), {}).Chip ==
          QStringLiteral("Checking"));
    // A stale engine is a detail row and turns a ready check into one that needs attention.
    const ProjectSetupSummary stale =
        DescribeProjectSetup(QStringLiteral("CMake setup ready: Setup checked"), QStringLiteral("Built from 0361c59."));
    CHECK(stale.Tone == StatusTone::Warning);
    REQUIRE(stale.Details.isEmpty() == false);
    CHECK(stale.Details.last().first == QStringLiteral("Engine"));
}

namespace
{
/// Left edge of widget in the page column's coordinates.
int GetColumnX(const QWidget& widget, const QWidget& column)
{
    return widget.mapTo(&column, QPoint(0, 0)).x();
}

/// A version-2 project with an engine, and a stand-in tool adapter whose setup check reports detail lines.
QString WriteCheckedProject(const QTemporaryDir& directory, ToolingPaths& outTooling)
{
    QFile file(directory.filePath(QStringLiteral("ludus.project.json")));
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral(R"json({"version":2,"name":"Sample","provider":"cmake","source_dir":".",
"preset":"macos-clang-development","target":"sample","run":{"cwd":".","args":[]},
"engine":{"version":"0.1.0","components":["FoundationBase"],"features":[]}})json"));
    file.close();
    QFile lists(directory.filePath(QStringLiteral("CMakeLists.txt")));
    REQUIRE(lists.open(QIODevice::WriteOnly));
    lists.write(QByteArrayLiteral("cmake_minimum_required(VERSION 3.29)\n"));
    lists.close();
    const QString adapter = directory.filePath(QStringLiteral("setup_adapter.py"));
    QFile script(adapter);
    REQUIRE(script.open(QIODevice::WriteOnly));
    script.write(QByteArrayLiteral(R"py(import json, sys
def send(kind, **fields):
    fields.update(protocol=1, type=kind)
    if kind != 'ready': fields['job'] = request['job']
    print(json.dumps(fields), flush=True)
send('ready')
request = json.loads(sys.stdin.readline())
send('setup_details', text='Setup checked: selectable presets, SDKs, rendering tools and IDE settings\n'
     'Selected engine: Ludus 0.1.0 - Development - arm64-apple-darwin\n'
     'Engine revision: bba94d877466a34628aa473c015639d05336fe6a\n'
     'Selection: saved .ludus/local.json override (local override)\n'
     'Installation: /Users/someone/Ludus/out/install/macos-clang-development-c8b3337c1b31fe65')
send('result', outcome='success', stage='configuring', code='Ok', message='Setup checked',
     cleanup_confirmed=True, exit_code=None, signal=None)
)py"));
    script.close();
    outTooling.PythonPath = QString::fromUtf8(LUDUS_TEST_PYTHON);
    outTooling.AdapterPath = adapter;
    outTooling.ToolingRoot = directory.path();
    return file.fileName();
}
} // namespace

TEST_CASE("Project Settings is one readable column of aligned sections at MacBook Air and wide sizes",
          "[editor][lifecycle][overview][layout]")
{
    QTemporaryDir directory;
    ToolingPaths tooling;
    const QString path = WriteCheckedProject(directory, tooling);
    EditorController controller(tooling, nullptr, directory.filePath(QStringLiteral("recent.json")));
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.show();
    controller.OpenProject(path);
    QWidget* settings = window.findChild<QWidget*>(QStringLiteral("projectSettingsScroll"));
    REQUIRE(settings != nullptr);
    REQUIRE(window.GetDocumentWell().OpenDocument(settings) == true);
    StatusChip* chip = window.findChild<StatusChip*>(QStringLiteral("projectSetupChip"));
    REQUIRE(chip != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(chip->GetTone() == StatusTone::Success, 10000);
    QScrollArea* scroll = qobject_cast<QScrollArea*>(settings);
    SettingsColumn* column = window.findChild<SettingsColumn*>(QStringLiteral("projectSettingsColumn"));
    QLineEdit* name = window.findChild<QLineEdit*>(QStringLiteral("projectName"));
    QComboBox* profile = window.findChild<QComboBox*>(QStringLiteral("projectProfile"));
    QComboBox* resolution = window.findChild<QComboBox*>(QStringLiteral("projectResolutionProvider"));
    QLabel* folder = window.findChild<QLabel*>(QStringLiteral("projectFolder"));
    QPushButton* save = window.findChild<QPushButton*>(QStringLiteral("projectSaveSettings"));
    QAbstractButton* advanced = window.findChild<QAbstractButton*>(QStringLiteral("projectAdvanced"));
    QWidget* setupDetails = window.findChild<QWidget*>(QStringLiteral("projectSetupDetails"));
    REQUIRE(scroll != nullptr);
    REQUIRE(column != nullptr);
    REQUIRE(name != nullptr);
    REQUIRE(profile != nullptr);
    REQUIRE(resolution != nullptr);
    REQUIRE(folder != nullptr);
    REQUIRE(save != nullptr);
    REQUIRE(advanced != nullptr);
    REQUIRE(setupDetails != nullptr);
    CHECK(setupDetails->findChildren<QLabel*>(QStringLiteral("projectSetupDetail")).size() == 4);
    const QString capture = qEnvironmentVariable("LUDUS_PROJECT_SETTINGS_CAPTURE");
    // A MacBook Air's default logical size, and a wide external display.
    for (const QSize size : {QSize(1470, 920), QSize(2560, 1400)})
    {
        INFO(size.width());
        window.resize(size);
        QApplication::processEvents();
        QApplication::processEvents();
        const int readable = column->fontMetrics().averageCharWidth() * SETTINGS_COLUMN_CHARACTERS;
        CHECK(column->width() <= readable);
        // The column starts at the page's left margin rather than being centred, and never runs past the page.
        CHECK(column->mapTo(scroll->viewport(), QPoint(0, 0)).x() < column->fontMetrics().averageCharWidth() * 8);
        CHECK(column->mapTo(scroll->viewport(), QPoint(column->width(), 0)).x() <= scroll->viewport()->width());
        CHECK(scroll->horizontalScrollBar()->isVisible() == false);
        // Fields of every section start on one edge, left of their column's middle.
        const int fieldX = GetColumnX(*name, *column);
        CHECK(GetColumnX(*profile, *column) == fieldX);
        CHECK(GetColumnX(*resolution, *column) == fieldX);
        CHECK(GetColumnX(*folder, *column) == fieldX);
        CHECK(fieldX < column->width() / 2);
        // Labels sit on the left edge of the column.
        for (QFormLayout* form : column->findChildren<QFormLayout*>())
        {
            if (form->parentWidget() == setupDetails)
            {
                continue;
            }
            for (int row = 0; row < form->rowCount(); ++row)
            {
                const QLayoutItem* labelOrNull = form->itemAt(row, QFormLayout::LabelRole);
                if (labelOrNull != nullptr && labelOrNull->widget() != nullptr && labelOrNull->widget()->isVisible())
                {
                    CHECK(GetColumnX(*labelOrNull->widget(), *column) == 0);
                }
            }
        }
        // The whole profile name is shown.
        CHECK(profile->width() >= profile->fontMetrics().horizontalAdvance(profile->currentText()));
        // Actions are sized to their words, not stretched into a grid.
        for (const QString& identity : {QStringLiteral("projectCheckSetup"),
                                        QStringLiteral("projectRepairSetup"),
                                        QStringLiteral("projectBuildRun"),
                                        QStringLiteral("projectStop"),
                                        QStringLiteral("projectSaveSettings"),
                                        QStringLiteral("projectEnableResolution")})
        {
            INFO(identity.toStdString());
            QPushButton* action = window.findChild<QPushButton*>(identity);
            REQUIRE(action != nullptr);
            if (action->isVisible() == true)
            {
                CHECK(action->width() == action->sizeHint().width());
            }
        }
        CHECK(advanced->width() == advanced->sizeHint().width());
        // Save is the page's primary action, top right of the column.
        CHECK(save->property("visualRole").toString() == QStringLiteral("primary"));
        CHECK(GetColumnX(*save, *column) + save->width() == column->width());
        CHECK(save->mapTo(column, QPoint(0, 0)).y() < name->mapTo(column, QPoint(0, 0)).y());
        if (capture.isEmpty() == false)
        {
            REQUIRE(window.grab().save(QStringLiteral("%1-%2.png").arg(capture).arg(size.width())));
        }
    }
    // Values can be selected and copied.
    CHECK(folder->textInteractionFlags().testFlag(Qt::TextSelectableByMouse) == true);
    CHECK(folder->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard) == true);
    // Save waits for a change.
    CHECK(save->isEnabled() == false);
    name->setFocus();
    QTest::keyClicks(name, QStringLiteral(" renamed"));
    QApplication::processEvents();
    CHECK(save->isEnabled() == true);
    save->click();
    CHECK(save->isEnabled() == false);
    // Details fold away until asked for.
    CHECK(window.findChild<QWidget*>(QStringLiteral("projectBuildDetails"))->isVisible() == false);
}
