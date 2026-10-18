#include "internal/scene_preview.h"
#include "internal/world_workspace.h"
#import <Cocoa/Cocoa.h>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
namespace
{
void NativeKey(NSWindow* window, ludus::foundation::uint16 key, bool down)
{
    auto* event = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp
                                   location:NSZeroPoint
                              modifierFlags:0
                                  timestamp:0
                               windowNumber:window.windowNumber
                                    context:nil
                                 characters:@""
                charactersIgnoringModifiers:@""
                                  isARepeat:NO
                                    keyCode:key];
    [window sendEvent:event];
}
bool Frames(ScenePreview& preview, ludus::foundation::uint64 target)
{
    QElapsedTimer timer;
    timer.start();
    while (preview.PresentedFrames() < target && timer.elapsed() < 5000)
    {
        QTest::qWait(20);
    }
    return preview.PresentedFrames() >= target;
}
} // namespace
TEST_CASE("Cocoa World authoring presents Metal frames and accepts native keyboard transforms", "[.scene-native]")
{
    REQUIRE(QApplication::platformName() == QStringLiteral("cocoa"));
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto bytes = ludus::world_demo::ExampleLevel();
    const auto scenePath =
        QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath(QStringLiteral("room.world.json"));
    QFile source(scenePath);
    REQUIRE(source.open(QIODevice::WriteOnly));
    REQUIRE(source.write(bytes.data(), static_cast<ludus::foundation::int64>(bytes.size())) ==
            static_cast<ludus::foundation::int64>(bytes.size()));
    source.close();
    WorldWorkspace workspace;
    workspace.SetProject(directory.path(), 1);
    REQUIRE(workspace.Open(scenePath));
    const auto& document = workspace.WorldDemoProjection();
    auto& preview = workspace.WorldDemoPreview();
    REQUIRE(workspace.StartPreview());
    REQUIRE(Frames(preview, 2));
    auto* window = static_cast<NSWindow*>(preview.WindowInfo().CocoaWindow);
    REQUIRE(window != nil);
    [window makeKeyAndOrderFront:nil];
    QTest::qWait(40);
    const auto original = document.Draft();
    const auto originalWorld = workspace.Document().Draft();
    preview.Select(original.Level.Entities[0].Id);
    NativeKey(window, 124, true);
    preview.Tick();
    REQUIRE(document.Previewing());
    CHECK(document.Draft() == original);
    CHECK(workspace.Document().Draft() == originalWorld);
    CHECK_FALSE(workspace.CanSave());
    NativeKey(window, 124, false);
    preview.Tick();
    REQUIRE(workspace.Dirty());
    CHECK_FALSE(document.Previewing());
    CHECK(document.Draft().Level.Entities[0].Position.X == original.Level.Entities[0].Position.X + 0.1F);
    workspace.Undo();
    CHECK(document.Draft() == original);
    CHECK_FALSE(workspace.Dirty());
    CHECK(workspace.Document().Draft() == originalWorld);
    CHECK_FALSE(workspace.CanUndo());
    NativeKey(window, 124, true);
    preview.Tick();
    REQUIRE(document.Previewing());
    NativeKey(window, 53, true);
    preview.Tick();
    CHECK_FALSE(document.Previewing());
    CHECK(document.Draft() == original);
    NativeKey(window, 124, false);
    preview.Tick();
    const auto previous = preview.PresentedStamp();
    [window setContentSize:NSMakeSize(720, 480)];
    REQUIRE(Frames(preview, preview.PresentedFrames() + 2));
    CHECK(preview.PresentedStamp().Surface != previous.Surface);
    // Invalidate a cancelled preview frame even though document content/revision
    // is unchanged, then prove native Enter uses the currently presented stamp.
    const auto frameBefore = preview.PresentedStamp();
    NativeKey(window, 36, true);
    preview.Tick();
    CHECK(preview.Selected().View().empty());
    CHECK(preview.PresentedStamp().Document == frameBefore.Document);
    preview.Select(original.Level.Entities[0].Id);
    NativeKey(window, 124, true);
    preview.Tick();
    NativeKey(window, 124, false);
    preview.Tick();
    REQUIRE(workspace.Dirty());
    REQUIRE(workspace.Save());
    CHECK_FALSE(workspace.Dirty());
    WorldWorkspace reopened;
    reopened.SetProject(directory.path(), 1);
    REQUIRE(reopened.Open(scenePath));
    CHECK(reopened.Document().Draft() == workspace.Document().Draft());
    const auto player = qEnvironmentVariable("LUDUS_SCENE_TEST_PLAYER");
    if (player.isEmpty() == false)
    {
        QProcess process;
        process.start(player, {QStringLiteral("--level"), scenePath, QStringLiteral("--frames"), QStringLiteral("3")});
        REQUIRE(process.waitForStarted(5000));
        REQUIRE(process.waitForFinished(10000));
        INFO(process.readAllStandardError().constData());
        CHECK(process.exitStatus() == QProcess::NormalExit);
        CHECK(process.exitCode() == 0);
        CHECK(process.readAllStandardOutput().contains(QByteArrayLiteral("presented 3 frames")));
        // A malformed authored source is a normal status failure, never an
        // abort caused by leaving the player's logging worker running.
        const auto invalidPath = scenePath + QStringLiteral(".invalid.json");
        QFile invalid(invalidPath);
        REQUIRE(invalid.open(QIODevice::WriteOnly));
        REQUIRE(invalid.write(QByteArrayLiteral("{}")) == 2);
        invalid.close();
        process.start(player, {QStringLiteral("--level"), invalidPath});
        REQUIRE(process.waitForStarted(5000));
        REQUIRE(process.waitForFinished(10000));
        CHECK(process.exitStatus() == QProcess::NormalExit);
        CHECK(process.exitCode() == 1);
    }
    CHECK(workspace.PreviewRunning());
    workspace.StopPreview();
    CHECK_FALSE(workspace.PreviewRunning());
    CHECK(document.Loaded());
    CHECK(workspace.Document().Draft() == reopened.Document().Draft());
    workspace.ResumePreview();
    CHECK(workspace.PreviewRunning());
    REQUIRE(Frames(preview, 2));
    workspace.StopPreview();
}
