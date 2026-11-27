// Material gate 4, part 3 of #1310: a project-owned BRDF provider through World, the Editor and the installed SDK.
//
// The project owns its material: tests/scenes/materials/oren-nayar-provider.luscene.json authors the
// `project/oren-nayar` closure program, a qualitative Oren-Nayar lobe. Importing it registers the material in the
// project catalog, and Assign Project BRDF binds a World slot to it through the level's `project_brdf` binding, the
// same registration and cook path as a version-1 project BRDF. The two-board World keeps one board on the built-in
// Diffuse control. Two World directional lights light both boards, and the compute ray-tracing template renders
// them with unchanged graph source.
//
// Slusallek and Seidel put reflection behind interfaces in a small extensible kernel: Philipp Slusallek and
// Hans-Peter Seidel, "Towards an Open Rendering Kernel for Image Synthesis", Rendering Techniques '96, pp. 51-60,
// https://doi.org/10.1007/978-3-7091-7484-5_6. These tests check Ludus's equivalent boundary from project
// registration through cooking and graph execution. See
// docs/architecture/renderer-systems.md#project-oren-nayar-lobe-under-renderer-owned-lights.
#include "internal/builtin_world.h"
#include "internal/level_workspace.h"
#include "internal/light_switches.h"
#include "internal/material_inspection.h"
#include "internal/playable_world.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/renderer_workspace.h"
#include "internal/viewport_graph_capture.h"
#include "internal/viewport_raster_capture.h"
#include "internal/viewport_reference_capture.h"
#include "internal/world_document.h"
#include "internal/world_graph_view.h"
#include "internal/world_lighting_coverage.h"
#include "internal/world_workspace.h"
#include "material_program_fixtures.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/scene_format/material.h>
#include <ludus/content/scene_format/material_program.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/render_graph_capture.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/platform/entropy.h>
#include <ludus/runtime/resources/scene.h>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <cstring>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace format = ludus::content::scene_format;
namespace fixtures = ludus::renderer_tests;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace resources = ludus::resources;

namespace
{
/// Longest wait for one scene operation, placement cook or viewport frame on a loaded machine.
constexpr int64 SCENE_WAIT_MILLISECONDS = 60000;
/// Longest wait for one reference or graph capture of the small fixture image.
constexpr int64 CAPTURE_WAIT_MILLISECONDS = 120000;
/// Service budget of one reference capture step.
constexpr uint64 SERVICE_BUDGET_NANOSECONDS = 4000000;
/// Capture extent: the boards' 2:1 framing at a size whose CPU oracle captures take a second.
constexpr uint32 CAPTURE_WIDTH = 64;
constexpr uint32 CAPTURE_HEIGHT = 32;
/// Samples per pixel of each capture. A one-light reference pixel is deterministic with centred rays; two lights
/// share one uniform light draw per vertex, and the graph also jitters its camera rays.
constexpr uint32 CAPTURE_SAMPLES = 128;
/// Seed of every reference capture, so CPU and GPU estimate with the same random numbers.
constexpr uint64 CAPTURE_SEED = 1310;
/// Pixels whose hit lies within this distance of a board edge, in metres, are left out of region checks: a
/// jittered graph ray or a one-pixel camera difference can cross the edge there.
constexpr float32 EDGE_MARGIN_METRES = 0.08F;
/// The provider's authored base colour, the albedo of both boards.
constexpr math::Vector3 BOARD_ALBEDO{0.9F, 0.6F, 0.3F};
/// The provider program's sigma constant, in radians; equal to the renderer fixture's.
constexpr float32 PROVIDER_SIGMA_RADIANS = 0.5F;
static_assert(PROVIDER_SIGMA_RADIANS == fixtures::OREN_NAYAR_SIGMA_RADIANS);
/// The two-light fixture of the renderer's reference test, in the World's Y-up frame: each light's unit
/// direction toward it, its colour and its illuminance. Each light's azimuth is 60 degrees from the camera's.
constexpr math::Vector3 FIRST_TOWARD_UNNORMALIZED{0.6F, 0.72F, 0.35F};
constexpr math::Vector3 SECOND_TOWARD_UNNORMALIZED{-0.55F, 0.77F, 0.32F};
constexpr math::Vector3 FIRST_COLOR{1, 0.55F, 0.2F};
constexpr math::Vector3 SECOND_COLOR{0.25F, 0.5F, 1};
constexpr float32 FIRST_ILLUMINANCE = 2;
constexpr float32 SECOND_ILLUMINANCE = 3;
/// Region labels of a capture pixel.
constexpr uint8 LABEL_NONE = 0;
constexpr uint8 LABEL_CONTROL = 1;
constexpr uint8 LABEL_PROJECT = 2;
constexpr usize LABEL_COUNT = 3;
/// Relative bound of a deterministic one-light CPU oracle pixel against the analytic response.
constexpr float32 EXACT_TOLERANCE = 2e-4F;
/// Relative bound of a region mean against its analytic mean: the light draw of two lights, and the graph's
/// jittered camera rays across each pixel's footprint.
constexpr float32 MEAN_TOLERANCE = 0.03F;
/// The typed-material parity bounds for matched raw linear CPU and GPU radiance.
constexpr float32 PARITY_RMS_TOLERANCE = 1e-3F;

/// Which World lights a capture keeps switched on.
enum class LightSet : uint8
{
    First,
    Second,
    Both,
};
constexpr StaticArray<LightSet, 3> LIGHT_SETS{{LightSet::First, LightSet::Second, LightSet::Both}};

const char* GetLightSetName(LightSet set) noexcept
{
    return set == LightSet::First ? "first" : set == LightSet::Second ? "second" : "both";
}

void WriteFile(const QString& path, const QByteArray& bytes)
{
    QSaveFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly) == true);
    REQUIRE(file.write(bytes) == bytes.size());
    REQUIRE(file.commit() == true);
}

QByteArray ReadFile(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly) == true);
    return file.readAll();
}

/// Copy one engine material test scene into the project, as an author adds a provider scene to a game.
QString CopyMaterialScene(const QDir& root, const QString& name)
{
    const QString source =
        QDir(QStringLiteral(LUDUS_EDITOR_PROJECT_ROOT)).filePath(QStringLiteral("tests/scenes/materials/") + name);
    REQUIRE(QDir().mkpath(root.filePath(QStringLiteral("scenes"))) == true);
    const QString destination = root.filePath(QStringLiteral("scenes/") + name);
    REQUIRE(QFile::copy(source, destination) == true);
    return destination;
}

LevelWorkspace& GetLevel(const WorldWorkspace& workspace)
{
    LevelWorkspace* sceneOrNull = workspace.findChild<LevelWorkspace*>();
    REQUIRE(sceneOrNull != nullptr);
    return *sceneOrNull;
}

/// Wait until no scene operation or placement cook runs. A failed operation also settles.
void WaitForSettledScene(const WorldWorkspace& workspace, RendererWorkspace* rendererOrNull = nullptr)
{
    LevelWorkspace& scene = GetLevel(workspace);
    QElapsedTimer timer;
    timer.start();
    const auto isSettled = [&scene]() {
        return scene.CanStartOperation() == true && scene.Busy() == false &&
               scene.GetPlacementValidation() == PlacementValidation::Ready;
    };
    while (isSettled() == false && timer.elapsed() < SCENE_WAIT_MILLISECONDS)
    {
        QTest::qWait(10);
        if (rendererOrNull != nullptr)
        {
            rendererOrNull->ServicePreview();
        }
    }
    REQUIRE(isSettled() == true);
}

/// Wait until the presented scene is validated and the World can save.
void WaitForValidatedScene(const WorldWorkspace& workspace, RendererWorkspace* rendererOrNull = nullptr)
{
    LevelWorkspace& scene = GetLevel(workspace);
    QElapsedTimer timer;
    timer.start();
    const auto isSettled = [&workspace, &scene]() {
        return workspace.CanSave() == true && scene.CanStartOperation() == true &&
               scene.GetPlacementValidation() == PlacementValidation::Ready;
    };
    while (isSettled() == false && timer.elapsed() < SCENE_WAIT_MILLISECONDS)
    {
        QTest::qWait(10);
        if (rendererOrNull != nullptr)
        {
            rendererOrNull->ServicePreview();
        }
    }
    REQUIRE(isSettled() == true);
}

/// The World's status line, where it reports a rejected edit.
QString GetStatusText(const WorldWorkspace& workspace)
{
    const QLabel* statusOrNull = workspace.findChild<QLabel*>(QStringLiteral("worldStatus"));
    REQUIRE(statusOrNull != nullptr);
    return statusOrNull->text();
}

/// The presented object named `name`; fails the test when the World lacks it.
QString FindObject(const RenderScene& scene, const QString& name)
{
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Name == name)
        {
            return object.Id;
        }
    }
    FAIL("The presented World lacks " << name.toStdString());
    return {};
}

const PreviewObject& GetObject(const RenderScene& scene, const QString& id)
{
    for (const PreviewObject& object : scene.Objects)
    {
        if (object.Id == id)
        {
            return object;
        }
    }
    FAIL("The presented World lacks object " << id.toStdString());
    return scene.Objects[0];
}

/// Author the World's two directional lights, as a lighting artist adds and colours two Suns, and save it.
/// Scene sources cannot author lights, so the World owns them.
QString AuthorTwoLightWorld(const QDir& root)
{
    QByteArray schema;
    QString error;
    REQUIRE(ReadBuiltInWorldAsset(QStringLiteral("world.schema.json"), schema, error) == true);
    WorldDocument document;
    REQUIRE(document.LoadSchema(schema, error) == true);
    REQUIRE(document.New(error) == true);
    const identity::EntropySource entropy = ludus::platform::GetSystemEntropySource();
    struct AuthoredLight final
    {
        math::Vector3 TowardWorld;
        math::Vector3 Color;
        float32 Illuminance;
        const char* Name;
    };
    const StaticArray<AuthoredLight, 2> lights{{
        {FIRST_TOWARD_UNNORMALIZED, FIRST_COLOR, FIRST_ILLUMINANCE, "Warm light"},
        {SECOND_TOWARD_UNNORMALIZED, SECOND_COLOR, SECOND_ILLUMINANCE, "Cool light"},
    }};
    for (const AuthoredLight& light : lights)
    {
        const math::Vector3 towardWorld = light.TowardWorld / math::Length(light.TowardWorld);
        const WorldSunPlacement placement
        {
            .RotationDegrees = ComputeWorldLightRotationDegrees(-towardWorld),
            .IlluminanceLux = light.Illuminance,
        };
        QString id;
        REQUIRE(CreateWorldSun(document, entropy, placement, id, error) == true);
        const QJsonObject entity = document.Entity(id);
        QJsonObject components = entity.value(QStringLiteral("components")).toObject();
        QJsonObject values = components.value(QStringLiteral("light")).toObject();
        values.insert(QStringLiteral("r"), static_cast<float64>(light.Color.X));
        values.insert(QStringLiteral("g"), static_cast<float64>(light.Color.Y));
        values.insert(QStringLiteral("b"), static_cast<float64>(light.Color.Z));
        components.insert(QStringLiteral("light"), values);
        REQUIRE(document.Edit(id, components, QString::fromLatin1(light.Name), error) == true);
    }
    const QString path = root.filePath(QStringLiteral("boards.world.json"));
    WriteFile(path, document.Encode());
    return path;
}

/// Import the boards and the provider, then bind the Project board's slot to the provider's material through
/// the Material group's Assign Project BRDF, as an author does.
void ImportAndAssignProvider(WorldWorkspace& inoutWorld,
                             const QString& boards,
                             const QString& provider,
                             RendererWorkspace* rendererOrNull = nullptr)
{
    REQUIRE(inoutWorld.ImportScene(provider) == true);
    WaitForValidatedScene(inoutWorld, rendererOrNull);
    REQUIRE(inoutWorld.ImportScene(boards) == true);
    WaitForValidatedScene(inoutWorld, rendererOrNull);
    const RenderScene& scene = GetLevel(inoutWorld).PreviewScene();
    const QString project = FindObject(scene, QStringLiteral("Project board"));
    REQUIRE(inoutWorld.SelectEntity(project) == true);
    QComboBox* choicesOrNull = inoutWorld.findChild<QComboBox*>(QStringLiteral("worldProjectBrdfChoice"));
    REQUIRE(choicesOrNull != nullptr);
    // The provider's closure program is listed as a project material by its project name.
    const int32 choice = choicesOrNull->findText(QStringLiteral("project/oren-nayar"), Qt::MatchStartsWith);
    REQUIRE(choice >= 0);
    choicesOrNull->setCurrentIndex(choice);
    QPushButton* assignOrNull = inoutWorld.findChild<QPushButton*>(QStringLiteral("worldAssignProjectBrdf"));
    REQUIRE(assignOrNull != nullptr);
    REQUIRE(assignOrNull->isEnabled() == true);
    assignOrNull->click();
    WaitForValidatedScene(inoutWorld, rendererOrNull);
}

/// Check that the Project board draws the provider's Oren-Nayar program and the Control board the built-in
/// Diffuse material.
void CheckAssignedMaterials(const RenderScene& scene)
{
    const resources::SceneMaterial* projectOrNull =
        GetSceneObjectMaterialOrNull(scene, FindObject(scene, QStringLiteral("Project board")));
    REQUIRE(projectOrNull != nullptr);
    CHECK(projectOrNull->Name.View() == "project/oren-nayar");
    CHECK(projectOrNull->Scattering.Model == format::MaterialModel::Program);
    REQUIRE(projectOrNull->Program.LobeCount == 1);
    CHECK(projectOrNull->Program.Lobes[0].Operation == format::MaterialLobeOperation::OrenNayar);
    CHECK(format::SupportsFixedDimensionReflection(projectOrNull->Scattering, projectOrNull->Program) == true);
    const resources::SceneMaterial* controlOrNull =
        GetSceneObjectMaterialOrNull(scene, FindObject(scene, QStringLiteral("Control board")));
    REQUIRE(controlOrNull != nullptr);
    CHECK(controlOrNull->Scattering.Model == format::MaterialModel::Diffuse);
    CHECK(controlOrNull->Reflectance == projectOrNull->Reflectance);
}

/// World-space plane and bounds of one board.
struct Board final
{
    math::Vector3 NormalWorld{};
    math::Vector3 OriginWorld{};
    math::Vector3 MinimumWorld{};
    math::Vector3 MaximumWorld{};
};

Board MakeBoard(const RenderScene& scene, const QString& id)
{
    // Imported geometry lives in the cooked definition; the presented object's mesh indexes its meshes.
    const PreviewObject& object = GetObject(scene, id);
    REQUIRE(scene.Definition != nullptr);
    REQUIRE(object.Mesh >= 0);
    REQUIRE(static_cast<usize>(object.Mesh) < scene.Definition->Meshes.GetSize());
    const resources::SceneMesh& mesh = scene.Definition->Meshes[static_cast<usize>(object.Mesh)];
    REQUIRE(mesh.Indices.GetSize() >= 3);
    Board board;
    const math::Vector3 a = math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[0]]);
    const math::Vector3 b = math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[1]]);
    const math::Vector3 c = math::TransformPoint(object.Transform, mesh.Positions[mesh.Indices[2]]);
    const math::Vector3 normal = math::Cross(b - a, c - a);
    board.NormalWorld = normal / math::Length(normal);
    board.OriginWorld = a;
    board.MinimumWorld = a;
    board.MaximumWorld = a;
    for (const math::Vector3& position : mesh.Positions)
    {
        const math::Vector3 world = math::TransformPoint(object.Transform, position);
        board.MinimumWorld = {math::Min(board.MinimumWorld.X, world.X),
                              math::Min(board.MinimumWorld.Y, world.Y),
                              math::Min(board.MinimumWorld.Z, world.Z)};
        board.MaximumWorld = {math::Max(board.MaximumWorld.X, world.X),
                              math::Max(board.MaximumWorld.Y, world.Y),
                              math::Max(board.MaximumWorld.Z, world.Z)};
    }
    // The bounds must be one board: about 2 m on each side in the plane.
    CAPTURE(board.MinimumWorld.X, board.MinimumWorld.Z, board.MaximumWorld.X, board.MaximumWorld.Z);
    return board;
}

/// Report whether a point on the board's plane lies inside its bounds by at least the edge margin. An axis the
/// board does not span (its thickness) is not shrunk.
bool IsInsideBoard(const Board& board, math::Vector3 pointWorld) noexcept
{
    constexpr float32 FLAT_EXTENT_METRES = 1e-3F;
    const StaticArray<float32, 3> point{{pointWorld.X, pointWorld.Y, pointWorld.Z}};
    const StaticArray<float32, 3> minimum{{board.MinimumWorld.X, board.MinimumWorld.Y, board.MinimumWorld.Z}};
    const StaticArray<float32, 3> maximum{{board.MaximumWorld.X, board.MaximumWorld.Y, board.MaximumWorld.Z}};
    for (usize axis = 0; axis < point.GetSize(); ++axis)
    {
        const bool isFlat = maximum[axis] - minimum[axis] < FLAT_EXTENT_METRES;
        const float32 margin = isFlat == true ? FLAT_EXTENT_METRES : EDGE_MARGIN_METRES;
        if (point[axis] < minimum[axis] - (isFlat == true ? margin : -margin) ||
            point[axis] > maximum[axis] + (isFlat == true ? margin : -margin))
        {
            return false;
        }
    }
    return true;
}

/// One capture pixel's centre ray in World space, from the view's inverse projection.
struct PixelRay final
{
    math::Vector3 OriginWorld{};
    math::Vector3 DirectionWorld{};
};

/// @param pixel The pixel's index in top-left row order.
PixelRay MakePixelRay(const math::Matrix4& clipToWorld, math::Vector3 cameraWorld, usize pixel)
{
    const usize column = pixel % CAPTURE_WIDTH;
    const usize row = pixel / CAPTURE_WIDTH;
    const float32 clipX = 2 * (static_cast<float32>(column) + 0.5F) / CAPTURE_WIDTH - 1;
    const float32 clipY = 1 - 2 * (static_cast<float32>(row) + 0.5F) / CAPTURE_HEIGHT;
    const math::Vector4 world = clipToWorld * math::Vector4{clipX, clipY, 0.5F, 1};
    const math::Vector3 pointWorld = math::Vector3{world.X, world.Y, world.Z} * (1 / world.W);
    const math::Vector3 direction = pointWorld - cameraWorld;
    return {cameraWorld, direction / math::Length(direction)};
}

/// The analytic single-bounce response of a board to one directional light: albedo times illuminance times the
/// published BRDF times the cosine, at the World-space directions the renderer uses. The Lambertian control is
/// the Oren-Nayar lobe at zero roughness.
math::Vector3 EvaluateDirectResponse(const Board& board,
                                     bool isProject,
                                     math::Vector3 viewTowardWorld,
                                     const renderer::DirectLight& light)
{
    const math::Vector3 lightTowardWorld = -light.Direction / math::Length(light.Direction);
    const float32 cosine = math::Dot(board.NormalWorld, lightTowardWorld);
    if (cosine <= 0)
    {
        return {};
    }
    // The independent evaluation measures angles about +Z: express both directions in the board's frame.
    const math::Vector3 helper =
        math::Abs(board.NormalWorld.X) < 0.9F ? math::Vector3{1, 0, 0} : math::Vector3{0, 1, 0};
    const math::Vector3 tangentUnnormalized = math::Cross(helper, board.NormalWorld);
    const math::Vector3 tangent = tangentUnnormalized / math::Length(tangentUnnormalized);
    const math::Vector3 bitangent = math::Cross(board.NormalWorld, tangent);
    const auto toLocal = [&](math::Vector3 direction) {
        return math::Vector3{math::Dot(direction, tangent),
                             math::Dot(direction, bitangent),
                             math::Dot(direction, board.NormalWorld)};
    };
    const float64 sigma = isProject == true ? static_cast<float64>(PROVIDER_SIGMA_RADIANS) : 0.0;
    const float64 brdf =
        fixtures::EvaluatePublishedOrenNayar(sigma, toLocal(viewTowardWorld), toLocal(lightTowardWorld));
    return math::Hadamard(BOARD_ALBEDO, light.Color) *
           (static_cast<float32>(brdf * static_cast<float64>(cosine)) * light.Intensity);
}

/// Region labels and per-pixel analytic responses of the saved World under each light set.
struct AnalyticImage final
{
    Array<uint8> Labels;
    StaticArray<Array<math::Vector3>, 3> Expected;
};

/// The saved World's lights as the reference renderer receives them, in authored order.
struct WorldLights final
{
    renderer::DirectLight First;
    renderer::DirectLight Second;
};

WorldLights GetWorldLights(const RenderScene& scene)
{
    REQUIRE(scene.AuthoredLights.Count == 2);
    // The renderer orders its lights by source ID, not authoring order: tell them apart by their colour.
    const renderer::DirectLight& a = scene.AuthoredLights.Lights[0];
    const renderer::DirectLight& b = scene.AuthoredLights.Lights[1];
    const bool isFirstLeading = math::Distance(a.Color * a.Intensity, FIRST_COLOR * FIRST_ILLUMINANCE) <
                                math::Distance(b.Color * b.Intensity, FIRST_COLOR * FIRST_ILLUMINANCE);
    WorldLights lights{isFirstLeading == true ? a : b, isFirstLeading == true ? b : a};
    // The authored rotations shine each light along the fixture's directions, with its colour and illuminance.
    const math::Vector3 firstToward = FIRST_TOWARD_UNNORMALIZED / math::Length(FIRST_TOWARD_UNNORMALIZED);
    const math::Vector3 secondToward = SECOND_TOWARD_UNNORMALIZED / math::Length(SECOND_TOWARD_UNNORMALIZED);
    constexpr float32 DIRECTION_TOLERANCE = 1e-3F;
    CHECK(math::Distance(-lights.First.Direction / math::Length(lights.First.Direction), firstToward) <=
          DIRECTION_TOLERANCE);
    CHECK(math::Distance(-lights.Second.Direction / math::Length(lights.Second.Direction), secondToward) <=
          DIRECTION_TOLERANCE);
    CHECK(math::Distance(lights.First.Color * lights.First.Intensity, FIRST_COLOR * FIRST_ILLUMINANCE) <=
          DIRECTION_TOLERANCE);
    CHECK(math::Distance(lights.Second.Color * lights.Second.Intensity, SECOND_COLOR * SECOND_ILLUMINANCE) <=
          DIRECTION_TOLERANCE);
    return lights;
}

AnalyticImage MakeAnalyticImage(const RenderScene& scene, const renderer::ViewDescription& view)
{
    Board control = MakeBoard(scene, FindObject(scene, QStringLiteral("Control board")));
    Board project = MakeBoard(scene, FindObject(scene, QStringLiteral("Project board")));
    // Shade the side the camera sees, whatever the triangles' winding; the lights are on that side too.
    for (Board* board : {&control, &project})
    {
        if (math::Dot(board->NormalWorld, view.CameraPosition - board->OriginWorld) < 0)
        {
            board->NormalWorld = -board->NormalWorld;
        }
    }
    const WorldLights lights = GetWorldLights(scene);
    math::Matrix4 clipToWorld;
    REQUIRE(math::TryInverse(view.WorldToClip, clipToWorld) == math::MathStatus::Success);
    AnalyticImage image;
    const usize pixelCount = usize{CAPTURE_WIDTH} * CAPTURE_HEIGHT;
    REQUIRE(image.Labels.TryResize(pixelCount) == true);
    for (Array<math::Vector3>& expected : image.Expected)
    {
        REQUIRE(expected.TryResize(pixelCount) == true);
    }
    StaticArray<usize, LABEL_COUNT> counts{};
    for (uint32 row = 0; row < CAPTURE_HEIGHT; ++row)
    {
        for (uint32 column = 0; column < CAPTURE_WIDTH; ++column)
        {
            const usize index = usize{row} * CAPTURE_WIDTH + column;
            const PixelRay ray = MakePixelRay(clipToWorld, view.CameraPosition, index);
            image.Labels[index] = LABEL_NONE;
            for (const uint8 label : {LABEL_CONTROL, LABEL_PROJECT})
            {
                const Board& board = label == LABEL_CONTROL ? control : project;
                const float32 facing = math::Dot(ray.DirectionWorld, board.NormalWorld);
                if (facing >= 0)
                {
                    continue;
                }
                const float32 distance = math::Dot(board.OriginWorld - ray.OriginWorld, board.NormalWorld) / facing;
                const math::Vector3 hitWorld = ray.OriginWorld + ray.DirectionWorld * distance;
                if (distance > 0 && IsInsideBoard(board, hitWorld) == true)
                {
                    image.Labels[index] = label;
                }
            }
            const uint8 label = image.Labels[index];
            ++counts[label];
            if (label == LABEL_NONE)
            {
                continue;
            }
            const Board& board = label == LABEL_CONTROL ? control : project;
            const bool isProject = label == LABEL_PROJECT;
            const math::Vector3 first = EvaluateDirectResponse(board, isProject, -ray.DirectionWorld, lights.First);
            const math::Vector3 second = EvaluateDirectResponse(board, isProject, -ray.DirectionWorld, lights.Second);
            image.Expected[0][index] = first;
            image.Expected[1][index] = second;
            image.Expected[2][index] = first + second;
        }
    }
    // Both boards fill a substantial share of the image.
    REQUIRE(counts[LABEL_CONTROL] >= pixelCount / 8);
    REQUIRE(counts[LABEL_PROJECT] >= pixelCount / 8);
    return image;
}

/// Mean of one labelled region of a linear image.
math::Vector3 MeasureRegion(const Array<math::Vector3>& pixels, const Array<uint8>& labels, uint8 label)
{
    math::Vector3 sum{};
    usize count = 0;
    for (usize index = 0; index < pixels.GetSize(); ++index)
    {
        if (labels[index] == label)
        {
            sum = sum + pixels[index];
            ++count;
        }
    }
    REQUIRE(count > 0);
    return sum * (1 / static_cast<float32>(count));
}

bool IsNearRelative(math::Vector3 actual, math::Vector3 expected, float32 tolerance)
{
    return math::Distance(actual, expected) <= tolerance * math::Length(expected);
}

/// The saved World with only `set`'s lights switched on, through the World's light switches.
RenderScene SwitchLights(const RenderScene& scene, LightSet set)
{
    RenderScene presented = scene;
    const WorldLights lights = GetWorldLights(scene);
    LightSwitchSet switches;
    if (set == LightSet::First)
    {
        switches.Disabled.append({LightSourceKind::Light, lights.Second.SourceId});
    }
    else if (set == LightSet::Second)
    {
        switches.Disabled.append({LightSourceKind::Light, lights.First.SourceId});
    }
    LightSwitchReport report;
    REQUIRE(ApplyLightSwitches(switches, presented, report) == true);
    CHECK(report.MissingSources.isEmpty() == true);
    CHECK(presented.AuthoredLights.Count == (set == LightSet::Both ? 2U : 1U));
    return presented;
}

/// One GPU device for the captures, released after them.
struct CaptureDevice final
{
    rhi::DeviceHandle Device{};
    rhi::SurfaceHandle Surface{};
    bool IsAvailable = false;
    CaptureDevice()
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.PortableRaster = true;
        description.Preferred.RayQueries = true;
        rhi::StartupInfo failure;
        const rhi::DeviceStatus started = rhi::CreateDevice({},
                                                            {
                                                                .Width = CAPTURE_WIDTH,
                                                                .Height = CAPTURE_HEIGHT,
                                                            },
                                                            description,
                                                            Device,
                                                            Surface,
                                                            &failure);
        IsAvailable = started == rhi::DeviceStatus::Ready;
        if (started != rhi::DeviceStatus::Ready)
        {
            REQUIRE(failure.Error == rhi::StartupError::AdapterUnavailable);
        }
    }
    ~CaptureDevice()
    {
        if (IsAvailable == true)
        {
            static_cast<void>(rhi::DestroyDevice(Device));
        }
        rhi::Shutdown();
    }
    CaptureDevice(const CaptureDevice&) = delete;
    CaptureDevice& operator=(const CaptureDevice&) = delete;
};

/// Capture the World's paired raw reference image with one traversal. Optionally save its LUHDR01 file.
Array<math::Vector3> CaptureReference(const CaptureDevice& device,
                                      const RenderScene& scene,
                                      const renderer::ViewDescription& view,
                                      renderer::ReferenceTraversal traversal,
                                      const QString& savePath = {})
{
    ViewportReferenceSettings settings;
    settings.LongSide = CAPTURE_WIDTH;
    settings.Samples = CAPTURE_SAMPLES;
    // One scattering event: the direct response of each board.
    settings.MaxDepth = 1;
    settings.Traversal = traversal;
    settings.Seed = CAPTURE_SEED;
    settings.ShouldCenterRays = true;
    const QSize extent(static_cast<int32>(CAPTURE_WIDTH), static_cast<int32>(CAPTURE_HEIGHT));
    ViewportReferenceCapture capture;
    QString error;
    const bool isStarted = capture.Start(device.Device, device.Surface, scene, view, extent, settings, error);
    INFO(error.toStdString());
    REQUIRE(isStarted == true);
    QElapsedTimer timer;
    timer.start();
    ViewportReferencePhase phase = capture.GetPhase();
    while ((phase == ViewportReferencePhase::Preparing || phase == ViewportReferencePhase::Tracing) &&
           timer.elapsed() < CAPTURE_WAIT_MILLISECONDS)
    {
        QThread::msleep(1);
        phase = capture.Service(SERVICE_BUDGET_NANOSECONDS);
    }
    INFO(capture.GetError().toStdString());
    REQUIRE(phase == ViewportReferencePhase::Complete);
    // A World without an Atmosphere or environment adds nothing beyond the two lights.
    CHECK(capture.HasAtmosphereSky() == false);
    renderer::ReferenceCaptureInfo info;
    REQUIRE(capture.GetInfo(info) == true);
    CHECK(info.InvalidContributions == 0);
    CHECK(info.MinimumSamples == CAPTURE_SAMPLES);
    Array<renderer::ReferencePixel> pixels;
    REQUIRE(capture.CopyPixels(pixels, error) == true);
    REQUIRE(pixels.GetSize() == usize{CAPTURE_WIDTH} * CAPTURE_HEIGHT);
    if (savePath.isEmpty() == false)
    {
        REQUIRE(capture.Save(savePath, error) == true);
    }
    capture.Release();
    Array<math::Vector3> radiance;
    REQUIRE(radiance.TryResize(pixels.GetSize()) == true);
    for (usize index = 0; index < pixels.GetSize(); ++index)
    {
        radiance[index] = pixels[index].Radiance;
    }
    return radiance;
}

/// Capture the World through the compute ray-tracing template, the general graph, as the Editor's World graph
/// capture does. Optionally save its LURGCAP file.
Array<math::Vector3> CaptureGraph(const CaptureDevice& device,
                                  const RenderScene& scene,
                                  const renderer::ViewDescription& view,
                                  const QString& savePath = {})
{
    ViewportRasterSource source;
    source.PackagePath = QStringLiteral(LUDUS_EDITOR_COMPUTE_RAY_GRAPH_FIXTURE);
    ViewportGraphCapture capture;
    QSize extent;
    QString error;
    const bool isLoaded = capture.Load(device.Device,
                                       device.Surface,
                                       source,
                                       {},
                                       QSize(static_cast<int32>(CAPTURE_WIDTH), static_cast<int32>(CAPTURE_HEIGHT)),
                                       CAPTURE_SAMPLES,
                                       extent,
                                       error);
    INFO(error.toStdString());
    REQUIRE(isLoaded == true);
    REQUIRE(extent == QSize(static_cast<int32>(CAPTURE_WIDTH), static_cast<int32>(CAPTURE_HEIGHT)));
    REQUIRE(capture.Begin(scene, view, error) == true);
    QElapsedTimer timer;
    timer.start();
    while (capture.IsRunning() == true && timer.elapsed() < CAPTURE_WAIT_MILLISECONDS)
    {
        static_cast<void>(capture.Service());
    }
    INFO(capture.GetError().toStdString());
    REQUIRE(capture.GetPhase() == renderer::RenderGraphCapturePhase::Complete);
    CHECK(capture.GetInfo().MinimumSamples == CAPTURE_SAMPLES);
    const BorrowedView<const renderer::RenderGraphCapturePixel> pixels = capture.GetPixels();
    REQUIRE(pixels.GetSize() == usize{CAPTURE_WIDTH} * CAPTURE_HEIGHT);
    if (savePath.isEmpty() == false)
    {
        REQUIRE(capture.Save(savePath, error) == true);
    }
    Array<math::Vector3> radiance;
    REQUIRE(radiance.TryResize(pixels.GetSize()) == true);
    for (usize index = 0; index < pixels.GetSize(); ++index)
    {
        radiance[index] = pixels[index].Mean;
    }
    capture.Release();
    return radiance;
}

/// Region means of one capture.
struct RegionMeans final
{
    math::Vector3 Control{};
    math::Vector3 Project{};
};

RegionMeans MeasureRegions(const Array<math::Vector3>& pixels, const Array<uint8>& labels)
{
    return {MeasureRegion(pixels, labels, LABEL_CONTROL), MeasureRegion(pixels, labels, LABEL_PROJECT)};
}

QJsonArray ToJson(math::Vector3 value)
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

QJsonObject ToJson(const RegionMeans& means)
{
    return {{QStringLiteral("control"), ToJson(means.Control)}, {QStringLiteral("project"), ToJson(means.Project)}};
}

/// The evidence of one light set: region means of the analytic response and of each Editor capture.
struct LightSetEvidence final
{
    RegionMeans Analytic;
    RegionMeans Cpu;
    RegionMeans Gpu;
    RegionMeans Graph;
};

/// Paths the hidden SDK export writes; empty runs the checks only.
struct ExportPaths final
{
    QString Directory;
};

/// Capture the saved World's paired raw CPU and GPU references and its general-graph image under every light set,
/// and check each against the analytic direct response and against each other.
StaticArray<LightSetEvidence, 3> CaptureSavedWorld(const CaptureDevice& device,
                                                   const RenderScene& saved,
                                                   const ExportPaths& paths,
                                                   AnalyticImage& outAnalytic,
                                                   renderer::ViewDescription& outGraphView)
{
    renderer::ViewDescription referenceView;
    QString error;
    REQUIRE(saved.DescribeSavedView(CAPTURE_WIDTH, CAPTURE_HEIGHT, false, referenceView, error) == true);
    REQUIRE(saved.DescribeSavedView(CAPTURE_WIDTH, CAPTURE_HEIGHT, true, outGraphView, error) == true);
    outAnalytic = MakeAnalyticImage(saved, referenceView);
    // The lobe's directional response differs measurably from the Lambertian control under each light.
    const RegionMeans firstAnalytic = MeasureRegions(outAnalytic.Expected[0], outAnalytic.Labels);
    const RegionMeans secondAnalytic = MeasureRegions(outAnalytic.Expected[1], outAnalytic.Labels);
    CHECK(math::Abs(firstAnalytic.Project.X - firstAnalytic.Control.X) > 0.05F * firstAnalytic.Control.X);
    CHECK(math::Abs(secondAnalytic.Project.Z - secondAnalytic.Control.Z) > 0.05F * secondAnalytic.Control.Z);
    StaticArray<LightSetEvidence, 3> evidence{};
    for (usize setIndex = 0; setIndex < LIGHT_SETS.GetSize(); ++setIndex)
    {
        const LightSet set = LIGHT_SETS[setIndex];
        const char* name = GetLightSetName(set);
        CAPTURE(name);
        const RenderScene presented = SwitchLights(saved, set);
        const QString stem = paths.Directory.isEmpty() == true
                                 ? QString{}
                                 : QDir(paths.Directory).filePath(QStringLiteral("editor-%1").arg(QLatin1String(name)));
        const Array<math::Vector3> cpu =
            CaptureReference(device,
                             presented,
                             referenceView,
                             renderer::ReferenceTraversal::CpuOracle,
                             stem.isEmpty() == true ? QString{} : stem + QStringLiteral("-cpu.luhdr"));
        const Array<math::Vector3> gpu =
            CaptureReference(device,
                             presented,
                             referenceView,
                             renderer::ReferenceTraversal::GpuCompute,
                             stem.isEmpty() == true ? QString{} : stem + QStringLiteral("-gpu.luhdr"));
        const Array<math::Vector3> graph =
            CaptureGraph(device,
                         presented,
                         outGraphView,
                         stem.isEmpty() == true ? QString{} : stem + QStringLiteral("-graph.lurgcap"));
        const Array<math::Vector3>& expected = outAnalytic.Expected[setIndex];
        LightSetEvidence& record = evidence[setIndex];
        record.Analytic = MeasureRegions(expected, outAnalytic.Labels);
        record.Cpu = MeasureRegions(cpu, outAnalytic.Labels);
        record.Gpu = MeasureRegions(gpu, outAnalytic.Labels);
        record.Graph = MeasureRegions(graph, outAnalytic.Labels);

        // Paired raw linear CPU and GPU references over the whole image, under the typed-material parity bound.
        float64 squared = 0;
        for (usize index = 0; index < cpu.GetSize(); ++index)
        {
            const float32 difference = math::Distance(cpu[index], gpu[index]);
            squared += static_cast<float64>(difference * difference);
        }
        const float32 rms = static_cast<float32>(math::Sqrt(squared / static_cast<float64>(cpu.GetSize())));
        CAPTURE(rms);
        CHECK(rms <= PARITY_RMS_TOLERANCE);
        // A one-light reference pixel is a single deterministic estimate: it equals the analytic response.
        if (set != LightSet::Both)
        {
            usize mismatches = 0;
            for (usize index = 0; index < cpu.GetSize(); ++index)
            {
                if (outAnalytic.Labels[index] != LABEL_NONE &&
                    IsNearRelative(cpu[index], expected[index], EXACT_TOLERANCE) == false)
                {
                    ++mismatches;
                }
            }
            CHECK(mismatches == 0);
        }
        // Every region mean of every capture equals the analytic mean: the linear sum for both lights, the
        // project lobe's directional response on its board and the Lambertian control on the other.
        for (const RegionMeans* actual : {&record.Cpu, &record.Gpu, &record.Graph})
        {
            CAPTURE(actual->Control.X, actual->Control.Y, actual->Control.Z);
            CAPTURE(actual->Project.X, actual->Project.Y, actual->Project.Z);
            CAPTURE(record.Analytic.Project.X, record.Analytic.Project.Y, record.Analytic.Project.Z);
            CHECK(IsNearRelative(actual->Control, record.Analytic.Control, MEAN_TOLERANCE) == true);
            CHECK(IsNearRelative(actual->Project, record.Analytic.Project, MEAN_TOLERANCE) == true);
        }
    }
    return evidence;
}

/// Open the saved World in a fresh workspace, as a second session does, and return its presented scene.
RenderScene ReopenSavedWorld(const QDir& root, const QString& worldPath)
{
    WorldWorkspace reopened;
    reopened.SetProject(root.path(), 2);
    REQUIRE(reopened.Open(worldPath) == true);
    WaitForValidatedScene(reopened);
    RenderScene saved = GetLevel(reopened).PreviewScene();
    CheckAssignedMaterials(saved);
    // The saved slot keeps its project_brdf binding to the provider's material.
    const QString project = FindObject(saved, QStringLiteral("Project board"));
    const QJsonObject binding = reopened.Document()
                                    .ScenePlacement(reopened.Document().Entity(project))
                                    .value(QStringLiteral("project_brdf"))
                                    .toObject();
    CHECK(binding.value(QStringLiteral("material")).toString().startsWith(QStringLiteral("material/")) == true);
    reopened.StopPreview();
    return saved;
}

/// Author the provider World in `root`, save it and return its path.
QString AuthorProviderWorld(const QDir& root)
{
    const QString boards = CopyMaterialScene(root, QStringLiteral("oren-nayar-two-light-boards.luscene.json"));
    const QString provider = CopyMaterialScene(root, QStringLiteral("oren-nayar-provider.luscene.json"));
    const QString worldPath = AuthorTwoLightWorld(root);
    WorldWorkspace world;
    world.SetProject(root.path(), 1);
    REQUIRE(world.Open(worldPath) == true);
    ImportAndAssignProvider(world, boards, provider);
    CheckAssignedMaterials(GetLevel(world).PreviewScene());
    REQUIRE(world.Save() == true);
    WaitForValidatedScene(world);
    CHECK(world.Dirty() == false);
    world.StopPreview();
    return worldPath;
}

bool CanCaptureGraph(const CaptureDevice& device)
{
    renderer::RenderGraphRenderer automatic;
    return renderer::SelectRenderGraphRenderer(device.Device, renderer::RenderGraphPreference::Automatic, automatic) ==
               rhi::RasterStatus::Ready &&
           automatic != renderer::RenderGraphRenderer::Raster;
}

/// Write the World's capture view for the standalone app: "LUVIEW01", then little-endian uint32 depth convention
/// and orthographic flag, the sixteen float32 of WorldToClip column by column, and the camera position.
QByteArray EncodeView(const renderer::ViewDescription& view)
{
    QByteArray bytes("LUVIEW01", 8);
    const auto appendWord = [&bytes](uint32 word) {
        for (uint32 shift = 0; shift < 32; shift += 8)
        {
            bytes.append(static_cast<char>((word >> shift) & 0xFFU));
        }
    };
    const auto appendFloat = [&](float32 value) {
        uint32 word = 0;
        std::memcpy(&word, &value, sizeof(word));
        appendWord(word);
    };
    appendWord(static_cast<uint32>(view.Depth));
    appendWord(view.OrthographicCamera == true ? 1U : 0U);
    for (const math::Vector4& column : view.WorldToClip.Columns)
    {
        appendFloat(column.X);
        appendFloat(column.Y);
        appendFloat(column.Z);
        appendFloat(column.W);
    }
    appendFloat(view.CameraPosition.X);
    appendFloat(view.CameraPosition.Y);
    appendFloat(view.CameraPosition.Z);
    return bytes;
}
} // namespace

TEST_CASE("World registers a project Oren-Nayar provider and renders it beside a built-in control under two lights",
          "[editor][world][project-provider][gpu][reference-compute]")
{
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QDir root(QFileInfo(project.path()).canonicalFilePath());
    const QString worldPath = AuthorProviderWorld(root);
    const RenderScene saved = ReopenSavedWorld(root, worldPath);

    CaptureDevice device;
    if (device.IsAvailable == false)
    {
        SKIP("No GPU adapter on this host");
    }
    if (CanCaptureGraph(device) == false)
    {
        SKIP("The compute ray-tracing template needs GPU compute");
    }
    AnalyticImage analytic;
    renderer::ViewDescription graphView;
    const StaticArray<LightSetEvidence, 3> evidence = CaptureSavedWorld(device, saved, {}, analytic, graphView);
    // Both lights together are the linear sum of each alone, for every capture.
    for (const RegionMeans LightSetEvidence::* capture :
         {&LightSetEvidence::Cpu, &LightSetEvidence::Gpu, &LightSetEvidence::Graph})
    {
        const RegionMeans& first = evidence[0].*capture;
        const RegionMeans& second = evidence[1].*capture;
        const RegionMeans& both = evidence[2].*capture;
        CHECK(IsNearRelative(both.Control, first.Control + second.Control, MEAN_TOLERANCE) == true);
        CHECK(IsNearRelative(both.Project, first.Project + second.Project, MEAN_TOLERANCE) == true);
    }
}

TEST_CASE("World names an unavailable project material provider and keeps the last drawable World",
          "[editor][world][project-provider][gpu]")
{
    rhi::Shutdown();
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QDir root(QFileInfo(project.path()).canonicalFilePath());
    const QString boards = CopyMaterialScene(root, QStringLiteral("oren-nayar-two-light-boards.luscene.json"));
    const QString provider = CopyMaterialScene(root, QStringLiteral("oren-nayar-provider.luscene.json"));
    const QString worldPath = AuthorTwoLightWorld(root);
    RendererWorkspace workspace({});
    WorldWorkspace world;
    world.UseProjectGraph(workspace);
    workspace.SetPreviewSource(&world.GetRenderPreview());
    QObject::connect(&world, &WorldWorkspace::RenderPreviewChanged, &workspace, [&]() {
        workspace.SetPreviewSource(&world.GetRenderPreview());
    });
    workspace.EnableWorldRendering();
    workspace.SetProject(root.path(), 1);
    world.SetProject(root.path(), 1);
    world.resize(1000, 700);
    world.show();
    REQUIRE(world.Open(worldPath) == true);
    ImportAndAssignProvider(world, boards, provider, &workspace);
    world.ResumePreview();
    QWidget* viewportOrNull = world.findChild<QWidget*>(QStringLiteral("worldViewport"));
    REQUIRE(viewportOrNull != nullptr);
    WorldGraphView* graphViewOrNull = viewportOrNull->findChild<WorldGraphView*>();
    REQUIRE(graphViewOrNull != nullptr);
    RenderGraphPreview* previewOrNull = workspace.GetPreviewOrNull();
    REQUIRE(previewOrNull != nullptr);
    const auto waitFor = [&](const auto& isReady) {
        QElapsedTimer timer;
        timer.start();
        while (isReady() == false && timer.elapsed() < SCENE_WAIT_MILLISECONDS)
        {
            QTest::qWait(1);
            workspace.ServicePreview();
        }
        return isReady();
    };
    REQUIRE(waitFor([&]() {
                return graphViewOrNull->GetImage().isNull() == false && previewOrNull->HasPendingWork() == false;
            }) == true);
    const QImage drawn = graphViewOrNull->GetImage();
    const uint64 drawnRevision = previewOrNull->GetPresentedRevision();
    const LevelWorkspace& level = GetLevel(world);
    const QByteArray drawnIdentity = level.GetPresentedScene().Identity;
    CheckAssignedMaterials(level.GetPresentedScene());

    // The provider becomes unavailable: its material's record leaves the project catalog, as when the provider's
    // cooked assets were deleted or not checked in.
    const QString catalogPath = root.filePath(QStringLiteral("assets/catalog.json"));
    QJsonObject catalog = QJsonDocument::fromJson(ReadFile(catalogPath)).object();
    const QString control = FindObject(level.GetPresentedScene(), QStringLiteral("Control board"));
    const QString projectBoard = FindObject(level.GetPresentedScene(), QStringLiteral("Project board"));
    const QString providerId =
        world.Document().GetMaterialResourceId(world.Document().ScenePlacement(world.Document().Entity(projectBoard)));
    REQUIRE(providerId.startsWith(QStringLiteral("material/")) == true);
    QJsonObject catalogResources = catalog.value(QStringLiteral("resources")).toObject();
    REQUIRE(catalogResources.contains(providerId) == true);
    catalogResources.remove(providerId);
    catalog.insert(QStringLiteral("resources"), catalogResources);
    WriteFile(catalogPath, QJsonDocument(catalog).toJson(QJsonDocument::Compact));

    // A replacement World assigns the provider to the Control board too. Its preparation is rejected.
    REQUIRE(world.SelectEntity(control) == true);
    QComboBox* choicesOrNull = world.findChild<QComboBox*>(QStringLiteral("worldProjectBrdfChoice"));
    REQUIRE(choicesOrNull != nullptr);
    const int32 choice = choicesOrNull->findText(QStringLiteral("project/oren-nayar"), Qt::MatchStartsWith);
    REQUIRE(choice >= 0);
    choicesOrNull->setCurrentIndex(choice);
    QPushButton* assignOrNull = world.findChild<QPushButton*>(QStringLiteral("worldAssignProjectBrdf"));
    REQUIRE(assignOrNull != nullptr);
    assignOrNull->click();
    WaitForSettledScene(world, &workspace);
    REQUIRE(waitFor([&]() { return previewOrNull->HasPendingWork() == false; }) == true);
    // The diagnostic names the project material, by its project name and resource ID, and the slot, and offers
    // concrete repairs.
    const QString status = GetStatusText(world);
    INFO(status.toStdString());
    CHECK(status.contains(QStringLiteral("The project material 'project/oren-nayar' (") + providerId +
                          QStringLiteral(")")) == true);
    CHECK(status.contains(QStringLiteral("is not in the project catalog")) == true);
    CHECK(status.contains(QStringLiteral("Import its provider scene and Assign Project BRDF again")) == true);
    CHECK(status.contains(QStringLiteral("Use Inherited Material")) == true);
    // The rejected replacement keeps the last drawable World: its scene, image and presented revision.
    CHECK(level.GetPresentedScene().Identity == drawnIdentity);
    CheckAssignedMaterials(level.GetPresentedScene());
    CHECK(previewOrNull->GetPresentedRevision() == drawnRevision);
    CHECK(graphViewOrNull->GetImage() == drawn);

    // Repair as the diagnostic says: import the provider scene again. Its material keeps its resource ID, so the
    // replacement World, with both boards bound to the provider, now prepares and replaces the drawn one.
    REQUIRE(world.ImportScene(provider) == true);
    WaitForValidatedScene(world, &workspace);
    REQUIRE(waitFor([&]() {
                return previewOrNull->GetPresentedRevision() > drawnRevision &&
                       previewOrNull->HasPendingWork() == false;
            }) == true);
    INFO(GetStatusText(world).toStdString());
    CHECK(GetStatusText(world).contains(QStringLiteral("is not in the project catalog")) == false);
    CHECK(level.GetPresentedScene().Identity != drawnIdentity);
    for (const QString& board : {control, projectBoard})
    {
        const resources::SceneMaterial* materialOrNull = GetSceneObjectMaterialOrNull(level.GetPresentedScene(), board);
        REQUIRE(materialOrNull != nullptr);
        CHECK(materialOrNull->Scattering.Model == format::MaterialModel::Program);
    }
    world.StopPreview();
    workspace.SetPreviewSource(nullptr);
    rhi::Shutdown();
}

// Same-World source of truth for the standalone SDK qualification of #1310. It saves the provider World, exports
// its cooked scene.luscn and manifest, the capture view, the region labels and the Editor's paired CPU/GPU reference
// and general-graph captures under each light set, with their region means and the analytic means.
// tests/sdk_consumer/project_brdf_world/verify.py sets LUDUS_PROJECT_BRDF_EXPORT and compares the standalone app.
TEST_CASE("The project Oren-Nayar provider World exports its cooked scene and matched captures",
          "[.sdk-project-brdf-export]")
{
    const QString directory = qEnvironmentVariable("LUDUS_PROJECT_BRDF_EXPORT");
    REQUIRE(directory.isEmpty() == false);
    REQUIRE(QDir().mkpath(directory) == true);
    QTemporaryDir project;
    REQUIRE(project.isValid() == true);
    const QDir root(QFileInfo(project.path()).canonicalFilePath());
    const QString worldPath = AuthorProviderWorld(root);
    const RenderScene saved = ReopenSavedWorld(root, worldPath);
    REQUIRE(QFile::copy(worldPath, QDir(directory).filePath(QStringLiteral("boards.world.json"))) == true);

    WorldDocument document;
    {
        WorldWorkspace reopened;
        reopened.SetProject(root.path(), 3);
        REQUIRE(reopened.Open(worldPath) == true);
        WaitForValidatedScene(reopened);
        QString exportError;
        const bool isExported = ExportCookedWorldScene(root.path(),
                                                       reopened.Document(),
                                                       QDir(directory).filePath(QStringLiteral("scene.luscn")),
                                                       QDir(directory).filePath(QStringLiteral("scene.manifest.json")),
                                                       exportError);
        INFO(exportError.toStdString());
        REQUIRE(isExported == true);
        reopened.StopPreview();
    }

    CaptureDevice device;
    REQUIRE(device.IsAvailable == true);
    REQUIRE(CanCaptureGraph(device) == true);
    AnalyticImage analytic;
    renderer::ViewDescription graphView;
    const StaticArray<LightSetEvidence, 3> evidence =
        CaptureSavedWorld(device, saved, {directory}, analytic, graphView);
    WriteFile(QDir(directory).filePath(QStringLiteral("view.bin")), EncodeView(graphView));
    QByteArray labels(static_cast<qsizetype>(analytic.Labels.GetSize()), '\0');
    for (usize index = 0; index < analytic.Labels.GetSize(); ++index)
    {
        labels[static_cast<qsizetype>(index)] = static_cast<char>(analytic.Labels[index]);
    }
    WriteFile(QDir(directory).filePath(QStringLiteral("labels.bin")), labels);

    // The cooked light table order the standalone app switches by.
    const WorldLights lights = GetWorldLights(saved);
    REQUIRE(saved.Definition != nullptr);
    QJsonObject lightIndices;
    for (usize index = 0; index < saved.Definition->Lights.GetSize(); ++index)
    {
        const uint64 sourceId = saved.Definition->Lights[index].SourceId;
        if (sourceId == lights.First.SourceId)
        {
            lightIndices.insert(QStringLiteral("first"), static_cast<int32>(index));
        }
        else if (sourceId == lights.Second.SourceId)
        {
            lightIndices.insert(QStringLiteral("second"), static_cast<int32>(index));
        }
    }
    REQUIRE(lightIndices.size() == 2);
    QJsonObject sets;
    for (usize setIndex = 0; setIndex < LIGHT_SETS.GetSize(); ++setIndex)
    {
        const LightSetEvidence& record = evidence[setIndex];
        sets.insert(QLatin1String(GetLightSetName(LIGHT_SETS[setIndex])),
                    QJsonObject{{QStringLiteral("analytic"), ToJson(record.Analytic)},
                                {QStringLiteral("editorCpu"), ToJson(record.Cpu)},
                                {QStringLiteral("editorGpu"), ToJson(record.Gpu)},
                                {QStringLiteral("editorGraph"), ToJson(record.Graph)}});
    }
    const QJsonObject expectations{{QStringLiteral("version"), 1},
                                   {QStringLiteral("issue"), 1310},
                                   {QStringLiteral("width"), static_cast<int32>(CAPTURE_WIDTH)},
                                   {QStringLiteral("height"), static_cast<int32>(CAPTURE_HEIGHT)},
                                   {QStringLiteral("samples"), static_cast<int32>(CAPTURE_SAMPLES)},
                                   {QStringLiteral("meanTolerance"), static_cast<float64>(MEAN_TOLERANCE)},
                                   {QStringLiteral("sigmaRadians"), static_cast<float64>(PROVIDER_SIGMA_RADIANS)},
                                   {QStringLiteral("lightIndices"), lightIndices},
                                   {QStringLiteral("sets"), sets}};
    WriteFile(QDir(directory).filePath(QStringLiteral("expectations.json")),
              QJsonDocument(expectations).toJson(QJsonDocument::Indented));
}
