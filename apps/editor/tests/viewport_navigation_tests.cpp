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
#include <QComboBox>
#include <QFileInfo>
#include <QFocusEvent>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::editor;
namespace math = ludus::foundation::math;
namespace camera = ludus::gameplay::camera;
namespace rr = ludus::graphics::renderer;
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
