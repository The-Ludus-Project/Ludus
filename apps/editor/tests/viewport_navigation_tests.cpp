#include "internal/editor_camera_controller.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "viewport_fixture.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/gameplay/camera/camera.hpp>

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileInfo>
#include <QFocusEvent>
#include <QImage>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::editor;
namespace math = ludus::foundation::math;
namespace camera = ludus::gameplay::camera;
namespace rr = ludus::graphics::renderer;
#if defined(Q_OS_MACOS) && defined(LUDUS_EDITOR_NATIVE_PREVIEW)
namespace ludus::editor::tests
{
void ActivateViewportForPacing(PreviewViewport& inoutViewport);
}
#endif
namespace
{
RenderScene MakeNavigationScene()
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("navigation-fixture");
    scene.Radius = 1000;
    PreviewMesh mesh;
    mesh.Positions = {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
    scene.Meshes.push_back(mesh);
    scene.Textures.push_back({});
    scene.Surfaces.push_back({});
    PreviewObject object;
    object.Id = QStringLiteral("selected-cube");
    object.Texture = 0;
    object.Surface = 0;
    object.Transform = math::ToAffine3(math::TransformTRS{});
    scene.Objects.push_back(object);
    PreviewCamera authored;
    authored.Id = QStringLiteral("authored-camera");
    authored.Sample.Pose.Position = {0, 1, 6};
    REQUIRE(math::TryFromAxisAngle({0, 0, 1}, 0.3F, authored.Sample.Pose.Orientation) == math::MathStatus::Success);
    authored.Sample.Lens.Projection = camera::CameraProjection::PerspectiveInfinite;
    authored.Sample.Lens.NearPlane = 0.01F;
    scene.Cameras.push_back(authored);
    return scene;
}
} // namespace
TEST_CASE("Detaching an authored viewport camera preserves its pose lens and visible image", "[editor][navigation]")
{
    RenderPreview preview;
    const RenderScene scene = MakeNavigationScene();
    REQUIRE(preview.SetScene(scene) == true);
    preview.Navigate(0, {25, 20}, 3, false);
    rr::ViewDescription before;
    rr::ViewDescription after;
    QString error;
    REQUIRE(preview.DescribeView(800, 600, before, error) == true);
    const QJsonObject authoredState = preview.Navigation();
    preview.Zoom(0);
    preview.Orbit(0, 0);
    preview.Pan(0, 0);
    CHECK(preview.Navigation() == authoredState);
    preview.Navigate(-1, {-80, -40}, 20, false);
    REQUIRE(preview.DescribeView(800, 600, after, error) == true);
    CHECK(after.WorldToClip == before.WorldToClip);
    CHECK(after.CameraPosition == before.CameraPosition);
    CHECK(scene.Cameras[0].Sample.Pose.Position.Z == 6);
    preview.Look(0.25F, 0);
    REQUIRE(preview.DescribeView(800, 600, after, error) == true);
    CHECK(after.CameraPosition == before.CameraPosition);
    CHECK(after.CameraForward != before.CameraForward);
}
TEST_CASE("Selected object framing ignores scene-wide radius and fits portrait and wide viewports",
          "[editor][navigation]")
{
    constexpr usize ASPECT_COUNT = 3;
    constexpr StaticArray<float32, ASPECT_COUNT> aspects{0.25F, 1, 3};
    constexpr uint32 CORNER_COUNT = 8;
    for (const float32 aspect : aspects)
    {
        EditorCameraController controller;
        const math::Quaternion orientation = controller.GetSample().Pose.Orientation;
        REQUIRE(controller.TryFrame({{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}}, aspect) == true);
        CHECK(controller.GetSample().Pose.Orientation == orientation);
        const camera::CameraSample sample = controller.GetSample();
        math::TransformTRS pose;
        pose.Translation = {static_cast<float32>(sample.Pose.Position.X),
                            static_cast<float32>(sample.Pose.Position.Y),
                            static_cast<float32>(sample.Pose.Position.Z)};
        pose.Rotation = sample.Pose.Orientation;
        math::Affine3 worldToView;
        math::Matrix4 viewToClip;
        REQUIRE(math::TryInverse(math::ToAffine3(pose), worldToView) == math::MathStatus::Success);
        REQUIRE(
            math::TryPerspectiveReverseZInfinite(sample.Lens.VerticalFov, aspect, sample.Lens.NearPlane, viewToClip) ==
            math::MathStatus::Success);
        for (uint32 corner = 0; corner < CORNER_COUNT; ++corner)
        {
            const math::Vector4 positionWorld{(corner & 1U) == 0 ? -0.5F : 0.5F,
                                              (corner & 2U) == 0 ? -0.5F : 0.5F,
                                              (corner & 4U) == 0 ? -0.5F : 0.5F,
                                              1};
            math::Vector3 positionNdc;
            REQUIRE(math::TryPerspectiveDivide(viewToClip * math::ToMatrix4(worldToView) * positionWorld,
                                               0,
                                               positionNdc) == math::MathStatus::Success);
            CHECK(math::Abs(positionNdc.X) < 1);
            CHECK(math::Abs(positionNdc.Y) < 1);
        }
    }
    RenderPreview preview;
    REQUIRE(preview.SetScene(MakeNavigationScene()) == true);
    preview.Resize(200, 800);
    preview.SetSelection(QStringLiteral("selected-cube"));
    REQUIRE(preview.FrameSelection() == true);
    CHECK(preview.GetFocusDistance() < 10);
    for (uint32 step = 0; step < 80; ++step)
    {
        preview.Zoom(1);
    }
    CHECK(preview.GetFocusDistance() < 0.001F);
    const QJsonObject accepted = preview.Navigation();
    preview.SetSelection(QStringLiteral("deleted-object"));
    CHECK(preview.FrameSelection() == false);
    CHECK(preview.Navigation() == accepted);
}
TEST_CASE("Orbit retains the pivot and pan scales with drawable height", "[editor][navigation]")
{
    EditorCameraController first;
    EditorCameraController second;
    const math::Vector3d pivotWorld = first.GetPivotWorld();
    REQUIRE(first.TryRotate(60, 10, true) == true);
    CHECK(math::Distance(first.GetPivotWorld(), pivotWorld) < 0.00001);
    REQUIRE(second.TryAdopt(first.GetSample(), first.GetDistance()) == true);
    REQUIRE(first.TryPan(20, 30, 400) == true);
    REQUIRE(second.TryPan(40, 60, 800) == true);
    CHECK(first.GetSample().Pose.Position == second.GetSample().Pose.Position);
}
TEST_CASE("Fly displacement is stable across presentation rates and persists as an owning C0 sample",
          "[editor][navigation]")
{
    constexpr usize RATE_COUNT = 4;
    constexpr StaticArray<uint32, RATE_COUNT> rates{30, 60, 120, 240};
    for (const uint32 rate : rates)
    {
        EditorCameraController controller;
        camera::CameraSample initial = controller.GetSample();
        initial.Pose.Position = {};
        initial.Pose.Orientation = {};
        REQUIRE(controller.TryAdopt(initial, 1) == true);
        for (uint32 frame = 0; frame < rate; ++frame)
        {
            REQUIRE(controller.TryFly({0, 0, -1}, 1 / static_cast<float32>(rate)) == true);
        }
        CHECK(controller.GetSample().Pose.Position.Z == Catch::Approx(-1).margin(0.00001));
    }
    RenderPreview first;
    REQUIRE(first.SetScene(MakeNavigationScene()) == true);
    first.Look(13.25F, -4.5F);
    first.Fly({1, 0, -1}, 0.05F, 3);
    RenderPreview restored;
    restored.RestoreNavigation(first.Navigation());
    REQUIRE(restored.SetScene(MakeNavigationScene()) == true);
    CHECK(restored.Navigation() == first.Navigation());
    const QJsonObject legacy{{QStringLiteral("yaw"), 45},
                             {QStringLiteral("pitch"), 30},
                             {QStringLiteral("zoom"), 0.002},
                             {QStringLiteral("radius"), 1000},
                             {QStringLiteral("x"), 1},
                             {QStringLiteral("y"), 2},
                             {QStringLiteral("z"), 3}};
    restored.RestoreNavigation(legacy);
    CHECK(restored.GetFocusDistance() == Catch::Approx(2));
    CHECK(restored.Navigation().value(QStringLiteral("x")).toDouble() == Catch::Approx(1));
    CHECK(restored.Navigation().value(QStringLiteral("y")).toDouble() == Catch::Approx(2));
    CHECK(restored.Navigation().value(QStringLiteral("z")).toDouble() == Catch::Approx(3));
}
TEST_CASE("Viewport input frames selection, consumes fractional scrolling and stops fly on focus loss",
          "[editor][navigation]")
{
    RenderPreview preview;
    REQUIRE(preview.SetScene(MakeNavigationScene()) == true);
    PreviewViewport viewport(&preview);
    viewport.resize(800, 600);
    viewport.show();
    viewport.activateWindow();
    viewport.setFocus();
    QApplication::processEvents();
    preview.SetSelection(QStringLiteral("selected-cube"));
    QTest::keyClick(&viewport, Qt::Key_F);
    CHECK(preview.GetFocusDistance() < 10);
    preview.Navigate(0, {25, 20}, 3, false);
    const QJsonObject before = preview.Navigation();
    QWheelEvent
        emptyWheel(QPointF(100, 100), QPointF(100, 100), {}, {}, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(&viewport, &emptyWheel);
    CHECK(preview.Navigation() == before);
    QWheelEvent trackpad(QPointF(100, 100),
                         QPointF(100, 100),
                         QPoint(0, 2),
                         {},
                         Qt::NoButton,
                         Qt::NoModifier,
                         Qt::ScrollUpdate,
                         false);
    QApplication::sendEvent(&viewport, &trackpad);
    CHECK(preview.Navigation().value(QStringLiteral("camera")).toString().isEmpty() == true);
    const QJsonObject beforeFly = preview.Navigation();
    QTest::mousePress(&viewport, Qt::RightButton);
    QTest::keyPress(&viewport, Qt::Key_W);
    REQUIRE(QTest::qWaitFor(
                [&preview, &beforeFly]() {
                    return preview.Navigation().value(QStringLiteral("eyeZ")) !=
                           beforeFly.value(QStringLiteral("eyeZ"));
                },
                2000) == true);
    const QJsonObject moving = preview.Navigation();
    CHECK(moving.value(QStringLiteral("eyeZ")) != beforeFly.value(QStringLiteral("eyeZ")));
    QFocusEvent lostFocus(QEvent::FocusOut);
    QApplication::sendEvent(&viewport, &lostFocus);
    QTest::qWait(40);
    CHECK(preview.Navigation() == moving);
    QTest::mouseRelease(&viewport, Qt::RightButton);
    QTest::mousePress(&viewport, Qt::RightButton);
    QTest::qWait(40);
    CHECK(preview.Navigation() == moving);
    QTest::mouseRelease(&viewport, Qt::RightButton);
}

TEST_CASE("Orthographic pan uses the active width constraint and invalid restore preserves the camera",
          "[editor][navigation]")
{
    EditorCameraController controller;
    camera::CameraSample sample = controller.GetSample();
    sample.Lens.Projection = camera::CameraProjection::OrthographicFinite;
    sample.Lens.VerticalSpan = 2;
    sample.Lens.MinimumVisibleWidth = 8;
    sample.Lens.FarPlane = 100;
    REQUIRE(controller.TryAdopt(sample, 3) == true);
    const math::Vector3d before = controller.GetSample().Pose.Position;
    REQUIRE(controller.TryPan(0, 100, 400, 0.5F) == true);
    CHECK(math::Length(controller.GetSample().Pose.Position - before) == Catch::Approx(4));
    REQUIRE(controller.TryDolly(1) == true);
    CHECK(controller.GetSample().Lens.VerticalSpan == Catch::Approx(1.7F));
    const camera::CameraSample accepted = controller.GetSample();
    CHECK(controller.TrySetDistance(-1) == false);
    CHECK(controller.GetSample().Pose.Position == accepted.Pose.Position);
    RenderPreview preview;
    REQUIRE(preview.SetScene(MakeNavigationScene()) == true);
    const QJsonObject navigation = preview.Navigation();
    QJsonObject invalid = navigation;
    invalid.insert(QStringLiteral("projection"), 256);
    invalid.insert(QStringLiteral("radius"), 123);
    preview.RestoreNavigation(invalid);
    CHECK(preview.Navigation() == navigation);
    invalid = navigation;
    invalid.insert(QStringLiteral("distance"), -1);
    invalid.insert(QStringLiteral("radius"), 123);
    invalid.insert(QStringLiteral("camera"), QStringLiteral("authored-camera"));
    preview.RestoreNavigation(invalid);
    CHECK(preview.Navigation() == navigation);
}

TEST_CASE("Orthographic viewport projection agrees with its minimum visible width", "[editor][navigation]")
{
    RenderScene scene = MakeNavigationScene();
    scene.Cameras[0].Sample.Lens.Projection = camera::CameraProjection::OrthographicFinite;
    scene.Cameras[0].Sample.Lens.VerticalSpan = 2;
    scene.Cameras[0].Sample.Lens.MinimumVisibleWidth = 8;
    scene.Cameras[0].Sample.Lens.FarPlane = 100;
    const PreviewViewOptions options{ .Camera = 0, .Width = 300, .Height = 600 };
    rr::ViewDescription constrained;
    rr::ViewDescription equivalent;
    QString error;
    REQUIRE(scene.View(options, constrained, error) == true);
    scene.Cameras[0].Sample.Lens.VerticalSpan = 16;
    scene.Cameras[0].Sample.Lens.MinimumVisibleWidth = 0;
    REQUIRE(scene.View(options, equivalent, error) == true);
    CHECK(constrained.WorldToClip == equivalent.WorldToClip);
}

TEST_CASE("Lit viewport gives the render surface its available height without baking",
          "[editor][viewport][layout][gpu]")
{
    if (PreviewViewport::CanPresent() == false)
    {
        SKIP("A native display and GPU presentation path are required");
    }
    QTemporaryDir directory;
    RenderScene scene;
    tests::MakeLitScene(QFileInfo(directory.path()).canonicalFilePath(), scene, { .HasCollapsedTriangle = true });
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(800, 600);
    viewport.show();
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetShading(PreviewShading::Lit);
    REQUIRE(preview.Start() == true);
    REQUIRE(QTest::qWaitFor([&viewport]() { return viewport.PresentedFrames() > 0; }, 120000) == true);
    CHECK(viewport.findChild<QWidget*>(QStringLiteral("viewportBakeAction")) == nullptr);
    CHECK(viewport.findChild<QWidget*>(QStringLiteral("viewportBakeNotice")) == nullptr);
    CHECK(preview.Status().contains(QStringLiteral("direct lighting")) == true);
    CHECK(preview.Status().contains(QStringLiteral("bake")) == false);
    QWidget* surfaceOrNull = viewport.findChild<QWidget*>(QStringLiteral("viewportRenderSurface"));
    if (surfaceOrNull == nullptr)
    {
        surfaceOrNull = viewport.findChild<QWidget*>(QStringLiteral("LudusOffscreenViewport"));
    }
    REQUIRE(surfaceOrNull != nullptr);
    for (const QSize& size : {QSize(800, 600), QSize(500, 360), QSize(1000, 700)})
    {
        const uint64 presented = viewport.PresentedFrames();
        viewport.resize(size);
        preview.Orbit(1, 1);
        REQUIRE(QTest::qWaitFor([&viewport, presented]() { return viewport.PresentedFrames() > presented; }, 30000) ==
                true);
        CHECK(surfaceOrNull->height() > viewport.height() / 2);
        CHECK(surfaceOrNull->width() == viewport.width());
        CHECK(viewport.rect().contains(surfaceOrNull->geometry()) == true);
        CHECK(preview.Running() == true);
    }
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_VIEWPORT_LAYOUT_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(viewport.grab().save(capture) == true);
    }
    preview.Stop();
}

TEST_CASE("Continuous camera input does not postpone viewport frames",
          "[.render-native][editor][viewport][navigation-cadence][gpu]")
{
    if (PreviewViewport::CanPresent() == false)
    {
        SKIP("A native display and GPU presentation path are required");
    }
    QTemporaryDir directory;
    RenderScene scene;
    tests::MakeLitScene(QFileInfo(directory.path()).canonicalFilePath(), scene);
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(800, 600);
    viewport.show();
#if defined(Q_OS_MACOS) && defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    tests::ActivateViewportForPacing(viewport);
    REQUIRE(QTest::qWaitFor([]() { return QGuiApplication::applicationState() == Qt::ApplicationActive; }) == true);
#endif
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetStatisticsVisible(true);
    REQUIRE(preview.Start() == true);
    REQUIRE(QTest::qWaitFor([&viewport]() { return viewport.PresentedFrames() > 0; }, 120000) == true);
    constexpr int32 SAMPLE_MILLISECONDS = 1000;
    const auto sampleFrames = []() {
        QEventLoop loop;
        QTimer::singleShot(SAMPLE_MILLISECONDS, &loop, &QEventLoop::quit);
        loop.exec();
    };
    const uint64 beforeIdle = viewport.PresentedFrames();
    sampleFrames();
    const uint64 idleFrames = viewport.PresentedFrames() - beforeIdle;
    const uint64 beforeInput = viewport.PresentedFrames();
    uint32 inputCount = 0;
    QTimer input;
    input.setTimerType(Qt::PreciseTimer);
    QObject::connect(&input, &QTimer::timeout, &viewport, [&preview, &inputCount]() {
        ++inputCount;
        preview.Look(0, inputCount % 2 == 0 ? 1 : -1);
    });
    input.start(2);
    sampleFrames();
    input.stop();
    const uint64 inputFrames = viewport.PresentedFrames() - beforeInput;
    CAPTURE(idleFrames, inputFrames, inputCount, viewport.GetStatisticsText().toStdString());
    CHECK(inputCount > 20);
    CHECK(inputFrames > 2);
#if defined(Q_OS_MACOS)
    QScreen* screenOrNull = viewport.screen();
    REQUIRE(screenOrNull != nullptr);
    constexpr float64 MINIMUM_REFRESH_FRACTION = 0.75;
    constexpr float64 MILLISECONDS_PER_SECOND = 1000;
    const float64 refreshRate = screenOrNull->refreshRate();
    const uint64 minimumFrames =
        static_cast<uint64>(refreshRate * SAMPLE_MILLISECONDS / MILLISECONDS_PER_SECOND * MINIMUM_REFRESH_FRACTION);
    CAPTURE(refreshRate, minimumFrames);
    CHECK(idleFrames >= minimumFrames);
    CHECK(inputFrames >= minimumFrames);
#endif
    preview.Stop();
}

TEST_CASE("An empty viewport draws a navigable coordinate grid without a renderer", "[editor][navigation][grid]")
{
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(800, 600);
    viewport.show();
    QApplication::processEvents();
    QLabel* gridOrNull = viewport.findChild<QLabel*>(QStringLiteral("emptyWorldView"));
    REQUIRE(gridOrNull != nullptr);
    REQUIRE(gridOrNull->isVisible() == true);
    const QImage initial = gridOrNull->grab().toImage();
    REQUIRE(initial.isNull() == false);
    const QString capture = qEnvironmentVariable("LUDUS_EDITOR_EMPTY_VIEW_CAPTURE");
    if (capture.isEmpty() == false)
    {
        REQUIRE(initial.save(capture) == true);
    }
    // Inspect the drawable center, excluding explanatory text at the edges.
    const auto countLines = [](const QImage& image) {
        usize count = 0;
        for (int32 y = image.height() / 4; y < image.height() * 3 / 4; ++y)
        {
            for (int32 x = image.width() / 4; x < image.width() * 3 / 4; ++x)
            {
                if (image.pixelColor(x, y) != QColor(9, 12, 15))
                {
                    ++count;
                }
            }
        }
        return count;
    };
    CHECK(countLines(initial) > 100);
    preview.Orbit(60, 20);
    CHECK(gridOrNull->grab().toImage() != initial);
    const QImage orbited = gridOrNull->grab().toImage();
    preview.Pan(30, 20, static_cast<float32>(gridOrNull->height()));
    CHECK(gridOrNull->grab().toImage() != orbited);
    const QImage panned = gridOrNull->grab().toImage();
    preview.Zoom(2);
    CHECK(gridOrNull->grab().toImage() != panned);
    viewport.resize(400, 800);
    QApplication::processEvents();
    CHECK(countLines(gridOrNull->grab().toImage()) > 100);
    CHECK(preview.Scene().Identity.isEmpty() == true);
    CHECK(preview.Scene().Objects.isEmpty() == true);
    CHECK(preview.Running() == false);
    CHECK(preview.HasDevice() == false);
    viewport.ClearFrame(QStringLiteral("No visible scene instances."));
    CHECK(gridOrNull->isVisible() == true);
    CHECK(countLines(gridOrNull->grab().toImage()) > 100);
    REQUIRE(preview.SetScene(MakeNavigationScene()) == true);
    viewport.ClearFrame(QStringLiteral("Preparing scene."));
    CHECK(countLines(gridOrNull->grab().toImage()) == 0);
    REQUIRE(preview.SetScene({}) == true);
    viewport.ClearFrame(QStringLiteral("No visible scene instances."));
    CHECK(countLines(gridOrNull->grab().toImage()) > 100);
}

TEST_CASE("Empty orthographic views draw the facing Cartesian plane", "[editor][navigation][grid]")
{
    constexpr usize DIRECTION_COUNT = 3;
    const StaticArray<math::Vector3, DIRECTION_COUNT> eyes{{{0, 0, 5}, {5, 0, 0}, {0, 5, 0}}};
    const StaticArray<math::Vector3, DIRECTION_COUNT> axes{{{0, 1, 0}, {0, 1, 0}, {1, 0, 0}}};
    constexpr StaticArray<float32, DIRECTION_COUNT> angles{0, math::kHalfPiF, -math::kHalfPiF};
    for (usize direction = 0; direction < DIRECTION_COUNT; ++direction)
    {
        RenderPreview preview;
        QJsonObject navigation = preview.Navigation();
        math::Quaternion rotation;
        REQUIRE(math::TryFromAxisAngle(axes[direction], angles[direction], rotation) == math::MathStatus::Success);
        navigation.insert(QStringLiteral("projection"),
                          static_cast<int32>(camera::CameraProjection::OrthographicFinite));
        navigation.insert(QStringLiteral("span"), 10);
        navigation.insert(QStringLiteral("near"), 0.01);
        navigation.insert(QStringLiteral("far"), 100);
        navigation.insert(QStringLiteral("eyeX"), static_cast<float64>(eyes[direction].X));
        navigation.insert(QStringLiteral("eyeY"), static_cast<float64>(eyes[direction].Y));
        navigation.insert(QStringLiteral("eyeZ"), static_cast<float64>(eyes[direction].Z));
        navigation.insert(QStringLiteral("rotationX"), static_cast<float64>(rotation.X));
        navigation.insert(QStringLiteral("rotationY"), static_cast<float64>(rotation.Y));
        navigation.insert(QStringLiteral("rotationZ"), static_cast<float64>(rotation.Z));
        navigation.insert(QStringLiteral("rotationW"), static_cast<float64>(rotation.W));
        preview.RestoreNavigation(navigation);
        rr::ViewDescription view;
        QString error;
        REQUIRE(preview.DescribeView(400, 400, view, error) == true);
        REQUIRE(view.OrthographicCamera == true);
        PreviewViewport viewport(&preview);
        viewport.resize(600, 600);
        viewport.show();
        QApplication::processEvents();
        QLabel* gridOrNull = viewport.findChild<QLabel*>(QStringLiteral("emptyWorldView"));
        REQUIRE(gridOrNull != nullptr);
        const QImage image = gridOrNull->grab().toImage();
        const QString capture = qEnvironmentVariable("LUDUS_EDITOR_EMPTY_VIEW_CAPTURE");
        if (capture.isEmpty() == false)
        {
            REQUIRE(image.save(capture + QString::number(direction) + QStringLiteral(".png")) == true);
        }
        // Each facing plane must have both horizontal and vertical lines away from its axes.
        usize horizontalChanges = 0;
        usize verticalChanges = 0;
        for (int32 i = 1; i < image.width() - 1; ++i)
        {
            horizontalChanges += image.pixel(i, image.height() / 3) != image.pixel(i - 1, image.height() / 3) ? 1 : 0;
        }
        for (int32 i = image.height() / 4; i < image.height() * 3 / 4; ++i)
        {
            verticalChanges += image.pixel(image.width() / 3, i) != image.pixel(image.width() / 3, i - 1) ? 1 : 0;
        }
        CHECK(horizontalChanges > 10);
        CHECK(verticalChanges > 10);
        CHECK(preview.HasDevice() == false);
    }
}

TEST_CASE("Loaded editor cameras enable the world grid without adding scene objects", "[editor][viewport][grid]")
{
    RenderPreview preview;
    REQUIRE(preview.SetScene(MakeNavigationScene()) == true);
    rr::ViewDescription view;
    QString error;
    REQUIRE(preview.DescribeView(400, 300, view, error) == true);
    CHECK(view.WorldGrid.IsVisible == true);
    CHECK(view.WorldGrid.Width == 400);
    CHECK(view.WorldGrid.Height == 300);
    CHECK(view.WorldGrid.FocusDistance == preview.GetFocusDistance());
    REQUIRE(preview.DescribeConventionalView(640, 480, view, error) == true);
    CHECK(view.WorldGrid.IsVisible == true);
    CHECK(view.WorldGrid.Width == 640);
    CHECK(view.WorldGrid.Height == 480);
    CHECK(view.Depth == rr::DepthConvention::Conventional);
    CHECK(preview.Scene().Objects.size() == 1);
    CHECK(preview.Scene().Meshes.size() == 1);
    CHECK(preview.HasDevice() == false);
}

TEST_CASE("Vertical look pitches about camera right after yaw without rolling", "[editor][navigation]")
{
    constexpr usize YAW_COUNT = 4;
    constexpr StaticArray<float32, YAW_COUNT> yawDegrees{0, 45, 90, -135};
    for (const float32 yaw : yawDegrees)
    {
        EditorCameraController controller;
        REQUIRE(controller.TrySetOrbit({}, {yaw, 20}, 3) == true);
        const camera::CameraSample before = controller.GetSample();
        const math::Vector3 rightWorld = math::Rotate(before.Pose.Orientation, {1, 0, 0});
        const math::Vector3 forwardWorld = math::Rotate(before.Pose.Orientation, {0, 0, -1});
        REQUIRE(controller.TryRotate(0, 40, false) == true);
        const camera::CameraSample after = controller.GetSample();
        CHECK(after.Pose.Position == before.Pose.Position);
        CHECK(math::Distance(math::Rotate(after.Pose.Orientation, {1, 0, 0}), rightWorld) < 0.00001F);
        CHECK(math::Distance(math::Rotate(after.Pose.Orientation, {0, 0, -1}), forwardWorld) > 0.1F);
        CHECK(controller.GetPitchDegrees() == Catch::Approx(30).margin(0.0001));
        const math::Vector3 upWorld = math::Rotate(after.Pose.Orientation, {0, 1, 0});
        CHECK(math::Dot(rightWorld, math::Vector3{0, 1, 0}) == Catch::Approx(0).margin(0.00001));
        CHECK(upWorld.Y > 0);
    }
}

TEST_CASE("Viewport sensitivity scales look and orbit and persists without moving the camera", "[editor][navigation]")
{
    for (const bool isOrbit : {false, true})
    {
        RenderPreview first;
        RenderPreview second;
        REQUIRE(first.SetScene(MakeNavigationScene()) == true);
        REQUIRE(second.SetScene(MakeNavigationScene()) == true);
        const QJsonObject before = first.Navigation();
        first.SetLookSensitivity(0.5F);
        CHECK(first.Navigation().value(QStringLiteral("eyeX")) == before.value(QStringLiteral("eyeX")));
        CHECK(first.Navigation().value(QStringLiteral("rotationX")) == before.value(QStringLiteral("rotationX")));
        if (isOrbit == true)
        {
            first.Orbit(40, 20);
            second.Orbit(20, 10);
        }
        else
        {
            first.Look(40, 20);
            second.Look(20, 10);
        }
        QJsonObject scaled = first.Navigation();
        scaled.remove(QStringLiteral("lookSensitivity"));
        QJsonObject normal = second.Navigation();
        normal.remove(QStringLiteral("lookSensitivity"));
        CHECK(scaled == normal);
        RenderPreview restored;
        restored.RestoreNavigation(first.Navigation());
        CHECK(restored.GetLookSensitivity() == 0.5F);
        restored.SetLookSensitivity(0);
        restored.SetLookSensitivity(RenderPreview::MAXIMUM_LOOK_SENSITIVITY + 1);
        CHECK(restored.GetLookSensitivity() == 0.5F);
        PreviewViewport viewport(&restored, nullptr, false);
        QDoubleSpinBox* sensitivityOrNull =
            viewport.findChild<QDoubleSpinBox*>(QStringLiteral("viewportLookSensitivity"));
        REQUIRE(sensitivityOrNull != nullptr);
        CHECK(sensitivityOrNull->value() == 0.5);
        sensitivityOrNull->setValue(2);
        CHECK(restored.GetLookSensitivity() == 2);
        restored.SetLookSensitivity(1);
        CHECK(sensitivityOrNull->value() == 1);
    }
}

// Thanks to The Qt Company, "QCursor Class", setPos documentation:
// https://doc.qt.io/qt-6/qcursor.html#setPos
// Keep this fixture hidden: injected events must not mix with native cursor warps,
// whose OS button state can differ. This checks Qt input mapping, not physical Cocoa input.
TEST_CASE("Right drag vertical input preserves the rendered horizontal camera axis", "[editor][navigation]")
{
    RenderPreview preview;
    REQUIRE(preview.SetScene(MakeNavigationScene()) == true);
    PreviewViewport viewport(&preview, nullptr, false);
    viewport.resize(800, 600);
    rr::ViewDescription before;
    rr::ViewDescription after;
    QString error;
    REQUIRE(preview.DescribeView(800, 600, before, error) == true);
    const QJsonObject initial = preview.Navigation();
    const math::Quaternion orientation{static_cast<float32>(initial.value(QStringLiteral("rotationX")).toDouble()),
                                       static_cast<float32>(initial.value(QStringLiteral("rotationY")).toDouble()),
                                       static_cast<float32>(initial.value(QStringLiteral("rotationZ")).toDouble()),
                                       static_cast<float32>(initial.value(QStringLiteral("rotationW")).toDouble())};
    const math::Vector3 rightWorld = math::Rotate(orientation, {1, 0, 0});
    const QPointF startLocal(200, 200);
    const QPointF startGlobal = viewport.mapToGlobal(startLocal.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress,
                      startLocal,
                      startGlobal,
                      Qt::RightButton,
                      Qt::RightButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&viewport, &press);
    const QPointF movementLocal(0, 40);
    QMouseEvent move(QEvent::MouseMove,
                     startLocal + movementLocal,
                     startGlobal + movementLocal,
                     Qt::NoButton,
                     Qt::RightButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&viewport, &move);
    QMouseEvent release(QEvent::MouseButtonRelease,
                        startLocal + movementLocal,
                        startGlobal + movementLocal,
                        Qt::RightButton,
                        Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&viewport, &release);
    REQUIRE(preview.DescribeView(800, 600, after, error) == true);
    CHECK(after.CameraPosition == before.CameraPosition);
    CHECK(math::Distance(after.CameraForward, before.CameraForward) > 0.1F);
    const math::Vector4 rightClip = after.WorldToClip * math::Vector4{rightWorld.X, rightWorld.Y, rightWorld.Z, 0};
    CHECK(rightClip.X > 0);
    CHECK(rightClip.Y == Catch::Approx(0).margin(0.00001));
    CHECK(rightClip.W == Catch::Approx(0).margin(0.00001));
}
