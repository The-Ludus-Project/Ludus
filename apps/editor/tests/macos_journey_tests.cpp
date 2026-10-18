// Explicit native Cocoa acceptance; the default offscreen suite does not claim
// native window integration. Requires a prepared Development SDK and tools.
#include "internal/controller.h"
#include "internal/main_window.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>

using namespace ludus::editor;

#if defined(Q_OS_MACOS)
namespace
{
template <typename Predicate>
bool WaitMac(Predicate&& predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (predicate() == false && timer.elapsed() < 90000)
    {
        QTest::qWait(20);
    }
    return predicate();
}
} // namespace

TEST_CASE("Cocoa editor creates checks repairs builds runs stops and reopens a macOS game", "[.macos-journey]")
{
    REQUIRE(QApplication::platformName() == QStringLiteral("cocoa"));
    const auto sdk = qEnvironmentVariable("LUDUS_SETUP_TEST_SDK");
    REQUIRE(sdk.isEmpty() == false);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto root = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    ToolingPaths paths;
    paths.PythonPath = QDir(root).filePath(QStringLiteral("out/host-tools/venv/bin/python"));
    paths.AdapterPath = QDir(root).filePath(QStringLiteral("scripts/python/editor_tool.py"));
    paths.ToolingRoot = root;
    EditorController controller(paths);
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    const auto destination = directory.filePath(QStringLiteral("Mac Game"));
    ProjectCreationOptions creation;
    creation.Destination = destination;
    creation.Name = QStringLiteral("Mac Game");
    creation.Sdk = sdk;
    controller.CreateProject(creation);
    const bool created = WaitMac(
        [&]() { return controller.State().Result.Kind != Outcome::None && controller.Caps().CanCloseImmediately; });
    INFO(controller.JobDetails().toStdString());
    REQUIRE(created);
    REQUIRE(controller.State().Result.Kind == Outcome::Success);
    REQUIRE(controller.State().Document == DocumentState::ProjectLoaded);
    REQUIRE(controller.State().Saved.Preset == QStringLiteral("macos-clang-development"));
    CHECK_FALSE(controller.State().Dirty());
    CHECK_FALSE(controller.Caps().CanBuildDebug);
    CHECK_FALSE(controller.CanPlay());
    // The visible profile selection must match the loaded saved descriptor.
    const auto selectors = window.findChildren<QComboBox*>();
    bool selected = false;
    for (const auto* selector : selectors)
    {
        selected = selected || selector->currentData().toString() == QStringLiteral("macos-clang-development");
    }
    REQUIRE(selected);
    controller.BuildRun();
    REQUIRE(WaitMac([&]() { return controller.Caps().CanCloseImmediately; }));
    INFO(controller.JobDetails().toStdString());
    REQUIRE(controller.State().Result.Kind == Outcome::Success);
    // Make the same game long-lived, then stop its owned runtime explicitly.
    const auto source = QDir(destination).filePath(QStringLiteral("src/main.cpp"));
    QFile originalFile(source);
    REQUIRE(originalFile.open(QIODevice::ReadOnly));
    const auto original = originalFile.readAll();
    originalFile.close();
    QSaveFile file(source);
    REQUIRE(file.open(QIODevice::WriteOnly));
    const QByteArray program =
        QByteArrayLiteral("#include <unistd.h>\nint main() noexcept { for (;;) { pause(); } }\n");
    REQUIRE(file.write(program) == program.size());
    REQUIRE(file.commit());
    controller.BuildRun();
    REQUIRE(WaitMac([&]() { return controller.State().OperationPhase == Phase::Running; }));
    REQUIRE(controller.Caps().CanStop);
    controller.Stop();
    REQUIRE(WaitMac([&]() { return controller.Caps().CanCloseImmediately; }));
    REQUIRE(controller.State().Result.Kind == Outcome::Cancelled);
    REQUIRE(controller.State().Result.CleanupConfirmed);
    QSaveFile restore(source);
    REQUIRE(restore.open(QIODevice::WriteOnly));
    REQUIRE(restore.write(original) == original.size());
    REQUIRE(restore.commit());
    // Opening after a fresh clone must only diagnose the missing local setup.
    const auto descriptor = controller.State().DescriptorPath;
    REQUIRE(controller.CloseProject());
    const auto presets = QDir(destination).filePath(QStringLiteral("CMakeUserPresets.json"));
    REQUIRE(QFile::remove(presets));
    controller.OpenProject(descriptor);
    REQUIRE(WaitMac(
        [&]() { return controller.State().Result.Kind != Outcome::None && controller.Caps().CanCloseImmediately; }));
    CHECK_FALSE(QFile::exists(presets));
    REQUIRE(controller.State().Result.Kind == Outcome::Failed);
    for (ludus::foundation::uint32 repetition = 0; repetition < 2; ++repetition)
    {
        controller.SetupProject(sdk, {}, false);
        REQUIRE(WaitMac([&]() { return controller.Caps().CanCloseImmediately; }));
        INFO(controller.JobDetails().toStdString());
        REQUIRE(controller.State().Result.Kind == Outcome::Success);
        REQUIRE(QFile::exists(presets));
        CHECK(controller.State().SetupStatus.contains(QStringLiteral("Installation: ") + sdk));
        CHECK(controller.State().SetupStatus.contains(QStringLiteral("Engine revision: ")));
    }
    const auto capture = qEnvironmentVariable("LUDUS_MACOS_EDITOR_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(window.grab().save(capture));
    }
}

TEST_CASE("Cocoa editor authors and verifies a macOS Release app", "[.macos-release-journey]")
{
    REQUIRE(QApplication::platformName() == QStringLiteral("cocoa"));
    const auto sdk = qEnvironmentVariable("LUDUS_RELEASE_TEST_SDK");
    REQUIRE(sdk.isEmpty() == false);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto root = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    ToolingPaths paths;
    paths.PythonPath = QDir(root).filePath(QStringLiteral("out/host-tools/venv/bin/python"));
    paths.AdapterPath = QDir(root).filePath(QStringLiteral("scripts/python/editor_tool.py"));
    paths.ToolingRoot = root;
    EditorController controller(paths);
    MainWindow window(&controller, nullptr, directory.filePath(QStringLiteral("workspace.json")));
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    ProjectCreationOptions creation;
    creation.Destination = directory.filePath(QStringLiteral("Release Game"));
    creation.Name = QStringLiteral("Release Game");
    creation.Sdk = qEnvironmentVariable("LUDUS_SETUP_TEST_SDK");
    REQUIRE(creation.Sdk.isEmpty() == false);
    controller.CreateProject(creation);
    REQUIRE(WaitMac(
        [&]() { return controller.State().Result.Kind != Outcome::None && controller.Caps().CanCloseImmediately; }));
    INFO(controller.JobDetails().toStdString());
    REQUIRE(controller.State().Result.Kind == Outcome::Success);
    REQUIRE(controller.Caps().CanReleaseInit);
    controller.SetupRelease(QStringLiteral("macos"), QStringLiteral("tester/game"));
    REQUIRE(WaitMac([&]() { return controller.Caps().CanCloseImmediately; }));
    INFO(controller.JobDetails().toStdString());
    REQUIRE(controller.State().Result.Kind == Outcome::Success);
    REQUIRE(QFile::exists(QDir(creation.Destination).filePath(QStringLiteral("ludus.release.json"))));
    REQUIRE(controller.Caps().CanPackage);
    controller.PackageRelease(QStringLiteral("macos-release"), QStringLiteral("0.1.0"), sdk);
    REQUIRE(WaitMac([&]() { return controller.Caps().CanCloseImmediately; }));
    INFO(controller.JobDetails().toStdString());
    REQUIRE(controller.State().Result.Kind == Outcome::Success);
    const QDir packages(QDir(creation.Destination).filePath(QStringLiteral("out/packages/macos-release")));
    const auto directories = packages.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    REQUIRE(directories.size() == 1);
    REQUIRE(QFile::exists(packages.filePath(directories.first() + QStringLiteral("/game.zip"))));
    REQUIRE(QFile::exists(packages.filePath(directories.first() + QStringLiteral("/validation.json"))));
    controller.PackageRelease(QStringLiteral("macos-release"), QStringLiteral("0.1.1"), sdk);
    REQUIRE(controller.Caps().CanStop);
    controller.Stop();
    REQUIRE(WaitMac([&]() { return controller.Caps().CanCloseImmediately; }));
    REQUIRE(controller.State().Result.Kind == Outcome::Cancelled);
    REQUIRE(controller.State().Result.CleanupConfirmed);
    REQUIRE(packages.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == 1);
    REQUIRE(controller.CloseProject());
    window.close();
}
#endif
