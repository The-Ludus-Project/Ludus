#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/viewport_frame_source.h"

#include <ludus/graphics/renderer/viewport.hpp>
#include <ludus/graphics/rhi/rhi.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QObject>
#include <QString>
#include <QTest>
#include <QtGlobal>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;

namespace
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
/// Frames the viewport presents in each phase of the test.
constexpr uint64 PRESENTED_FRAMES = 3;
/// Upper bound for device startup, preparation and presentation on a software rasterizer.
constexpr int32 PRESENT_TIMEOUT_MILLISECONDS = 60000;
/// Qt event-loop slice between checks while waiting for frames.
constexpr int32 POLL_MILLISECONDS = 20;
/// Embedded viewport size in device-independent pixels.
constexpr int32 VIEWPORT_WIDTH = 320;
constexpr int32 VIEWPORT_HEIGHT = 240;

/// Test source that lends a RenderPreview's frames through the ViewportFrameSource contract
/// and counts what the viewport takes, releases and requests.
class ForwardingFrameSource final : public ViewportFrameSource
{
public:
    explicit ForwardingFrameSource(RenderPreview& inoutProducer) : mProducer(inoutProducer)
    {
        QObject::connect(&inoutProducer, &ViewportFrameSource::FrameReady, this, &ViewportFrameSource::FrameReady);
        // The producer reclaims its images when it stops; the viewport must release them first.
        QObject::connect(&inoutProducer,
                         &ViewportFrameSource::ResettingFrames,
                         this,
                         &ViewportFrameSource::ResettingFrames);
    }
    [[nodiscard]] bool HasPendingFrame() const noexcept override
    {
        return mProducer.HasPendingFrame();
    }
    [[nodiscard]] bool TakeFrame(renderer::ViewportFrame& outFrame, renderer::ViewportImage& outImage) noexcept override
    {
        if (mProducer.TakeFrame(outFrame, outImage) == false)
        {
            return false;
        }
        ++mTakenFrames;
        return true;
    }
    void ReleaseFrame(renderer::ViewportFrame& inoutFrame) noexcept override
    {
        ++mReleasedFrames;
        mProducer.ReleaseFrame(inoutFrame);
    }
    void Resize(int32 width, int32 height) override
    {
        ++mResizeRequests;
        mProducer.Resize(width, height);
    }
    /// Ask every consumer to return its leases, as a source does before replacing its images.
    void ReclaimLeases()
    {
        Q_EMIT ResettingFrames();
    }
    [[nodiscard]] uint64 GetTakenFrames() const noexcept
    {
        return mTakenFrames;
    }
    [[nodiscard]] uint64 GetOutstandingFrames() const noexcept
    {
        return mTakenFrames - mReleasedFrames;
    }
    [[nodiscard]] uint64 GetResizeRequests() const noexcept
    {
        return mResizeRequests;
    }

private:
    RenderPreview& mProducer;
    uint64 mTakenFrames = 0;
    uint64 mReleasedFrames = 0;
    uint64 mResizeRequests = 0;
};

RenderScene MakeTriangleScene()
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("frame-source");
    scene.AssetsIdentity = scene.Identity;
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Uvs = {{0, 0}, {1, 0}, {.5F, 1}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("triangle");
    scene.Objects.append(object);
    return scene;
}

/// Waits until the viewport has presented target frames, the preview stops, or the timeout expires.
bool AwaitPresentedFrames(const PreviewViewport& viewport, const RenderPreview& preview, uint64 target)
{
    QElapsedTimer wait;
    wait.start();
    while (viewport.PresentedFrames() < target && preview.Running() == true &&
           wait.elapsed() < PRESENT_TIMEOUT_MILLISECONDS)
    {
        QTest::qWait(POLL_MILLISECONDS);
    }
    return viewport.PresentedFrames() >= target;
}

/// Restores LUDUS_EDITOR_VIEWPORT when the test ends.
class ViewportEnvironmentGuard final
{
public:
    ViewportEnvironmentGuard() : mPrevious(qgetenv("LUDUS_EDITOR_VIEWPORT")) {}
    ~ViewportEnvironmentGuard()
    {
        if (mPrevious.isEmpty() == true)
        {
            qunsetenv("LUDUS_EDITOR_VIEWPORT");
        }
        else
        {
            qputenv("LUDUS_EDITOR_VIEWPORT", mPrevious);
        }
        rhi::Shutdown();
    }

private:
    QByteArray mPrevious;
};
#endif
} // namespace

TEST_CASE("Viewport takes and releases every frame through its frame source", "[editor][viewport][frame-source][gpu]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    const ViewportEnvironmentGuard guard;
#    if defined(Q_OS_MACOS)
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
    {
        SKIP("macOS presents only through the Metal-shared device (run with QT_QPA_PLATFORM=cocoa)");
    }
#    else
    // Readback runs without a display; it shares the lease path's take and release calls.
    qputenv("LUDUS_EDITOR_VIEWPORT", QByteArrayLiteral("offscreen"));
#    endif
    rhi::Shutdown();
    RenderPreview preview;
    ForwardingFrameSource source(preview);
    PreviewViewport viewport(&preview, &source);
    viewport.resize(VIEWPORT_WIDTH, VIEWPORT_HEIGHT);
    viewport.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport) == true);
    REQUIRE(preview.SetScene(MakeTriangleScene()) == true);
    const bool hasStarted = preview.Start();
    if (hasStarted == false || preview.Running() == false)
    {
        SKIP("No GPU device for the viewport (install lavapipe on Linux): " << preview.Status().toStdString());
    }
    INFO(preview.Status().toStdString());
    REQUIRE(AwaitPresentedFrames(viewport, preview, PRESENTED_FRAMES) == true);
    // Every presented image was leased from the source, never from the preview directly.
    CHECK(source.GetTakenFrames() >= viewport.PresentedFrames());
    CHECK(source.GetResizeRequests() != 0);
    // Two presentation slots bound what the viewport may hold at once.
    CHECK(source.GetOutstandingFrames() <= 2);

    // A source reset makes the viewport return every lease before the signal returns.
    source.ReclaimLeases();
    CHECK(source.GetOutstandingFrames() == 0);
    // Presentation resumes from the next frame the source publishes.
    CHECK(AwaitPresentedFrames(viewport, preview, viewport.PresentedFrames() + PRESENTED_FRAMES) == true);

    // Stopping the producer forwards its reset, so no lease outlives the device.
    preview.Stop();
    CHECK(source.GetOutstandingFrames() == 0);
    CHECK(source.GetTakenFrames() >= viewport.PresentedFrames());
#else
    SKIP("Frame-source presentation requires the native GPU preview");
#endif
}
