#include <ludus/graphics/renderer/static_lighting.hpp>

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/runtime/resources/scene.h>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace
{
using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace resources = ludus::resources;
namespace math = ludus::foundation::math;
constexpr usize SYNTHETIC_EMITTER_COUNT = 13;
constexpr usize TRIANGLE_CORNERS = 3;
constexpr usize PATCH_VERTICES = 4;
constexpr usize PATCH_INDICES = 6;
resources::SceneDefinition MakeBakeScene(bool hasAreaEmitters, bool hasBlocker, bool hasPointLight)
{
    resources::SceneDefinition scene;
    REQUIRE(scene.Textures.TryResize(1) == true);
    scene.Textures[0].Width = scene.Textures[0].Height = 1;
    REQUIRE(scene.Textures[0].Pixels.TryResize(4, 255) == true);
    REQUIRE(scene.Materials.TryResize(2) == true);
    scene.Materials[0].Texture = 0;
    scene.Materials[0].Roughness = 1;
    scene.Materials[1].Emission = hasAreaEmitters == true ? math::Vector3{10, 20, 30} : math::Vector3{};
    scene.Materials[1].AreaSourceId = 20;
    scene.Materials[1].IsEmissionTwoSided = false;
    REQUIRE(scene.Meshes.TryResize(2) == true);
    for (resources::SceneMesh& mesh : scene.Meshes)
    {
        REQUIRE(mesh.Positions.TryResize(TRIANGLE_CORNERS) == true);
        REQUIRE(mesh.Normals.TryResize(TRIANGLE_CORNERS) == true);
        REQUIRE(mesh.Texcoords.TryResize(TRIANGLE_CORNERS) == true);
        REQUIRE(mesh.Indices.TryResize(TRIANGLE_CORNERS) == true);
        for (usize i = 0; i < TRIANGLE_CORNERS; ++i)
        {
            mesh.Indices[i] = static_cast<uint32>(i);
        }
    }
    scene.Meshes[0].Positions[0] = {-1, -1, 0};
    scene.Meshes[0].Positions[1] = {1, -1, 0};
    scene.Meshes[0].Positions[2] = {0, 2, 0};
    scene.Meshes[1].Positions[0] = {-.3F, -.3F, 0};
    scene.Meshes[1].Positions[1] = {0, .3F, 0};
    scene.Meshes[1].Positions[2] = {.3F, -.3F, 0};
    for (usize i = 0; i < TRIANGLE_CORNERS; ++i)
    {
        scene.Meshes[0].Normals[i] = {0, 0, 1};
        scene.Meshes[1].Normals[i] = {0, 0, -1};
    }
    const usize objectCount =
        1 + (hasAreaEmitters == true ? SYNTHETIC_EMITTER_COUNT : 0) + (hasBlocker == true ? 1 : 0);
    REQUIRE(scene.Objects.TryResize(objectCount) == true);
    scene.Objects[0].SourceId = 1;
    for (usize i = 1; i <= (hasAreaEmitters == true ? SYNTHETIC_EMITTER_COUNT : 0); ++i)
    {
        scene.Objects[i].SourceId = i + 1;
        scene.Objects[i].Mesh = 1;
        scene.Objects[i].Material = 1;
        scene.Objects[i].Transform.Translation = {float32(i) * .1F - .7F, 0, 2};
    }
    if (hasBlocker == true)
    {
        resources::SceneObject& blocker = scene.Objects[objectCount - 1];
        blocker.SourceId = 100;
        blocker.Mesh = 1;
        blocker.Material = hasPointLight == true ? 0 : 1;
        blocker.Transform.Columns[0].X = 1000;
        blocker.Transform.Columns[1].Y = 1000;
        blocker.Transform.Translation.Z = 1;
    }
    if (hasPointLight == true)
    {
        REQUIRE(scene.Lights.TryResize(1) == true);
        scene.Lights[0].SourceId = 200;
        scene.Lights[0].Kind = resources::SceneLightKind::Point;
        scene.Lights[0].IntensityRgb = {50, 0, 0};
        scene.Lights[0].PositionWorld = {0, 0, .5F};
        scene.Lights[0].Range = 10;
    }
    return scene;
}
rr::StaticLightingBakeDescription MakeBakeDescription(uint32 samples = 256)
{
    rr::StaticLightingBakeDescription description;
    description.SceneRevision = 7;
    description.DependencyRevision = 11;
    description.SamplesPerTriangle = samples;
    description.MaxDepth = 2;
    return description;
}
rr::StaticLightingChart CompleteBake(rr::StaticLightingBake& inoutBake, uint32 step)
{
    rhi::RasterStatus status = rhi::RasterStatus::Pending;
    while (status == rhi::RasterStatus::Pending)
    {
        status = inoutBake.Step(step);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
    rr::StaticLightingChart chart;
    REQUIRE(inoutBake.GetChart(0, chart) == rhi::RasterStatus::Ready);
    return chart;
}
math::Vector3 ReadChart(const rr::StaticLightingChart& chart)
{
    return {float32(chart.Pixels[0]) / 255 * chart.Scale.X,
            float32(chart.Pixels[1]) / 255 * chart.Scale.Y,
            float32(chart.Pixels[2]) / 255 * chart.Scale.Z};
}
namespace threading = ludus::foundation::threading;
constexpr uint32 ORACLE_GRID_CELLS = 6;  // 72 receiver triangles plus emitters and a blocker.
constexpr uint32 CANCEL_GRID_CELLS = 32; // 2048 receiver triangles; long enough to cancel mid-bake.
constexpr uint32 CANCEL_SAMPLES = 256;   // Uncancelled, this bake takes seconds.
constexpr uint32 QUAD_TRIANGLES = 2;
// The point-lit receiver of MakeBakeScene, retessellated as a cells x cells grid so
// ranges split real work. Emitters and the blocker keep their single triangles.
resources::SceneDefinition MakeGridBakeScene(uint32 cells)
{
    resources::SceneDefinition scene = MakeBakeScene(true, true, true);
    resources::SceneMesh& grid = scene.Meshes[0];
    const usize vertexCount = usize(cells + 1) * (cells + 1);
    REQUIRE(grid.Positions.TryResize(vertexCount) == true);
    REQUIRE(grid.Normals.TryResize(vertexCount) == true);
    REQUIRE(grid.Texcoords.TryResize(vertexCount) == true);
    REQUIRE(grid.Indices.TryResize(usize(cells) * cells * QUAD_TRIANGLES * TRIANGLE_CORNERS) == true);
    for (uint32 y = 0; y <= cells; ++y)
    {
        for (uint32 x = 0; x <= cells; ++x)
        {
            const usize vertex = usize(y) * (cells + 1) + x;
            const float32 u = float32(x) / float32(cells);
            const float32 v = float32(y) / float32(cells);
            grid.Positions[vertex] = {u * 4 - 2, v * 4 - 2, 0};
            grid.Normals[vertex] = {0, 0, 1};
            grid.Texcoords[vertex] = {u, v};
        }
    }
    usize index = 0;
    for (uint32 y = 0; y < cells; ++y)
    {
        for (uint32 x = 0; x < cells; ++x)
        {
            const uint32 corner = y * (cells + 1) + x;
            for (const uint32 vertex :
                 {corner, corner + 1, corner + cells + 2, corner, corner + cells + 2, corner + cells + 1})
            {
                grid.Indices[index++] = vertex;
            }
        }
    }
    // A small blocker above part of the grid so neighbouring triangles differ.
    resources::SceneObject& blocker = scene.Objects[scene.Objects.GetSize() - 1];
    blocker.Transform.Columns[0].X = 2;
    blocker.Transform.Columns[1].Y = 2;
    blocker.Transform.Translation = {.5F, .5F, .25F};
    return scene;
}
void RequireIdenticalCharts(const rr::StaticLightingBake& oracle,
                            const rr::StaticLightingBake& candidate,
                            usize objectCount)
{
    for (usize i = 0; i < objectCount; ++i)
    {
        rr::StaticLightingChart expected;
        rr::StaticLightingChart actual;
        REQUIRE(oracle.GetChart(i, expected) == rhi::RasterStatus::Ready);
        REQUIRE(candidate.GetChart(i, actual) == rhi::RasterStatus::Ready);
        REQUIRE(actual.Width == expected.Width);
        REQUIRE(actual.Height == expected.Height);
        REQUIRE(actual.SourceId == expected.SourceId);
        REQUIRE(actual.SceneRevision == expected.SceneRevision);
        REQUIRE(actual.DependencyRevision == expected.DependencyRevision);
        REQUIRE(actual.Transform == expected.Transform);
        // Scales are finite and at least 1e-6, so equality is exact bit equality.
        REQUIRE(actual.Scale == expected.Scale);
        REQUIRE(actual.Pixels.GetSize() == expected.Pixels.GetSize());
        REQUIRE(std::memcmp(actual.Pixels.GetDataOrNull(), expected.Pixels.GetDataOrNull(), actual.Pixels.GetSize()) ==
                0);
    }
}
// Polls until the first range has published progress, or the bake resolves.
rhi::RasterStatus WaitForProgress(rr::StaticLightingBake& inoutBake)
{
    rhi::RasterStatus status = rhi::RasterStatus::Pending;
    rr::StaticLightingBakeProgress progress;
    while (status == rhi::RasterStatus::Pending && progress.CompletedSamples == 0)
    {
        inoutBake.GetProgress(progress);
        status = inoutBake.Poll();
    }
    return status;
}
} // namespace
TEST_CASE("Always-on bake isolates selected analytic light and respects occlusion", "[static-lighting]")
{
    resources::SceneDefinition scene = MakeBakeScene(false, true, true);
    constexpr uint64 SELECTED_LIGHT = 200;
    constexpr uint64 REALTIME_LIGHT = 201;
    const uint64 selected = SELECTED_LIGHT;
    rr::StaticLightingBakeDescription description = MakeBakeDescription(4);
    description.AnalyticLightIds = {&selected, 1};
    REQUIRE(scene.Lights.TryResize(2) == true);
    scene.Lights[0].PositionWorld.Z = 2;
    scene.Lights[1] = scene.Lights[0];
    scene.Lights[1].SourceId = REALTIME_LIGHT;
    scene.Lights[1].IntensityRgb = {0, 100, 0};
    scene.Environment.Rgb = {0, 0, 2};

    rr::StaticLightingBake blocked;
    REQUIRE(blocked.Initialize(scene, description) == rhi::RasterStatus::Pending);
    CHECK(ReadChart(CompleteBake(blocked, 1)) == math::Vector3{});

    scene.Objects[1].Transform.Translation.X = 1000;
    rr::StaticLightingBake clear;
    REQUIRE(clear.Initialize(scene, description) == rhi::RasterStatus::Pending);
    const math::Vector3 irradiance = ReadChart(CompleteBake(clear, 1));
    CHECK(irradiance.X > 12);
    CHECK(irradiance.Y == 0);
    CHECK(irradiance.Z == 0);

    threading::JobSystem jobs;
    REQUIRE(jobs.Initialize({ .WorkerCount = 2 }) == threading::JobStatus::Ok);
    rr::StaticLightingBake scheduled;
    REQUIRE(scheduled.Submit(jobs, scene, description) == rhi::RasterStatus::Pending);
    REQUIRE(scheduled.Finish() == rhi::RasterStatus::Ready);
    RequireIdenticalCharts(clear, scheduled, scene.Objects.GetSize());
}

TEST_CASE("Static area bake lights all thirteen emitters, respects blockers and preserves pending charts",
          "[renderer][static-lighting]")
{
    resources::SceneDefinition scene = MakeBakeScene(true, false, false);
    rr::StaticLightingBake bake;
    REQUIRE(bake.Initialize(scene, MakeBakeDescription()) == rhi::RasterStatus::Pending);
    rr::StaticLightingChart pending;
    pending.SourceId = 999;
    REQUIRE(bake.GetChart(0, pending) == rhi::RasterStatus::NotReady);
    REQUIRE(pending.SourceId == 999);
    const rr::StaticLightingChart chart = CompleteBake(bake, 7);
    const math::Vector3 lit = ReadChart(chart);
    REQUIRE(lit.X > 1);
    REQUIRE(lit.Z > lit.X * 2.5F);
    REQUIRE(chart.SourceId == 1);
    REQUIRE(chart.SceneRevision == 7);
    REQUIRE(chart.DependencyRevision == 11);
    REQUIRE(chart.Width == 4);
    REQUIRE(chart.Height == 4);
    for (usize i = 0; i < chart.Pixels.GetSize(); i += 4)
    {
        REQUIRE(chart.Pixels[i] == chart.Pixels[0]);
        REQUIRE(chart.Pixels[i + 3] == 255);
    }
    rr::StaticLightingChart emitter;
    REQUIRE(bake.GetChart(SYNTHETIC_EMITTER_COUNT, emitter) == rhi::RasterStatus::Ready);
    REQUIRE(emitter.SourceId == SYNTHETIC_EMITTER_COUNT + 1);
    resources::SceneDefinition blockedScene = MakeBakeScene(true, true, false);
    blockedScene.Materials[1].IsEmissionTwoSided = false;
    // Absorbing blocker uses its own material; its downward orientation faces the receiver.
    REQUIRE(blockedScene.Materials.TryResize(3) == true);
    blockedScene.Objects[blockedScene.Objects.GetSize() - 1].Material = 2;
    rr::StaticLightingBake blocked;
    REQUIRE(blocked.Initialize(blockedScene, MakeBakeDescription()) == rhi::RasterStatus::Pending);
    const math::Vector3 dark = ReadChart(CompleteBake(blocked, 17));
    REQUIRE(dark.X < lit.X * .01F);
    REQUIRE(dark.Z < lit.Z * .01F);
}
TEST_CASE("Static bake respects mirrored geometric emitter sidedness", "[renderer][static-lighting]")
{
    resources::SceneDefinition scene = MakeBakeScene(true, false, false);
    for (usize i = 1; i < scene.Objects.GetSize(); ++i)
    {
        scene.Objects[i].Transform.Columns[2].Z = -1;
    }
    {
        rr::StaticLightingBake oneSided;
        REQUIRE(oneSided.Initialize(scene, MakeBakeDescription()) == rhi::RasterStatus::Pending);
        REQUIRE(ReadChart(CompleteBake(oneSided, 13)).X == 0);
    }
    // Destroy the worker job before mutating its borrowed source.
    scene.Materials[1].IsEmissionTwoSided = true;
    rr::StaticLightingBake twoSided;
    REQUIRE(twoSided.Initialize(scene, MakeBakeDescription()) == rhi::RasterStatus::Pending);
    REQUIRE(ReadChart(CompleteBake(twoSided, 13)).X > 1);
}
TEST_CASE("Static bake separates analytic direct from indirect and is independent of step partition",
          "[renderer][static-lighting]")
{
    resources::SceneDefinition emptyScene = MakeBakeScene(false, false, true);
    rr::StaticLightingBake directOnly;
    REQUIRE(directOnly.Initialize(emptyScene, MakeBakeDescription()) == rhi::RasterStatus::Pending);
    const math::Vector3 direct = ReadChart(CompleteBake(directOnly, 256));
    REQUIRE(direct.X == 0);
    resources::SceneDefinition scene = MakeBakeScene(false, true, true);
    rr::StaticLightingBake first;
    rr::StaticLightingBake second;
    REQUIRE(first.Initialize(scene, MakeBakeDescription()) == rhi::RasterStatus::Pending);
    REQUIRE(second.Initialize(scene, MakeBakeDescription()) == rhi::RasterStatus::Pending);
    const rr::StaticLightingChart chart = CompleteBake(first, 1);
    const rr::StaticLightingChart same = CompleteBake(second, 19);
    REQUIRE(ReadChart(chart).X > 1);
    REQUIRE(ReadChart(chart).Y == 0);
    REQUIRE(chart.Scale == same.Scale);
    REQUIRE(chart.Pixels.GetSize() == same.Pixels.GetSize());
    REQUIRE(std::memcmp(chart.Pixels.GetDataOrNull(), same.Pixels.GetDataOrNull(), chart.Pixels.GetSize()) == 0);
}
TEST_CASE("Static bake rejects budgets and malformed chart inputs without binding a job", "[renderer][static-lighting]")
{
    resources::SceneDefinition scene = MakeBakeScene(true, false, false);
    rr::StaticLightingBake bake;
    rr::StaticLightingBakeDescription description = MakeBakeDescription();
    description.CpuByteBudget = 1;
    REQUIRE(bake.Initialize(scene, description) == rhi::RasterStatus::CapacityExceeded);
    description = MakeBakeDescription(1);
    REQUIRE(scene.Meshes[0].BakeTexcoords.TryResize(TRIANGLE_CORNERS) == true);
    REQUIRE(bake.Initialize(scene, description) == rhi::RasterStatus::Unsupported);
    scene.Meshes[0].BakeTexcoords.Clear();
    REQUIRE(bake.Initialize(scene, description) == rhi::RasterStatus::Pending);
    REQUIRE(bake.Initialize(scene, description) == rhi::RasterStatus::InvalidState);
    REQUIRE(bake.Step(0) == rhi::RasterStatus::InvalidState);
    static_cast<void>(CompleteBake(bake, 100));
}
TEST_CASE("Planar emitter centroid is independent of quad tessellation and cube projections preserve depth",
          "[renderer][lighting]")
{
    StaticArray<math::Vector3, PATCH_VERTICES> positions{{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}}};
    StaticArray<uint32, PATCH_INDICES> indices{0, 1, 2, 0, 2, 3};
    rr::PlanarAreaLightDescription description;
    description.PositionsMesh = {positions.GetData(), positions.GetSize()};
    description.Indices = {indices.GetData(), indices.GetSize()};
    description.Emission = {10, 20, 30};
    description.SourceId = 1;
    rr::DirectLight light;
    REQUIRE(rr::BuildPlanarAreaLight(description, light) == rhi::RasterStatus::Ready);
    REQUIRE(light.Kind == rr::LightKind::AreaOneSided);
    REQUIRE(light.Position == math::Vector3{});
    REQUIRE(light.Direction.Z == 1);
    REQUIRE(light.Intensity == 120);
    REQUIRE(light.Color.Z == 1);
    indices = {0, 1, 3, 1, 2, 3};
    rr::DirectLight same;
    REQUIRE(rr::BuildPlanarAreaLight(description, same) == rhi::RasterStatus::Ready);
    REQUIRE(same.Position == light.Position);
    REQUIRE(same.Intensity == light.Intensity);
    positions[3].Z = 1;
    REQUIRE(rr::BuildPlanarAreaLight(description, same) == rhi::RasterStatus::Unsupported);
    REQUIRE(same.Intensity == light.Intensity);
    constexpr float32 NEAR_METRES = .1F;
    constexpr float32 FAR_METRES = 10;
    StaticArray<math::Vector3, rr::POINT_SHADOW_FACES> axes{
        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    for (uint32 face = 0; face < rr::POINT_SHADOW_FACES; ++face)
    {
        math::Matrix4 worldToClip;
        REQUIRE(rr::BuildPointShadowMatrix({}, face, NEAR_METRES, FAR_METRES, worldToClip) == rhi::RasterStatus::Ready);
        const math::Vector3 nearWorld = axes[face] * NEAR_METRES;
        const math::Vector3 farWorld = axes[face] * FAR_METRES;
        const math::Vector4 nearClip = worldToClip * math::Vector4{nearWorld.X, nearWorld.Y, nearWorld.Z, 1};
        const math::Vector4 farClip = worldToClip * math::Vector4{farWorld.X, farWorld.Y, farWorld.Z, 1};
        REQUIRE(math::Abs(nearClip.Z / nearClip.W) < 1e-5F);
        REQUIRE(math::Abs(farClip.Z / farClip.W - 1) < 1e-5F);
    }
}

TEST_CASE("Static transport copies current per-object World material terms", "[renderer][static-lighting]")
{
    resources::SceneDefinition scene = MakeBakeScene(false, true, true);
    Array<rr::StaticLightingSurfaceDescription> surfaces;
    REQUIRE(surfaces.TryResize(scene.Objects.GetSize()) == true);
    for (rr::StaticLightingSurfaceDescription& surface : surfaces)
    {
        surface.Texture = 0;
        surface.Roughness = 1;
    }
    rr::StaticLightingBakeDescription description = MakeBakeDescription();
    description.Surfaces = {surfaces.GetData(), surfaces.GetSize()};
    rr::StaticLightingBake white;
    REQUIRE(white.Initialize(scene, description) == rhi::RasterStatus::Pending);
    surfaces[1].Tint = {0, 0, 0, 1}; // Copied overrides may change after Initialize.
    const math::Vector3 reflected = ReadChart(CompleteBake(white, 31));
    REQUIRE(reflected.X > 1);
    rr::StaticLightingBake dark;
    REQUIRE(dark.Initialize(scene, description) == rhi::RasterStatus::Pending);
    REQUIRE(ReadChart(CompleteBake(dark, 31)).X < reflected.X * .1F);
    description.Surfaces = {surfaces.GetData(), 1};
    rr::StaticLightingBake invalid;
    REQUIRE(invalid.Initialize(scene, description) == rhi::RasterStatus::InvalidDescription);
}

TEST_CASE("Static chart encoding rejects excess irradiance without publishing partial data",
          "[renderer][static-lighting]")
{
    resources::SceneDefinition scene = MakeBakeScene(false, false, false);
    scene.Environment.Rgb = {10000, 10000, 10000};
    rr::StaticLightingBake bake;
    REQUIRE(bake.Initialize(scene, MakeBakeDescription()) == rhi::RasterStatus::Pending);
    rhi::RasterStatus status = rhi::RasterStatus::Pending;
    while (status == rhi::RasterStatus::Pending)
    {
        status = bake.Step(13);
    }
    REQUIRE(status == rhi::RasterStatus::CapacityExceeded);
    rr::StaticLightingChart output;
    output.SourceId = 999;
    REQUIRE(bake.GetChart(0, output) == rhi::RasterStatus::CapacityExceeded);
    REQUIRE(output.SourceId == 999);
}

TEST_CASE("Scheduled static bake matches the serial Step oracle bit for bit", "[renderer][static-lighting][threading]")
{
    const resources::SceneDefinition scene = MakeGridBakeScene(ORACLE_GRID_CELLS);
    const rr::StaticLightingBakeDescription description = MakeBakeDescription(16);
    rr::StaticLightingBake oracle;
    REQUIRE(oracle.Initialize(scene, description) == rhi::RasterStatus::Pending);
    static_cast<void>(CompleteBake(oracle, 97));
    rr::StaticLightingBakeProgress oracleProgress;
    oracle.GetProgress(oracleProgress);
    REQUIRE(oracleProgress.TotalSamples != 0);
    REQUIRE(oracleProgress.CompletedSamples == oracleProgress.TotalSamples);
    // Zero workers is the serial executor; 1024 ranges exceed the triangle count.
    for (const uint32 workers : {0U, 1U, 2U, 4U})
    {
        threading::JobSystem system;
        REQUIRE(system.Initialize({ .WorkerCount = workers }) == threading::JobStatus::Ok);
        for (const uint32 ranges : {1U, 3U, 64U, rr::MAX_STATIC_LIGHTING_RANGES})
        {
            CAPTURE(workers, ranges);
            rr::StaticLightingBake bake;
            REQUIRE(bake.Submit(system, scene, description, { .RangeCount = ranges }) == rhi::RasterStatus::Pending);
            REQUIRE(bake.Step(1) == rhi::RasterStatus::InvalidState);
            REQUIRE(bake.Initialize(scene, description) == rhi::RasterStatus::InvalidState);
            rhi::RasterStatus status = rhi::RasterStatus::Pending;
            if (workers == 2)
            {
                while (status == rhi::RasterStatus::Pending)
                {
                    status = bake.Poll();
                }
            }
            else
            {
                status = bake.Finish();
            }
            REQUIRE(status == rhi::RasterStatus::Ready);
            REQUIRE(bake.Poll() == rhi::RasterStatus::Ready);
            REQUIRE(bake.Finish() == rhi::RasterStatus::Ready);
            RequireIdenticalCharts(oracle, bake, scene.Objects.GetSize());
            rr::StaticLightingBakeProgress progress;
            bake.GetProgress(progress);
            REQUIRE(progress.TotalSamples == oracleProgress.TotalSamples);
            REQUIRE(progress.CompletedSamples == progress.TotalSamples);
            rr::StaticLightingBakeProfile profile;
            REQUIRE(bake.GetProfile(profile) == rhi::RasterStatus::Ready);
            REQUIRE(profile.RangeCount == ranges);
            REQUIRE(profile.ExecutedRangeCount == ranges);
            REQUIRE(profile.LongestRangeNanoseconds <= profile.RangeNanoseconds);
        }
    }
}
TEST_CASE("Scheduled static bake reports admission and preparation failures", "[renderer][static-lighting][threading]")
{
    const resources::SceneDefinition scene = MakeGridBakeScene(2);
    rr::StaticLightingBakeDescription description = MakeBakeDescription(4);
    threading::JobSystem uninitialized;
    rr::StaticLightingBake bake;
    REQUIRE(bake.Submit(uninitialized, scene, description) == rhi::RasterStatus::InvalidState);
    REQUIRE(bake.Poll() == rhi::RasterStatus::InvalidState);
    REQUIRE(bake.Finish() == rhi::RasterStatus::InvalidState);
    rr::StaticLightingBakeProfile profile;
    profile.RangeCount = 77;
    REQUIRE(bake.GetProfile(profile) == rhi::RasterStatus::InvalidState);
    REQUIRE(profile.RangeCount == 77);
    threading::JobSystem system;
    REQUIRE(system.Initialize({ .WorkerCount = 2 }) == threading::JobStatus::Ok);
    REQUIRE(bake.Submit(system, scene, description, { .RangeCount = 0 }) == rhi::RasterStatus::InvalidDescription);
    REQUIRE(bake.Submit(system, scene, description, { .RangeCount = rr::MAX_STATIC_LIGHTING_RANGES + 1 }) ==
            rhi::RasterStatus::InvalidDescription);
    // Validation runs in the preparation job; its failure blocks every range.
    description.SceneRevision = 0;
    REQUIRE(bake.Submit(system, scene, description) == rhi::RasterStatus::Pending);
    rr::StaticLightingBake busy;
    REQUIRE(busy.Submit(system, scene, MakeBakeDescription(4)) == rhi::RasterStatus::InvalidState);
    REQUIRE(bake.Submit(system, scene, description) == rhi::RasterStatus::InvalidState);
    REQUIRE(bake.Finish() == rhi::RasterStatus::InvalidDescription);
    REQUIRE(bake.Poll() == rhi::RasterStatus::InvalidDescription);
    REQUIRE(bake.GetProfile(profile) == rhi::RasterStatus::Ready);
    REQUIRE(profile.ExecutedRangeCount == 0);
    rr::StaticLightingChart chart;
    chart.SourceId = 999;
    REQUIRE(bake.GetChart(0, chart) == rhi::RasterStatus::InvalidDescription);
    REQUIRE(chart.SourceId == 999);
    // The released system accepts the next bake.
    REQUIRE(busy.Submit(system, scene, MakeBakeDescription(4)) == rhi::RasterStatus::Pending);
    REQUIRE(busy.Finish() == rhi::RasterStatus::Ready);
}
TEST_CASE("Cancelling a scheduled static bake mid-evaluation stops its ranges",
          "[renderer][static-lighting][threading]")
{
    const resources::SceneDefinition scene = MakeGridBakeScene(CANCEL_GRID_CELLS);
    threading::JobSystem system;
    REQUIRE(system.Initialize({ .WorkerCount = 2 }) == threading::JobStatus::Ok);
    rr::StaticLightingBake bake;
    REQUIRE(bake.Submit(system, scene, MakeBakeDescription(CANCEL_SAMPLES)) == rhi::RasterStatus::Pending);
    REQUIRE(WaitForProgress(bake) == rhi::RasterStatus::Pending);
    bake.Cancel();
    REQUIRE(bake.Finish() == rhi::RasterStatus::NotReady);
    REQUIRE(bake.Poll() == rhi::RasterStatus::NotReady);
    rr::StaticLightingBakeProgress progress;
    bake.GetProgress(progress);
    REQUIRE(progress.CompletedSamples > 0);
    REQUIRE(progress.CompletedSamples < progress.TotalSamples);
    rr::StaticLightingBakeProfile profile;
    REQUIRE(bake.GetProfile(profile) == rhi::RasterStatus::Ready);
    REQUIRE(profile.ExecutedRangeCount < profile.RangeCount);
    rr::StaticLightingChart chart;
    chart.SourceId = 999;
    REQUIRE(bake.GetChart(0, chart) == rhi::RasterStatus::NotReady);
    REQUIRE(chart.SourceId == 999);
    bake.Cancel(); // No effect once released.
    // The cancelled bake released the system; a later bake still matches its oracle.
    const resources::SceneDefinition small = MakeGridBakeScene(2);
    rr::StaticLightingBake oracle;
    REQUIRE(oracle.Initialize(small, MakeBakeDescription(8)) == rhi::RasterStatus::Pending);
    static_cast<void>(CompleteBake(oracle, 64));
    rr::StaticLightingBake next;
    REQUIRE(next.Submit(system, small, MakeBakeDescription(8)) == rhi::RasterStatus::Pending);
    REQUIRE(next.Finish() == rhi::RasterStatus::Ready);
    RequireIdenticalCharts(oracle, next, small.Objects.GetSize());
}
TEST_CASE("Static bake survives system shutdown and destruction while in flight",
          "[renderer][static-lighting][threading]")
{
    const resources::SceneDefinition large = MakeGridBakeScene(CANCEL_GRID_CELLS);
    const resources::SceneDefinition small = MakeGridBakeScene(2);
    SECTION("Cancel then Shutdown drains promptly and reports cancellation")
    {
        threading::JobSystem system;
        REQUIRE(system.Initialize({ .WorkerCount = 2 }) == threading::JobStatus::Ok);
        rr::StaticLightingBake bake;
        REQUIRE(bake.Submit(system, large, MakeBakeDescription(CANCEL_SAMPLES)) == rhi::RasterStatus::Pending);
        REQUIRE(WaitForProgress(bake) == rhi::RasterStatus::Pending);
        bake.Cancel();
        REQUIRE(system.Shutdown() == threading::JobStatus::Ok);
        REQUIRE(bake.Poll() == rhi::RasterStatus::NotReady);
        REQUIRE(bake.Finish() == rhi::RasterStatus::NotReady);
    }
    SECTION("Shutdown without cancellation drains the bake to its oracle result")
    {
        rr::StaticLightingBake oracle;
        REQUIRE(oracle.Initialize(small, MakeBakeDescription(8)) == rhi::RasterStatus::Pending);
        static_cast<void>(CompleteBake(oracle, 64));
        threading::JobSystem system;
        REQUIRE(system.Initialize({ .WorkerCount = 2 }) == threading::JobStatus::Ok);
        rr::StaticLightingBake bake;
        REQUIRE(bake.Submit(system, small, MakeBakeDescription(8)) == rhi::RasterStatus::Pending);
        REQUIRE(system.Shutdown() == threading::JobStatus::Ok);
        REQUIRE(bake.Poll() == rhi::RasterStatus::Ready);
        RequireIdenticalCharts(oracle, bake, small.Objects.GetSize());
    }
    SECTION("Destroying an in-flight bake cancels and drains it, then releases the system")
    {
        threading::JobSystem system;
        REQUIRE(system.Initialize({ .WorkerCount = 2 }) == threading::JobStatus::Ok);
        {
            rr::StaticLightingBake bake;
            REQUIRE(bake.Submit(system, large, MakeBakeDescription(CANCEL_SAMPLES)) == rhi::RasterStatus::Pending);
            REQUIRE(WaitForProgress(bake) == rhi::RasterStatus::Pending);
        }
        threading::JobSnapshot snapshot;
        REQUIRE(system.Snapshot(snapshot) == threading::JobStatus::Ok);
        REQUIRE(snapshot.Running == 0);
        REQUIRE(snapshot.Pending == 0);
        rr::StaticLightingBake next;
        REQUIRE(next.Submit(system, small, MakeBakeDescription(8)) == rhi::RasterStatus::Pending);
        REQUIRE(next.Finish() == rhi::RasterStatus::Ready);
        REQUIRE(system.Shutdown() == threading::JobStatus::Ok);
    }
}
namespace
{
/// Collapsed triangles appended to the final object: one repeats a vertex, one is collinear.
constexpr uint32 COLLAPSED_TRIANGLE_COUNT = 2;
constexpr uint64 FINAL_OBJECT_SOURCE_ID = 500;
/// Path samples per Step call; any positive budget yields the same charts.
constexpr uint32 SERIAL_STEP_SAMPLES = 97;
namespace format = ludus::content::scene_format;
/// MakeGridBakeScene plus a final object with its own mesh: one real triangle, then
/// optionally the collapsed ones. They sit at the end of the global triangle order, so
/// every real triangle keeps its index (and random stream) with or without them.
resources::SceneDefinition MakeSceneWithFinalObject(uint32 cells, bool hasCollapsedTriangles)
{
    resources::SceneDefinition scene = MakeGridBakeScene(cells);
    const uint32 meshIndex = static_cast<uint32>(scene.Meshes.GetSize());
    REQUIRE(scene.Meshes.TryResize(scene.Meshes.GetSize() + 1) == true);
    resources::SceneMesh& mesh = scene.Meshes[meshIndex];
    const math::Vector3 positions[] = {{-.5F, -.5F, .4F}, {.5F, -.5F, .4F}, {0, .5F, .4F}, {0, -.5F, .4F}};
    REQUIRE(mesh.Positions.TryAddRange(positions) == true);
    REQUIRE(mesh.Normals.TryResize(mesh.Positions.GetSize()) == true);
    REQUIRE(mesh.Texcoords.TryResize(mesh.Positions.GetSize()) == true);
    for (math::Vector3& normal : mesh.Normals)
    {
        normal = {0, 0, 1};
    }
    const uint32 real[] = {0, 1, 2};
    REQUIRE(mesh.Indices.TryAddRange(real) == true);
    if (hasCollapsedTriangles == true)
    {
        // A repeated vertex, then three collinear points along the bottom edge.
        const uint32 collapsed[] = {0, 0, 1, 0, 3, 1};
        REQUIRE(mesh.Indices.TryAddRange(collapsed) == true);
    }
    const usize objectIndex = scene.Objects.GetSize();
    REQUIRE(scene.Objects.TryResize(objectIndex + 1) == true);
    scene.Objects[objectIndex].SourceId = FINAL_OBJECT_SOURCE_ID;
    scene.Objects[objectIndex].Mesh = meshIndex;
    scene.Objects[objectIndex].Material = 0;
    return scene;
}
/// Reads the RGB of one triangle's chart tile.
StaticArray<uint8, 3> ReadTile(const rr::StaticLightingChart& chart, usize triangle)
{
    const usize columns = chart.Width / format::BAKE_CHART_TILE_PIXELS;
    const usize x = (triangle % columns) * format::BAKE_CHART_TILE_PIXELS;
    const usize y = (triangle / columns) * format::BAKE_CHART_TILE_PIXELS;
    const usize offset = (y * chart.Width + x) * format::TEXEL_BYTES;
    return {chart.Pixels[offset], chart.Pixels[offset + 1], chart.Pixels[offset + 2]};
}
void CompleteSerialBake(rr::StaticLightingBake& inoutBake)
{
    rhi::RasterStatus status = rhi::RasterStatus::Pending;
    while (status == rhi::RasterStatus::Pending)
    {
        status = inoutBake.Step(SERIAL_STEP_SAMPLES);
    }
    REQUIRE(status == rhi::RasterStatus::Ready);
}
} // namespace
TEST_CASE("Static bake skips zero-area triangles without perturbing the rest", "[renderer][static-lighting]")
{
    const resources::SceneDefinition clean = MakeSceneWithFinalObject(ORACLE_GRID_CELLS, false);
    const resources::SceneDefinition flawed = MakeSceneWithFinalObject(ORACLE_GRID_CELLS, true);
    const rr::StaticLightingBakeDescription description = MakeBakeDescription(16);
    const usize objectCount = flawed.Objects.GetSize();
    const usize finalObject = objectCount - 1;
    rr::StaticLightingBake cleanBake;
    rr::StaticLightingBake flawedBake;
    REQUIRE(cleanBake.Initialize(clean, description) == rhi::RasterStatus::Pending);
    REQUIRE(flawedBake.Initialize(flawed, description) == rhi::RasterStatus::Pending);
    // Serial preparation completes inside Initialize, so the report is available before any Step.
    rr::StaticLightingBakeDiagnostics diagnostics;
    REQUIRE(flawedBake.GetDiagnostics(diagnostics) == rhi::RasterStatus::Ready);
    CHECK(diagnostics.SkippedTriangleCount == COLLAPSED_TRIANGLE_COUNT);
    CHECK(diagnostics.AffectedObjectCount == 1);
    uint32 skipped = 99;
    REQUIRE(flawedBake.GetSkippedTriangleCount(finalObject, skipped) == rhi::RasterStatus::Ready);
    CHECK(skipped == COLLAPSED_TRIANGLE_COUNT);
    REQUIRE(flawedBake.GetSkippedTriangleCount(0, skipped) == rhi::RasterStatus::Ready);
    CHECK(skipped == 0);
    skipped = 99;
    CHECK(flawedBake.GetSkippedTriangleCount(objectCount, skipped) == rhi::RasterStatus::InvalidDescription);
    CHECK(skipped == 99);
    REQUIRE(cleanBake.GetDiagnostics(diagnostics) == rhi::RasterStatus::Ready);
    CHECK(diagnostics.SkippedTriangleCount == 0);
    CHECK(diagnostics.AffectedObjectCount == 0);
    CompleteSerialBake(cleanBake);
    CompleteSerialBake(flawedBake);
    // Collapsed triangles are never hit, sampled as emitters or probed, so every other
    // object's chart is bit-identical to the clean scene's.
    RequireIdenticalCharts(cleanBake, flawedBake, finalObject);
    rr::StaticLightingChart cleanChart;
    rr::StaticLightingChart flawedChart;
    REQUIRE(cleanBake.GetChart(finalObject, cleanChart) == rhi::RasterStatus::Ready);
    REQUIRE(flawedBake.GetChart(finalObject, flawedChart) == rhi::RasterStatus::Ready);
    CHECK(cleanChart.SkippedTriangleCount == 0);
    CHECK(flawedChart.SkippedTriangleCount == COLLAPSED_TRIANGLE_COUNT);
    // The layout keeps one tile per triangle; the real tile matches and collapsed tiles are zero.
    CHECK(flawedChart.Scale == cleanChart.Scale);
    CHECK(ReadTile(flawedChart, 0) == ReadTile(cleanChart, 0));
    const StaticArray<uint8, 3> zero{0, 0, 0};
    for (usize triangle = 1; triangle <= COLLAPSED_TRIANGLE_COUNT; ++triangle)
    {
        CHECK(ReadTile(flawedChart, triangle) == zero);
    }
    // Scheduled evaluation skips the same triangles and stays bit-identical to Step.
    threading::JobSystem system;
    REQUIRE(system.Initialize({ .WorkerCount = 2 }) == threading::JobStatus::Ok);
    rr::StaticLightingBake scheduled;
    REQUIRE(scheduled.Submit(system, flawed, description, { .RangeCount = 7 }) == rhi::RasterStatus::Pending);
    REQUIRE(scheduled.Finish() == rhi::RasterStatus::Ready);
    RequireIdenticalCharts(flawedBake, scheduled, objectCount);
    rr::StaticLightingBakeProgress progress;
    scheduled.GetProgress(progress);
    CHECK(progress.CompletedSamples == progress.TotalSamples);
}
TEST_CASE("Scheduled static bake reports skipped triangles while ranges run and after cancellation",
          "[renderer][static-lighting][threading]")
{
    rr::StaticLightingBake unbound;
    rr::StaticLightingBakeDiagnostics diagnostics;
    diagnostics.SkippedTriangleCount = 77;
    REQUIRE(unbound.GetDiagnostics(diagnostics) == rhi::RasterStatus::InvalidState);
    REQUIRE(diagnostics.SkippedTriangleCount == 77);
    const resources::SceneDefinition scene = MakeSceneWithFinalObject(CANCEL_GRID_CELLS, true);
    threading::JobSystem system;
    REQUIRE(system.Initialize({ .WorkerCount = 2 }) == threading::JobStatus::Ok);
    rr::StaticLightingBake bake;
    REQUIRE(bake.Submit(system, scene, MakeBakeDescription(CANCEL_SAMPLES)) == rhi::RasterStatus::Pending);
    rhi::RasterStatus prepared = rhi::RasterStatus::Pending;
    while (prepared == rhi::RasterStatus::Pending)
    {
        prepared = bake.GetDiagnostics(diagnostics);
    }
    REQUIRE(prepared == rhi::RasterStatus::Ready);
    // The report arrives with preparation, long before this multi-second bake finishes,
    // so a caller can warn and offer Cancel.
    REQUIRE(bake.Poll() == rhi::RasterStatus::Pending);
    CHECK(diagnostics.SkippedTriangleCount == COLLAPSED_TRIANGLE_COUNT);
    CHECK(diagnostics.AffectedObjectCount == 1);
    bake.Cancel();
    REQUIRE(bake.Finish() == rhi::RasterStatus::NotReady);
    rr::StaticLightingBakeDiagnostics afterCancel;
    REQUIRE(bake.GetDiagnostics(afterCancel) == rhi::RasterStatus::Ready);
    CHECK(afterCancel.SkippedTriangleCount == COLLAPSED_TRIANGLE_COUNT);
    // A failed preparation never reports.
    rr::StaticLightingBakeDescription invalid = MakeBakeDescription(4);
    invalid.SceneRevision = 0;
    rr::StaticLightingBake failed;
    REQUIRE(failed.Submit(system, scene, invalid) == rhi::RasterStatus::Pending);
    REQUIRE(failed.Finish() == rhi::RasterStatus::InvalidDescription);
    diagnostics.SkippedTriangleCount = 77;
    CHECK(failed.GetDiagnostics(diagnostics) == rhi::RasterStatus::NotReady);
    CHECK(diagnostics.SkippedTriangleCount == 77);
}
TEST_CASE("Static bake treats a fully collapsed emitter as emitting nothing", "[renderer][static-lighting]")
{
    resources::SceneDefinition scene = MakeBakeScene(true, false, false);
    // Every emitter instance shares mesh 1; collapse it onto one point.
    scene.Materials[1].HasEmissionPower = true;
    scene.Materials[1].EmissionPower = 100;
    for (math::Vector3& position : scene.Meshes[1].Positions)
    {
        position = {0, 0, 0};
    }
    rr::StaticLightingBake bake;
    REQUIRE(bake.Initialize(scene, MakeBakeDescription(8)) == rhi::RasterStatus::Pending);
    rr::StaticLightingBakeDiagnostics diagnostics;
    REQUIRE(bake.GetDiagnostics(diagnostics) == rhi::RasterStatus::Ready);
    CHECK(diagnostics.SkippedTriangleCount == SYNTHETIC_EMITTER_COUNT);
    CHECK(diagnostics.AffectedObjectCount == SYNTHETIC_EMITTER_COUNT);
    // Normalizing the authored power over zero area must not poison the bake.
    const rr::StaticLightingChart receiver = CompleteBake(bake, 64);
    CHECK(ReadChart(receiver) == math::Vector3{});
}
TEST_CASE("Static bake still rejects triangles whose area is not representable", "[renderer][static-lighting]")
{
    resources::SceneDefinition scene = MakeBakeScene(false, false, false);
    // Finite coordinates whose edge cross product overflows binary32.
    scene.Meshes[0].Positions[0] = {3.0e38F, 0, 0};
    scene.Meshes[0].Positions[1] = {0, 3.0e38F, 0};
    scene.Meshes[0].Positions[2] = {0, 0, 0};
    rr::StaticLightingBake bake;
    CHECK(bake.Initialize(scene, MakeBakeDescription(4)) == rhi::RasterStatus::InvalidDescription);
    rr::StaticLightingBakeDiagnostics diagnostics;
    CHECK(bake.GetDiagnostics(diagnostics) == rhi::RasterStatus::InvalidState);
}
