#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QQuickWindow>
#include <QStringList>
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

void RequirePresentation(PreviewViewport& inoutViewport,
                         uint64 previous,
                         const RenderPreview* previewOrNull = nullptr,
                         const RenderGraphPreview* graphOrNull = nullptr)
{
    [NSApp activateIgnoringOtherApps:YES];
    inoutViewport.window()->raise();
    inoutViewport.window()->activateWindow();
    const bool hasPresented =
        QTest::qWaitFor([&inoutViewport, previous]() { return inoutViewport.PresentedFrames() > previous; }, 10000);
    const QString status = previewOrNull != nullptr ? previewOrNull->Status() : QString();
    const uint64 produced = previewOrNull != nullptr ? previewOrNull->Frames() : 0;
    CAPTURE(status.toStdString(), produced, previous, inoutViewport.PresentedFrames());
    const QString graphStatus = graphOrNull != nullptr ? graphOrNull->GetStatus() : QString();
    const uint64 revision = graphOrNull != nullptr ? graphOrNull->GetPresentedRevision() : 0;
    const uint64 generation = graphOrNull != nullptr ? graphOrNull->GetPresentedGeneration() : 0;
    const bool hasGpuFrame = graphOrNull != nullptr && graphOrNull->HasGpuFrame() == true;
    const bool hasPendingWork = graphOrNull != nullptr && graphOrNull->HasPendingWork() == true;
    const bool isCopying = graphOrNull != nullptr && graphOrNull->IsCopyingOutput() == true;
    CAPTURE(graphStatus.toStdString(), revision, generation, hasGpuFrame, hasPendingWork, isCopying);
    REQUIRE(hasPresented == true);
}
} // namespace

namespace ludus::editor::tests
{
/// Bring the pacing fixture to the foreground, as an actual viewport drag does.
void ActivateViewportForPacing(PreviewViewport& inoutViewport)
{
    [NSApp activateIgnoringOtherApps:YES];
    inoutViewport.raise();
    inoutViewport.activateWindow();
}
} // namespace ludus::editor::tests

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
    const QString runtime = qEnvironmentVariable("LUDUS_NATIVE_GRAPH_RUNTIME");
    WorldDocument world;
    QString error;
    REQUIRE(world.LoadSchema(ReadNativeFixture(root + QStringLiteral("/authoring/world.schema.json")), error) == true);
    REQUIRE(world.Load(ReadNativeFixture(root + QStringLiteral("/authoring/rendering.world.json")), error) == true);
    if (runtime.isEmpty() == false)
    {
        // Explicitly author the fixture's point light: the graph does not invent raster preview lighting.
        QJsonObject litWorld = world.Draft();
        QJsonArray entities = litWorld.value(QStringLiteral("entities")).toArray();
        for (int32 index = 0; index < entities.size(); ++index)
        {
            QJsonObject entity = entities.at(index).toObject();
            if (entity.value(QStringLiteral("id")).toString() != QStringLiteral("preview.camera"))
                continue;
            QJsonObject components = entity.value(QStringLiteral("components")).toObject();
            components.insert(QStringLiteral("light"),
                              QJsonObject{{QStringLiteral("enabled"), true},
                                          {QStringLiteral("kind"), QStringLiteral("point")},
                                          {QStringLiteral("r"), 1},
                                          {QStringLiteral("g"), 1},
                                          {QStringLiteral("b"), 1},
                                          {QStringLiteral("intensity"), 20},
                                          {QStringLiteral("range"), 100},
                                          {QStringLiteral("innerAngle"), 10},
                                          {QStringLiteral("outerAngle"), 40}});
            entity.insert(QStringLiteral("components"), components);
            entities.replace(index, entity);
        }
        litWorld.insert(QStringLiteral("entities"), entities);
        REQUIRE(world.Load(QJsonDocument(litWorld).toJson(), error) == true);
    }
    const QJsonObject original = world.Draft();
    RenderScene scene;
    REQUIRE(RenderScene::Load(root, world, scene, error) == true);
    RendererWorkspace workspace({});
    RenderPreview preview;
    QWidget host;
    QVBoxLayout layout(&host);
    QLineEdit field;
    PreviewViewport viewport(&preview);
    if (runtime.isEmpty() == false)
    {
        REQUIRE(scene.AuthoredLights.Count == 1);
        viewport.UseProjectGraph(workspace);
        workspace.SetPreviewSource(&preview);
        workspace.SetProject(root, 1);
        workspace.EnableWorldRendering();
        REQUIRE(workspace.GetPreviewOrNull() != nullptr);
    }
    layout.addWidget(&field);
    layout.addWidget(&viewport, 1);
    host.resize(runtime.isEmpty() == true ? 800 : 480, runtime.isEmpty() == true ? 600 : 360);
    if (runtime.isEmpty() == false)
    {
        // Qt stops painting an occluded native child; keep the GPU acceptance fixture exposed.
        host.setWindowFlag(Qt::WindowStaysOnTopHint);
        host.move(100, 100);
    }
    host.show();
    host.raise();
    host.activateWindow();
    REQUIRE(preview.SetScene(scene) == true);
    preview.Navigate(0, {0, 0}, 3, false);
    REQUIRE(preview.Start() == true);
    RequirePresentation(viewport, 0, &preview, workspace.GetPreviewOrNull());
    if (runtime.isEmpty() == false)
    {
        RenderGraphPreview& graph = *workspace.GetPreviewOrNull();
        const uint64 previousPaint = viewport.PresentedFrames();
        REQUIRE(graph.Load(runtime) == RenderGraphPreviewLoad::Staged);
        CHECK(viewport.HasRetainedGraphFrame() == true);
        [NSApp activateIgnoringOtherApps:YES];
        host.raise();
        host.activateWindow();
        const uint64 candidate = graph.GetReport().CandidateGeneration;
        const bool hasPresentedCandidate = QTest::qWaitFor(
            [&graph, &viewport, candidate, previousPaint]() {
                return graph.GetPresentedGeneration() == candidate && viewport.PresentedFrames() > previousPaint;
            },
            30000);
        QStringList windows;
        for (const QWindow* window : QGuiApplication::allWindows())
        {
            if (window->objectName() == QStringLiteral("LudusGpuViewport"))
                windows.append(QStringLiteral("%1x%2 at %3,%4 exposed=%5 visible=%6")
                                   .arg(window->width())
                                   .arg(window->height())
                                   .arg(window->x())
                                   .arg(window->y())
                                   .arg(window->isExposed() == true ? 1 : 0)
                                   .arg(window->isVisible() == true ? 1 : 0));
        }
        INFO(windows.join(QStringLiteral("; ")).toStdString());
        INFO("viewport " << viewport.width() << "x" << viewport.height() << ", paints=" << viewport.PresentedFrames()
                         << ", latest=" << graph.GetDisplayFrames().GetLatestSerial()
                         << ", gpu=" << graph.HasGpuFrame());
        INFO(graph.GetStatus().toStdString());
        INFO("candidate=" << candidate << ", presented=" << graph.GetPresentedGeneration()
                          << ", active=" << graph.GetReport().ActiveGeneration
                          << ", failure=" << static_cast<uint32>(graph.GetReport().LastFailure));
        REQUIRE(hasPresentedCandidate == true);
        INFO(graph.GetRequestedPackagePath().toStdString());
        CHECK(graph.GetRequestedPackagePath() == runtime);
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
    field.setFocus();
    const QJsonObject beforeOrbit = preview.Navigation();
    const QImage originalImage = runtime.isEmpty() == true ? surface.grabWindow() : QImage{};
    if (runtime.isEmpty() == true)
        REQUIRE(originalImage.isNull() == false);
    const QPoint start(surface.width() / 2, surface.height() / 2);
    const QPoint moved = start + QPoint(60, 25);
    uint64 presented = viewport.PresentedFrames();
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, start, NSEventModifierFlagOption);
    SendNativeMouse(surface, NSEventTypeLeftMouseDragged, moved, NSEventModifierFlagOption);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, moved, NSEventModifierFlagOption);
    REQUIRE(preview.Navigation() != beforeOrbit);
    RequirePresentation(viewport, presented, &preview, workspace.GetPreviewOrNull());
    if (runtime.isEmpty() == true)
        REQUIRE(QTest::qWaitFor([&surface, &originalImage]() { return surface.grabWindow() != originalImage; }, 5000) ==
                true);

    const QJsonObject beforeTrackpadPan = preview.Navigation();
    const uint64 beforeTrackpadPaint = viewport.PresentedFrames();
    const QImage beforeTrackpadPanImage = runtime.isEmpty() == true ? surface.grabWindow() : QImage{};
    const NSEventModifierFlags panModifiers = NSEventModifierFlagOption | NSEventModifierFlagShift;
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, start, panModifiers);
    SendNativeMouse(surface, NSEventTypeLeftMouseDragged, moved, panModifiers);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, moved, panModifiers);
    const QJsonObject afterTrackpadPan = preview.Navigation();
    CHECK(afterTrackpadPan != beforeTrackpadPan);
    CHECK(afterTrackpadPan.value(QStringLiteral("yaw")) == beforeTrackpadPan.value(QStringLiteral("yaw")));
    CHECK(afterTrackpadPan.value(QStringLiteral("pitch")) == beforeTrackpadPan.value(QStringLiteral("pitch")));
    if (runtime.isEmpty() == true)
        REQUIRE(QTest::qWaitFor(
                    [&surface, &beforeTrackpadPanImage]() { return surface.grabWindow() != beforeTrackpadPanImage; },
                    5000) == true);
    else
        RequirePresentation(viewport, beforeTrackpadPaint, &preview, workspace.GetPreviewOrNull());

    const QJsonObject beforePan = preview.Navigation();
    presented = viewport.PresentedFrames();
    SendNativeMouse(surface, NSEventTypeOtherMouseDown, start);
    SendNativeMouse(surface, NSEventTypeOtherMouseDragged, moved);
    SendNativeMouse(surface, NSEventTypeOtherMouseUp, moved);
    CHECK(preview.Navigation() != beforePan);
    RequirePresentation(viewport, presented, &preview, workspace.GetPreviewOrNull());

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
    RequirePresentation(viewport, presented, &preview, workspace.GetPreviewOrNull());
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
    if (runtime.isEmpty() == false)
    {
        RenderGraphPreview& graph = *workspace.GetPreviewOrNull();
        CHECK(graph.HasGpuFrame() == true);
        CHECK(viewport.HasRetainedGraphFrame() == true);
        const QSize beforeResize = surface.size();
        presented = viewport.PresentedFrames();
        host.resize(551, 417);
        RequirePresentation(viewport, presented, &preview, workspace.GetPreviewOrNull());
        CHECK(surface.size() != beforeResize);
        const uint64 generation = graph.GetPresentedGeneration();
        const uint64 revision = graph.GetPresentedRevision();
        const uint64 beforeEdit = viewport.PresentedFrames();
        // Reload an edited current World through the same extraction boundary used by the project.
        QJsonObject edited = original;
        QJsonArray entities = edited.value(QStringLiteral("entities")).toArray();
        REQUIRE(entities.isEmpty() == false);
        QJsonObject entity = entities.at(0).toObject();
        QJsonObject components = entity.value(QStringLiteral("components")).toObject();
        QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
        transform.insert(QStringLiteral("x"), -1);
        components.insert(QStringLiteral("transform"), transform);
        entity.insert(QStringLiteral("components"), components);
        entities.replace(0, entity);
        edited.insert(QStringLiteral("entities"), entities);
        REQUIRE(world.Load(QJsonDocument(edited).toJson(), error) == true);
        REQUIRE(RenderScene::Load(root, world, scene, error) == true);
        REQUIRE(preview.SetScene(scene) == true);
        REQUIRE(QTest::qWaitFor([&graph, revision]() { return graph.GetPresentedRevision() > revision; }, 10000) ==
                true);
        REQUIRE(QTest::qWaitFor([&viewport, beforeEdit]() { return viewport.PresentedFrames() > beforeEdit; }, 10000) ==
                true);
        CHECK(graph.GetPresentedGeneration() == generation);
        CHECK(graph.HasGpuFrame() == true);
        const QString capture = qEnvironmentVariable("LUDUS_NATIVE_GRAPH_CAPTURE");
        if (capture.isEmpty() == false)
            REQUIRE(surface.grabWindow().save(capture) == true);
    }
    preview.Stop();
    workspace.SetPreviewSource(nullptr);
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
