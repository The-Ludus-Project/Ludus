// Offscreen tests for EditorController action validation and failure handling
// (design 5, 6, 9; E05/E09/E12). These do not launch real tools; the tool
// lifecycle is covered by the Python real-process suite. Here we verify that
// open/edit/save/reload/close and capability gating behave correctly, that a
// failed Save keeps the dirty draft, and that Copy Job Details is bounded and
// telemetry-free.

#include "internal/controller.h"
#include "internal/main_window.h"
#include "internal/project_store.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QToolBar>

using namespace ludus::editor;

namespace
{
// Upper bound for one Python adapter round trip (interpreter start, CMake
// preset inspection, exit). The adapter needs well under a second of CPU, but
// on a shared 10-core macOS host at load 46-48 a read-only setup probe took
// 6.4 s and the next one missed a 10 s bound. Waits return as soon as their
// condition holds, so this bound only decides when a hung adapter fails.
// Waits use QTest::qWaitFor under a Catch2 assertion: a QtTest QTRY_* macro
// returns silently from a Catch2 case on failure and skips the later checks.
constexpr int ADAPTER_WAIT_MILLISECONDS = 60000;

ToolingPaths NoTooling()
{
    ToolingPaths tooling;
    tooling.PythonPath = QStringLiteral("/nonexistent/python");
    tooling.AdapterPath = QStringLiteral("/nonexistent/editor_tool.py");
    tooling.ToolingRoot = QStringLiteral("/nonexistent");
    return tooling;
}

QString WriteDescriptor(QTemporaryDir& dir, const QByteArray& bytes)
{
    const QString path = dir.filePath(QStringLiteral("ludus.project.json"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(bytes);
    file.close();
    return path;
}

ToolingPaths AvailableTooling()
{
    const QString root = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    ToolingPaths tooling;
    tooling.PythonPath = QDir(root).filePath(QStringLiteral("out/host-tools/venv/bin/python"));
    tooling.AdapterPath = QDir(root).filePath(QStringLiteral("scripts/python/editor_tool.py"));
    tooling.ToolingRoot = root;
    return tooling;
}

void WriteCMakeProject(const QString& directory, bool withTestPreset)
{
    QFile cmakelists(QDir(directory).filePath(QStringLiteral("CMakeLists.txt")));
    REQUIRE(cmakelists.open(QIODevice::WriteOnly));
    cmakelists.close();

    const QByteArray testPresets =
        withTestPreset
            ? QByteArrayLiteral(
                  ",\"testPresets\":[{\"name\":\"linux-clang-debug\",\"configurePreset\":\"linux-clang-debug\"}]")
            : QByteArray{};
    QFile presets(QDir(directory).filePath(QStringLiteral("CMakePresets.json")));
    REQUIRE(presets.open(QIODevice::WriteOnly));
    presets.write(QByteArrayLiteral("{\"version\":6,\"configurePresets\":[{\"name\":\"linux-clang-debug\","
                                    "\"generator\":\"Ninja\",\"binaryDir\":\"${sourceDir}/out/build/"
                                    "linux-clang-debug\"}],\"buildPresets\":[{\"name\":\"linux-clang-debug\","
                                    "\"configurePreset\":\"linux-clang-debug\"}]") +
                  testPresets + QByteArrayLiteral("}"));
    presets.close();
}

QByteArray ValidDescriptor()
{
    return QByteArrayLiteral("{\n  \"version\": 1,\n  \"name\": \"demo\",\n  \"provider\": \"cmake\",\n"
                             "  \"source_dir\": \".\",\n  \"preset\": \"linux-clang-debug\",\n  \"target\": \"app\",\n"
                             "  \"run\": {\n    \"cwd\": \".\",\n    \"args\": []\n  }\n}\n");
}
} // namespace

TEST_CASE("Open loads a project and executes no project code", "[editor][controller]")
{
    QTemporaryDir dir;
    const QString path = WriteDescriptor(dir, ValidDescriptor());
    EditorController controller(NoTooling());
    QSignalSpy spy(&controller, &EditorController::StateChanged);
    controller.OpenProject(path);
    CHECK(controller.State().Document == DocumentState::ProjectLoaded);
    CHECK(controller.State().Saved.Name == QStringLiteral("demo"));
    CHECK_FALSE(controller.State().Dirty());
    CHECK(spy.count() >= 1);
}

TEST_CASE("Open reports a valid CMake setup using read-only preset inspection", "[editor][controller][setup]")
{
    QTemporaryDir dir;
    const QString descriptor = WriteDescriptor(dir, ValidDescriptor());
    WriteCMakeProject(dir.path(), true);
    EditorController controller(AvailableTooling());
    controller.OpenProject(descriptor);
    const bool ready = QTest::qWaitFor(
        [&controller] { return controller.State().SetupStatus.contains(QStringLiteral("CMake setup ready")); },
        ADAPTER_WAIT_MILLISECONDS);
    INFO(controller.State().SetupStatus.toStdString());
    REQUIRE(ready);
    // A terminal result is provisional until the one-shot adapter exits.
    REQUIRE(QTest::qWaitFor([&controller] { return controller.Caps().CanOpen; }, ADAPTER_WAIT_MILLISECONDS));
    CHECK_FALSE(QFileInfo(QDir(dir.path()).filePath(QStringLiteral("out"))).exists());
}

TEST_CASE("Open reports a stale CMake cache without rewriting it", "[editor][controller][setup]")
{
    QTemporaryDir dir;
    const QString descriptor = WriteDescriptor(dir, ValidDescriptor());
    WriteCMakeProject(dir.path(), true);
    const QString build = QDir(dir.path()).filePath(QStringLiteral("out/build/linux-clang-debug"));
    REQUIRE(QDir().mkpath(build));
    const QString cachePath = QDir(build).filePath(QStringLiteral("CMakeCache.txt"));
    QFile cache(cachePath);
    REQUIRE(cache.open(QIODevice::WriteOnly));
    cache.write(QByteArrayLiteral("CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_MAKE_PROGRAM:FILEPATH=/moved/tools/ninja\n"));
    cache.close();
    QFile original(cachePath);
    REQUIRE(original.open(QIODevice::ReadOnly));
    const QByteArray before = original.readAll();
    original.close();

    EditorController controller(AvailableTooling());
    controller.OpenProject(descriptor);
    const bool stale = QTest::qWaitFor(
        [&controller] { return controller.State().SetupStatus.contains(QStringLiteral("stale CMake cache")); },
        ADAPTER_WAIT_MILLISECONDS);
    INFO(controller.State().SetupStatus.toStdString());
    REQUIRE(stale);
    REQUIRE(QTest::qWaitFor([&controller] { return controller.Caps().CanOpen; }, ADAPTER_WAIT_MILLISECONDS));
    QFile after(cachePath);
    REQUIRE(after.open(QIODevice::ReadOnly));
    CHECK(after.readAll() == before);
}

TEST_CASE("A failed open leaves the previous workspace intact", "[editor][controller]")
{
    QTemporaryDir dir;
    const QString good = WriteDescriptor(dir, ValidDescriptor());
    EditorController controller(NoTooling());
    controller.OpenProject(good);
    const WorkspaceState before = controller.State();

    const QString bad = dir.filePath(QStringLiteral("broken.json"));
    QFile file(bad);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("{ not json"));
    file.close();
    controller.OpenProject(bad);
    // The previous workspace is unchanged; the failure is a last result.
    CHECK(controller.State().DescriptorPath == before.DescriptorPath);
    CHECK(controller.State().Saved == before.Saved);
    CHECK(controller.State().Result.Kind == Outcome::Failed);
}

TEST_CASE("Editing produces a dirty draft; Save makes it clean and survives reload", "[editor][controller]")
{
    QTemporaryDir dir;
    const QString path = WriteDescriptor(dir, ValidDescriptor());
    EditorController controller(NoTooling());
    controller.OpenProject(path);

    ProjectDescriptor edited = controller.State().Draft;
    edited.Name = QStringLiteral("renamed");
    controller.EditDraft(edited);
    CHECK(controller.State().Dirty());

    controller.Save();
    CHECK_FALSE(controller.State().Dirty());

    controller.Reload();
    CHECK(controller.State().Saved.Name == QStringLiteral("renamed"));
}

TEST_CASE("A failed Save keeps the dirty draft", "[editor][controller]")
{
    QTemporaryDir dir;
    const QString path = WriteDescriptor(dir, ValidDescriptor());
    EditorController controller(NoTooling());
    controller.OpenProject(path);

    ProjectStore::FaultHooks hooks;
    hooks.FailCommit = []() { return true; };
    controller.Store().SetFaultHooks(hooks);

    ProjectDescriptor edited = controller.State().Draft;
    edited.Name = QStringLiteral("willfail");
    controller.EditDraft(edited);
    controller.Save();
    CHECK(controller.State().Dirty()); // draft retained
    CHECK(controller.State().Result.Kind == Outcome::Failed);
}

TEST_CASE("Preset change invalidates the discovered-target cache", "[editor][controller]")
{
    QTemporaryDir dir;
    const QString path = WriteDescriptor(dir, ValidDescriptor());
    EditorController controller(NoTooling());
    controller.OpenProject(path);
    // Simulate a prior discovery by editing then confirm a preset change clears.
    ProjectDescriptor edited = controller.State().Draft;
    edited.Preset = QStringLiteral("linux-clang-development");
    controller.EditDraft(edited);
    CHECK(controller.State().DiscoveredTargets.isEmpty());
}

TEST_CASE("Copy Job Details is bounded and omits telemetry", "[editor][controller]")
{
    QTemporaryDir dir;
    const QString path = WriteDescriptor(dir, ValidDescriptor());
    EditorController controller(NoTooling());
    controller.OpenProject(path);
    const QString details = controller.JobDetails();
    CHECK(details.contains(QStringLiteral("descriptor_digest:")));
    CHECK(details.contains(QStringLiteral("preset:")));
    CHECK(details.contains(QStringLiteral("cleanup_confirmed:")));
    // No environment dump or upload claims.
    CHECK_FALSE(details.contains(QStringLiteral("PATH=")));
    CHECK(static_cast<ludus::foundation::usize>(details.toUtf8().size()) <=
          ludus::foundation::usize{1} * 1024u * 1024u);
}

TEST_CASE("Project setup actions gate clean SDK projects and new project dialogs", "[editor][setup]")
{
    EditorController controller(NoTooling());
    MainWindow window(&controller);
    auto* create = window.findChild<QAction*>(QStringLiteral("newProjectAction"));
    auto* check = window.findChild<QAction*>(QStringLiteral("checkSetupAction"));
    auto* repair = window.findChild<QAction*>(QStringLiteral("setupProjectAction"));
    REQUIRE(create != nullptr);
    REQUIRE(check != nullptr);
    REQUIRE(repair != nullptr);
    CHECK(create->isEnabled());
    CHECK_FALSE(check->isEnabled());
    CHECK_FALSE(repair->isEnabled());
    create->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("newProjectDialog"));
    REQUIRE(dialog != nullptr);
    CHECK(dialog->isVisible());
    dialog->reject();

    WorkspaceState state;
    state.Document = DocumentState::ProjectLoaded;
    state.HasSaved = true;
    state.Saved.Version = 2;
    state.Saved.ProviderKind = Provider::Cmake;
    state.Draft = state.Saved;
    CHECK(ComputeCapabilities(state).CanProjectCheck);
    CHECK(ComputeCapabilities(state).CanProjectSetup);
    state.Draft.Name = QStringLiteral("dirty");
    CHECK_FALSE(ComputeCapabilities(state).CanProjectSetup);
    CHECK_FALSE(ComputeCapabilities(state).CanProjectCreate);
    state.OperationPhase = Phase::CleanupUnknown;
    CHECK_FALSE(ComputeCapabilities(state).CanProjectCheck);
}

TEST_CASE("Opening an SDK project automatically reports missing setup without running hooks", "[editor][setup]")
{
    QTemporaryDir dir;
    QFile fixture(QStringLiteral(LUDUS_EDITOR_SOURCE_DIR) + QStringLiteral("/tests/fixtures/valid_v2_cmake.json"));
    REQUIRE(fixture.open(QIODevice::ReadOnly));
    const QString descriptor = WriteDescriptor(dir, fixture.readAll());
    QFile cmake(dir.filePath(QStringLiteral("CMakeLists.txt")));
    REQUIRE(cmake.open(QIODevice::WriteOnly));
    cmake.write("cmake_minimum_required(VERSION 3.29)\n");
    cmake.close();
    QFile hook(dir.filePath(QStringLiteral("init.sh")));
    REQUIRE(hook.open(QIODevice::WriteOnly));
    hook.write("#!/bin/sh\ntouch should-never-exist\n");
    hook.close();
    const auto root = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    ToolingPaths tooling;
    tooling.PythonPath = QStringLiteral("/usr/bin/python3");
    tooling.AdapterPath = root + QStringLiteral("/scripts/python/editor_tool.py");
    tooling.ToolingRoot = root;
    EditorController controller(tooling);
    auto* tool = controller.findChild<ToolProcess*>();
    REQUIRE(tool != nullptr);
    QSignalSpy finished(tool, &ToolProcess::Finished);
    controller.OpenProject(descriptor);
    QDeadlineTimer deadline(ADAPTER_WAIT_MILLISECONDS);
    while (controller.State().Result.Kind == Outcome::None && deadline.hasExpired() == false)
    {
        QTest::qWait(10);
    }
    CHECK(controller.State().Document == DocumentState::ProjectLoaded);
    CHECK(controller.State().Result.Kind == Outcome::Failed);
    CHECK(controller.State().Result.Message.contains(QStringLiteral("setup is missing")));
    if (finished.isEmpty())
    {
        REQUIRE(finished.wait(5000));
    }
    CHECK_FALSE(QFile::exists(dir.filePath(QStringLiteral("should-never-exist"))));
    CHECK_FALSE(QFile::exists(dir.filePath(QStringLiteral("CMakeUserPresets.json"))));
    CHECK_FALSE(QFile::exists(dir.filePath(QStringLiteral(".ludus"))));
}

TEST_CASE("Editor New Project verifies the SDK project and opens it after bridge shutdown",
          "[editor][setup][integration]")
{
    const QString sdk = qEnvironmentVariable("LUDUS_SETUP_TEST_SDK");
    const QString tools = qEnvironmentVariable("LUDUS_SETUP_TEST_TOOLS");
    if (sdk.isEmpty() || tools.isEmpty())
    {
        SKIP("Requires an installed native SDK and prepared tooling checkout");
    }
    QTemporaryDir dir;
    const QString destination = dir.filePath(QStringLiteral("NewGame"));
    ToolingPaths paths;
    paths.PythonPath = QStringLiteral("/usr/bin/python3");
    paths.AdapterPath = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT) + QStringLiteral("/scripts/python/editor_tool.py");
    paths.ToolingRoot = tools;
    EditorController controller(paths);
    ProjectCreationOptions creation;
    creation.Destination = destination;
    creation.Name = QStringLiteral("NewGame");
    creation.Sdk = sdk;
    controller.CreateProject(creation);
    QDeadlineTimer deadline(60000);
    while ((controller.State().Document != DocumentState::ProjectLoaded ||
            controller.Caps().CanCloseImmediately == false) &&
           deadline.hasExpired() == false)
    {
        QTest::qWait(10);
    }
    CHECK(controller.State().Document == DocumentState::ProjectLoaded);
    CHECK(controller.State().Saved.Name == QStringLiteral("NewGame"));
    CHECK_FALSE(controller.State().Dirty());
    CHECK(controller.State().Result.Kind == Outcome::Success);
    CHECK(controller.State().Result.Message.contains(QStringLiteral("Setup checked")));
    CHECK(QFile::exists(destination + QStringLiteral("/CMakeUserPresets.json")));
    CHECK_FALSE(QFile::exists(destination + QStringLiteral("/out/build/linux-clang-development/CMakeCache.txt")));
}

namespace
{
ToolingPaths DebugTooling(QTemporaryDir& dir)
{
    const QString adapter = dir.filePath(QStringLiteral("debug_adapter.py"));
    QFile file(adapter);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral(R"py(import json, os, sys
from pathlib import Path
def send(kind, **fields):
    fields.update(protocol=1, type=kind)
    if kind != 'ready': fields['job'] = request['job']
    print(json.dumps(fields), flush=True)
send('ready')
request = json.loads(sys.stdin.readline())
def finish(code, outcome):
    send('result', code=code, outcome=outcome, stage='configuring',
         message='RAD unavailable' if code == 'MissingDebugger' else 'session stopped',
         cleanup_confirmed=True, exit_code=None, signal=None)
if request['operation'] == 'inspect_setup':
    send('result', outcome='success', stage='configuring', code='Ok', message='fixture setup ready', cleanup_confirmed=True, exit_code=None, signal=None)
    sys.exit(0)
if not request.get('setup_debugger') and not request.get('debugger'):
    finish('MissingDebugger', 'failed')
else:
    Path(request['project']).with_name('debug_request.json').write_text(json.dumps(request))
    send('phase', stage='configuring')
    send('phase', stage='launching')
    send('debugger_started', pid=os.getpid(), debugger='/fixture/RAD', executable='/fixture/game')
    for line in sys.stdin:
        if json.loads(line).get('type') == 'cancel': break
    finish('Cancelled', 'cancelled')
)py"));
    file.close();
    ToolingPaths paths;
    paths.PythonPath = QStringLiteral("/usr/bin/python3");
    paths.AdapterPath = adapter;
    paths.ToolingRoot = dir.path();
    return paths;
}

QMessageBox* WaitForDebugPrompt(MainWindow& window)
{
    QDeadlineTimer deadline(ADAPTER_WAIT_MILLISECONDS);
    QMessageBox* dialog = nullptr;
    while (dialog == nullptr && deadline.hasExpired() == false)
    {
        QTest::qWait(10);
        dialog = window.findChild<QMessageBox*>(QStringLiteral("debuggerSetupDialog"));
    }
    return dialog;
}

void WaitForDebugSession(EditorController& controller)
{
    QDeadlineTimer deadline(ADAPTER_WAIT_MILLISECONDS);
    while (controller.State().OperationPhase != Phase::Debugging && deadline.hasExpired() == false)
    {
        QTest::qWait(10);
    }
    REQUIRE(controller.State().OperationPhase == Phase::Debugging);
}

void StopDebugSession(EditorController& controller)
{
    controller.Stop();
    QDeadlineTimer deadline(ADAPTER_WAIT_MILLISECONDS);
    while (controller.Caps().CanCloseImmediately == false && deadline.hasExpired() == false)
    {
        QTest::qWait(10);
    }
    REQUIRE(controller.State().OperationPhase == Phase::Idle);
    CHECK(controller.State().Result.Kind == Outcome::Cancelled);
}
} // namespace

TEST_CASE("Debug is visible and optional setup is offered only after a Debug request", "[editor][debug]")
{
#if defined(Q_OS_MACOS)
    SKIP("RAD sessions are Linux-only; macOS capability gates are tested separately");
#endif
    QTemporaryDir dir;
    EditorController controller(DebugTooling(dir));
    MainWindow window(&controller);
    auto* action = window.findChild<QAction*>(QStringLiteral("buildDebugAction"));
    auto* toolbar = window.findChild<QToolBar*>(QStringLiteral("gameToolbar"));
    REQUIRE(action != nullptr);
    REQUIRE(toolbar != nullptr);
    CHECK(toolbar->actions().contains(action));
    CHECK_FALSE(action->isEnabled());
    controller.OpenProject(WriteDescriptor(dir, ValidDescriptor()));
    QTest::qWait(10);
    CHECK(window.findChild<QMessageBox*>(QStringLiteral("debuggerSetupDialog")) == nullptr);
    REQUIRE(QTest::qWaitFor([&] { return action->isEnabled(); }, ADAPTER_WAIT_MILLISECONDS));
    action->trigger();
    auto* dialog = WaitForDebugPrompt(window);
    REQUIRE(dialog != nullptr);
    CHECK(controller.Caps().CanCloseImmediately);
    CHECK(controller.State().Result.Code == ResultCode::MissingDebugger);
    REQUIRE(dialog->defaultButton() != nullptr);
    CHECK(dialog->standardButton(dialog->defaultButton()) == QMessageBox::Cancel);
    dialog->reject();
    QTest::qWait(10);
    CHECK_FALSE(QFile::exists(dir.filePath(QStringLiteral("debug_request.json"))));
    CHECK(controller.Caps().CanBuildDebug);
}

TEST_CASE("Explicit RAD setup continues the same clean project and Stop closes the session", "[editor][debug]")
{
#if defined(Q_OS_MACOS)
    SKIP("RAD sessions are Linux-only; macOS capability gates are tested separately");
#endif
    QTemporaryDir dir;
    EditorController controller(DebugTooling(dir));
    MainWindow window(&controller);
    const QString project = WriteDescriptor(dir, ValidDescriptor());
    controller.OpenProject(project);
    const QString digest = controller.State().SavedDigest;
    controller.BuildDebug();
    auto* dialog = WaitForDebugPrompt(window);
    REQUIRE(dialog != nullptr);
    auto* setup = dialog->findChild<QPushButton*>(QStringLiteral("setupRadButton"));
    REQUIRE(setup != nullptr);
    setup->click();
    WaitForDebugSession(controller);
    CHECK_FALSE(controller.Caps().CanBuildDebug);
    auto* recentMenu = window.findChild<QMenu*>(QStringLiteral("recentProjectsMenu"));
    auto* recentOpen = window.findChild<QPushButton*>(QStringLiteral("openRecentProjectButton"));
    auto* clearRecent = window.findChild<QAction*>(QStringLiteral("clearRecentProjectsAction"));
    REQUIRE(recentMenu != nullptr);
    REQUIRE(recentOpen != nullptr);
    REQUIRE(clearRecent != nullptr);
    CHECK_FALSE(recentMenu->isEnabled());
    CHECK_FALSE(recentOpen->isEnabled());
    CHECK_FALSE(clearRecent->isEnabled());
    const auto recentCount = controller.RecentProjects().size();
    controller.ClearRecentProjects();
    CHECK(controller.RecentProjects().size() == recentCount);
    CHECK(controller.Caps().CanStop);
    CHECK(controller.State().DescriptorPath == project);
    CHECK(controller.State().SavedDigest == digest);
    CHECK(controller.State().Saved.Preset == QStringLiteral("linux-clang-debug"));
    QFile request(dir.filePath(QStringLiteral("debug_request.json")));
    REQUIRE(request.open(QIODevice::ReadOnly));
    CHECK(request.readAll().contains("\"setup_debugger\": true"));
    StopDebugSession(controller);
}

TEST_CASE("A stale debugger setup dialog cannot debug a different project", "[editor][debug]")
{
#if defined(Q_OS_MACOS)
    SKIP("RAD sessions are Linux-only; macOS capability gates are tested separately");
#endif
    QTemporaryDir dir;
    EditorController controller(DebugTooling(dir));
    MainWindow window(&controller);
    controller.OpenProject(WriteDescriptor(dir, ValidDescriptor()));
    controller.BuildDebug();
    auto* dialog = WaitForDebugPrompt(window);
    REQUIRE(dialog != nullptr);
    auto* setup = dialog->findChild<QPushButton*>(QStringLiteral("setupRadButton"));
    REQUIRE(setup != nullptr);
    QTemporaryDir second;
    const QString project = WriteDescriptor(second, ValidDescriptor());
    controller.OpenProject(project);
    setup->click();
    QTest::qWait(30);
    CHECK(controller.State().DescriptorPath == project);
    REQUIRE(QTest::qWaitFor([&] { return controller.Caps().CanBuildDebug; }, ADAPTER_WAIT_MILLISECONDS));
    CHECK_FALSE(QFile::exists(second.filePath(QStringLiteral("debug_request.json"))));
    CHECK_FALSE(QFile::exists(dir.filePath(QStringLiteral("debug_request.json"))));
}

TEST_CASE("An existing RAD executable can open a session without installation", "[editor][debug]")
{
#if defined(Q_OS_MACOS)
    SKIP("RAD sessions are Linux-only; macOS capability gates are tested separately");
#endif
    QTemporaryDir dir;
    EditorController controller(DebugTooling(dir));
    controller.OpenProject(WriteDescriptor(dir, ValidDescriptor()));
    controller.BuildDebug(QStringLiteral("/fixture/RAD executable"));
    WaitForDebugSession(controller);
    CHECK(controller.State().OperationPhase != Phase::Running);
    StopDebugSession(controller);
}

TEST_CASE("Play menu follows saved project and tooling eligibility", "[editor][controller][play]")
{
    QTemporaryDir dir;
    const QString path = WriteDescriptor(dir, ValidDescriptor());
    QFile sidecar(dir.filePath(QStringLiteral("ludus.play.json")));
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    sidecar.write(QByteArrayLiteral("{\"version\":1,\"host_target\":\"game_host\",\"module_target\":\"game_module\"}"));
    sidecar.close();

    ToolingPaths tooling;
    tooling.PythonPath = QStringLiteral("/usr/bin/python3");
    tooling.ToolingRoot = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    tooling.AdapterPath = QDir(tooling.ToolingRoot).filePath(QStringLiteral("scripts/python/editor_tool.py"));
    EditorController controller(tooling);
    MainWindow window(&controller);
    controller.OpenProject(path);
    auto* play = window.findChild<QAction*>(QStringLiteral("play.start"));
    auto* reload = window.findChild<QAction*>(QStringLiteral("play.reload"));
    REQUIRE(play != nullptr);
    REQUIRE(reload != nullptr);
#if defined(Q_OS_MACOS)
    CHECK_FALSE(play->isEnabled());
#else
    REQUIRE(QTest::qWaitFor([&] { return play->isEnabled(); }, ADAPTER_WAIT_MILLISECONDS));
#endif
    CHECK_FALSE(reload->isEnabled());

    auto edited = controller.State().Draft;
    edited.Name = QStringLiteral("dirty");
    controller.EditDraft(edited);
    CHECK_FALSE(play->isEnabled());
}

TEST_CASE("Live Play ownership prevents a competing RAD debug job", "[editor][debug][play]")
{
#if defined(Q_OS_MACOS)
    SKIP("RAD sessions are Linux-only; macOS capability gates are tested separately");
#endif
    QTemporaryDir dir;
    const auto tooling = DebugTooling(dir);
    REQUIRE(QDir(dir.path()).mkpath(QStringLiteral("scripts/python")));
    QFile playTool(dir.filePath(QStringLiteral("scripts/python/play_tool.py")));
    REQUIRE(playTool.open(QIODevice::WriteOnly));
    playTool.close();
    QFile sidecar(dir.filePath(QStringLiteral("ludus.play.json")));
    REQUIRE(sidecar.open(QIODevice::WriteOnly));
    sidecar.write("{}");
    sidecar.close();
    EditorController controller(tooling);
    MainWindow window(&controller);
    controller.OpenProject(WriteDescriptor(dir, ValidDescriptor()));
    REQUIRE(QTest::qWaitFor([&] { return controller.CanPlay(); }, ADAPTER_WAIT_MILLISECONDS));
    controller.Play();
    REQUIRE(controller.PlayState().Phase == PlayPhase::Starting);
    CHECK_FALSE(controller.Caps().CanBuildDebug);
    auto* debug = window.findChild<QAction*>(QStringLiteral("buildDebugAction"));
    REQUIRE(debug != nullptr);
    CHECK_FALSE(debug->isEnabled());
    const auto job = controller.State().ActiveJob;
    controller.BuildDebug(QStringLiteral("/fixture/RAD executable"));
    CHECK(controller.State().ActiveJob == job);
    REQUIRE(QTest::qWaitFor([&] { return controller.Caps().CanCloseImmediately; }, ADAPTER_WAIT_MILLISECONDS));
    CHECK(controller.PlayState().Phase == PlayPhase::Stopped);
    CHECK_FALSE(QFile::exists(dir.filePath(QStringLiteral("debug_request.json"))));
}

#if defined(Q_OS_MACOS)
TEST_CASE("macOS keeps native build/run while gating deferred debugger and missing play tools", "[editor][macos]")
{
    QTemporaryDir dir;
    const QByteArray descriptor = QByteArrayLiteral(R"json({"version":2,"name":"Mac Game","provider":"cmake",
"source_dir":".","preset":"macos-clang-development","target":"game","run":{"cwd":".","args":[]},
"engine":{"version":"0.1.0","components":["FoundationBase"],"features":[]}})json");
    EditorController controller(NoTooling());
    MainWindow window(&controller);
    controller.OpenProject(WriteDescriptor(dir, descriptor));
    CHECK(controller.State().Saved.Preset == QStringLiteral("macos-clang-development"));
    CHECK(controller.Caps().CanBuildRun);
    CHECK_FALSE(controller.Caps().CanBuildDebug);
    CHECK(controller.Caps().CanReleaseInit);
    CHECK(controller.Caps().CanPackage);
    CHECK_FALSE(controller.CanPlay());
    const auto job = controller.State().ActiveJob;
    controller.BuildDebug();
    controller.Play();
    CHECK(controller.State().ActiveJob == job);
}
#endif
