// Explicit hardware journey: run the hidden [live-journey] case with a native
// Qt platform and LUDUS_SDK_PREFIX. Missing prerequisites fail this invocation;
// default offscreen unit CTest does not claim hardware acceptance.
#include "internal/controller.h"
#include "internal/main_window.h"

#include <ludus/foundation/base/types.h>

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>

using namespace ludus::editor;

namespace
{
template <typename Predicate>
bool Wait(Predicate&& predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (predicate() == false && timer.elapsed() < 90000)
    {
        QTest::qWait(20);
    }
    return predicate();
}
void Write(const QString& path, const QByteArray& contents)
{
    QSaveFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(contents) == contents.size());
    REQUIRE(file.commit());
}
void SelectNativeProfile(const QString& descriptor)
{
#if defined(Q_OS_MACOS)
    QFile input(descriptor);
    REQUIRE(input.open(QIODevice::ReadOnly));
    auto document = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    document.insert(QStringLiteral("preset"), QStringLiteral("macos-clang-development"));
    Write(descriptor, QJsonDocument(document).toJson());
#else
    Q_UNUSED(descriptor);
#endif
}
QString NativeTarget()
{
#if defined(Q_OS_MACOS)
    return QSysInfo::currentCpuArchitecture() == QStringLiteral("arm64") ? QStringLiteral("arm64-apple-darwin")
                                                                         : QStringLiteral("x86_64-apple-darwin");
#else
    return QStringLiteral("x86_64-linux-gnu");
#endif
}
int SpeedRow(const EditorController& controller)
{
    const auto& properties = controller.PlayState().Properties;
    for (int i = 0; i < properties.size(); ++i)
    {
        if (properties.at(i).toObject().value(QStringLiteral("property")) == QStringLiteral("000000000000b001"))
        {
            return i;
        }
    }
    return -1;
}
bool SpeedIs(const EditorController& controller, ludus::foundation::float32 expected)
{
    const auto row = SpeedRow(controller);
    if (row < 0)
    {
        return false;
    }
    const auto bits = static_cast<ludus::foundation::uint32>(
        controller.PlayState().Properties.at(row).toObject().value(QStringLiteral("bits")).toInteger());
    ludus::foundation::float32 actual = 0;
    std::memcpy(&actual, &bits, sizeof(bits));
    return actual == expected;
}
} // namespace

TEST_CASE("native editor installed SDK build play edit reload asset and reopen", "[.live-journey]")
{
    REQUIRE(QApplication::platformName() != QStringLiteral("offscreen"));
    REQUIRE(QApplication::platformName() != QStringLiteral("minimal"));
    REQUIRE(qEnvironmentVariable("LUDUS_SDK_PREFIX").isEmpty() == false);
    QTemporaryDir project;
    QTemporaryDir secondProject;
    REQUIRE(project.isValid());
    REQUIRE(secondProject.isValid());
    const QString root = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT);
    const QDir sample(QDir(root).filePath(QStringLiteral("examples/live-edit-game")));
    REQUIRE(QDir(project.path()).mkdir(QStringLiteral("src")));
    for (const auto& name : {QStringLiteral("CMakeLists.txt"),
                             QStringLiteral("CMakePresets.json"),
                             QStringLiteral("body.schema.json"),
                             QStringLiteral("body.schema.baseline.json"),
                             QStringLiteral("ludus.play.json"),
                             QStringLiteral("game.tuning.json"),
                             QStringLiteral("frame-clear.json"),
                             QStringLiteral("src/game.cpp"),
                             QStringLiteral("src/body.h")})
    {
        REQUIRE(QFile::copy(sample.filePath(name), project.filePath(name)));
    }
    const auto descriptor = project.filePath(QStringLiteral("ludus.project.json"));
    REQUIRE(QFile::copy(sample.filePath(QStringLiteral("ludus.project.json")), descriptor));
    SelectNativeProfile(descriptor);
    REQUIRE(QFile::copy(sample.filePath(QStringLiteral("ludus.lock.json")),
                        project.filePath(QStringLiteral("ludus.lock.json"))));
    REQUIRE(QDir(project.path()).mkdir(QStringLiteral(".ludus")));
    const QJsonObject sdkOverride{{QStringLiteral("target"), NativeTarget()},
                                  {QStringLiteral("flavor"), QStringLiteral("Development")},
                                  {QStringLiteral("prefix"), qEnvironmentVariable("LUDUS_SDK_PREFIX")}};
    Write(project.filePath(QStringLiteral(".ludus/local.json")),
          QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1},
                                    {QStringLiteral("sdk_overrides"), QJsonArray{sdkOverride}}})
              .toJson());
    REQUIRE(QDir(secondProject.path()).mkdir(QStringLiteral("src")));
    for (const auto& name : {QStringLiteral("CMakeLists.txt"),
                             QStringLiteral("CMakePresets.json"),
                             QStringLiteral("body.schema.json"),
                             QStringLiteral("body.schema.baseline.json"),
                             QStringLiteral("ludus.project.json"),
                             QStringLiteral("ludus.lock.json"),
                             QStringLiteral("ludus.play.json"),
                             QStringLiteral("game.tuning.json"),
                             QStringLiteral("frame-clear.json"),
                             QStringLiteral("src/game.cpp"),
                             QStringLiteral("src/body.h")})
    {
        REQUIRE(QFile::copy(sample.filePath(name), secondProject.filePath(name)));
    }
    SelectNativeProfile(secondProject.filePath(QStringLiteral("ludus.project.json")));
    REQUIRE(QDir(secondProject.path()).mkdir(QStringLiteral(".ludus")));
    REQUIRE(QFile::copy(project.filePath(QStringLiteral(".ludus/local.json")),
                        secondProject.filePath(QStringLiteral(".ludus/local.json"))));
    EditorController controller({QDir(root).filePath(QStringLiteral("out/host-tools/venv/bin/python")),
                                 QDir(root).filePath(QStringLiteral("scripts/python/editor_tool.py")),
                                 root});
    MainWindow window(&controller);
    QElapsedTimer elapsed;
    elapsed.start();
    QJsonArray builds;
    QJsonArray edits;
    QJsonObject buildTimes;
    bool measureGeneration = false;
    auto beginBuildMeasurement = [&](const QString& label) {
        measureGeneration = true;
        buildTimes = {{QStringLiteral("label"), label}, {QStringLiteral("start_ms"), elapsed.elapsed()}};
    };
    auto* tool = controller.findChild<ToolProcess*>();
    REQUIRE(tool != nullptr);
    QObject measurementContext; // Retires captured-stack callbacks before those values die.
    QObject::connect(tool, &ToolProcess::Event, &measurementContext, [&](const ProtocolEvent& event) {
        if (measureGeneration == false)
        {
            return;
        }
        if (event.Kind == ProtocolEvent::Type::Phase)
        {
            buildTimes.insert(event.Stage + QStringLiteral("_ms"), elapsed.elapsed());
        }
        if (event.Kind == ProtocolEvent::Type::Generation)
        {
            const auto now = elapsed.elapsed();
            builds.append(QJsonObject{
                {QStringLiteral("label"), buildTimes.value(QStringLiteral("label"))},
                {QStringLiteral("configure_ms"),
                 buildTimes.value(QStringLiteral("building_ms")).toInteger() -
                     buildTimes.value(QStringLiteral("configuring_ms")).toInteger()},
                {QStringLiteral("build_ms"),
                 buildTimes.value(QStringLiteral("publishing_ms")).toInteger() -
                     buildTimes.value(QStringLiteral("building_ms")).toInteger()},
                {QStringLiteral("publication_ms"), now - buildTimes.value(QStringLiteral("publishing_ms")).toInteger()},
                {QStringLiteral("adapter_total_ms"), now - buildTimes.value(QStringLiteral("start_ms")).toInteger()}});
            measureGeneration = false;
        }
    });
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    controller.OpenProject(descriptor);
    // Exercise main's shared setup repair before playing: actual configure,
    // build and CTest, followed by the read-only check of owned local presets.
    REQUIRE(Wait([&]() { return controller.State().Busy() == false && controller.Caps().CanProjectSetup; }));
    controller.SetupProject(qEnvironmentVariable("LUDUS_SDK_PREFIX"), {}, false);
    REQUIRE(Wait([&]() { return controller.State().Busy() == false && controller.Caps().CanProjectSetup; }));
    INFO(controller.JobDetails().toStdString());
    INFO(controller.Log().Text().toStdString());
    REQUIRE(controller.State().Result.Kind == Outcome::Success);
    REQUIRE(QFileInfo::exists(project.filePath(QStringLiteral("CMakeUserPresets.json"))));
    REQUIRE(controller.CanPlay());
    beginBuildMeasurement(QStringLiteral("post-repair-start"));
    controller.Play();
    const bool started = Wait([&]() {
        return (controller.PlayState().Phase == PlayPhase::Running && SpeedRow(controller) >= 0) ||
               (controller.State().Result.Kind == Outcome::Failed && controller.State().Busy() == false);
    });
    INFO(controller.JobDetails().toStdString());
    INFO(controller.Log().Text().toStdString());
    REQUIRE(started);
    REQUIRE(controller.PlayState().Phase == PlayPhase::Running);
    REQUIRE(controller.PlayState().HostPid > 0);
    REQUIRE(controller.PlayState().HostArgv.isEmpty() == false);
    REQUIRE(controller.PlayState().HostCwd == QFileInfo(project.path()).canonicalFilePath());
    REQUIRE(controller.PlayState().SdkIdentity.isEmpty() == false);
    REQUIRE(controller.JobDetails().contains(controller.PlayState().HostArgv.front()));
    controller.PlayCommand(QStringLiteral("Pause"));
    REQUIRE(
        Wait([&]() { return controller.PlayState().Phase == PlayPhase::Paused && controller.CanEditProperties(); }));
    for (int iteration = 0; iteration < 10; ++iteration)
    {
        const auto previous = controller.PlayState().Generation;
        REQUIRE(Wait([&]() { return controller.CanBuildReload(); }));
        beginBuildMeasurement(QStringLiteral("warm-no-change"));
        controller.BuildReload();
        REQUIRE(
            Wait([&]() { return controller.PlayState().Generation != previous && controller.CanEditProperties(); }));
        REQUIRE(controller.PlayState().Phase == PlayPhase::Paused);
    }
    for (int iteration = 0; iteration < 10; ++iteration)
    {
        const auto expected = iteration % 2 == 0 ? 0.4F : 0.5F;
        const auto startedAt = elapsed.elapsed();
        controller.EditProperty(SpeedRow(controller),
                                iteration % 2 == 0 ? QStringLiteral("0.4") : QStringLiteral("0.5"));
        const bool edited = Wait([&]() { return SpeedIs(controller, expected) && controller.CanEditProperties(); });
        INFO(controller.JobDetails().toStdString());
        INFO(controller.Log().Text().toStdString());
        CAPTURE(iteration, expected);
        REQUIRE(edited);
        edits.append(elapsed.elapsed() - startedAt);
    }
    controller.EditProperty(SpeedRow(controller), QStringLiteral("0.25"));
    REQUIRE(Wait([&]() { return SpeedIs(controller, 0.25F) && controller.CanUndoSession(); }));
    controller.UndoSessionEdit();
    REQUIRE(Wait([&]() { return SpeedIs(controller, 0.25F) == false && controller.CanRedoSession(); }));
    controller.RedoSessionEdit();
    REQUIRE(Wait(
        [&]() { return SpeedIs(controller, 0.25F) && controller.CanApplyToTuningDocument(SpeedRow(controller)); }));
    controller.ApplyLivePropertyToTuningDocument(SpeedRow(controller));
    REQUIRE(Wait([&]() { return controller.CanSaveTuningDocument(); }));
    controller.SaveTuningDocument();
    REQUIRE(Wait([&]() { return controller.PlayState().TuningDocumentDirty == false; }));
    controller.SetAutoReload(true);
    REQUIRE(controller.AutoReloadEnabled());
    const auto generation = controller.PlayState().Generation;
    const auto source = project.filePath(QStringLiteral("src/game.cpp"));
    QFile file(source);
    REQUIRE(file.open(QIODevice::ReadOnly));
    auto code = file.readAll();
    file.close();
    REQUIRE(code.contains("ClearGreen = 0.1F"));
    code.replace("ClearGreen = 0.1F", "ClearGreen = 0.7F");
    beginBuildMeasurement(QStringLiteral("body-edit-with-debounce"));
    Write(source, code);
    REQUIRE(Wait([&]() { return controller.PlayState().Generation != generation && controller.CanEditProperties(); }));
    REQUIRE(controller.PlayState().Phase == PlayPhase::Paused);
    REQUIRE(SpeedIs(controller, 0.25F));
    const auto secondDescriptor = secondProject.filePath(QStringLiteral("ludus.project.json"));
    controller.OpenProject(secondDescriptor);
    REQUIRE(controller.State().DescriptorPath == descriptor); // A still owns a live host.
    REQUIRE_FALSE(controller.Caps().CanProjectSetup);
    REQUIRE_FALSE(controller.Caps().CanProjectCreate);
    const auto acceptedGeneration = controller.PlayState().Generation;
    REQUIRE(code.contains("kCheckpointSchema = 1"));
    code.replace("kCheckpointSchema = 1", "kCheckpointSchema = 2");
    Write(source, code);
    REQUIRE(Wait([&]() {
        return controller.PlayState().Message.contains(QStringLiteral("RestartRequired")) &&
               controller.CanBuildReload();
    }));
    REQUIRE(controller.PlayState().Generation == acceptedGeneration);
    REQUIRE(controller.PlayState().Phase == PlayPhase::Paused);
    REQUIRE(SpeedIs(controller, 0.25F));
    controller.PlayCommand(QStringLiteral("Step"));
    controller.ReloadClearConfiguration(project.filePath(QStringLiteral("frame-clear.json")));
    REQUIRE(Wait([&]() { return controller.PlayState().Message.contains(QStringLiteral("configuration replaced")); }));
    controller.RefreshSessionDetails();
    REQUIRE(Wait([&]() {
        return controller.PlayState().HostStatus.value(QStringLiteral("clear_asset_generation")).toString() !=
                   QStringLiteral("0000000000000000") &&
               controller.PlayState().HostStatus.value(QStringLiteral("presented_frames")).toInteger() > 0;
    }));
    REQUIRE(controller.PlayState().HostStatus.value(QStringLiteral("windowed")).toBool());
    // Runtime timing crosses the supervisor unchanged and decodes in the editor.
    controller.RefreshTiming();
    REQUIRE(Wait(
        [&]() { return controller.PlayState().HasTiming == true && controller.PlayState().Timing.WindowFrames > 0; }));
    CHECK(controller.PlayState().Timing.PacingIntervalNanoseconds == 16'666'667);
    CHECK(controller.PlayState().TimingMessage.isEmpty());
    const auto session = controller.PlayState().Session;
    controller.Stop();
    REQUIRE(Wait([&]() { return controller.PlayState().Phase == PlayPhase::Stopped && controller.CanPlay(); }));
    controller.OpenProject(descriptor);
    REQUIRE(Wait([&]() { return controller.CanPlay(); }));
    beginBuildMeasurement(QStringLiteral("cold-project-B"));
    controller.Play();
    REQUIRE(Wait([&]() {
        return controller.PlayState().Phase == PlayPhase::Running && controller.PlayState().Session != session &&
               SpeedIs(controller, 0.25F);
    }));
    controller.Stop();
    REQUIRE(Wait([&]() { return controller.Caps().CanOpen; }));
    controller.OpenProject(secondDescriptor);
    REQUIRE(controller.State().DescriptorPath == secondDescriptor);
    REQUIRE(Wait([&]() { return controller.CanPlay(); }));
    controller.Play();
    REQUIRE(Wait([&]() {
        return controller.PlayState().Phase == PlayPhase::Running && controller.PlayState().Session != session &&
               SpeedRow(controller) >= 0;
    }));
    REQUIRE_FALSE(SpeedIs(controller, 0.25F)); // B has its own authored document.
    controller.BuildReload();
    REQUIRE(controller.RequestClose() == false);
    controller.Stop();
    REQUIRE(Wait([&]() { return controller.Caps().CanCloseImmediately; }));
    REQUIRE(controller.State().Result.Code != ResultCode::ProtocolError);
    REQUIRE(controller.RequestClose());
    const auto metrics = qEnvironmentVariable("LUDUS_LIVE_JOURNEY_METRICS");
    if (metrics.isEmpty() == false)
    {
        Write(metrics,
              QJsonDocument(QJsonObject{{QStringLiteral("platform"), QApplication::platformName()},
                                        {QStringLiteral("builds"), builds},
                                        {QStringLiteral("copied_property_roundtrip_ms"), edits},
                                        {QStringLiteral("poll_interval_ms"), 20}})
                  .toJson());
    }
}
