#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/workspace_style.h"
#include "internal/world_document.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>

#import <Cocoa/Cocoa.h>

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;

QByteArray ReadNativeFixture(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

NSView* GetNativeView(QWindow& inoutWindow)
{
    // Qt's Cocoa WId stores the NSView identity in an integer. This test-only
    // ABI boundary must recover that pointer to enter the real responder chain.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    NSView* view = reinterpret_cast<NSView*>(inoutWindow.winId());
    REQUIRE(view != nil);
    return view;
}

// Enter through Cocoa's hit testing and first responder, not sendEvent on the
// outer QWidget or QWindow: those shortcuts bypass the embedded NSView routing.
void SendNativeMouse(QQuickWindow& inoutSurface,
                     NSEventType type,
                     QPoint position,
                     NSEventModifierFlags modifiers = 0,
                     QPoint motion = {})
{
    NSView* view = GetNativeView(inoutSurface);
    NSWindow* window = view.window;
    REQUIRE(window != nil);
    const NSPoint local =
        NSMakePoint(position.x(), view.isFlipped == YES ? position.y() : view.bounds.size.height - position.y());
    const NSPoint point = [view convertPoint:local toView:nil];
    NSEvent* event = [NSEvent mouseEventWithType:type
                                        location:point
                                   modifierFlags:modifiers
                                       timestamp:NSProcessInfo.processInfo.systemUptime
                                    windowNumber:window.windowNumber
                                         context:nil
                                     eventNumber:0
                                      clickCount:1
                                        pressure:1];
    if (type == NSEventTypeOtherMouseDown || type == NSEventTypeOtherMouseDragged || type == NSEventTypeOtherMouseUp)
    {
        CGEventRef mouse = CGEventCreateCopy(event.CGEvent);
        REQUIRE(mouse != nullptr);
        CGEventSetIntegerValueField(mouse, kCGMouseEventButtonNumber, kCGMouseButtonCenter);
        event = [NSEvent eventWithCGEvent:mouse];
        CFRelease(mouse);
        REQUIRE(event.buttonNumber == kCGMouseButtonCenter);
    }
    if (motion.isNull() == false)
    {
        CGEventRef mouse = CGEventCreateCopy(event.CGEvent);
        REQUIRE(mouse != nullptr);
        CGEventSetIntegerValueField(mouse, kCGMouseEventDeltaX, motion.x());
        CGEventSetIntegerValueField(mouse, kCGMouseEventDeltaY, motion.y());
        event = [NSEvent eventWithCGEvent:mouse];
        CFRelease(mouse);
    }
    // NSWindow::sendEvent bypasses Qt's application native filter. Enter that
    // production boundary first, then let Cocoa translate unconsumed events.
    qintptr result = 0;
    if (QAbstractEventDispatcher::instance()->filterNativeEvent(QByteArrayLiteral("mac_generic_NSEvent"),
                                                                event,
                                                                &result) == false)
    {
        [window sendEvent:event];
    }
    QApplication::processEvents();
}

void SendNativeKey(QQuickWindow& inoutSurface, uint16 key, NSString* characters, bool isDown)
{
    NSView* view = GetNativeView(inoutSurface);
    NSWindow* window = view.window;
    NSEvent* event = [NSEvent keyEventWithType:isDown == true ? NSEventTypeKeyDown : NSEventTypeKeyUp
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:NSProcessInfo.processInfo.systemUptime
                                  windowNumber:window.windowNumber
                                       context:nil
                                    characters:characters
                   charactersIgnoringModifiers:characters
                                     isARepeat:NO
                                       keyCode:key];
    [window sendEvent:event];
    QApplication::processEvents();
}

void RequirePresentation(PreviewViewport& inoutViewport, uint64 previous, const RenderPreview* previewOrNull = nullptr)
{
    const bool hasPresented =
        QTest::qWaitFor([&inoutViewport, previous]() { return inoutViewport.PresentedFrames() > previous; }, 10000);
    const QString status = previewOrNull != nullptr ? previewOrNull->Status() : QString();
    const uint64 produced = previewOrNull != nullptr ? previewOrNull->Frames() : 0;
    CAPTURE(status.toStdString(), produced, previous, inoutViewport.PresentedFrames());
    REQUIRE(hasPresented == true);
}
} // namespace

TEST_CASE("Cocoa embedded Metal viewport routes native navigation and releases capture",
          "[.render-native][viewport-native-input]")
{
    REQUIRE(QApplication::platformName() == QStringLiteral("cocoa"));
    QTemporaryDir directory;
    REQUIRE(directory.isValid() == true);
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    REQUIRE(QDir(root).mkpath(QStringLiteral("authoring")) == true);
    for (const QString& name : {QStringLiteral("world.schema.json"),
                                QStringLiteral("rendering.world.json"),
                                QStringLiteral("render.preview.json"),
                                QStringLiteral("cube.mesh.json"),
                                QStringLiteral("ludus.png")})
    {
        const QByteArray bytes = ReadNativeFixture(QStringLiteral(":/samples/live-edit-game/authoring/") + name);
        QFile target(root + QStringLiteral("/authoring/") + name);
        REQUIRE(target.open(QIODevice::WriteOnly) == true);
        REQUIRE(target.write(bytes) == bytes.size());
    }
    WorldDocument world;
    QString error;
    REQUIRE(world.LoadSchema(ReadNativeFixture(root + QStringLiteral("/authoring/world.schema.json")), error) == true);
    REQUIRE(world.Load(ReadNativeFixture(root + QStringLiteral("/authoring/rendering.world.json")), error) == true);
    const QJsonObject original = world.Draft();
    RenderScene scene;
    REQUIRE(RenderScene::Load(root, world, scene, error) == true);
    RenderPreview preview;
    QWidget host;
    QVBoxLayout layout(&host);
    QLineEdit field;
    PreviewViewport viewport(&preview);
    layout.addWidget(&field);
    layout.addWidget(&viewport, 1);
    host.resize(800, 600);
    host.show();
    host.raise();
    host.activateWindow();
    REQUIRE(preview.SetScene(scene) == true);
    preview.Navigate(0, {0, 0}, 3, false);
    REQUIRE(preview.Start() == true);
    RequirePresentation(viewport, 0);
    QQuickWindow* surfaceOrNull = nullptr;
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport"))
        {
            surfaceOrNull = qobject_cast<QQuickWindow*>(window);
        }
    }
    REQUIRE(surfaceOrNull != nullptr);
    QQuickWindow& surface = *surfaceOrNull;
    REQUIRE(QTest::qWaitForWindowExposed(&surface) == true);
    NSView* view = GetNativeView(surface);
    [NSApp activateIgnoringOtherApps:YES];
    [view.window makeKeyAndOrderFront:nil];
    QTest::qWait(100);
    field.setFocus();
    const QJsonObject beforeOrbit = preview.Navigation();
    const QImage originalImage = surface.grabWindow();
    REQUIRE(originalImage.isNull() == false);
    const QPoint start(surface.width() / 2, surface.height() / 2);
    const QPoint moved = start + QPoint(60, 25);
    uint64 presented = viewport.PresentedFrames();
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, start, NSEventModifierFlagOption);
    SendNativeMouse(surface, NSEventTypeLeftMouseDragged, moved, NSEventModifierFlagOption);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, moved, NSEventModifierFlagOption);
    REQUIRE(preview.Navigation() != beforeOrbit);
    RequirePresentation(viewport, presented);
    REQUIRE(QTest::qWaitFor([&surface, &originalImage]() { return surface.grabWindow() != originalImage; }, 5000) ==
            true);

    const QJsonObject beforeTrackpadPan = preview.Navigation();
    const QImage beforeTrackpadPanImage = surface.grabWindow();
    const NSEventModifierFlags panModifiers = NSEventModifierFlagOption | NSEventModifierFlagShift;
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, start, panModifiers);
    SendNativeMouse(surface, NSEventTypeLeftMouseDragged, moved, panModifiers);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, moved, panModifiers);
    const QJsonObject afterTrackpadPan = preview.Navigation();
    CHECK(afterTrackpadPan != beforeTrackpadPan);
    CHECK(afterTrackpadPan.value(QStringLiteral("yaw")) == beforeTrackpadPan.value(QStringLiteral("yaw")));
    CHECK(afterTrackpadPan.value(QStringLiteral("pitch")) == beforeTrackpadPan.value(QStringLiteral("pitch")));
    REQUIRE(QTest::qWaitFor(
                [&surface, &beforeTrackpadPanImage]() { return surface.grabWindow() != beforeTrackpadPanImage; },
                5000) == true);

    const QJsonObject beforePan = preview.Navigation();
    presented = viewport.PresentedFrames();
    SendNativeMouse(surface, NSEventTypeOtherMouseDown, start);
    SendNativeMouse(surface, NSEventTypeOtherMouseDragged, moved);
    SendNativeMouse(surface, NSEventTypeOtherMouseUp, moved);
    CHECK(preview.Navigation() != beforePan);
    RequirePresentation(viewport, presented, &preview);

    SendNativeMouse(surface, NSEventTypeRightMouseDown, start);
    const QJsonObject beforeLook = preview.Navigation();
    SendNativeMouse(surface, NSEventTypeRightMouseDragged, moved, 0, moved - start);
    CHECK(preview.Navigation() != beforeLook);
    const QJsonObject beforeFly = preview.Navigation();
    SendNativeKey(surface, 13, @"w", true);
    REQUIRE(QTest::qWaitFor([&preview, &beforeFly]() { return preview.Navigation() != beforeFly; }, 2000) == true);
    SendNativeKey(surface, 13, @"w", false);
    SendNativeMouse(surface, NSEventTypeRightMouseUp, moved);
    const QJsonObject stopped = preview.Navigation();
    QTest::qWait(80);
    CHECK(preview.Navigation() == stopped);
    SendNativeKey(surface, 13, @"w", true);
    SendNativeKey(surface, 13, @"w", false);
    CHECK(field.text().isEmpty() == true);

    preview.SetSelection(QStringLiteral("starter.cube"));
    presented = viewport.PresentedFrames();
    SendNativeKey(surface, 3, @"f", true);
    SendNativeKey(surface, 3, @"f", false);
    CHECK(preview.Navigation() != stopped);
    RequirePresentation(viewport, presented);
    // macOS users can produce a secondary click with Control + the primary
    // button even when their trackpad has no configured two-finger click.
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, start, NSEventModifierFlagControl);
    const QJsonObject beforeControlFly = preview.Navigation();
    SendNativeKey(surface, 13, @"w", true);
    REQUIRE(QTest::qWaitFor([&preview, &beforeControlFly]() { return preview.Navigation() != beforeControlFly; },
                            2000) == true);
    SendNativeKey(surface, 13, @"w", false);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, moved, NSEventModifierFlagControl);
    QToolButton* frameAll = viewport.findChild<QToolButton*>(QStringLiteral("viewportFrameAll"));
    REQUIRE(frameAll != nullptr);
    const QJsonObject beforeFrameAll = preview.Navigation();
    const QPoint frameAllPosition = surface.mapFromGlobal(frameAll->mapToGlobal(frameAll->rect().center()));
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, frameAllPosition);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, frameAllPosition);
    CHECK(preview.Navigation() != beforeFrameAll);
    CHECK(world.Draft() == original);
    CHECK(world.Dirty() == false);
    preview.Stop();
}

TEST_CASE("Supplied imported World accepts native drag navigation with restored camera preferences",
          "[.render-native][imported-native-input]")
{
    const QString root = qEnvironmentVariable("LUDUS_VIEWPORT_TEST_PROJECT");
    if (root.isEmpty() == true)
    {
        SUCCEED("Set LUDUS_VIEWPORT_TEST_PROJECT and LUDUS_VIEWPORT_TEST_WORLD for supplied-scene navigation.");
        return;
    }
    REQUIRE(QApplication::platformName() == QStringLiteral("cocoa"));
    WorldWorkspace workspace;
    ApplyWorkspaceStyle(&workspace);
    workspace.resize(1100, 520);
    workspace.show();
    workspace.raise();
    workspace.activateWindow();
    workspace.SetProject(root, 1);
    REQUIRE(workspace.Open(QDir(root).filePath(qEnvironmentVariable("LUDUS_VIEWPORT_TEST_WORLD"))) == true);
    PreviewViewport* viewport =
        static_cast<PreviewViewport*>(workspace.findChild<QWidget*>(QStringLiteral("worldSceneViewport")));
    LevelWorkspace* scene = workspace.findChild<LevelWorkspace*>();
    REQUIRE(viewport != nullptr);
    REQUIRE(scene != nullptr);
    REQUIRE(QTest::qWaitFor([viewport, scene]() { return scene->Busy() == false && viewport->PresentedFrames() > 0; },
                            30000) == true);
    const QString preferences = qEnvironmentVariable("LUDUS_VIEWPORT_TEST_NAVIGATION");
    if (preferences.isEmpty() == false)
    {
        const QJsonObject saved = QJsonDocument::fromJson(ReadNativeFixture(preferences)).object();
        scene->RestoreNavigation(
            saved.value(QStringLiteral("world")).toObject().value(QStringLiteral("sceneCamera")).toObject());
        RequirePresentation(*viewport, viewport->PresentedFrames());
    }
    QQuickWindow* surfaceOrNull = nullptr;
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport"))
        {
            surfaceOrNull = qobject_cast<QQuickWindow*>(window);
        }
    }
    REQUIRE(surfaceOrNull != nullptr);
    QQuickWindow& surface = *surfaceOrNull;
    REQUIRE(QTest::qWaitForWindowExposed(&surface) == true);
    NSView* view = GetNativeView(surface);
    [NSApp activateIgnoringOtherApps:YES];
    [view.window makeKeyAndOrderFront:nil];
    QTest::qWait(100);
    const QJsonObject originalWorld = workspace.Document().Draft();
    const QPoint start(surface.width() / 2, surface.height() / 2);
    const QPoint moved = start + QPoint(45, -35);
    const QJsonObject originalNavigation = scene->Navigation();
    const QImage originalImage = surface.grabWindow();
    REQUIRE(originalImage.isNull() == false);
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, start, NSEventModifierFlagOption);
    SendNativeMouse(surface, NSEventTypeLeftMouseDragged, moved, NSEventModifierFlagOption);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, moved, NSEventModifierFlagOption);
    CHECK(scene->Navigation() != originalNavigation);
    REQUIRE(QTest::qWaitFor([&surface, &originalImage]() { return surface.grabWindow() != originalImage; }, 10000) ==
            true);
    const QJsonObject beforePan = scene->Navigation();
    const NSEventModifierFlags panModifiers = NSEventModifierFlagOption | NSEventModifierFlagShift;
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, start, panModifiers);
    SendNativeMouse(surface, NSEventTypeLeftMouseDragged, moved, panModifiers);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, moved, panModifiers);
    CHECK(scene->Navigation() != beforePan);
    CHECK(scene->Navigation().value(QStringLiteral("yaw")) == beforePan.value(QStringLiteral("yaw")));
    CHECK(scene->Navigation().value(QStringLiteral("pitch")) == beforePan.value(QStringLiteral("pitch")));
    SendNativeMouse(surface, NSEventTypeRightMouseDown, start);
    const QJsonObject beforeLook = scene->Navigation();
    SendNativeMouse(surface, NSEventTypeRightMouseDragged, moved, 0, moved - start);
    CHECK(scene->Navigation() != beforeLook);
    const QJsonObject beforeFly = scene->Navigation();
    SendNativeKey(surface, 13, @"w", true);
    REQUIRE(QTest::qWaitFor([scene, &beforeFly]() { return scene->Navigation() != beforeFly; }, 2000) == true);
    SendNativeKey(surface, 13, @"w", false);
    SendNativeMouse(surface, NSEventTypeRightMouseUp, moved);
    const QJsonObject stopped = scene->Navigation();
    QTest::qWait(80);
    CHECK(scene->Navigation() == stopped);
    const QString imagePath = qEnvironmentVariable("LUDUS_GPU_NAVIGATION_IMAGE");
    if (imagePath.isEmpty() == false)
    {
        RequirePresentation(*viewport, viewport->PresentedFrames());
        REQUIRE(surface.grabWindow().save(imagePath) == true);
    }
    CHECK(workspace.Document().Draft() == originalWorld);
    CHECK(workspace.Dirty() == false);
    workspace.StopPreview();
}
