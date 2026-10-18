#include "internal/controller.h"
#include "internal/main_window.h"
#include "internal/preview_area_lights.h"
#include "internal/preview_shading.h"
#include "internal/preview_viewport.h"
#include "internal/qt_numeric_stream.h"
#include "internal/render_panel.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/static_lighting_job.h"
#include "internal/workspace_style.h"
#include "internal/world_workspace.h"

#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/platform/entropy.h>

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSlider>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace rr = ludus::graphics::renderer;
namespace
{
QByteArray Read(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
void Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(bytes) == bytes.size());
}
struct Property final
{
    QString Component;
    QString Field;
};
struct Fixture final
{
    QTemporaryDir Directory;
    QString Root;
    WorldDocument World;
    RenderScene Scene;
    QString Error;
    Fixture()
    {
        REQUIRE(Directory.isValid());
        Root = QFileInfo(Directory.path()).canonicalFilePath();
        REQUIRE(QDir(Root).mkpath(QStringLiteral("authoring")));
        for (const auto& name : {QStringLiteral("world.schema.json"),
                                 QStringLiteral("rendering.world.json"),
                                 QStringLiteral("render.preview.json"),
                                 QStringLiteral("cube.mesh.json"),
                                 QStringLiteral("ludus.png")})
        {
            Write(Path(name), Read(QStringLiteral(":/samples/live-edit-game/authoring/") + name));
        }
        REQUIRE(World.LoadSchema(Read(Path(QStringLiteral("world.schema.json"))), Error));
        REQUIRE(World.Load(Read(Path(QStringLiteral("rendering.world.json"))), Error));
        const auto loaded = RenderScene::Load(Root, World, Scene, Error);
        INFO(Error.toStdString());
        REQUIRE(loaded);
    }
    [[nodiscard]] QString Path(const QString& file) const
    {
        return Root + QStringLiteral("/authoring/") + file;
    }
    void Descriptor(const QJsonObject& descriptor)
    {
        Write(Path(QStringLiteral("render.preview.json")), QJsonDocument(descriptor).toJson());
    }
    [[nodiscard]] QJsonObject Descriptor() const
    {
        return QJsonDocument::fromJson(Read(Path(QStringLiteral("render.preview.json")))).object();
    }
    bool Edit(const QString& id, const Property& property, const QJsonValue& value)
    {
        const auto entity = World.Entity(id);
        auto components = entity.value(QStringLiteral("components")).toObject();
        auto properties = components.value(property.Component).toObject();
        properties.insert(property.Field, value);
        components.insert(property.Component, properties);
        return World.Edit(id, components, entity.value(QStringLiteral("name")).toString(), Error);
    }
};
} // namespace
TEST_CASE("A creator opens a textured 3D World through its project bindings without writing files",
          "[editor][render-authoring]")
{
    Fixture f;
    CHECK(f.Scene.Objects.size() == 1);
    CHECK(f.Scene.Meshes.size() == 1);
    CHECK(f.Scene.Meshes[0].Positions.size() == 24);
    CHECK(f.Scene.Meshes[0].Indices.size() == 36);
    CHECK(f.Scene.Textures.size() == 1);
    CHECK(f.Scene.Textures[0].Pixels.format() == QImage::Format_RGBA8888);
    CHECK(f.Scene.Cameras.size() == 1);
    CHECK_FALSE(f.World.Dirty());
    const auto before = Read(f.Path(QStringLiteral("rendering.world.json")));
    rr::ViewDescription view;
    REQUIRE(f.Scene.View(
        { .Camera = 0, .OrbitDegrees = {0, 0}, .Distance = 3, .Width = 800, .Height = 600, .Reference = false },
        view,
        f.Error));
    math::Vector3 center;
    REQUIRE(math::TryPerspectiveDivide(view.WorldToClip * math::Vector4{0, 0, 0, 1}, .0001F, center) ==
            math::MathStatus::Success);
    CHECK(center.X == Catch::Approx(0));
    CHECK(center.Y == Catch::Approx(0));
    CHECK(center.Z > 0);
    CHECK(center.Z < 1);
    CHECK(view.Depth == rr::DepthConvention::ReverseZ);
    CHECK(Read(f.Path(QStringLiteral("rendering.world.json"))) == before);
}
TEST_CASE("Invalid preview bindings and duplicate JSON preserve the last accepted scene", "[editor][render-authoring]")
{
    Fixture f;
    const auto identity = f.Scene.Identity;
    auto descriptor = f.Descriptor();
    auto transform = descriptor.value(QStringLiteral("transform")).toObject();
    auto properties = transform.value(QStringLiteral("properties")).toObject();
    properties.insert(QStringLiteral("x"), QStringLiteral("missing"));
    transform.insert(QStringLiteral("properties"), properties);
    descriptor.insert(QStringLiteral("transform"), transform);
    f.Descriptor(descriptor);
    CHECK_FALSE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Error.contains(QStringLiteral("transform.x")));
    CHECK(f.Scene.Identity == identity);
    Write(f.Path(QStringLiteral("render.preview.json")), QByteArrayLiteral("{\"version\":1,\"version\":1}"));
    CHECK_FALSE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Scene.Identity == identity);
    CHECK_FALSE(f.World.Dirty());
}
TEST_CASE("Mesh import validation rejects corrupt indices and missing UVs atomically", "[editor][render-authoring]")
{
    Fixture f;
    const auto identity = f.Scene.Identity;
    const auto source = Read(f.Path(QStringLiteral("cube.mesh.json")));
    for (const QJsonValue& index : {QJsonValue(-1), QJsonValue(1.5), QJsonValue(24)})
    {
        auto mesh = QJsonDocument::fromJson(source).object();
        auto indices = mesh.value(QStringLiteral("indices")).toArray();
        indices[0] = index;
        mesh.insert(QStringLiteral("indices"), indices);
        Write(f.Path(QStringLiteral("cube.mesh.json")), QJsonDocument(mesh).toJson());
        CHECK_FALSE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
        CHECK(f.Scene.Identity == identity);
    }
    auto mesh = QJsonDocument::fromJson(source).object();
    mesh.insert(QStringLiteral("uvs"), QJsonArray{});
    Write(f.Path(QStringLiteral("cube.mesh.json")), QJsonDocument(mesh).toJson());
    CHECK_FALSE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Scene.Identity == identity);
}
TEST_CASE("Preview files reject traversal and symlinks and do not escape the project", "[editor][render-authoring]")
{
    Fixture f;
    auto descriptor = f.Descriptor();
    auto meshes = descriptor.value(QStringLiteral("meshes")).toArray();
    auto mesh = meshes[0].toObject();
    mesh.insert(QStringLiteral("path"), QStringLiteral("authoring/../authoring/cube.mesh.json"));
    meshes[0] = mesh;
    descriptor.insert(QStringLiteral("meshes"), meshes);
    f.Descriptor(descriptor);
    const auto identity = f.Scene.Identity;
    CHECK_FALSE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Scene.Identity == identity);
    REQUIRE(QFile::link(f.Path(QStringLiteral("cube.mesh.json")), f.Path(QStringLiteral("linked.json"))));
    mesh.insert(QStringLiteral("path"), QStringLiteral("authoring/linked.json"));
    meshes[0] = mesh;
    descriptor.insert(QStringLiteral("meshes"), meshes);
    f.Descriptor(descriptor);
    CHECK_FALSE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Scene.Identity == identity);
}
TEST_CASE("Camera lens and rigid-pose validation explain failures without replacing preview data",
          "[editor][render-authoring]")
{
    Fixture f;
    const auto identity = f.Scene.Identity;
    REQUIRE(f.Edit(QStringLiteral("preview.camera"), {QStringLiteral("camera"), QStringLiteral("near")}, 200));
    CHECK_FALSE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Error.contains(QStringLiteral("camera"), Qt::CaseInsensitive));
    CHECK(f.Scene.Identity == identity);
    REQUIRE(f.World.Undo());
    REQUIRE(f.Edit(QStringLiteral("preview.camera"), {QStringLiteral("transform"), QStringLiteral("sx")}, 2));
    CHECK_FALSE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Error.contains(QStringLiteral("unit scale")));
    CHECK(f.Scene.Identity == identity);
}
TEST_CASE("An orthographic follow camera resolves the same authored target and publishes a C0 trace",
          "[editor][render-authoring]")
{
    Fixture f;
    REQUIRE(f.Edit(QStringLiteral("starter.cube"), {QStringLiteral("transform"), QStringLiteral("x")}, 4));
    REQUIRE(f.Edit(QStringLiteral("preview.camera"),
                   {QStringLiteral("camera"), QStringLiteral("projection")},
                   QStringLiteral("orthographic")));
    REQUIRE(f.Edit(QStringLiteral("preview.camera"),
                   {QStringLiteral("camera"), QStringLiteral("follow")},
                   QStringLiteral("starter.cube")));
    REQUIRE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Scene.Cameras[0].Sample.Pose.Position.X == 4);
    CHECK(f.Scene.Cameras[0].Sample.Pose.Position.Z == 6);
    CHECK(f.Scene.Cameras[0].Trace.Recipe == ludus::gameplay::camera::CameraRecipe::OrthographicFollow);
    rr::ViewDescription view;
    REQUIRE(f.Scene.View(
        { .Camera = 0, .OrbitDegrees = {0, 0}, .Distance = 3, .Width = 800, .Height = 600, .Reference = false },
        view,
        f.Error));
    CHECK(view.OrthographicCamera);
    REQUIRE(f.Edit(QStringLiteral("preview.camera"), {QStringLiteral("transform"), QStringLiteral("ry")}, 20));
    CHECK_FALSE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
}
TEST_CASE("Editor orbit and reference comparison do not mutate authored cameras or World history",
          "[editor][render-authoring]")
{
    Fixture f;
    const auto draft = f.World.Draft();
    rr::ViewDescription view;
    REQUIRE(f.Scene.View(
        {
            .Camera = -1,
            .OrbitDegrees = {45, 30},
            .Distance = f.Scene.Radius * 3,
            .Width = 640,
            .Height = 480,
            .Reference = true,
        },
        view,
        f.Error));
    CHECK(view.Reference);
    CHECK(view.InfiniteFar);
    const auto matrix = view.WorldToClip;
    CHECK_FALSE(f.Scene.View(
        {
            .Camera = -1,
            .OrbitDegrees = {45, 90},
            .Distance = f.Scene.Radius * 3,
            .Width = 640,
            .Height = 480,
            .Reference = false,
        },
        view,
        f.Error));
    CHECK(view.WorldToClip == matrix);
    CHECK_FALSE(f.Scene.View(
        {
            .Camera = -1,
            .OrbitDegrees = {45, 30},
            .Distance = f.Scene.Radius * 3,
            .Width = 0,
            .Height = 480,
            .Reference = false,
        },
        view,
        f.Error));
    CHECK(view.WorldToClip == matrix);
    CHECK_FALSE(f.World.CanUndo());
    CHECK(f.World.Draft() == draft);
}
TEST_CASE("Preview UI validates assets, selects authored cameras and clears state on document replacement",
          "[editor][render-authoring]")
{
    Fixture f;
    RenderPanel panel;
    panel.SetWorld(f.Root, &f.World);
    QSignalSpy loaded(&panel, &RenderPanel::Loaded);
    REQUIRE(panel.Refresh(false));
    REQUIRE(loaded.wait(5000));
    REQUIRE(loaded.last()[0].toBool());
    auto* cameras = panel.findChild<QComboBox*>(QStringLiteral("worldPreviewCamera"));
    REQUIRE(cameras != nullptr);
    REQUIRE(cameras->count() == 2);
    cameras->setCurrentIndex(1);
    CHECK_FALSE(panel.findChild<QSlider*>(QStringLiteral("worldPreviewYaw"))->isEnabled());
    CHECK(panel.findChild<QLabel*>(QStringLiteral("worldPreviewTrace"))->text().contains(QStringLiteral("C0 fixed")));
    const QJsonObject draft = f.World.Draft();
    panel.Preview().Orbit(0.5F, 0);
    CHECK(cameras->currentIndex() == 0);
    CHECK(panel.findChild<QSlider*>(QStringLiteral("worldPreviewYaw"))->isEnabled() == true);
    CHECK(f.World.Draft() == draft);
    panel.findChild<QPushButton*>(QStringLiteral("worldPreviewFrameAll"))->click();
    CHECK(cameras->currentIndex() == 0);
    CHECK(f.World.Draft() == draft);
    const auto identity = panel.Preview().Scene().Identity;
    Write(f.Path(QStringLiteral("ludus.png")), QByteArrayLiteral("invalid PNG"));
    REQUIRE(panel.Refresh(false));
    REQUIRE(loaded.wait(5000));
    CHECK_FALSE(loaded.last()[0].toBool());
    CHECK(panel.Preview().Scene().Identity == identity);
    CHECK_FALSE(f.World.Dirty());
    panel.SetWorld({}, nullptr);
    CHECK(panel.Preview().Scene().Identity.isEmpty());
    CHECK(cameras->count() == 1);
    CHECK_FALSE(panel.findChild<QPushButton*>(QStringLiteral("worldPreview3D"))->isEnabled());
}
TEST_CASE("Late preview loads are discarded and repeated edits coalesce to the latest applied World",
          "[editor][render-authoring]")
{
    Fixture f;
    RenderPanel panel;
    panel.SetWorld(f.Root, &f.World);
    QSignalSpy loaded(&panel, &RenderPanel::Loaded);
    REQUIRE(panel.Refresh(false));
    panel.SetWorld({}, nullptr);
    QTest::qWait(100);
    CHECK(loaded.isEmpty());
    CHECK(panel.Preview().Scene().Identity.isEmpty());
    panel.SetWorld(f.Root, &f.World);
    panel.Stop();
    REQUIRE(panel.Refresh(false));
    REQUIRE(f.Edit(QStringLiteral("starter.cube"), {QStringLiteral("transform"), QStringLiteral("x")}, 3));
    panel.WorldChanged();
    REQUIRE(f.Edit(QStringLiteral("starter.cube"), {QStringLiteral("transform"), QStringLiteral("x")}, 6));
    panel.WorldChanged();
    REQUIRE(loaded.wait(5000));
    REQUIRE(loaded.last()[0].toBool());
    REQUIRE(panel.Preview().Scene().Objects.size() == 1);
    CHECK(panel.Preview().Scene().Objects[0].Transform.Translation.X == 6);
    CHECK_FALSE(panel.Preview().Running());
}
TEST_CASE("Preview extraction keeps the GUI event loop responsive while assets decode", "[editor][render-authoring]")
{
    Fixture f;
    RenderPanel panel;
    panel.SetWorld(f.Root, &f.World);
    QSignalSpy loaded(&panel, &RenderPanel::Loaded);
    bool heartbeat = false;
    QTimer::singleShot(0, &panel, [&heartbeat]() { heartbeat = true; });
    REQUIRE(panel.Refresh(false));
    CHECK_FALSE(heartbeat);
    REQUIRE(loaded.wait(5000));
    CHECK(heartbeat);
    CHECK(loaded.last()[0].toBool());
    CHECK_FALSE(f.World.Dirty());
}
TEST_CASE("Closing a pending preview prevents its worker from opening a native surface later",
          "[editor][render-authoring]")
{
    Fixture f;
    RenderPanel panel;
    panel.SetWorld(f.Root, &f.World);
    QSignalSpy loaded(&panel, &RenderPanel::Loaded);
    REQUIRE(panel.Refresh(true));
    CHECK(panel.findChild<QPushButton*>(QStringLiteral("worldPreviewStop"))->isEnabled());
    panel.findChild<QPushButton*>(QStringLiteral("worldPreviewStop"))->click();
    QTest::qWait(100);
    CHECK(loaded.isEmpty());
    CHECK_FALSE(panel.Preview().Running());
    CHECK(panel.Preview().Scene().Identity.isEmpty());
    CHECK_FALSE(panel.findChild<QPushButton*>(QStringLiteral("worldPreviewStop"))->isEnabled());
}
TEST_CASE("Renderer controls and inspector remain reachable in a small styled World window",
          "[editor][render-authoring]")
{
    Fixture f;
    WorldWorkspace world;
    world.SetProject(f.Root, 1);
    REQUIRE(world.Open(f.Path(QStringLiteral("rendering.world.json"))));
    ApplyWorkspaceStyle(&world);
    world.findChild<QListWidget*>(QStringLiteral("worldEntities"))->setCurrentRow(0);
    world.resize(800, 640);
    world.show();
    world.findChild<QPushButton*>(QStringLiteral("worldPreviewDetails"))->click();
    QTest::qWait(30);
    CHECK(world.width() <= 800);
    CHECK(world.height() <= 640);
    auto* scroll = world.findChild<QScrollArea*>(QStringLiteral("worldPreviewSettingsScroll"));
    REQUIRE(scroll != nullptr);
    CHECK(scroll->height() <= 120);
    CHECK(scroll->verticalScrollBar()->maximum() > 0);
    QDoubleSpinBox* distance = world.findChild<QDoubleSpinBox*>(QStringLiteral("worldPreviewZoom"));
    REQUIRE(distance != nullptr);
    scroll->ensureWidgetVisible(distance);
    QApplication::processEvents();
    QLineEdit* distanceEditor = distance->findChild<QLineEdit*>();
    REQUIRE(distanceEditor != nullptr);
    CHECK(distance->isVisible() == true);
    CHECK(distanceEditor->height() >= distanceEditor->minimumSizeHint().height());
    for (const auto& name : {QStringLiteral("worldPreview3D"),
                             QStringLiteral("worldPreviewReload"),
                             QStringLiteral("worldAdd"),
                             QStringLiteral("worldApply")})
    {
        auto* control = world.findChild<QPushButton*>(name);
        REQUIRE(control != nullptr);
        CHECK(control->isVisible());
        const auto top = control->mapTo(&world, QPoint(0, 0));
        CHECK(top.y() >= 0);
        CHECK(top.y() + control->height() <= world.height());
    }
    if (qEnvironmentVariable("LUDUS_RENDER_SCREENSHOT").isEmpty() == false)
    {
        QSignalSpy frame(&world.findChild<RenderPanel*>()->Preview(), &RenderPreview::FrameReady);
        REQUIRE(frame.wait(5000));
        REQUIRE(world.grab().save(qEnvironmentVariable("LUDUS_RENDER_SCREENSHOT")));
    }
}
TEST_CASE("Artists choose display tint into pending linear fields with atomic Apply and Undo",
          "[editor][render-authoring]")
{
    Fixture f;
    WorldWorkspace world;
    world.SetProject(f.Root, 1);
    REQUIRE(world.Open(f.Path(QStringLiteral("rendering.world.json"))));
    world.findChild<QListWidget*>(QStringLiteral("worldEntities"))->setCurrentRow(0);
    auto* panel = world.findChild<RenderPanel*>();
    REQUIRE(panel != nullptr);
    QSignalSpy loaded(panel, &RenderPanel::Loaded);
    REQUIRE(panel->Refresh(false));
    REQUIRE(loaded.wait(5000));
    const auto original = world.Document().Draft();
    auto* tint = world.findChild<QPushButton*>(QStringLiteral("worldPickTint"));
    REQUIRE(tint->isEnabled());
    tint->click();
    auto* picker = world.findChild<QColorDialog*>(QStringLiteral("worldTintPicker"));
    REQUIRE(picker != nullptr);
    picker->setCurrentColor(QColor(128, 128, 128));
    picker->accept();
    CHECK(world.Document().Draft() == original);
    CHECK(world.Dirty());
    CHECK_FALSE(world.CanUndo());
    CHECK_FALSE(tint->isEnabled());
    REQUIRE(world.Apply());
    const auto rgb = world.Document()
                         .Entity(QStringLiteral("starter.cube"))
                         .value(QStringLiteral("components"))
                         .toObject()
                         .value(QStringLiteral("renderable"))
                         .toObject();
    // Independent broad band distinguishes linear gray from the display 0.502 value.
    CHECK(rgb.value(QStringLiteral("r")).toDouble() > .21);
    CHECK(rgb.value(QStringLiteral("r")).toDouble() < .22);
    CHECK(rgb.value(QStringLiteral("g")) == rgb.value(QStringLiteral("r")));
    CHECK(rgb.value(QStringLiteral("b")) == rgb.value(QStringLiteral("r")));
    world.Undo();
    CHECK(world.Document().Draft() == original);
}
TEST_CASE("Orbit-only projects and tiny geometry do not need a camera component or unit-sized framing",
          "[editor][render-authoring]")
{
    Fixture f;
    auto descriptor = f.Descriptor();
    descriptor.insert(QStringLiteral("camera"), QJsonValue(QJsonValue::Null));
    f.Descriptor(descriptor);
    for (const auto& axis : {QStringLiteral("sx"), QStringLiteral("sy"), QStringLiteral("sz")})
    {
        REQUIRE(f.Edit(QStringLiteral("starter.cube"), {QStringLiteral("transform"), axis}, .001));
    }
    REQUIRE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Scene.Cameras.isEmpty());
    CHECK(f.Scene.Radius < .01F);
    rr::ViewDescription view;
    REQUIRE(f.Scene.View(
        {
            .Camera = -1,
            .OrbitDegrees = {25, 20},
            .Distance = f.Scene.Radius * 3,
            .Width = 800,
            .Height = 600,
            .Reference = false,
        },
        view,
        f.Error));
    CHECK(view.InfiniteFar);
}
TEST_CASE("Tint dialogs preserve data on unchanged color and reject callbacks after World replacement",
          "[editor][render-authoring]")
{
    Fixture f;
    WorldWorkspace world;
    world.SetProject(f.Root, 1);
    const auto path = f.Path(QStringLiteral("rendering.world.json"));
    REQUIRE(world.Open(path));
    auto* list = world.findChild<QListWidget*>(QStringLiteral("worldEntities"));
    list->setCurrentRow(0);
    auto* panel = world.findChild<RenderPanel*>();
    QSignalSpy loaded(panel, &RenderPanel::Loaded);
    REQUIRE(panel->Refresh(false));
    REQUIRE(loaded.wait(5000));
    auto* tint = world.findChild<QPushButton*>(QStringLiteral("worldPickTint"));
    REQUIRE(tint->isEnabled());
    tint->click();
    auto* picker = world.findChild<QColorDialog*>(QStringLiteral("worldTintPicker"));
    REQUIRE(picker != nullptr);
    picker->accept();
    CHECK_FALSE(world.Dirty());
    QTest::qWait(1);
    tint->click();
    picker = world.findChild<QColorDialog*>(QStringLiteral("worldTintPicker"));
    REQUIRE(picker != nullptr);
    REQUIRE(world.Open(path));
    picker->setCurrentColor(QColor(128, 128, 128));
    picker->accept();
    CHECK_FALSE(world.Dirty());
    CHECK(world.findChild<QLabel*>(QStringLiteral("worldStatus"))
              ->text()
              .contains(QStringLiteral("World or selection changed")));
}
TEST_CASE("Duplicate textured objects share surfaces while distinct material capacity failures preserve the scene",
          "[editor][render-authoring]")
{
    Fixture f;
    for (int32 i = 0; i < 11; ++i)
    {
        QString id;
        REQUIRE(f.World.DuplicateEntity(ludus::platform::GetSystemEntropySource(),
                                        f.Error,
                                        QStringLiteral("starter.cube"),
                                        id));
    }
    REQUIRE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Scene.Objects.size() == 12);
    CHECK(f.Scene.Surfaces.size() == 1);
    for (const auto& object : f.Scene.Objects)
    {
        CHECK(object.Surface == 0);
    }
    const auto identity = f.Scene.Identity;
    const auto objects = f.Scene.Objects;
    for (int32 i = 0; i < 9; ++i)
    {
        REQUIRE(
            f.Edit(objects[i].Id, {QStringLiteral("renderable"), QStringLiteral("r")}, static_cast<float64>(i) / 10));
    }
    CHECK_FALSE(RenderScene::Load(f.Root, f.World, f.Scene, f.Error));
    CHECK(f.Error.contains(QStringLiteral("eight distinct")));
    CHECK(f.Scene.Identity == identity);
}
TEST_CASE("Viewport shading is visible by default and restores independently of authored World data",
          "[editor][render-authoring]")
{
    Fixture fixture;
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    REQUIRE(preview.SetScene(fixture.Scene));
    QComboBox* shading = viewport.findChild<QComboBox*>(QStringLiteral("viewportShading"));
    REQUIRE(shading != nullptr);
    CHECK(preview.GetShading() == PreviewShading::Solid);
    CHECK(shading->currentIndex() == 0);
    const QJsonObject draft = fixture.World.Draft();
    preview.Orbit(35, 10);
    QJsonObject navigation = preview.Navigation();
    shading->setCurrentIndex(1);
    CHECK(preview.GetShading() == PreviewShading::Albedo);
    CHECK(preview.Navigation().value(QStringLiteral("yaw")) == navigation.value(QStringLiteral("yaw")));
    navigation = preview.Navigation();
    shading->setCurrentIndex(2);
    CHECK(preview.GetShading() == PreviewShading::Lit);
    preview.RestoreNavigation(navigation);
    CHECK(preview.GetShading() == PreviewShading::Albedo);
    CHECK(shading->currentIndex() == 1);
    CHECK(preview.Scene().Identity == fixture.Scene.Identity);
    CHECK(fixture.World.Draft() == draft);
    navigation.remove(QStringLiteral("shading"));
    preview.RestoreNavigation(navigation);
    CHECK(preview.GetShading() == PreviewShading::Solid);
    CHECK(shading->currentIndex() == 0);
}
TEST_CASE("Renderer snapshot admission protects fixed handle arrays even for an invalid private caller",
          "[editor][render-authoring]")
{
    Fixture f;
    RenderPreview preview;
    REQUIRE(preview.SetScene(f.Scene));
    auto invalid = f.Scene;
    invalid.Surfaces.clear();
    CHECK_FALSE(preview.SetScene(invalid));
    CHECK(preview.Scene().Identity == f.Scene.Identity);
    invalid = f.Scene;
    for (int32 i = 0; i < 6; ++i)
    {
        invalid.Meshes.append(f.Scene.Meshes[0]);
    }
    CHECK_FALSE(preview.SetScene(invalid));
    CHECK(preview.Scene().Identity == f.Scene.Identity);
}

TEST_CASE("World composes schema meshes into cooked renderer data with bounded failure",
          "[editor][unified][render-authoring]")
{
    Fixture fixture;
    const QJsonObject level{
        {QStringLiteral("id"), QStringLiteral("a8517f65-8127-4bf4-99c2-e53b1a6d9700")},
        {QStringLiteral("objects"), QJsonArray{}},
        {QStringLiteral("camera"),
         QJsonObject{{QStringLiteral("eye"), QJsonArray{0, 0, 3}},
                     {QStringLiteral("target"), QJsonArray{0, 0, 0}},
                     {QStringLiteral("up"), QJsonArray{0, 1, 0}},
                     {QStringLiteral("fov"), 1.0}}},
    };
    RenderScene combined;
    REQUIRE(RenderScene::Merge(fixture.Scene, {}, level, combined, fixture.Error, {}));
    REQUIRE(combined.Definition != nullptr);
    CHECK(combined.Lit);
    CHECK(combined.Objects.size() == fixture.Scene.Objects.size());
    CHECK(combined.Objects[0].Id == fixture.Scene.Objects[0].Id);
    CHECK(combined.Objects[0].Transform == fixture.Scene.Objects[0].Transform);
    CHECK(combined.Definition->Meshes.GetSize() == 1);
    CHECK(combined.Definition->Meshes[0].Normals.GetSize() == combined.Definition->Meshes[0].Positions.GetSize());
    const auto retained = combined.Identity;
    auto excessive = fixture.Scene;
    for (int32 i = 0; i < 8; ++i)
    {
        excessive.Objects.append(fixture.Scene.Objects[0]);
    }
    CHECK_FALSE(RenderScene::Merge(excessive, {}, level, combined, fixture.Error, {}));
    CHECK(combined.Identity == retained);
    const WeakPtr<const ludus::resources::SceneDefinition> definition = combined.Definition;
    {
        RenderPreview preview;
        REQUIRE(preview.SetScene(combined));
        combined = {};
        CHECK(definition.IsExpired() == false);
        REQUIRE(preview.SetScene(fixture.Scene));
        CHECK(definition.IsExpired() == true); // Replacement retires the previous lease and definition.
        REQUIRE(RenderScene::Merge(fixture.Scene, {}, level, combined, fixture.Error, {}));
        const WeakPtr<const ludus::resources::SceneDefinition> lastDefinition = combined.Definition;
        REQUIRE(preview.SetScene(combined));
        combined = {};
        REQUIRE(preview.SetScene({}));
        CHECK(lastDefinition.IsExpired() == true);
    }
}

TEST_CASE("Embedded World preview follows valid inspector drafts without committing them", "[editor][render-authoring]")
{
    Fixture f;
    WorldWorkspace world;
    world.SetProject(f.Root, 1);
    REQUIRE(world.Open(f.Path(QStringLiteral("rendering.world.json"))));
    world.findChild<QListWidget*>(QStringLiteral("worldEntities"))->setCurrentRow(0);
    auto* panel = world.findChild<RenderPanel*>();
    QSignalSpy loaded(panel, &RenderPanel::Loaded);
    REQUIRE(loaded.wait(5000));
    QWidget* viewport = world.findChild<QWidget*>(QStringLiteral("worldViewport"));
    REQUIRE(viewport != nullptr);
    CHECK(viewport->parentWidget()->objectName() == QStringLiteral("worldSplitter"));
    CHECK_FALSE(viewport->isWindow());
    const auto applied = world.Document().Draft();
    auto* field = world.findChild<QLineEdit*>(QStringLiteral("worldField:transform/x"));
    REQUIRE(field != nullptr);
    field->selectAll();
    QTest::keyClicks(field, QStringLiteral("0.75"));
    REQUIRE(loaded.wait(5000));
    CHECK(panel->Preview().Scene().Objects[0].Transform.Translation.X == Catch::Approx(.75F));
    CHECK(world.Document().Draft() == applied);
    CHECK(world.Dirty());
    CHECK_FALSE(world.CanUndo());
    const auto identity = panel->Preview().Scene().Identity;
    field->selectAll();
    QTest::keyClicks(field, QStringLiteral("-"));
    QTest::qWait(300);
    CHECK(field->text() == QStringLiteral("-"));
    CHECK(panel->Preview().Scene().Identity == identity);
    CHECK(world.Document().Draft() == applied);
    world.findChild<QPushButton*>(QStringLiteral("worldReset"))->click();
    REQUIRE(loaded.wait(5000));
    CHECK(panel->Preview().Scene().Identity == f.Scene.Identity);
    CHECK_FALSE(world.Dirty());
}

TEST_CASE("World session restores orbit position and rejects stale or external document paths", "[editor][layout]")
{
    Fixture f;
    WorldWorkspace first;
    first.SetProject(f.Root, 1);
    REQUIRE(first.Open(f.Path(QStringLiteral("rendering.world.json"))));
    QSignalSpy loaded(first.findChild<RenderPanel*>(), &RenderPanel::Loaded);
    REQUIRE(loaded.wait(5000));
    auto& preview = first.findChild<RenderPanel*>()->Preview();
    preview.Orbit(80, -15);
    preview.Pan(30, 10);
    preview.Zoom(2);
    const auto session = first.Session();
    first.StopPreview();
    WorldWorkspace reopened;
    reopened.SetProject(f.Root, 2);
    reopened.RestoreSession(session);
    QSignalSpy restored(reopened.findChild<RenderPanel*>(), &RenderPanel::Loaded);
    REQUIRE(restored.wait(5000));
    CHECK(reopened.findChild<RenderPanel*>()->Preview().Navigation() ==
          session.value(QStringLiteral("boundCamera")).toObject());
    CHECK(reopened.Document().Draft() == first.Document().Draft());
    CHECK_FALSE(reopened.Dirty());
    reopened.SetProject(f.Root, 3);
    auto stale = session;
    stale.insert(QStringLiteral("world"), QStringLiteral("authoring/deleted.world.json"));
    reopened.RestoreSession(stale);
    CHECK_FALSE(reopened.Document().Loaded());
    stale.insert(QStringLiteral("world"), f.Path(QStringLiteral("rendering.world.json")));
    reopened.RestoreSession(stale);
    CHECK_FALSE(reopened.Document().Loaded());
    stale.insert(QStringLiteral("world"), QStringLiteral("../../outside.world.json"));
    reopened.RestoreSession(stale);
    CHECK_FALSE(reopened.Document().Loaded());
}

TEST_CASE("Reopening projects restores their own embedded viewport and dock layouts", "[editor][layout]")
{
    Fixture a;
    Fixture b;
    QTemporaryDir settings;
    const auto preferences = settings.filePath(QStringLiteral("workspace.json"));
    const auto descriptor = [](Fixture& fixture) {
        const auto path = fixture.Path(QStringLiteral("ludus.project.json"));
        Write(
            path,
            QByteArrayLiteral(
                R"json({"version":1,"name":"viewport","provider":"cmake","source_dir":"..","preset":"linux-clang-debug","target":"app","run":{"cwd":".","args":[]}})json"));
        return path;
    };
    const auto projectA = descriptor(a), projectB = descriptor(b);
    const auto close = [](MainWindow& window) {
        (void)window.close();
        for (int32 attempt = 0; window.isVisible() && attempt < 100; ++attempt)
        {
            QTest::qWait(20);
        }
        REQUIRE_FALSE(window.isVisible());
    };
    QJsonObject navigation;
    {
        EditorController controller(ToolingPaths{}, nullptr, settings.filePath(QStringLiteral("recent.json")));
        MainWindow window(&controller, nullptr, preferences);
        window.show();
        controller.OpenProject(projectA);
        REQUIRE(controller.State().Document == DocumentState::ProjectLoaded);
        auto* world = window.findChild<WorldWorkspace*>();
        REQUIRE(world->Open(a.Path(QStringLiteral("rendering.world.json"))));
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("workspaceTabs"));
        tabs->setCurrentWidget(world);
        world->findChild<RenderPanel*>()->Preview().Orbit(65, 12);
        world->findChild<RenderPanel*>()->Preview().Pan(17, 8);
        navigation = world->Session().value(QStringLiteral("boundCamera")).toObject();
        window.addDockWidget(Qt::LeftDockWidgetArea, window.findChild<QDockWidget*>(QStringLiteral("outputDock")));
        window.findChild<QDockWidget*>(QStringLiteral("liveInspectorDock"))->hide();
        close(window);
    }
    {
        EditorController controller(ToolingPaths{}, nullptr, settings.filePath(QStringLiteral("recent.json")));
        MainWindow window(&controller, nullptr, preferences);
        window.show();
        controller.OpenProject(projectB);
        auto* world = window.findChild<WorldWorkspace*>();
        CHECK_FALSE(world->Document().Loaded());
        REQUIRE(world->Open(b.Path(QStringLiteral("rendering.world.json"))));
        window.addDockWidget(Qt::RightDockWidgetArea, window.findChild<QDockWidget*>(QStringLiteral("outputDock")));
        window.findChild<QDockWidget*>(QStringLiteral("liveInspectorDock"))->show();
        close(window);
    }
    {
        EditorController controller(ToolingPaths{}, nullptr, settings.filePath(QStringLiteral("recent.json")));
        MainWindow window(&controller, nullptr, preferences);
        window.show();
        controller.OpenProject(projectA);
        auto* world = window.findChild<WorldWorkspace*>();
        CHECK(world->Document().Loaded());
        CHECK_FALSE(world->Dirty());
        CHECK(world->Session().value(QStringLiteral("boundCamera")).toObject() == navigation);
        CHECK(window.findChild<QTabWidget*>(QStringLiteral("workspaceTabs"))->currentWidget() == world);
        CHECK(window.dockWidgetArea(window.findChild<QDockWidget*>(QStringLiteral("outputDock"))) ==
              Qt::LeftDockWidgetArea);
        CHECK(window.findChild<QDockWidget*>(QStringLiteral("liveInspectorDock"))->isHidden());
        close(window);
    }
}

TEST_CASE("World light bindings preserve authored photometric terms and reject invalid replacements",
          "[editor][render-authoring][lighting]")
{
    Fixture fixture;
    QJsonObject schema = fixture.World.Schema();
    QJsonArray components = schema.value(QStringLiteral("components")).toArray();
    for (int32 i = static_cast<int32>(components.size()) - 1; i >= 0; --i)
    {
        if (components[i].toObject().value(QStringLiteral("id")).toString() == QStringLiteral("light"))
        {
            components.removeAt(i);
        }
    }
    QJsonArray properties;
    const QStringList numberNames{QStringLiteral("r"),
                                  QStringLiteral("g"),
                                  QStringLiteral("b"),
                                  QStringLiteral("intensity"),
                                  QStringLiteral("range"),
                                  QStringLiteral("innerAngle"),
                                  QStringLiteral("outerAngle")};
    for (const QString& name : numberNames)
    {
        properties.append(QJsonObject{{QStringLiteral("id"), name},
                                      {QStringLiteral("label"), name},
                                      {QStringLiteral("type"), QStringLiteral("number")},
                                      {QStringLiteral("default"), 1}});
    }
    properties.append(
        QJsonObject{{QStringLiteral("id"), QStringLiteral("kind")},
                    {QStringLiteral("label"), QStringLiteral("Kind")},
                    {QStringLiteral("type"), QStringLiteral("enum")},
                    {QStringLiteral("choices"),
                     QJsonArray{QStringLiteral("point"), QStringLiteral("spot"), QStringLiteral("directional")}},
                    {QStringLiteral("default"), QStringLiteral("point")}});
    properties.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("enabled")},
                                  {QStringLiteral("label"), QStringLiteral("Enabled")},
                                  {QStringLiteral("type"), QStringLiteral("boolean")},
                                  {QStringLiteral("default"), true}});
    for (int32 i = 0; i < properties.size(); ++i)
    {
        QJsonObject property = properties[i].toObject();
        property.insert(QStringLiteral("help"), QStringLiteral("Authored light field"));
        property.insert(QStringLiteral("unit"), QString());
        if (property.value(QStringLiteral("type")) == QJsonValue(QStringLiteral("number")))
        {
            property.insert(QStringLiteral("min"), 0);
            property.insert(QStringLiteral("max"), 1000000);
        }
        properties[i] = property;
    }
    components.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("light")},
                                  {QStringLiteral("label"), QStringLiteral("Light")},
                                  {QStringLiteral("help"), QStringLiteral("Authored photometric lighting")},
                                  {QStringLiteral("required"), false},
                                  {QStringLiteral("properties"), properties}});
    schema.insert(QStringLiteral("components"), components);
    const QByteArray originalWorld = fixture.World.Encode();
    fixture.World.Clear();
    const bool hasSchema = fixture.World.LoadSchema(QJsonDocument(schema).toJson(), fixture.Error);
    INFO(fixture.Error.toStdString());
    REQUIRE(hasSchema == true);
    REQUIRE(fixture.World.Load(originalWorld, fixture.Error));
    QJsonObject descriptor = fixture.Descriptor();
    QJsonObject mapping;
    for (const QJsonValue property : properties)
    {
        const QString id = property.toObject().value(QStringLiteral("id")).toString();
        mapping.insert(id, id);
    }
    descriptor.insert(
        QStringLiteral("light"),
        QJsonObject{{QStringLiteral("component"), QStringLiteral("light")}, {QStringLiteral("properties"), mapping}});
    fixture.Descriptor(descriptor);
    QJsonObject entity = fixture.World.Entity(QStringLiteral("starter.cube"));
    QJsonObject attached = entity.value(QStringLiteral("components")).toObject();
    QJsonObject transform = attached.value(QStringLiteral("transform")).toObject();
    transform.insert(QStringLiteral("rx"), 0);
    transform.insert(QStringLiteral("ry"), 0);
    transform.insert(QStringLiteral("rz"), 0);
    attached.insert(QStringLiteral("transform"), transform);
    attached.insert(QStringLiteral("light"),
                    QJsonObject{{QStringLiteral("kind"), QStringLiteral("spot")},
                                {QStringLiteral("enabled"), true},
                                {QStringLiteral("r"), 1},
                                {QStringLiteral("g"), .5},
                                {QStringLiteral("b"), .25},
                                {QStringLiteral("intensity"), 200},
                                {QStringLiteral("range"), 50},
                                {QStringLiteral("innerAngle"), 20},
                                {QStringLiteral("outerAngle"), 30}});
    REQUIRE(fixture.World.Edit(QStringLiteral("starter.cube"), attached, QStringLiteral("Spot lamp"), fixture.Error));
    REQUIRE(RenderScene::Load(fixture.Root, fixture.World, fixture.Scene, fixture.Error));
    REQUIRE(fixture.Scene.AuthoredLights.Count == 1);
    const rr::DirectLight light = fixture.Scene.AuthoredLights.Lights[0];
    CHECK(light.Kind == rr::LightKind::Spot);
    CHECK(light.SourceId != 0);
    CHECK(light.Intensity == 200);
    CHECK(light.Range == 50);
    CHECK(light.Color == math::Vector3{1, .5F, .25F});
    CHECK(light.Direction == math::Vector3{0, 0, -1});
    CHECK(light.InnerCos > light.OuterCos);
    const QByteArray identity = fixture.Scene.Identity;
    REQUIRE(fixture.Edit(QStringLiteral("starter.cube"), {QStringLiteral("light"), QStringLiteral("intensity")}, 300));
    REQUIRE(RenderScene::Load(fixture.Root, fixture.World, fixture.Scene, fixture.Error));
    CHECK(fixture.Scene.Identity != identity);
    CHECK(fixture.Scene.AuthoredLights.Lights[0].SourceId == light.SourceId);
    CHECK(fixture.Scene.AuthoredLights.Lights[0].Intensity == 300);
    const QByteArray accepted = fixture.Scene.Identity;
    REQUIRE(fixture.Edit(QStringLiteral("starter.cube"), {QStringLiteral("light"), QStringLiteral("r")}, 2));
    CHECK(RenderScene::Load(fixture.Root, fixture.World, fixture.Scene, fixture.Error) == false);
    CHECK(fixture.Scene.Identity == accepted);
    REQUIRE(fixture.Edit(QStringLiteral("starter.cube"), {QStringLiteral("light"), QStringLiteral("enabled")}, false));
    REQUIRE(RenderScene::Load(fixture.Root, fixture.World, fixture.Scene, fixture.Error));
    CHECK(fixture.Scene.AuthoredLights.Count == 0);
    CHECK(fixture.Scene.Lit == true);
    REQUIRE(fixture.Scene.Definition != nullptr);
    CHECK(fixture.Scene.Definition->Lights.IsEmpty() == true);
}

TEST_CASE("Deindexed emitter meshes split into connected planar area proxies without losing energy",
          "[editor][render-authoring][lighting]")
{
    ludus::resources::SceneMesh mesh;
    const math::Vector3 positions[] = {
        {0, 0, 0},
        {1, 0, 0},
        {1, 1, 0},
        {0, 0, 0},
        {1, 1, 0},
        {0, 1, 0},
        {0, 0, 2},
        {1, 0, 2},
        {1, 1, 2},
        {0, 0, 2},
        {1, 1, 2},
        {0, 1, 2},
        {0, 0, 0},
        {0, 1, 0},
        {0, 1, 1},
        {0, 0, 0},
        {0, 1, 1},
        {0, 0, 1},
    };
    REQUIRE(mesh.Positions.TryAddRange(positions));
    for (uint32 i = 0; i < mesh.Positions.GetSize(); ++i)
    {
        REQUIRE(mesh.Indices.TryAdd(i));
    }
    rr::LightSelection lights;
    uint32 added = 99;
    REQUIRE(AppendPreviewAreaLights(mesh, {}, {4, 2, 1}, false, 42, 100, lights, added) ==
            ludus::graphics::rhi::RasterStatus::Ready);
    REQUIRE(added == 3);
    REQUIRE(lights.Count == 3);
    float32 energy = 0;
    for (uint32 i = 0; i < lights.Count; ++i)
    {
        CHECK(lights.Lights[i].Kind == rr::LightKind::AreaOneSided);
        energy += lights.Lights[i].Intensity;
    }
    CHECK(energy == Catch::Approx(12.0F));
    rr::LightSelection repeat;
    REQUIRE(AppendPreviewAreaLights(mesh, {}, {4, 2, 1}, false, 42, 100, repeat, added) ==
            ludus::graphics::rhi::RasterStatus::Ready);
    for (uint32 i = 0; i < lights.Count; ++i)
    {
        CHECK(repeat.Lights[i].SourceId == lights.Lights[i].SourceId);
    }
    const rr::LightSelection original = lights;
    added = 77;
    CHECK(AppendPreviewAreaLights(mesh, {}, {4, 2, 1}, false, 42, 100, lights, added) ==
          ludus::graphics::rhi::RasterStatus::InvalidDescription);
    CHECK(added == 77);
    CHECK(lights.Count == original.Count);
    CHECK(lights.Lights[0].SourceId == original.Lights[0].SourceId);
    // A duplicated face creates a nonmanifold shared edge. It must preserve the
    // previous list instead of silently overcounting its source power.
    const uint32 duplicatedFace[] = {mesh.Indices[0], mesh.Indices[1], mesh.Indices[2]};
    REQUIRE(mesh.Indices.TryAddRange(duplicatedFace));
    CHECK(AppendPreviewAreaLights(mesh, {}, {4, 2, 1}, false, 90, 100, lights, added) !=
          ludus::graphics::rhi::RasterStatus::Ready);
    CHECK(lights.Count == original.Count);
}

TEST_CASE("Zero-area emitter triangles add no area proxy and do not reject the emitter",
          "[editor][render-authoring][lighting]")
{
    // Imported PBRT emitters (staircase2's lamp) carry collapsed triangles beside
    // their real faces. They emit nothing and must not disable Lit shading.
    ludus::resources::SceneMesh mesh;
    const math::Vector3 positions[] = {
        {0, 0, 0},
        {1, 0, 0},
        {1, 1, 0},
        {0, 0, 0},
        {1, 1, 0},
        {0, 1, 0},
        // Collapsed onto the shared edge, and onto one point.
        {0, 0, 0},
        {1, 1, 0},
        {.5F, .5F, 0},
        {1, 0, 0},
        {1, 0, 0},
        {1, 0, 0},
    };
    REQUIRE(mesh.Positions.TryAddRange(positions));
    for (uint32 i = 0; i < mesh.Positions.GetSize(); ++i)
    {
        REQUIRE(mesh.Indices.TryAdd(i));
    }
    rr::LightSelection lights;
    uint32 added = 99;
    REQUIRE(AppendPreviewAreaLights(mesh, {}, {4, 2, 1}, false, 42, 100, lights, added) ==
            ludus::graphics::rhi::RasterStatus::Ready);
    REQUIRE(added == 1);
    REQUIRE(lights.Count == 1);
    CHECK(lights.Lights[0].Kind == rr::LightKind::AreaOneSided);
    CHECK(lights.Lights[0].Intensity == Catch::Approx(4.0F));
    // An emitter whose every triangle is collapsed contributes no proxy.
    ludus::resources::SceneMesh collapsed;
    const math::Vector3 point[] = {{2, 2, 2}, {2, 2, 2}, {2, 2, 2}};
    REQUIRE(collapsed.Positions.TryAddRange(point));
    for (uint32 i = 0; i < collapsed.Positions.GetSize(); ++i)
    {
        REQUIRE(collapsed.Indices.TryAdd(i));
    }
    added = 99;
    REQUIRE(AppendPreviewAreaLights(collapsed, {}, {4, 2, 1}, false, 43, 100, lights, added) ==
            ludus::graphics::rhi::RasterStatus::Ready);
    CHECK(added == 0);
    CHECK(lights.Count == 1);
    // Geometry whose area overflows binary32 is still rejected and preserves both outputs.
    collapsed.Positions[0] = {3.0e38F, 0, 0};
    collapsed.Positions[1] = {0, 3.0e38F, 0};
    added = 77;
    CHECK(AppendPreviewAreaLights(collapsed, {}, {4, 2, 1}, false, 43, 100, lights, added) ==
          ludus::graphics::rhi::RasterStatus::InvalidDescription);
    CHECK(added == 77);
    CHECK(lights.Count == 1);
}

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
TEST_CASE("A stopped GPU preview shows its failure in place of the viewport placeholder",
          "[editor][render-authoring][viewport]")
{
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.ClearFrame(QStringLiteral("Import a reference scene to render it here."));
    const QString failure =
        QStringLiteral("Authored lighting admission failed (6); no lights were dropped. World edits are preserved.");
    Q_EMIT preview.Failed(failure);
    bool isFailureShown = false;
    bool isPlaceholderShown = false;
    for (const QLabel* label : viewport.findChildren<QLabel*>())
    {
        isFailureShown = isFailureShown || label->text() == failure;
        isPlaceholderShown = isPlaceholderShown || label->text().startsWith(QStringLiteral("Import a reference"));
    }
    CHECK(isFailureShown == true);
    CHECK(isPlaceholderShown == false);
}
#endif

namespace
{
// Lights the starter cube so RenderScene retains the decoded scene a bake needs.
RenderScene LoadLitStarterScene(Fixture& inoutFixture)
{
    Fixture& fixture = inoutFixture;
    QJsonObject entity = fixture.World.Entity(QStringLiteral("starter.cube"));
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    components.insert(QStringLiteral("light"),
                      QJsonObject{{QStringLiteral("kind"), QStringLiteral("point")},
                                  {QStringLiteral("enabled"), true},
                                  {QStringLiteral("r"), 1},
                                  {QStringLiteral("g"), 1},
                                  {QStringLiteral("b"), 1},
                                  {QStringLiteral("intensity"), 100},
                                  {QStringLiteral("range"), 10},
                                  {QStringLiteral("innerAngle"), 25},
                                  {QStringLiteral("outerAngle"), 30}});
    REQUIRE(fixture.World.Edit(QStringLiteral("starter.cube"), components, QStringLiteral("Lit cube"), fixture.Error));
    RenderScene scene;
    REQUIRE(RenderScene::Load(fixture.Root, fixture.World, scene, fixture.Error));
    REQUIRE(scene.Lit == true);
    REQUIRE(scene.Definition != nullptr);
    return scene;
}
} // namespace

TEST_CASE("Headless preview prepares authored lighting from the extracted World",
          "[editor][preview-generation][lighting][gpu]")
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW) && defined(Q_OS_LINUX)
    constexpr int32 PREPARATION_TIMEOUT_MILLISECONDS = 30000;
    Fixture fixture;
    const RenderScene scene = LoadLitStarterScene(fixture);
    RenderPreview preview;
    QSignalSpy failures(&preview, &RenderPreview::Failed);
    bool isAdmitted = false;
    QObject::connect(&preview, &RenderPreview::DeviceRequested, &preview, [&preview, &isAdmitted]() {
        isAdmitted = preview.AdmitOffscreenDevice();
    });
    preview.Resize(64, 64);
    REQUIRE(preview.SetScene(scene) == true);
    preview.SetShading(PreviewShading::Lit);
    const bool hasStarted = preview.Start();
    if (isAdmitted == false)
    {
        SKIP("No headless Vulkan device; install lavapipe to exercise authored-light preparation");
    }
    REQUIRE(hasStarted == true);
    const bool hasFrame =
        QTest::qWaitFor([&preview]() { return preview.HasPendingFrame(); }, PREPARATION_TIMEOUT_MILLISECONDS);
    INFO(preview.Status().toStdString());
    REQUIRE(hasFrame == true);
    CHECK(failures.count() == 0);
    CHECK(preview.Report().Visible > 0);
    CHECK(preview.Scene().Identity == scene.Identity);
    preview.Stop();
#else
    SKIP("Headless authored-light preview requires the native Linux preview");
#endif
}
TEST_CASE("Static lighting worker owns surface overrides until completed chart transfer",
          "[editor][render-authoring][lighting]")
{
    Fixture fixture;
    RenderScene scene = LoadLitStarterScene(fixture);
    Array<rr::StaticLightingSurfaceDescription> surfaces;
    REQUIRE(surfaces.TryResize(scene.Definition->Objects.GetSize()));
    rr::StaticLightingBakeDescription description;
    description.SamplesPerTriangle = 1;
    description.MaxDepth = 2;
    description.SceneRevision = 11;
    description.DependencyRevision = 22;
    description.Surfaces = {surfaces.GetData(), surfaces.GetSize()};
    StaticLightingJob worker(scene, description);
    // Invalidating caller storage after construction must not change the job.
    surfaces.Clear();
    surfaces = {};
    ludus::foundation::threading::JobSystem jobs;
    REQUIRE(jobs.Initialize({ .WorkerCount = STATIC_LIGHTING_WORKER_COUNT }) ==
            ludus::foundation::threading::JobStatus::Ok);
    REQUIRE(worker.Start(jobs) == ludus::graphics::rhi::RasterStatus::Pending);
    REQUIRE(worker.Start(jobs) == ludus::graphics::rhi::RasterStatus::InvalidState);
    REQUIRE(worker.Wait() == ludus::graphics::rhi::RasterStatus::Ready);
    REQUIRE(worker.Poll() == ludus::graphics::rhi::RasterStatus::Ready);
    REQUIRE(worker.GetProgressPercent() == 100);
    UniquePtr<rr::StaticLightingBake> bake = worker.TakeBake();
    REQUIRE(bake.Get() != nullptr);
    rr::StaticLightingChart chart;
    REQUIRE(bake->GetChart(0, chart) == ludus::graphics::rhi::RasterStatus::Ready);
    CHECK(chart.SourceId == scene.Definition->Objects[0].SourceId);
    CHECK(chart.SceneRevision == 11);
    CHECK(chart.DependencyRevision == 22);
    CHECK(chart.Transform == scene.Definition->Objects[0].Transform);
}

TEST_CASE("Static lighting worker interruption and destruction release the shared job system",
          "[editor][render-authoring][lighting]")
{
    Fixture fixture;
    RenderScene scene = LoadLitStarterScene(fixture);
    rr::StaticLightingBakeDescription description;
    description.SamplesPerTriangle = 65536;
    description.MaxDepth = 4;
    description.SceneRevision = 11;
    description.DependencyRevision = 22;
    ludus::foundation::threading::JobSystem jobs;
    REQUIRE(jobs.Initialize({ .WorkerCount = STATIC_LIGHTING_WORKER_COUNT }) ==
            ludus::foundation::threading::JobStatus::Ok);
    {
        StaticLightingJob interrupted(scene, description);
        REQUIRE(interrupted.Start(jobs) == ludus::graphics::rhi::RasterStatus::Pending);
        interrupted.RequestInterruption();
        REQUIRE(interrupted.Wait() == ludus::graphics::rhi::RasterStatus::NotReady);
        CHECK(interrupted.GetProgressPercent() < 100);
    }
    {
        // Destruction cancels and drains an uninterrupted in-flight bake.
        StaticLightingJob abandoned(scene, description);
        REQUIRE(abandoned.Start(jobs) == ludus::graphics::rhi::RasterStatus::Pending);
    }
    description.SamplesPerTriangle = 1;
    description.MaxDepth = 2;
    StaticLightingJob next(scene, description);
    REQUIRE(next.Start(jobs) == ludus::graphics::rhi::RasterStatus::Pending);
    REQUIRE(next.Wait() == ludus::graphics::rhi::RasterStatus::Ready);
}

TEST_CASE("Static lighting worker retains its scene definition across scene replacement",
          "[editor][render-authoring][lighting]")
{
    Fixture fixture;
    RenderScene scene = LoadLitStarterScene(fixture);
    const WeakPtr<const ludus::resources::SceneDefinition> definition = scene.Definition;
    const uint64 sourceId = scene.Definition->Objects[0].SourceId;
    rr::StaticLightingBakeDescription description;
    description.SamplesPerTriangle = 1;
    description.MaxDepth = 2;
    description.SceneRevision = 11;
    description.DependencyRevision = 22;
    ludus::foundation::threading::JobSystem jobs;
    REQUIRE(jobs.Initialize({ .WorkerCount = STATIC_LIGHTING_WORKER_COUNT }) ==
            ludus::foundation::threading::JobStatus::Ok);
    {
        StaticLightingJob worker(scene, description);
        REQUIRE(worker.Start(jobs) == ludus::graphics::rhi::RasterStatus::Pending);
        // Replace every other holder while the bake workers read the definition:
        // the workspace copy and the preview residency release theirs first.
        RenderPreview preview;
        REQUIRE(preview.SetScene(scene));
        scene = {};
        REQUIRE(preview.SetScene({}));
        CHECK(definition.IsExpired() == false);
        REQUIRE(worker.Wait() == ludus::graphics::rhi::RasterStatus::Ready);
        UniquePtr<rr::StaticLightingBake> bake = worker.TakeBake();
        REQUIRE(bake.Get() != nullptr);
        rr::StaticLightingChart chart;
        REQUIRE(bake->GetChart(0, chart) == ludus::graphics::rhi::RasterStatus::Ready);
        CHECK(chart.SourceId == sourceId);
        // The job is now the last holder; destroying it destroys the definition.
        CHECK(definition.IsExpired() == false);
    }
    CHECK(definition.IsExpired() == true);
    CHECK(definition.Lock() == nullptr);
}

TEST_CASE("Qt numeric stream preserves full-width light identity bytes across native integer ABIs",
          "[editor][render-authoring][lighting]")
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    WriteLittleEndianUint64(stream, uint64{0x0123456789abcdefULL});
    WriteLittleEndianUint64(stream, ~uint64{0});
    CHECK(stream.status() == QDataStream::Ok);
    CHECK(bytes.toHex() == QByteArrayLiteral("efcdab8967452301ffffffffffffffff"));
}
