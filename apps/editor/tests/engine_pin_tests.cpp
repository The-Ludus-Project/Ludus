// Engine pin drift detection, the one-click "Use this editor's engine" repair and
// engine install validation (scripts/python/ludus_tools/engine_pin.py owns the
// backend comparison; these cover the Editor's parsing, gating and dialog checks).

#include "internal/controller.h"
#include "internal/engine_pin.h"
#include "internal/main_window.h"
#include "internal/project_setup_dialog.h"

#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QDeadlineTimer>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

using namespace ludus::editor;

namespace
{
void WriteFile(const QString& path, const QByteArray& bytes)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
}

QString MakeInstall(const QString& folder, const QString& flavor, const QString& revision)
{
    WriteFile(QDir(folder).filePath(QStringLiteral("share/Ludus/LudusSdkManifest.json")),
              QJsonDocument(QJsonObject{
                                {QStringLiteral("name"), QStringLiteral("Ludus")},
                                {QStringLiteral("version"), QStringLiteral("0.1.0")},
                                {QStringLiteral("build_flavor"), flavor},
                                {QStringLiteral("source_revision"), revision},
                            })
                  .toJson());
    return folder;
}

QString MakeBuildFolder(const QString& folder)
{
    WriteFile(QDir(folder).filePath(QStringLiteral("CMakeCache.txt")), QByteArrayLiteral("CMAKE_BUILD_TYPE:STRING=\n"));
    WriteFile(QDir(folder).filePath(QStringLiteral("cmake/LudusSdkManifest.json")), QByteArrayLiteral("{}"));
    return folder;
}
} // namespace

TEST_CASE("Engine pin states parse strictly and only drift offers the editor engine", "[editor][setup][engine]")
{
    CHECK(ParseEnginePinState(QStringLiteral("current")) == EnginePinState::Current);
    CHECK(ParseEnginePinState(QStringLiteral("stale")) == EnginePinState::Stale);
    CHECK(ParseEnginePinState(QStringLiteral("invalid")) == EnginePinState::Invalid);
    CHECK(ParseEnginePinState(QStringLiteral("unknown")) == EnginePinState::Unknown);
    CHECK_FALSE(ParseEnginePinState(QStringLiteral("Stale")).has_value());
    CHECK_FALSE(ParseEnginePinState(QString()).has_value());

    EnginePinStatus status;
    CHECK_FALSE(ShouldOfferEditorEngine(status));
    for (const EnginePinState state : {EnginePinState::Current, EnginePinState::Unknown})
    {
        status.State = state;
        CHECK_FALSE(ShouldOfferEditorEngine(status));
    }
    for (const EnginePinState state : {EnginePinState::Stale, EnginePinState::Invalid})
    {
        status.State = state;
        CHECK(ShouldOfferEditorEngine(status));
    }
    CHECK(GetFlavorForProfile(QStringLiteral("macos-clang-development")) == QStringLiteral("Development"));
    CHECK(GetFlavorForProfile(QStringLiteral("linux-clang-debug")) == QStringLiteral("Debug"));
    CHECK(GetFlavorForProfile(QStringLiteral("linux-clang-release")) == QStringLiteral("Release"));
    CHECK(GetFlavorForProfile(QStringLiteral("linux-clang-asan-ubsan")).isEmpty());
}

TEST_CASE("Engine install folders reject build folders and wrong flavors", "[editor][setup][engine]")
{
    QTemporaryDir root;
    const QString install = MakeInstall(root.filePath(QStringLiteral("install")),
                                        QStringLiteral("Development"),
                                        QStringLiteral("5c74c12abc"));
    const EngineInstallCheck valid = CheckEngineInstallFolder(install, QStringLiteral("Development"));
    CHECK(valid.IsInstall);
    CHECK(valid.Revision == QStringLiteral("5c74c12abc"));
    CHECK(valid.Message.contains(QStringLiteral("5c74c12")));
    CHECK(CheckEngineInstallFolder(install, QString()).IsInstall);

    const EngineInstallCheck flavor = CheckEngineInstallFolder(install, QStringLiteral("Release"));
    CHECK_FALSE(flavor.IsInstall);
    CHECK(flavor.Message.contains(QStringLiteral("needs Release")));

    // The mistake seen in practice: local.json pointing at out/build/<profile>.
    const EngineInstallCheck build = CheckEngineInstallFolder(MakeBuildFolder(root.filePath(QStringLiteral("build"))),
                                                              QStringLiteral("Development"));
    CHECK_FALSE(build.IsInstall);
    CHECK(build.Message.contains(QStringLiteral("build folder")));

    REQUIRE(QDir(root.path()).mkdir(QStringLiteral("empty")));
    const EngineInstallCheck empty = CheckEngineInstallFolder(root.filePath(QStringLiteral("empty")), QString());
    CHECK_FALSE(empty.IsInstall);
    CHECK(empty.Message.contains(QStringLiteral("LudusSdkManifest.json")));
    CHECK_FALSE(CheckEngineInstallFolder(root.filePath(QStringLiteral("missing")), QString()).IsInstall);
    CHECK_FALSE(CheckEngineInstallFolder(QStringLiteral("relative/install"), QString()).IsInstall);

    WriteFile(root.filePath(QStringLiteral("partial/share/Ludus/LudusSdkManifest.json")), QByteArrayLiteral("{}"));
    CHECK_FALSE(CheckEngineInstallFolder(root.filePath(QStringLiteral("partial")), QString()).IsInstall);
}

TEST_CASE("Repair discovers this editor's installs and validates the chosen folder", "[editor][setup][engine]")
{
    QTemporaryDir tooling;
    QTemporaryDir project;
    const QString installs = tooling.filePath(QStringLiteral("out/install"));
    const QString current = MakeInstall(QDir(installs).filePath(QStringLiteral("linux-clang-development-0123")),
                                        QStringLiteral("Development"),
                                        QStringLiteral("aaaaaaa"));
    (void)MakeInstall(QDir(installs).filePath(QStringLiteral("linux-clang-release")),
                      QStringLiteral("Release"),
                      QStringLiteral("aaaaaaa"));
    (void)MakeBuildFolder(QDir(installs).filePath(QStringLiteral("not-installed")));
    CHECK(DiscoverEngineInstalls(tooling.path(), QStringLiteral("Development")) == QStringList{current});
    CHECK(DiscoverEngineInstalls(tooling.path(), QString()).size() == 2);
    CHECK(DiscoverEngineInstalls(QString(), QStringLiteral("Development")).isEmpty());

    ProjectSetupDialog dialog(project.path(),
                              nullptr,
                              { .ExpectedFlavor = QStringLiteral("Development"), .ToolingRoot = tooling.path() });
    dialog.show();
    QComboBox* engine = dialog.findChild<QComboBox*>(QStringLiteral("setupEngineChoice"));
    QComboBox* detected = dialog.findChild<QComboBox*>(QStringLiteral("setupDetectedEngines"));
    QLineEdit* native = dialog.findChild<QLineEdit*>(QStringLiteral("setupSdk"));
    QLabel* check = dialog.findChild<QLabel*>(QStringLiteral("setupSdkCheck"));
    QDialogButtonBox* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(engine != nullptr);
    REQUIRE(detected != nullptr);
    REQUIRE(native != nullptr);
    REQUIRE(check != nullptr);
    REQUIRE(buttons != nullptr);
    engine->setCurrentIndex(2);
    CHECK(detected->isVisible());
    REQUIRE(detected->count() == 2);
    CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());

    native->setText(QDir(installs).filePath(QStringLiteral("not-installed")));
    CHECK(check->text().contains(QStringLiteral("build folder")));
    CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    native->setText(QDir(installs).filePath(QStringLiteral("linux-clang-release")));
    CHECK(check->text().contains(QStringLiteral("needs Development")));
    CHECK_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());

    detected->setCurrentIndex(1);
    CHECK(native->text() == current);
    CHECK(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    CHECK(dialog.Options().Sdk == current);
    CHECK_FALSE(dialog.Options().PrepareEngine);
}

TEST_CASE("A stale engine pin offers one-click repair with this editor's engine", "[editor][setup][engine]")
{
    QTemporaryDir dir;
    QFile fixture(QStringLiteral(LUDUS_EDITOR_SOURCE_DIR) + QStringLiteral("/tests/fixtures/valid_v2_cmake.json"));
    REQUIRE(fixture.open(QIODevice::ReadOnly));
    const QString descriptor = dir.filePath(QStringLiteral("ludus.project.json"));
    WriteFile(descriptor, fixture.readAll());
    WriteFile(dir.filePath(QStringLiteral("CMakeLists.txt")),
              QByteArrayLiteral("cmake_minimum_required(VERSION 3.29)\n"));
    const QString adapter = dir.filePath(QStringLiteral("engine_pin_adapter.py"));
    WriteFile(adapter, QByteArrayLiteral(R"py(import json, sys
from pathlib import Path
def send(kind, **fields):
    fields.update(protocol=1, type=kind)
    if kind != 'ready': fields['job'] = request['job']
    print(json.dumps(fields), flush=True)
send('ready')
request = json.loads(sys.stdin.readline())
if request['operation'] == 'project_check':
    send('engine_pin', state='stale', installation='/checkout/out/install/linux-clang-release',
         pinned_revision='0361c59' + '0' * 33, editor_revision='5c74c12' + '0' * 33, editor_owned=True,
         message='The project engine install was built from 0361c59, but this editor runs 5c74c12.')
    send('result', outcome='failed', stage='configuring', code='InvalidProject',
         message='SDK or tool inputs changed; run repair', cleanup_confirmed=True, exit_code=None, signal=None)
else:
    Path(request['project']).with_name('setup_request.json').write_text(json.dumps(request))
    send('result', outcome='success', stage='configuring', code='Ok', message='Setup checked',
         cleanup_confirmed=True, exit_code=None, signal=None)
)py"));
    ToolingPaths tooling;
    tooling.PythonPath = QStringLiteral("/usr/bin/python3");
    tooling.AdapterPath = adapter;
    tooling.ToolingRoot = dir.path();
    EditorController controller(tooling);
    MainWindow window(&controller);
    QPushButton* useEngine = window.findChild<QPushButton*>(QStringLiteral("projectUseEditorEngine"));
    QLabel* status = window.findChild<QLabel*>(QStringLiteral("projectSetupStatus"));
    REQUIRE(useEngine != nullptr);
    REQUIRE(status != nullptr);
    window.show();
    CHECK_FALSE(useEngine->isVisible());

    controller.OpenProject(descriptor);
    QTRY_VERIFY_WITH_TIMEOUT(controller.State().EnginePin.State == EnginePinState::Stale &&
                                 controller.Caps().CanCloseImmediately,
                             10000);
    CHECK(controller.State().EnginePin.IsEditorOwned);
    CHECK(controller.State().EnginePin.PinnedRevision.startsWith(QStringLiteral("0361c59")));
    // The failed check still reports the drift and its fix.
    CHECK(controller.State().Result.Kind == Outcome::Failed);
    QTRY_VERIFY_WITH_TIMEOUT(useEngine->isVisible() && useEngine->isEnabled(), 10000);
    CHECK(status->text().contains(QStringLiteral("built from 0361c59")));

    useEngine->click();
    const QString requestPath = dir.filePath(QStringLiteral("setup_request.json"));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(requestPath) && controller.Caps().CanCloseImmediately, 10000);
    QFile requestFile(requestPath);
    REQUIRE(requestFile.open(QIODevice::ReadOnly));
    const QJsonObject request = QJsonDocument::fromJson(requestFile.readAll()).object();
    CHECK(request.value(QStringLiteral("operation")).toString() == QStringLiteral("project_setup"));
    CHECK(request.value(QStringLiteral("prepare_engine")).toBool());
    CHECK(request.value(QStringLiteral("sdk")).toString().isEmpty());
    // Saved browser setup is reused, not cleared.
    CHECK_FALSE(request.value(QStringLiteral("disable_web")).toBool());
    // A repair replaces the selection, so the stale report and its button are retired.
    CHECK(controller.State().EnginePin.State == EnginePinState::None);
    QTRY_VERIFY_WITH_TIMEOUT(useEngine->isVisible() == false, 10000);
}
