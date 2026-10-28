#include "internal/level_workspace.h"
#include "internal/preview_viewport.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"
#include "internal/workspace_style.h"
#include "internal/world_document.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/base/types.h>
#include <ludus/graphics/renderer/render_graph_activation.hpp>

#import <Cocoa/Cocoa.h>

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;

// Thanks to The Qt Company, QCocoaEventDispatcher::processEvents (Qt 6.12):
// https://github.com/qt/qtbase/blob/v6.12.0/src/plugins/platforms/cocoa/qcocoaeventdispatcher.mm
// Manual processEvents polling does not enter NSApp's normal run path. Use an exec-mode
// loop so native display-link/main-queue callbacks can present the independently produced image.
template <class Predicate>
bool WaitForNativeCondition(const Predicate& predicate, int32 timeoutMilliseconds)
{
    if (predicate() == true)
        return true;
    QEventLoop loop;
    QTimer poll;
    QTimer timeout;
    constexpr int32 NATIVE_POLL_MILLISECONDS = 10;
    poll.setInterval(NATIVE_POLL_MILLISECONDS);
    timeout.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&loop, &predicate]() {
        if (predicate() == true)
            loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    timeout.start(timeoutMilliseconds);
    loop.exec();
    return predicate();
}

QByteArray ReadNativeFixture(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

void CheckNativeWorkgroup(const QString& runtime, uint32 width, uint32 height)
{
    const QByteArray bytes = ReadNativeFixture(runtime);
    ludus::graphics::renderer::RenderGraphPackage package;
    REQUIRE(package.Decode({reinterpret_cast<const uint8*>(bytes.constData()), static_cast<usize>(bytes.size())}) ==
            ludus::graphics::renderer::RenderGraphPackageStatus::Ready);
    const ludus::graphics::renderer::RenderGraphPrograms programs = package.GetPrograms({}, {});
    CHECK(programs.RayCompute.WorkgroupSize[0] == width);
    CHECK(programs.RayCompute.WorkgroupSize[1] == height);
    CHECK(programs.RayCompute.WorkgroupSize[2] == 1);
}

NSView* GetNativeView(const QWindow& window)
{
    // Qt's Cocoa WId stores the NSView identity in an integer. This test-only
    // ABI boundary must recover that pointer to enter the real responder chain.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    NSView* view = reinterpret_cast<NSView*>(window.winId());
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
                         RenderGraphPreview* graphOrNull = nullptr,
                         uint64 previousRevision = 0)
{
    [NSApp activateIgnoringOtherApps:YES];
    inoutViewport.window()->raise();
    inoutViewport.window()->activateWindow();
    uint64 requiredSerial = 0;
    const bool hasPresented = WaitForNativeCondition(
        [&inoutViewport, previous, graphOrNull, previousRevision, &requiredSerial]() {
            if (previousRevision != 0 &&
                (graphOrNull == nullptr || graphOrNull->GetPresentedRevision() <= previousRevision))
                return false;
            if (graphOrNull != nullptr && graphOrNull->HasGpuFrame() == true)
            {
                if (requiredSerial == 0)
                    requiredSerial = graphOrNull->GetDisplayFrames().GetLatestSerial();
                if (inoutViewport.GetPresentedGraphFrameSerial() < requiredSerial)
                    return false;
            }
            return inoutViewport.PresentedFrames() > previous;
        },
        10000);
    QStringList nativeWindows;
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() != QStringLiteral("LudusGpuViewport"))
            continue;
        NSView* view = GetNativeView(*window);
        nativeWindows.append(QStringLiteral("%1x%2 exposed=%3 visible=%4 nativeVisible=%5 occlusion=%6 hidden=%7")
                                 .arg(window->width())
                                 .arg(window->height())
                                 .arg(window->isExposed())
                                 .arg(window->isVisible())
                                 .arg(view.window.isVisible)
                                 .arg(static_cast<uint64>(view.window.occlusionState))
                                 .arg(view.isHiddenOrHasHiddenAncestor));
    }
    INFO(nativeWindows.join(QStringLiteral("; ")).toStdString());
    const QString status = previewOrNull != nullptr ? previewOrNull->Status() : QString();
    const uint64 produced = previewOrNull != nullptr ? previewOrNull->Frames() : 0;
    CAPTURE(status.toStdString(),
            produced,
            previous,
            inoutViewport.PresentedFrames(),
            requiredSerial,
            inoutViewport.GetPresentedGraphFrameSerial());
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

namespace
{
void RunNativeViewportFixture(const QString& runtime, bool shouldVerifyReplacement)
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
        // Cooked candidates are supplied explicitly below. Cancel the built-in document's
        // debounced live preview so it cannot replace the requested MIS graph during the test.
        workspace.SetLivePreview(false);
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
        const uint64 previousCandidate = graph.GetReport().CandidateGeneration;
        REQUIRE(graph.Load(runtime) == RenderGraphPreviewLoad::Staged);
        CHECK(viewport.HasRetainedGraphFrame() == true);
        [NSApp activateIgnoringOtherApps:YES];
        host.raise();
        host.activateWindow();
        uint64 candidate = 0;
        uint64 requiredSerial = 0;
        const bool hasPresentedCandidate = WaitForNativeCondition(
            [&graph, &viewport, &candidate, &requiredSerial, previousCandidate, previousPaint]() {
                candidate = graph.GetReport().CandidateGeneration;
                if (candidate <= previousCandidate || graph.GetPresentedGeneration() != candidate)
                    return false;
                if (requiredSerial == 0)
                    requiredSerial = graph.GetDisplayFrames().GetLatestSerial();
                return viewport.PresentedFrames() > previousPaint &&
                       viewport.GetPresentedGraphFrameSerial() >= requiredSerial;
            },
            30000);
        QStringList windows;
        for (const QWindow* window : QGuiApplication::allWindows())
        {
            if (window->objectName() == QStringLiteral("LudusGpuViewport"))
                windows.append(QStringLiteral("%1x%2 at %3,%4 exposed=%5 visible=%6 nativeVisible=%7 occlusion=%8")
                                   .arg(window->width())
                                   .arg(window->height())
                                   .arg(window->x())
                                   .arg(window->y())
                                   .arg(window->isExposed() == true ? 1 : 0)
                                   .arg(window->isVisible() == true ? 1 : 0)
                                   .arg(GetNativeView(*window).window.isVisible)
                                   .arg(static_cast<uint64>(GetNativeView(*window).window.occlusionState)));
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
        CAPTURE(runtime.toStdString());
        REQUIRE(graph.GetRequestedPackagePath() == runtime);
        CHECK(graph.GetDisplayStatus().isEmpty() == true);
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
    const auto getGraphRevision = [&workspace, &runtime]() -> uint64 {
        return runtime.isEmpty() == true ? 0 : workspace.GetPreviewOrNull()->GetPresentedRevision();
    };
    uint64 previousRevision = getGraphRevision();
    uint64 presented = viewport.PresentedFrames();
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, start, NSEventModifierFlagOption);
    SendNativeMouse(surface, NSEventTypeLeftMouseDragged, moved, NSEventModifierFlagOption);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, moved, NSEventModifierFlagOption);
    REQUIRE(preview.Navigation() != beforeOrbit);
    RequirePresentation(viewport, presented, &preview, workspace.GetPreviewOrNull(), previousRevision);
    if (runtime.isEmpty() == true)
        REQUIRE(WaitForNativeCondition([&surface, &originalImage]() { return surface.grabWindow() != originalImage; },
                                       5000) == true);

    const QJsonObject beforeTrackpadPan = preview.Navigation();
    const uint64 beforeTrackpadPaint = viewport.PresentedFrames();
    const uint64 beforeTrackpadRevision = getGraphRevision();
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
        REQUIRE(WaitForNativeCondition(
                    [&surface, &beforeTrackpadPanImage]() { return surface.grabWindow() != beforeTrackpadPanImage; },
                    5000) == true);
    else
        RequirePresentation(viewport,
                            beforeTrackpadPaint,
                            &preview,
                            workspace.GetPreviewOrNull(),
                            beforeTrackpadRevision);

    const QJsonObject beforePan = preview.Navigation();
    presented = viewport.PresentedFrames();
    previousRevision = getGraphRevision();
    SendNativeMouse(surface, NSEventTypeOtherMouseDown, start);
    SendNativeMouse(surface, NSEventTypeOtherMouseDragged, moved);
    SendNativeMouse(surface, NSEventTypeOtherMouseUp, moved);
    CHECK(preview.Navigation() != beforePan);
    RequirePresentation(viewport, presented, &preview, workspace.GetPreviewOrNull(), previousRevision);

    presented = viewport.PresentedFrames();
    previousRevision = getGraphRevision();
    SendNativeMouse(surface, NSEventTypeRightMouseDown, start);
    const QJsonObject beforeLook = preview.Navigation();
    SendNativeMouse(surface, NSEventTypeRightMouseDragged, moved, 0, moved - start);
    CHECK(preview.Navigation() != beforeLook);
    const QJsonObject beforeFly = preview.Navigation();
    SendNativeKey(surface, 13, @"w", true);
    REQUIRE(WaitForNativeCondition([&preview, &beforeFly]() { return preview.Navigation() != beforeFly; }, 2000) ==
            true);
    SendNativeKey(surface, 13, @"w", false);
    SendNativeMouse(surface, NSEventTypeRightMouseUp, moved);
    RequirePresentation(viewport, presented, &preview, workspace.GetPreviewOrNull(), previousRevision);
    const QJsonObject stopped = preview.Navigation();
    QTest::qWait(80);
    CHECK(preview.Navigation() == stopped);
    SendNativeKey(surface, 13, @"w", true);
    SendNativeKey(surface, 13, @"w", false);
    CHECK(field.text().isEmpty() == true);

    preview.SetSelection(QStringLiteral("starter.cube"));
    presented = viewport.PresentedFrames();
    previousRevision = getGraphRevision();
    SendNativeKey(surface, 3, @"f", true);
    SendNativeKey(surface, 3, @"f", false);
    CHECK(preview.Navigation() != stopped);
    RequirePresentation(viewport, presented, &preview, workspace.GetPreviewOrNull(), previousRevision);
    // macOS users can produce a secondary click with Control + the primary
    // button even when their trackpad has no configured two-finger click.
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, start, NSEventModifierFlagControl);
    const QJsonObject beforeControlFly = preview.Navigation();
    SendNativeKey(surface, 13, @"w", true);
    REQUIRE(WaitForNativeCondition([&preview, &beforeControlFly]() { return preview.Navigation() != beforeControlFly; },
                                   2000) == true);
    SendNativeKey(surface, 13, @"w", false);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, moved, NSEventModifierFlagControl);
    QToolButton* frameAll = viewport.findChild<QToolButton*>(QStringLiteral("viewportFrameAll"));
    REQUIRE(frameAll != nullptr);
    const QJsonObject beforeFrameAll = preview.Navigation();
    presented = viewport.PresentedFrames();
    previousRevision = getGraphRevision();
    const QPoint frameAllPosition = surface.mapFromGlobal(frameAll->mapToGlobal(frameAll->rect().center()));
    SendNativeMouse(surface, NSEventTypeLeftMouseDown, frameAllPosition);
    SendNativeMouse(surface, NSEventTypeLeftMouseUp, frameAllPosition);
    CHECK(preview.Navigation() != beforeFrameAll);
    RequirePresentation(viewport, presented, &preview, workspace.GetPreviewOrNull(), previousRevision);
    CHECK(world.Draft() == original);
    CHECK(world.Dirty() == false);
    if (runtime.isEmpty() == false)
    {
        RenderGraphPreview& graph = *workspace.GetPreviewOrNull();
        CHECK(graph.HasGpuFrame() == true);
        CHECK(viewport.HasRetainedGraphFrame() == true);
        const QSize beforeResize = surface.size();
        const QSize beforeTarget = graph.GetTargetSize();
        presented = viewport.PresentedFrames();
        previousRevision = getGraphRevision();
        host.resize(551, 417);
        RequirePresentation(viewport, presented, &preview, &graph, previousRevision);
        CHECK(surface.size() != beforeResize);
        CHECK(graph.GetTargetSize() != beforeTarget);
        const uint64 generation = graph.GetPresentedGeneration();
        const uint64 revision = graph.GetPresentedRevision();
        const uint64 beforeEdit = viewport.PresentedFrames();
        // Reload an edited current World through the same extraction boundary used by the project.
        QJsonObject edited = original;
        QJsonArray entities = edited.value(QStringLiteral("entities")).toArray();
        REQUIRE(entities.isEmpty() == false);
        bool hasEditedCube = false;
        for (int32 index = 0; index < entities.size(); ++index)
        {
            QJsonObject entity = entities.at(index).toObject();
            if (entity.value(QStringLiteral("id")).toString() != QStringLiteral("starter.cube"))
                continue;
            QJsonObject components = entity.value(QStringLiteral("components")).toObject();
            QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
            transform.insert(QStringLiteral("x"), -1);
            components.insert(QStringLiteral("transform"), transform);
            entity.insert(QStringLiteral("components"), components);
            entities.replace(index, entity);
            hasEditedCube = true;
        }
        REQUIRE(hasEditedCube == true);
        edited.insert(QStringLiteral("entities"), entities);
        REQUIRE(world.Load(QJsonDocument(edited).toJson(), error) == true);
        REQUIRE(RenderScene::Load(root, world, scene, error) == true);
        REQUIRE(preview.SetScene(scene) == true);
        REQUIRE(WaitForNativeCondition([&graph, revision]() { return graph.GetPresentedRevision() > revision; },
                                       10000) == true);
        RequirePresentation(viewport, beforeEdit, &preview, &graph, revision);
        CHECK(graph.GetPresentedGeneration() == generation);
        CHECK(graph.HasGpuFrame() == true);
        if (shouldVerifyReplacement == true)
        {
            const uint64 retainedGeneration = graph.GetPresentedGeneration();
            QSignalSpy frameResets(&graph.GetDisplayFrames(), &ViewportFrameSource::ResettingFrames);
            const QByteArray originalPackage = ReadNativeFixture(runtime);
            QByteArray brokenPackage = originalPackage;
            const QByteArray validInclude = QByteArrayLiteral("#include <metal_stdlib>");
            const QByteArray brokenInclude = QByteArrayLiteral("#include <xxxxx_stdlib>");
            REQUIRE(brokenPackage.contains(validInclude) == true);
            brokenPackage.replace(validInclude, brokenInclude);
            const QString brokenPath = root + QStringLiteral("/broken.lurgrt");
            QFile brokenFile(brokenPath);
            REQUIRE(brokenFile.open(QIODevice::WriteOnly) == true);
            REQUIRE(brokenFile.write(brokenPackage) == brokenPackage.size());
            brokenFile.close();
            const RenderGraphPreviewLoad failedLoad = graph.Load(brokenPath);
            REQUIRE((failedLoad == RenderGraphPreviewLoad::Staged || failedLoad == RenderGraphPreviewLoad::Failed));
            CHECK(graph.GetPresentedGeneration() == retainedGeneration);
            CHECK(viewport.HasRetainedGraphFrame() == true);
            bool hasLostDrawable = false;
            const auto hasFailed = [&]() {
                hasLostDrawable = hasLostDrawable == true || viewport.HasRetainedGraphFrame() == false ||
                                  graph.HasGpuFrame() == false || graph.GetPresentedGeneration() != retainedGeneration;
                return graph.GetReport().Candidate == ludus::graphics::renderer::RenderGraphCandidateState::Failed;
            };
            REQUIRE(WaitForNativeCondition(hasFailed, 10000) == true);
            CHECK(hasLostDrawable == false);
            CHECK(frameResets.count() == 0);
            CHECK(graph.GetReport().LastFailure == ludus::graphics::rhi::RasterStatus::Failed);

            // The drawing generation still accepts the current camera after a replacement fails.
            const uint64 failedRevision = graph.GetPresentedRevision();
            const uint64 failedPaint = viewport.PresentedFrames();
            SendNativeMouse(surface, NSEventTypeLeftMouseDown, start, NSEventModifierFlagOption);
            SendNativeMouse(surface, NSEventTypeLeftMouseDragged, moved, NSEventModifierFlagOption);
            SendNativeMouse(surface, NSEventTypeLeftMouseUp, moved, NSEventModifierFlagOption);
            REQUIRE(WaitForNativeCondition(
                        [&graph, failedRevision]() { return graph.GetPresentedRevision() > failedRevision; },
                        10000) == true);
            RequirePresentation(viewport, failedPaint, &preview, &graph, failedRevision);
            CHECK(graph.GetPresentedGeneration() == retainedGeneration);

            const uint64 beforeReplacement = viewport.PresentedFrames();
            const uint64 previousCandidate = graph.GetReport().CandidateGeneration;
            REQUIRE(graph.Load(runtime) == RenderGraphPreviewLoad::Staged);
            uint64 replacementGeneration = 0;
            uint64 requiredReplacementSerial = 0;
            CHECK(graph.GetPresentedGeneration() == retainedGeneration);
            CHECK(viewport.HasRetainedGraphFrame() == true);
            const auto hasReplaced = [&]() {
                hasLostDrawable = hasLostDrawable == true || viewport.HasRetainedGraphFrame() == false ||
                                  graph.HasGpuFrame() == false;
                replacementGeneration = graph.GetReport().CandidateGeneration;
                if (replacementGeneration <= previousCandidate ||
                    graph.GetPresentedGeneration() != replacementGeneration)
                    return false;
                if (requiredReplacementSerial == 0)
                    requiredReplacementSerial = graph.GetDisplayFrames().GetLatestSerial();
                return viewport.PresentedFrames() > beforeReplacement &&
                       viewport.GetPresentedGraphFrameSerial() >= requiredReplacementSerial;
            };
            const bool hasPresentedReplacement = WaitForNativeCondition(hasReplaced, 10000);
            INFO(graph.GetStatus().toStdString());
            CAPTURE(retainedGeneration,
                    replacementGeneration,
                    graph.GetPresentedGeneration(),
                    graph.GetReport().ActiveGeneration,
                    static_cast<uint32>(graph.GetReport().Candidate),
                    static_cast<uint32>(graph.GetReport().LastFailure),
                    beforeReplacement,
                    viewport.PresentedFrames(),
                    graph.HasPendingWork(),
                    graph.IsCopyingOutput(),
                    graph.GetDisplayFrames().GetLatestSerial(),
                    requiredReplacementSerial,
                    viewport.GetPresentedGraphFrameSerial());
            REQUIRE(hasPresentedReplacement == true);
            CHECK(hasLostDrawable == false);
            CHECK(frameResets.count() == 0);
            CHECK(graph.GetReport().Candidate == ludus::graphics::renderer::RenderGraphCandidateState::None);
            CHECK(graph.GetReport().ActiveGeneration == replacementGeneration);
            CHECK(graph.GetDisplayStatus().isEmpty() == true);
        }
        const QString capture = qEnvironmentVariable("LUDUS_NATIVE_GRAPH_CAPTURE");
        if (capture.isEmpty() == false)
            REQUIRE(surface.grabWindow().save(capture) == true);
    }
    preview.Stop();
    workspace.SetPreviewSource(nullptr);
}

} // namespace

TEST_CASE("Cocoa embedded Metal viewport routes native navigation and releases capture",
          "[.render-native][viewport-native-input]")
{
    RunNativeViewportFixture(qEnvironmentVariable("LUDUS_NATIVE_GRAPH_RUNTIME"), false);
}

TEST_CASE("MIS viewport presents both workgroup shapes and retains output through graph replacement",
          "[.render-native][viewport-native-mis]")
{
    SECTION("64x1")
    {
        CheckNativeWorkgroup(QStringLiteral(LUDUS_EDITOR_MIS_LINEAR_FIXTURE), 64, 1);
        RunNativeViewportFixture(QStringLiteral(LUDUS_EDITOR_MIS_LINEAR_FIXTURE), true);
    }
    SECTION("8x8")
    {
        CheckNativeWorkgroup(QStringLiteral(LUDUS_EDITOR_MIS_TILED_FIXTURE), 8, 8);
        RunNativeViewportFixture(QStringLiteral(LUDUS_EDITOR_MIS_TILED_FIXTURE), true);
    }
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
    REQUIRE(WaitForNativeCondition(
                [viewport, scene]() { return scene->Busy() == false && viewport->PresentedFrames() > 0; },
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
    REQUIRE(WaitForNativeCondition([&surface, &originalImage]() { return surface.grabWindow() != originalImage; },
                                   10000) == true);
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
    REQUIRE(WaitForNativeCondition([scene, &beforeFly]() { return scene->Navigation() != beforeFly; }, 2000) == true);
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
