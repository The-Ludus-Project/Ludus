#include "internal/builtin_world.h"
#include "internal/play_viewport_source.h"
#include "internal/preview_lighting.h"
#include "internal/preview_viewport.h"
#include "internal/render_preview.h"
#include "internal/static_lighting_job.h"
#include "internal/world_document.h"
#include "internal/world_surface_bake.h"
#include "observations.h"
#include "probe_qualification.h"
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/tone_mapping.hpp>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/platform/hosted_window.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::editor;
using namespace ludus::foundation;
using namespace lighting_room;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
constexpr int32 PREPARATION_MS = 30000;
constexpr int32 VIEW_WIDTH = 640;
constexpr int32 VIEW_HEIGHT = 480;
constexpr int32 SETTLE_FRAMES = 1;
constexpr float32 DARK_TOLERANCE = .006F;
constexpr float32 MIN_CONTROL_SIGNAL = .01F;

struct Fixture final
{
    QTemporaryDir Directory;
    QByteArray Schema;
    QJsonObject Original;
    QString Error;
    Fixture()
    {
        REQUIRE(Directory.isValid() == true);
        QFile file(QFINDTESTDATA("../../../examples/lighting-room/authoring/lighting.world.json"));
        REQUIRE(file.open(QIODevice::ReadOnly) == true);
        Original = QJsonDocument::fromJson(file.readAll()).object();
        REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), Schema, Error) == true);
    }

    RenderScene LoadScene(const Observation& observation)
    {
        QJsonObject edited = Original;
        QJsonArray entities = edited.value(QStringLiteral("entities")).toArray();
        math::Vector3 targetWorld{0, .5F, 0};
        math::Vector3 eyeWorld = OVERVIEW_EYE;
        for (qsizetype i = 0; i < entities.size(); ++i)
        {
            QJsonObject entity = entities[i].toObject();
            const QString label = entity.value(QStringLiteral("id")).toString();
            QJsonObject components = entity.value(QStringLiteral("components")).toObject();
            QJsonObject transform = components.value(QStringLiteral("transform")).toObject();
            QJsonObject surface = components.value(QStringLiteral("renderable")).toObject();
            QJsonObject light = components.value(QStringLiteral("light")).toObject();
            math::Vector3 positionWorld{static_cast<float32>(transform.value(QStringLiteral("x")).toDouble()),
                                        static_cast<float32>(transform.value(QStringLiteral("y")).toDouble()),
                                        static_cast<float32>(transform.value(QStringLiteral("z")).toDouble())};
            if (label == QStringLiteral("moving-receiver"))
            {
                positionWorld = RECEIVER_PATH[observation.PathIndex];
                transform.insert(QStringLiteral("x"), static_cast<float64>(positionWorld.X));
                transform.insert(QStringLiteral("y"), static_cast<float64>(positionWorld.Y));
                transform.insert(QStringLiteral("z"), static_cast<float64>(positionWorld.Z));
                if (observation.Target == Patch::Moving)
                {
                    targetWorld = positionWorld;
                    eyeWorld = targetWorld + BOUNCE_VIEW_OFFSET;
                }
            }
            if (label == QStringLiteral("rough-dielectric") && observation.Target == Patch::Rough)
            {
                targetWorld = positionWorld + ROUGH_PATCH_OFFSET;
                eyeWorld = targetWorld + (observation.IsOblique == true ? OBLIQUE_VIEW_OFFSET : PEAK_VIEW_OFFSET);
            }
            if (label == QStringLiteral("bounce-receiver") && observation.Target == Patch::Bounce)
            {
                targetWorld = positionWorld -
                              math::Vector3{static_cast<float32>(transform.value(QStringLiteral("sx")).toDouble()),
                                            static_cast<float32>(transform.value(QStringLiteral("sz")).toDouble()),
                                            0} *
                                  TRIANGLE_CENTROID_OFFSET;
                eyeWorld = targetWorld + BOUNCE_VIEW_OFFSET;
            }
            if (label == QStringLiteral("bounce-panel") && observation.HasNeutralPanel == true)
            {
                surface.insert(QStringLiteral("r"), static_cast<float64>(NEUTRAL_COLOR.X));
                surface.insert(QStringLiteral("g"), static_cast<float64>(NEUTRAL_COLOR.Y));
                surface.insert(QStringLiteral("b"), static_cast<float64>(NEUTRAL_COLOR.Z));
            }
            if (label == QStringLiteral("bounce-blocker"))
            {
                surface.insert(QStringLiteral("visible"), observation.HasBlocker);
            }
            if (label == QStringLiteral("switchable"))
            {
                light.insert(QStringLiteral("enabled"), observation.HasLiveLight);
            }
            if (label == QStringLiteral("always-on") && observation.HasSelectedLight == false)
            {
                light.insert(QStringLiteral("enabled"), observation.HasSelectedLight);
                light.insert(QStringLiteral("bakeAlwaysOn"), false);
            }
            components.insert(QStringLiteral("transform"), transform);
            if (surface.isEmpty() == false)
            {
                components.insert(QStringLiteral("renderable"), surface);
            }
            if (light.isEmpty() == false)
            {
                components.insert(QStringLiteral("light"), light);
            }
            entity.insert(QStringLiteral("components"), components);
            entities[i] = entity;
        }
        edited.insert(QStringLiteral("entities"), entities);
        WorldDocument world;
        RenderScene scene;
        const bool isLoaded = world.LoadSchema(Schema, Error) && world.Load(QJsonDocument(edited).toJson(), Error) &&
                              RenderScene::Load(Directory.path(), world, scene, Error);
        INFO(Error.toStdString());
        REQUIRE(isLoaded == true);
        scene.HasStartView = true;
        scene.StartView.EyeWorld = {static_cast<float64>(eyeWorld.X),
                                    static_cast<float64>(eyeWorld.Y),
                                    static_cast<float64>(eyeWorld.Z)};
        scene.StartView.TargetWorld = {static_cast<float64>(targetWorld.X),
                                       static_cast<float64>(targetWorld.Y),
                                       static_cast<float64>(targetWorld.Z)};
        scene.StartView.UpWorld = {0, 1, 0};
        scene.StartView.VerticalFov = .8F;
        return scene;
    }
};

qsizetype FindObject(const RenderScene& scene, const QString& name)
{
    for (qsizetype i = 0; i < scene.Objects.size(); ++i)
    {
        if (scene.Objects[i].Name == name)
        {
            return i;
        }
    }
    FAIL("Missing authored plane " << name.toStdString());
    return -1;
}

// Composing an authored World can put its tint into a 1x1 sRGB texture.
// FlatTint includes that decoded texel for this fixture's uniform surfaces.
math::Vector3 ReadPlaneAlbedo(const RenderScene& scene, qsizetype index)
{
    const PreviewObject& object = scene.Objects[index];
    const math::Vector4 tint = scene.Surfaces[object.Surface].FlatTint;
    return {tint.X * object.Tint.X, tint.Y * object.Tint.Y, tint.Z * object.Tint.Z};
}

// Midpoint area quadrature for the horizontal Lambertian panel and vertical receiver.
// Integrate E_p * rho_p/pi * cos_p * cos_r / distance^2 over the visible panel.
math::Vector3 IntegratePanel(const RenderScene& scene,
                             const math::Vector3& receiverWorld,
                             const math::Vector3& panelAlbedo,
                             bool hasBlocker)
{
    const math::Affine3& panel = scene.Objects[FindObject(scene, QStringLiteral("Bounce Panel"))].Transform;
    const math::Affine3& divider = scene.Objects[FindObject(scene, QStringLiteral("Door Lower"))].Transform;
    const float32 openingBottom = divider.Translation.Y + math::Abs(divider.Columns[0].Y) * .5F;
    const math::Affine3& upper = scene.Objects[FindObject(scene, QStringLiteral("Door Upper"))].Transform;
    const float32 openingTop = upper.Translation.Y - math::Abs(upper.Columns[0].Y) * .5F;
    const float64 lux = scene.MeterDirectionalIlluminance({0, 1, 0});
    constexpr int32 STEPS = 128;
    const float64 cellArea = static_cast<float64>(math::Abs(panel.Columns[0].X * panel.Columns[2].Z)) / (STEPS * STEPS);
    const qsizetype blockerIndex = hasBlocker == true ? FindObject(scene, QStringLiteral("Bounce Blocker")) : 0;
    float64 sum = 0;
    for (int32 j = 0; j < STEPS; ++j)
    {
        for (int32 i = 0; i < STEPS; ++i)
        {
            const math::Vector3 delta = panel.Translation - receiverWorld +
                                        panel.Columns[0] * ((static_cast<float32>(i) + .5F) / STEPS - .5F) +
                                        panel.Columns[2] * ((static_cast<float32>(j) + .5F) / STEPS - .5F);
            if (receiverWorld.X > divider.Translation.X)
            {
                const float32 t = (divider.Translation.X - receiverWorld.X) / delta.X;
                const float32 y = receiverWorld.Y + t * delta.Y;
                if (t > 0 && t < 1 && (y <= openingBottom || y >= openingTop))
                {
                    continue;
                }
            }
            if (hasBlocker == true)
            {
                const math::Affine3& blocker = scene.Objects[blockerIndex].Transform;
                const float32 t = (blocker.Translation.Z - receiverWorld.Z) / delta.Z;
                const math::Vector3 hit = receiverWorld + delta * t - blocker.Translation;
                if (t > 0 && t < 1 && math::Abs(hit.X) <= math::Abs(blocker.Columns[0].X) * .5F &&
                    math::Abs(hit.Y) <= math::Abs(blocker.Columns[2].Y) * .5F)
                {
                    continue;
                }
            }
            const float64 squared = static_cast<float64>(math::Dot(delta, delta));
            sum += static_cast<float64>(math::Max(0.F, -delta.Y) * math::Max(0.F, delta.Z)) / (squared * squared);
        }
    }
    return panelAlbedo * static_cast<float32>(lux * cellArea * sum / math::kPi);
}

void CheckOneBounce(const RenderScene& scene, const Observation& observation)
{
    const qsizetype receiver = FindObject(scene, QStringLiteral("Bounce Receiver"));
    const math::Affine3& transform = scene.Objects[receiver].Transform;
    const math::Vector3 point =
        transform.Translation + (transform.Columns[2] - transform.Columns[0]) * TRIANGLE_CENTROID_OFFSET;
    preview_lighting::Inputs inputs;
    REQUIRE(preview_lighting::BuildInputs(scene,
                                          preview_lighting::MakeKey(scene),
                                          inputs,
                                          preview_lighting::InputsTarget::Surfaces) == rhi::RasterStatus::Ready);
    const qsizetype panel = FindObject(scene, QStringLiteral("Bounce Panel"));
    const math::Vector3 albedo = ReadPlaneAlbedo(scene, panel);
    // Isolate the matte lobe in this bounded-depth control. The viewport below uses the
    // unmodified authored material, including its small achromatic dielectric reflection.
    inputs.Surfaces[static_cast<usize>(panel)].Scattering.Model = ludus::content::scene_format::MaterialModel::Diffuse;
    const math::Vector3 expected = IntegratePanel(scene, point, albedo, observation.HasBlocker);
    rr::StaticLightingBakeDescription description;
    description.SamplesPerTriangle = 4096;
    description.MaxDepth = 2;
    description.SceneRevision = inputs.SceneRevision;
    description.DependencyRevision = inputs.DependencyRevision;
    description.AnalyticLightIds = {scene.AlwaysOnLightIds.constData(),
                                    static_cast<usize>(scene.AlwaysOnLightIds.size())};
    description.Surfaces = {inputs.Surfaces.GetData(), inputs.Surfaces.GetSize()};
    threading::JobSystem jobs;
    REQUIRE(jobs.Initialize({ .WorkerCount = STATIC_LIGHTING_WORKER_COUNT }) == threading::JobStatus::Ok);
    StaticLightingJob job(scene, description);
    REQUIRE(job.Start(jobs) == rhi::RasterStatus::Pending);
    REQUIRE(job.Wait() == rhi::RasterStatus::Ready);
    const UniquePtr<rr::StaticLightingBake> bake = job.TakeBake();
    rr::StaticLightingChart chart;
    REQUIRE(bake->GetChart(static_cast<usize>(receiver), chart) == rhi::RasterStatus::Ready);
    const math::Vector3 irradiance{chart.Scale.X * chart.Pixels[0] / 255.F,
                                   chart.Scale.Y * chart.Pixels[1] / 255.F,
                                   chart.Scale.Z * chart.Pixels[2] / 255.F};
    qInfo("Lighting Room %s one-bounce expected=(%.5f,%.5f,%.5f) measured=(%.5f,%.5f,%.5f)",
          observation.Name,
          static_cast<float64>(expected.X),
          static_cast<float64>(expected.Y),
          static_cast<float64>(expected.Z),
          static_cast<float64>(irradiance.X),
          static_cast<float64>(irradiance.Y),
          static_cast<float64>(irradiance.Z));
    CHECK(math::Abs(irradiance.X - expected.X) <= .01F + .08F * expected.X);
    CHECK(math::Abs(irradiance.Y - expected.Y) <= .01F + .08F * expected.Y);
    CHECK(math::Abs(irradiance.Z - expected.Z) <= .01F + .08F * expected.Z);
}

QImage GrabViewport()
{
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport"))
        {
            return static_cast<QQuickWindow*>(window)->grabWindow();
        }
    }
    return {};
}

math::Vector3 MeasureWorldPatch(RenderPreview& inoutPreview, PreviewViewport& inoutViewport)
{
    for (QWindow* window : QGuiApplication::allWindows())
    {
        if (window->objectName() == QStringLiteral("LudusGpuViewport") && window->isExposed() == false)
        {
            inoutViewport.raise();
            inoutViewport.activateWindow();
            REQUIRE(QTest::qWaitForWindowExposed(window) == true);
        }
    }
    const uint64 frame = inoutPreview.Frames();
    const uint64 presented = inoutViewport.PresentedFrames();
    const bool hasFrame =
        QTest::qWaitFor([&]() { return inoutPreview.Frames() >= frame + SETTLE_FRAMES; }, PREPARATION_MS);
    INFO(inoutPreview.Status().toStdString());
    CAPTURE(frame,
            inoutPreview.Frames(),
            presented,
            inoutViewport.PresentedFrames(),
            inoutPreview.ShouldRenderPresentation());
    REQUIRE(hasFrame == true);
    QTest::qWait(100);
    const QImage image = GrabViewport();
    REQUIRE(image.isNull() == false);
    rr::ViewDescription view;
    QString error;
    REQUIRE(inoutPreview.DescribeView(static_cast<uint32>(image.width()),
                                      static_cast<uint32>(image.height()),
                                      view,
                                      error) == true);
    const math::Vector3d target = inoutPreview.Scene().StartView.TargetWorld;
    const math::Vector4 clip = view.WorldToClip * math::Vector4{static_cast<float32>(target.X),
                                                                static_cast<float32>(target.Y),
                                                                static_cast<float32>(target.Z),
                                                                1};
    const QPoint pixel(static_cast<int32>((.5F + .5F * clip.X / clip.W) * static_cast<float32>(image.width())),
                       static_cast<int32>((.5F - .5F * clip.Y / clip.W) * static_cast<float32>(image.height())));
    REQUIRE(image.rect().contains(pixel) == true);
    const QColor center = image.pixelColor(pixel);
    return rr::InvertPbrNeutral(
        {math::DecodeSrgb(center.redF()), math::DecodeSrgb(center.greenF()), math::DecodeSrgb(center.blueF())});
}

TEST_CASE("Lighting Room World retains the baked diffuse and live specular components in native Lit",
          "[.render-native][lighting-room]")
{
    Fixture fixture;
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(VIEW_WIDTH, VIEW_HEIGHT);
    viewport.show();
    viewport.raise();
    viewport.activateWindow();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport) == true);
    const QString worldPath = fixture.Directory.filePath(QStringLiteral("lighting.world.json"));
    preview.SetSurfaceBakeWorldPath(worldPath);
    WorldSurfaceBakeState reopened;
    preview.SetShading(PreviewShading::Lit);
    QHash<QString, math::Vector3> samples;
    QHash<QString, float32> specular;
    QElapsedTimer startup;
    startup.start();
    float64 gpuMilliseconds = 0;
    QObject::connect(&preview,
                     &RenderPreview::GpuProfileSample,
                     &viewport,
                     [&](uint64, uint64, uint64, uint64 nanoseconds, bool isValid) {
                         if (isValid == true)
                         {
                             gpuMilliseconds = static_cast<float64>(nanoseconds) / 1000000;
                         }
                     });
    for (const Observation& observation : OBSERVATIONS)
    {
        INFO(observation.Name);
        const RenderScene scene = fixture.LoadScene(observation);
        if (observation.Target == Patch::Bounce && observation.HasLiveLight == false)
        {
            CheckOneBounce(scene, observation);
        }
        REQUIRE(preview.SetScene(scene) == true);
        if (samples.isEmpty() == false && observation.Target == Patch::Bounce && observation.HasNeutralPanel == true)
        {
            reopened.SetScene(scene);
            CHECK(reopened.GetReady().isNull() == true);
        }
        if (preview.Running() == false)
        {
            REQUIRE(preview.Start() == true);
            const bool hasFrame = QTest::qWaitFor([&]() { return viewport.PresentedFrames() != 0; }, PREPARATION_MS);
            INFO(preview.Status().toStdString());
            REQUIRE(hasFrame == true);
            static_cast<void>(preview.SetGpuProfilingEnabled(true));
        }
        REQUIRE(preview.GoToStartView() == true);
        if (observation.HasSelectedLight == true)
        {
            const QByteArray key = preview_lighting::MakeKey(scene);
            REQUIRE(QTest::qWaitFor(
                        [&]() {
                            const QSharedPointer<const WorldSurfaceBake> bake = preview.GetSurfaceBake();
                            return bake.isNull() == false && bake->LightingKey == key &&
                                   bake->ProbeAtlas.Pixels.isEmpty() == false &&
                                   preview.Status().contains(QStringLiteral("probe GI baked")) == true;
                        },
                        PREPARATION_MS) == true);
            if (samples.isEmpty() == true)
            {
                REQUIRE(preview.SaveSurfaceBake() == true);
                const QSharedPointer<const WorldSurfaceBake> bake = preview.GetSurfaceBake();
                reopened.SetWorldPath(worldPath);
                reopened.SetScene(scene);
                REQUIRE(reopened.GetReady().isNull() == false);
                qsizetype chartBytes = 0;
                for (const WorldSurfaceChart& chart : bake->Charts)
                {
                    chartBytes += chart.Pixels.size();
                }
                qInfo("Lighting Room chart bytes %lld, probe atlas bytes %lld",
                      static_cast<long long>(chartBytes),
                      static_cast<long long>(bake->ProbeAtlas.Pixels.size()));
            }
        }
        else
        {
            const QString expectedStatus = QStringLiteral("Lit: %1 lights").arg(scene.AuthoredLights.Count);
            const bool isCurrent =
                QTest::qWaitFor([&]() { return preview.Status().contains(expectedStatus); }, PREPARATION_MS);
            INFO(preview.Status().toStdString());
            REQUIRE(isCurrent == true);
        }
        const math::Vector3 linear = MeasureWorldPatch(preview, viewport);
        if (observation.Target == Patch::Bounce && observation.HasLiveLight == false)
        {
            const qsizetype receiver = FindObject(scene, QStringLiteral("Bounce Receiver"));
            const WorldSurfaceChart& chart = preview.GetSurfaceBake()->Charts[receiver];
            const math::Vector3 albedo = ReadPlaneAlbedo(scene, receiver);
            const math::Vector3 irradiance{chart.Scale.X * static_cast<uint8>(chart.Pixels[0]) / 255.F,
                                           chart.Scale.Y * static_cast<uint8>(chart.Pixels[1]) / 255.F,
                                           chart.Scale.Z * static_cast<uint8>(chart.Pixels[2]) / 255.F};
            const math::Vector3 expected = math::Hadamard(albedo, irradiance) * (.96F / math::kPiF);
            CHECK(math::Length(linear - expected) <= .012F);
        }
        if (samples.isEmpty() == true)
        {
            qInfo("Lighting Room native start-to-baked-overview ms=%lld GPU frame ms=%.4f",
                  static_cast<long long>(startup.elapsed()),
                  gpuMilliseconds);
        }
        qInfo("Lighting Room %s linear=(%f,%f,%f)",
              observation.Name,
              static_cast<float64>(linear.X),
              static_cast<float64>(linear.Y),
              static_cast<float64>(linear.Z));
        samples.insert(QString::fromLatin1(observation.Name), linear);
        if (observation.Target == Patch::Moving && observation.HasLiveLight == false)
        {
            const math::Vector3 position = RECEIVER_PATH[observation.PathIndex];
            const qsizetype panel = FindObject(scene, QStringLiteral("Bounce Panel"));
            const math::Vector3 tint = ReadPlaneAlbedo(scene, panel);
            const math::Vector3 expected = IntegratePanel(scene, position, {tint.X, tint.Y, tint.Z}, false);
            const float32 receiverAlbedo =
                ReadPlaneAlbedo(scene, FindObject(scene, QStringLiteral("Moving Receiver"))).X;
            const float32 measured = linear.X * math::kPiF / (receiverAlbedo * .96F);
            qInfo("Lighting Room %s red irradiance=%.5f one-bounce reference=%.5f",
                  observation.Name,
                  static_cast<float64>(measured),
                  static_cast<float64>(expected.X));
            if (QString::fromLatin1(observation.Name) == QStringLiteral("opening-right"))
            {
                CHECK(measured >= .5F * expected.X);
            }
            const float32 lookup =
                testing::ReadPositiveZProbeIrradiance(preview.GetSurfaceBake()->ProbeAtlas, scene.ProbeGrid, position);
            CHECK(math::Abs(measured - lookup) <= .02F + .03F * lookup);
            if (QString::fromLatin1(observation.Name) == QStringLiteral("divider") && measured > expected.X + .01F)
            {
                WARN("Closed-side probe leak exceeds the 0.01 irradiance criterion: " << measured);
            }
        }
        if (observation.Target == Patch::Rough && observation.HasSelectedLight == true &&
            observation.HasLiveLight == false)
        {
            const qsizetype index = FindObject(scene, QStringLiteral("Rough Dielectric"));
            const WorldSurfaceChart& chart = preview.GetSurfaceBake()->Charts[index];
            const float32 irradiance = chart.Scale.X * static_cast<uint8>(chart.Pixels[0]) / 255.F;
            CHECK(math::Abs(irradiance - 2.F) < .03F);
            const math::Vector3 offset = observation.IsOblique == true ? OBLIQUE_VIEW_OFFSET : PEAK_VIEW_OFFSET;
            const float32 grazing = 1 - offset.Y / math::Length(offset);
            const float32 transmission = .96F * (1 - grazing * grazing * grazing * grazing * grazing);
            const float32 diffuse = ReadPlaneAlbedo(scene, index).X * irradiance * transmission / math::kPiF;
            specular.insert(QString::fromLatin1(observation.Name), linear.X - diffuse);
            qInfo("Lighting Room %s chart E=%.5f diffuse=%.5f residual specular=%.5f",
                  observation.Name,
                  static_cast<float64>(irradiance),
                  static_cast<float64>(diffuse),
                  static_cast<float64>(linear.X - diffuse));
            // Retain the real selected light and add a missing source to the selection.
            // The old key is stale and the replacement cannot bake, so the same lit geometry draws with realtime
            // diffuse.
            RenderScene stale = scene;
            stale.AlwaysOnLightIds.append(0);
            stale.Identity.append("missing-source");
            REQUIRE(preview.SetScene(stale) == true);
            REQUIRE(preview.GoToStartView() == true);
            REQUIRE(
                QTest::qWaitFor([&]() { return preview.Status().contains(QStringLiteral("Using realtime lighting")); },
                                PREPARATION_MS) == true);
            REQUIRE(preview.GetSurfaceBake().isNull() == true);
            const math::Vector3 live = MeasureWorldPatch(preview, viewport);
            qInfo("Lighting Room %s fallback=(%f,%f,%f)",
                  observation.Name,
                  static_cast<float64>(live.X),
                  static_cast<float64>(live.Y),
                  static_cast<float64>(live.Z));
            CHECK(math::Abs(live.X - linear.X) < .025F);
            CHECK(math::Abs(live.Y - linear.Y) < .025F);
            CHECK(math::Abs(live.Z - linear.Z) < .025F);
        }
    }
    CHECK(samples[QStringLiteral("saturated")].X > samples[QStringLiteral("saturated")].Z + MIN_CONTROL_SIGNAL);
    const math::Vector3 saturated = samples[QStringLiteral("saturated")];
    const float32 chroma = saturated.X - saturated.Z;
    REQUIRE(chroma > MIN_CONTROL_SIGNAL);
    // Subtract the blue channel to cancel the dielectric's achromatic highlight.
    CHECK(math::Abs((saturated.Y - saturated.Z) / chroma - (.08F - .04F) / (.8F - .04F)) < .06F);
    CHECK(samples[QStringLiteral("neutral")].Z > samples[QStringLiteral("saturated")].Z + MIN_CONTROL_SIGNAL);
    CHECK(samples[QStringLiteral("blocked")].X < samples[QStringLiteral("saturated")].X * .5F);
    CHECK(samples[QStringLiteral("peak")].X > samples[QStringLiteral("oblique")].X + MIN_CONTROL_SIGNAL);
    CHECK(samples[QStringLiteral("rough-live")].Z > samples[QStringLiteral("peak")].Z + MIN_CONTROL_SIGNAL);
    CHECK(specular[QStringLiteral("peak")] > specular[QStringLiteral("oblique")] + MIN_CONTROL_SIGNAL);
    const math::Vector3 sum = samples[QStringLiteral("peak")] + samples[QStringLiteral("live-only")];
    CHECK(math::Length(sum - samples[QStringLiteral("rough-live")]) < .035F);
    CHECK(math::Length(samples[QStringLiteral("dark")]) < DARK_TOLERANCE);
    CHECK(samples[QStringLiteral("outside")].X < DARK_TOLERANCE);
    // The point-to-receiver ray crosses the upper divider. The native shadow map masks the blue light.
    CHECK(samples[QStringLiteral("outside-live")].Z < DARK_TOLERANCE);
    preview.Stop();
}

TEST_CASE("Lighting Room game measures its Forward Lit output while presented by the Play viewport",
          "[.lighting-room-hosted]")
{
    const QString gamePath = qEnvironmentVariable("LUDUS_LIGHTING_ROOM_PLAYER");
    REQUIRE(QFileInfo(gamePath).isExecutable() == true);
    Fixture fixture;
    RenderPreview preview;
    PreviewViewport viewport(&preview);
    viewport.resize(VIEW_WIDTH, VIEW_HEIGHT);
    viewport.show();
    viewport.raise();
    viewport.activateWindow();
    REQUIRE(QTest::qWaitForWindowExposed(&viewport) == true);
    REQUIRE(preview.SetScene(fixture.LoadScene(OBSERVATIONS[0])) == true);
    REQUIRE(preview.Start() == true);
    const bool hasFrame = QTest::qWaitFor([&]() { return viewport.PresentedFrames() > 0; }, PREPARATION_MS);
    INFO(preview.Status().toStdString());
    REQUIRE(hasFrame == true);
    rhi::NativeDeviceLease lease;
    rhi::NativeDeviceInfo device;
    REQUIRE(preview.PinDevice(lease) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::GetNativeDeviceInfo(lease, device) == rhi::RasterStatus::Ready);
    REQUIRE(rhi::ReleaseNativeDevice(lease) == rhi::RasterStatus::Ready);
    PlayViewportSource play;
    REQUIRE(play.Start(QStringLiteral("/tmp"), device.Identity, VIEW_WIDTH, VIEW_HEIGHT, 1) == true);
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(ludus::platform::HOSTED_WINDOW_ENVIRONMENT_VARIABLE),
                       play.GetChannelAddress());
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    viewport.SetFrameSource(&play);
    const QString verifier = QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT "/examples/lighting-room/qualify.py");
    child.start(QStringLiteral(LUDUS_TEST_PYTHON), {verifier, gamePath});
    REQUIRE(child.waitForStarted() == true);
    const bool isFinished = QTest::qWaitFor(
        [&]() {
            play.Pump();
            return child.state() == QProcess::NotRunning;
        },
        PREPARATION_MS);
    const QByteArray output = child.readAll();
    qInfo().noquote() << output;
    REQUIRE(isFinished == true);
    CHECK(child.exitCode() == 0);
    CHECK(output.contains("hosted=1") == true);
    CHECK(output.contains("outside-live rgb8=") == true);
    CHECK(play.GetTakenFrames() > 0);
    viewport.SetFrameSource(nullptr);
    play.Stop();
    preview.Stop();
}
} // namespace
