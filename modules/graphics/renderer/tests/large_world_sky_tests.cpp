// Fixed-world receivers on a floor under a shuttered roof. The graph rasterizes two triangles per
// panel. The oracle intersects the analytic rectangles and never samples a shadow map.
#include "../../../../tests/fixtures/large_world_sky.h"
#include "../src/internal/atmosphere_sky_tables.h"
#include "../src/internal/raster_mesh_light_shadows.h"
#include "render_graph_readback_fixture.h"
#include "renderer_composite.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>

namespace ludus::graphics::renderer::testing
{
namespace
{
using namespace foundation;
math::Vector3d ToVector3d(math::Vector3 value)
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

struct ReceiverPlane
{
    math::Vector3d OffsetWorld;
    math::Vector3d NormalWorld;
    math::Vector3d HalfUWorld;
    math::Vector3d HalfVWorld;
    float64 PlaneDistance;
    float64 USquared;
    float64 VSquared;
};
using ReceiverPlanes = StaticArray<ReceiverPlane, PANEL_COUNT>;
ReceiverPlanes MakeReceiverPlanes(const Panels& panels, Shutter shutter, math::Vector3d receiverWorld)
{
    ReceiverPlanes planes{};
    for (uint32 i = 0; i < PANEL_COUNT; ++i)
    {
        math::Vector3d centerWorld = ToVector3d(panels[i].CenterWorld);
        if (i == SHUTTER_INDEX)
        {
            centerWorld.X += static_cast<float64>(GetShutterTranslation(shutter));
        }
        const math::Vector3d u = ToVector3d(panels[i].HalfUWorld);
        const math::Vector3d v = ToVector3d(panels[i].HalfVWorld);
        const math::Vector3d normalWorld = math::Cross(u, v);
        const math::Vector3d offsetWorld = receiverWorld - centerWorld;
        planes[i] =
            {offsetWorld, normalWorld, u, v, -math::Dot(offsetWorld, normalWorld), math::Dot(u, u), math::Dot(v, v)};
    }
    return planes;
}
bool CanSeeSky(const ReceiverPlanes& planes, uint32 count, math::Vector3d directionWorld)
{
    for (uint32 i = 0; i < count; ++i)
    {
        const ReceiverPlane& plane = planes[i];
        const float64 denominator = math::Dot(directionWorld, plane.NormalWorld);
        if (denominator == 0)
        {
            continue;
        }
        const float64 distance = plane.PlaneDistance / denominator;
        if (distance <= 0)
        {
            continue;
        }
        const math::Vector3d offsetWorld = plane.OffsetWorld + distance * directionWorld;
        if (math::Abs(math::Dot(offsetWorld, plane.HalfUWorld)) <= plane.USquared &&
            math::Abs(math::Dot(offsetWorld, plane.HalfVWorld)) <= plane.VSquared)
        {
            return false;
        }
    }
    return true;
}
struct Integral
{
    float64 Visibility = 0;
    float64 Diffuse = 0;
};
constexpr float64 SOLAR_SINE = 0.25;
constexpr float32 SOLAR_ILLUMINANCE = 16;
constexpr float64 DIFFUSE_FACTOR = 0.96; // white dielectric at normal incidence
constexpr float64 UNIFORM_IRRADIANCE = 2;

float64 ComputeLuminance(math::Vector3d value)
{
    return math::Dot(value, math::Vector3d{0.2126, 0.7152, 0.0722});
}
// A midpoint grid uniform in azimuth and elevation sine integrates equal solid angles. With
// resolution zero we intersect exactly the host's directions, which isolates map lookup error.
Integral Integrate(const Panels& panels,
                   Shutter shutter,
                   math::Vector3d receiverWorld,
                   uint32 resolution,
                   const internal::AtmosphereSkyTables* tablesOrNull,
                   math::Vector3d sunWorld)
{
    const ReceiverPlanes planes = MakeReceiverPlanes(panels, shutter, receiverWorld);
    const uint32 planeCount = shutter == Shutter::RoofRemoved ? ROOF_INDEX : PANEL_COUNT;
    const uint32 azimuths = 4 * resolution;
    const uint32 count = resolution == 0 ? SKY_VISIBILITY_DIRECTION_COUNT : resolution * azimuths;
    float64 total = 0;
    float64 visible = 0;
    float64 radiance = 0;
    for (uint32 i = 0; i < count; ++i)
    {
        math::Vector3d directionWorld;
        if (resolution == 0)
        {
            directionWorld = ToVector3d(internal::GetSkyVisibilityDirection(i));
        }
        else
        {
            const uint32 elevationBand = i / azimuths;
            const float64 sine = (static_cast<float64>(elevationBand) + 0.5) / resolution;
            const float64 azimuth = 2 * math::kPi * (static_cast<float64>(i % azimuths) + 0.5) / azimuths;
            const float64 radius = std::sqrt(1 - sine * sine);
            directionWorld = {radius * std::cos(azimuth), sine, radius * std::sin(azimuth)};
        }
        // All probes face +Y. Their projected solid angle below the horizon is exactly zero.
        total += directionWorld.Y;
        if (CanSeeSky(planes, planeCount, directionWorld) == true)
        {
            visible += directionWorld.Y;
            if (tablesOrNull != nullptr)
            {
                radiance +=
                    directionWorld.Y * ComputeLuminance(tablesOrNull->SampleSky(directionWorld, {0, 1, 0}, sunWorld));
            }
        }
    }
    const float64 visibility = visible / total;
    return {visibility,
            tablesOrNull == nullptr ? DIFFUSE_FACTOR * UNIFORM_IRRADIANCE / math::kPi * visibility
                                    : DIFFUSE_FACTOR * static_cast<float64>(SOLAR_ILLUMINANCE) * 2 * radiance / count};
}
constexpr uint32 PROBE_COUNT = 6;
constexpr StaticArray<float32, PROBE_COUNT> PROBE_Z{{-2, 3.75F, 6.5F, -6.25F, 3, 7.5F}};
constexpr StaticArray<float32, PROBE_COUNT> PROBE_X{{0, 0, 0, 0, -1.25F, 0}};
constexpr StaticArray<Shutter, 4> CONTROLS{{Shutter::RoofRemoved, Shutter::Open, Shutter::Closed, Shutter::Narrow}};
constexpr uint32 DENSE_RESOLUTION = 256;

TEST_CASE("large World sky rays distinguish angular sampling from geometric visibility",
          "[large-world-sky][reference-compute]")
{
    const Panels panels = MakePanels();
    internal::AtmosphereSkyTables tables;
    REQUIRE(tables.TryBuild(0.001, SOLAR_SINE, true) == true);
    // Rotate solar azimuth around +Z, holding every geometric ray fixed.
    for (int32 degrees : {-60, -30, 0, 30, 60})
    {
        const float64 azimuth = static_cast<float64>(degrees) * math::kPi / 180;
        const float64 horizontal = std::sqrt(1 - SOLAR_SINE * SOLAR_SINE);
        const math::Vector3d sunWorld{horizontal * std::sin(azimuth), SOLAR_SINE, horizontal * std::cos(azimuth)};
        for (const internal::AtmosphereSkyTables* tablesOrNull :
             {static_cast<const internal::AtmosphereSkyTables*>(nullptr),
              static_cast<const internal::AtmosphereSkyTables*>(&tables)})
        {
            if (tablesOrNull == nullptr && degrees != 0)
            {
                continue;
            }
            for (uint32 i = 0; i < PROBE_COUNT; ++i)
            {
                Integral roofRemoved;
                for (Shutter shutter : CONTROLS)
                {
                    const math::Vector3d receiverWorld{static_cast<float64>(PROBE_X[i]),
                                                       0,
                                                       static_cast<float64>(PROBE_Z[i])};
                    const Integral matched = Integrate(panels, shutter, receiverWorld, 0, tablesOrNull, sunWorld);
                    const Integral dense =
                        Integrate(panels, shutter, receiverWorld, DENSE_RESOLUTION, tablesOrNull, sunWorld);
                    const Integral half =
                        Integrate(panels, shutter, receiverWorld, DENSE_RESOLUTION / 2, tablesOrNull, sunWorld);
                    if (shutter == Shutter::RoofRemoved)
                    {
                        roofRemoved = dense;
                    }
                    CAPTURE(degrees,
                            i,
                            static_cast<int>(shutter),
                            matched.Visibility,
                            dense.Visibility,
                            matched.Diffuse,
                            dense.Diffuse,
                            roofRemoved.Diffuse);
                    REQUIRE(roofRemoved.Diffuse > 0);
                    CHECK(math::Abs(half.Visibility - dense.Visibility) < 0.01);
                    CHECK(math::Abs(half.Diffuse - dense.Diffuse) < 0.01 * roofRemoved.Diffuse);
                    CHECK(math::Abs(matched.Visibility - dense.Visibility) <= 0.10);
                    CHECK(math::Abs(matched.Diffuse - dense.Diffuse) <= 0.10 * roofRemoved.Diffuse);
                    if (shutter == Shutter::Open && i == 0)
                    {
                        CHECK(dense.Diffuse <= 0.30 * roofRemoved.Diffuse);
                    }
                    if (shutter == Shutter::Open && i == 1)
                    {
                        CHECK(dense.Diffuse >= 0.40 * roofRemoved.Diffuse);
                    }
                }
            }
        }
    }
}

TEST_CASE("default graph resolves large World skylight at fixed floor probes",
          "[large-world-sky][gpu][reference-compute]")
{
    const bool shouldConstrainStorage = GENERATE(false, true);
    rhi::StorageLimits limits;
    if (shouldConstrainStorage == true)
    {
        limits.ResourceBytes = uint64{15} * 1024 * 1024; // admits the 1024 square and rejects the 2048 square
    }
    GraphGpuFixture fixture(limits);
    const core::UniquePtr<RenderGraphPackage> package =
        LoadPackage({LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY, "rg2_clustered_Default.lurgrt"});
    const RenderGraphPrograms programs =
        package->GetPrograms(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment());
    REQUIRE(PollAtmosphereGraph(
                fixture,
                fixture.Graph.Initialize(fixture.Device, package->GetPlan(), programs, nullptr, WIDTH, HEIGHT)) ==
            rhi::RasterStatus::Ready);
    const Panels panels = MakePanels();
    constexpr uint32 CORNERS = 4;
    using Corners = StaticArray<math::Vector3, CORNERS>;
    StaticArray<Corners, PANEL_COUNT> positions{};
    StaticArray<Corners, PANEL_COUNT> normals{};
    StaticArray<RenderGraphMesh, PANEL_COUNT> meshes{};
    StaticArray<RenderGraphItem, PANEL_COUNT> items{};
    const StaticArray<uint32, 6> indices{{0, 1, 2, 0, 2, 3}};
    const uint32 binding = 0;
    for (uint32 i = 0; i < PANEL_COUNT; ++i)
    {
        const Panel& panel = panels[i];
        const math::Vector3 u = panel.HalfUWorld;
        const math::Vector3 v = panel.HalfVWorld;
        const math::Vector3 centerWorld = panel.CenterWorld;
        positions[i] = {{centerWorld - u - v, centerWorld + u - v, centerWorld + u + v, centerWorld - u + v}};
        math::Vector3 normalWorld;
        REQUIRE(math::TryNormalize(math::Cross(u, v), normalWorld) == math::MathStatus::Success);
        normals[i] = {{normalWorld, normalWorld, normalWorld, normalWorld}};
        meshes[i].Geometry = {positions[i].GetData(), CORNERS, indices.GetData(), indices.GetSize()};
        meshes[i].Geometry.Normals = normals[i].GetData();
        meshes[i].MaterialBindings = {&binding, 1};
        items[i].SourceId = i + 1;
        items[i].MeshIndex = i;
    }
    StaticArray<SceneShaderMaterial, 2> materials{};
    materials[0].BaseColor = {1, 1, 1};
    materials[0].Roughness = 1;
    materials[1] = materials[0];
    const uint32 roofMaterial = 1;
    meshes[ROOF_INDEX].MaterialBindings = {&roofMaterial, 1};
    StaticArray<uint8, 4> roofTexel{{255, 255, 255, 255}};
    materials[1].IsAlphaMasked = true;
    materials[1].AlphaCutoff = 0.5F;
    materials[1].ColorTexture =
        {roofTexel.GetData(), roofTexel.GetSize(), roofTexel.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Srgb};
    SceneShaderInputs inputs;
    inputs.Materials = {materials.GetData(), materials.GetSize()};
    inputs.Environment = {UNIFORM_IRRADIANCE, UNIFORM_IRRADIANCE, UNIFORM_IRRADIANCE};
    ViewDescription& camera = fixture.Cameras[0];
    camera.OrthographicCamera = true;
    camera.CameraForward = {0, -1, 0};
    camera.Atmosphere.ShouldLightScene = true;
    camera.Atmosphere.SolarIlluminance = {SOLAR_ILLUMINANCE, SOLAR_ILLUMINANCE, SOLAR_ILLUMINANCE};
    const float32 horizontal = static_cast<float32>(std::sqrt(1 - SOLAR_SINE * SOLAR_SINE));
    camera.Atmosphere.DirectionToSunWorld = {0, SOLAR_SINE, horizontal};
    internal::AtmosphereSkyTables tables;
    REQUIRE(tables.TryBuild(.001, SOLAR_SINE, true) == true);
    uint64 revision = 0;
    uint64 content = 0;
    uint64 previousSkyRevision = 0;
    for (Shutter shutter :
         {Shutter::RoofRemoved, Shutter::Open, Shutter::Closed, Shutter::Narrow, Shutter::RoofRemoved})
    {
        // Repeat the exposed control by editing an admitted caster's mask while retaining its geometry.
        const bool isTransparentRoof = content != 0 && shutter == Shutter::RoofRemoved;
        roofTexel[3] = isTransparentRoof == true ? 0 : 255;
        ++content;
        uint64 skyRevision = 0;
        items[SHUTTER_INDEX].MeshToWorld.Translation.X = GetShutterTranslation(shutter);
        const usize count = shutter == Shutter::RoofRemoved && isTransparentRoof == false ? ROOF_INDEX : PANEL_COUNT;
        constexpr int32 UNIFORM_SKY = -180;
        for (int32 degrees : {UNIFORM_SKY, -60, -30, 0, 30, 60})
        {
            const bool hasAtmosphere = degrees != UNIFORM_SKY;
            if (hasAtmosphere == true && degrees != 0 && shutter != Shutter::Narrow)
            {
                continue;
            }
            const float64 azimuth = static_cast<float64>(degrees) * math::kPi / 180;
            camera.Atmosphere.DirectionToSunWorld = {horizontal * static_cast<float32>(std::sin(azimuth)),
                                                     SOLAR_SINE,
                                                     horizontal * static_cast<float32>(std::cos(azimuth))};
            camera.Atmosphere.IsEnabled = hasAtmosphere;
            StaticArray<Integral, PROBE_COUNT> matchedRays{};
            StaticArray<Integral, PROBE_COUNT> denseRays{};
            StaticArray<Integral, PROBE_COUNT> exposedRays{};
            if (shouldConstrainStorage == false)
            {
                for (uint32 i = 0; i < PROBE_COUNT; ++i)
                {
                    const internal::AtmosphereSkyTables* tablesOrNull = hasAtmosphere == true ? &tables : nullptr;
                    const math::Vector3d receiverWorld{static_cast<float64>(PROBE_X[i]),
                                                       0,
                                                       static_cast<float64>(PROBE_Z[i])};
                    const math::Vector3d sunWorld = ToVector3d(camera.Atmosphere.DirectionToSunWorld);
                    matchedRays[i] = Integrate(panels, shutter, receiverWorld, 0, tablesOrNull, sunWorld);
                    denseRays[i] = Integrate(panels, shutter, receiverWorld, DENSE_RESOLUTION, tablesOrNull, sunWorld);
                    exposedRays[i] = Integrate(panels,
                                               Shutter::RoofRemoved,
                                               receiverWorld,
                                               DENSE_RESOLUTION,
                                               tablesOrNull,
                                               sunWorld);
                }
            }
            for (DeferredLightingView view : {DeferredLightingView::SkyVisibility, DeferredLightingView::SkyDiffuse})
            {
                camera.LightingView = view;
                StaticArray<float64, PROBE_COUNT> initial{};
                for (int32 walk : {0, 4, -4})
                {
                    // One quarter metre per pixel. Translate by whole pixels so the pixel centres
                    // still sample the exact same receivers. Clip the primary rays below the roof.
                    const float32 centerX = static_cast<float32>(walk) * 0.25F - 0.125F;
                    camera.CameraPosition = {centerX, 1, -0.125F};
                    camera.WorldToClip.Columns[0] = {1.0F / 12, 0, 0, 0};
                    camera.WorldToClip.Columns[1] = {0, 0, -0.5F, 0};
                    camera.WorldToClip.Columns[2] = {0, -1.0F / 8, 0, 0};
                    camera.WorldToClip.Columns[3] = {-centerX / 12, -0.125F / 8, 0.5F, 1};
                    RenderGraphReport report;
                    const rhi::RasterStatus update = fixture.Graph.Update({meshes.GetData(), PANEL_COUNT},
                                                                          {items.GetData(), count},
                                                                          {&camera, 1},
                                                                          ++revision,
                                                                          RenderGraphCoveragePolicy::RejectIncomplete,
                                                                          report,
                                                                          &inputs,
                                                                          content,
                                                                          {1, content, content, 1, 1});
                    if (update == rhi::RasterStatus::Pending)
                    {
                        CHECK(fixture.Graph.GetSkyVisibilityReport().Revision == previousSkyRevision);
                    }
                    REQUIRE(PollAtmosphereGraph(fixture, update) == rhi::RasterStatus::Ready);
                    ReadAtmosphereFrame(fixture);
                    ReadAtmosphereFrame(fixture);
                    const SkyVisibilityReport sky = fixture.Graph.GetSkyVisibilityReport();
                    REQUIRE(sky.WholeWorldMaps == SKY_VISIBILITY_DIRECTION_COUNT);
                    CHECK(sky.CoarseMetresPerTexel > 100);
                    CHECK(sky.HasBudgetFallback == shouldConstrainStorage);
                    if (shouldConstrainStorage == true)
                    {
                        CHECK(sky.FinestSubmittedLevel == SkyVisibilityLevel::WholeWorld);
                        CHECK(sky.LocalMaps == 0);
                        CHECK(sky.LocalMetresPerTexel == 0);
                        return;
                    }
                    CHECK(sky.FinestSubmittedLevel == SkyVisibilityLevel::Local);
                    CHECK(sky.LocalMaps == SKY_VISIBILITY_DIRECTION_COUNT);
                    CHECK(sky.LocalMetresPerTexel > 0);
                    CHECK(sky.LocalMetresPerTexel < 0.13F);
                    if (skyRevision == 0)
                    {
                        REQUIRE(sky.Revision != previousSkyRevision);
                        skyRevision = sky.Revision;
                    }
                    CHECK(sky.Revision == skyRevision);
                    previousSkyRevision = skyRevision;
                    for (uint32 i = 0; i < PROBE_COUNT; ++i)
                    {
                        const int32 row = static_cast<int32>(HEIGHT / 2) + static_cast<int32>(4 * PROBE_Z[i]);
                        const int32 column = static_cast<int32>(WIDTH / 2) - walk + static_cast<int32>(4 * PROBE_X[i]);
                        const usize pixel = GetPixel(static_cast<usize>(column), static_cast<usize>(row));
                        const float64 actual = ComputeLuminance({static_cast<float64>(fixture.Pixels[pixel]),
                                                                 static_cast<float64>(fixture.Pixels[pixel + 1]),
                                                                 static_cast<float64>(fixture.Pixels[pixel + 2])}) /
                                               255;
                        if (walk != 0)
                        {
                            CHECK(math::Abs(actual - initial[i]) <= 1.0 / 255);
                            continue;
                        }
                        initial[i] = actual;
                        const Integral& matched = matchedRays[i];
                        const Integral& dense = denseRays[i];
                        const Integral& control = exposedRays[i];
                        const bool isVisibility = view == DeferredLightingView::SkyVisibility;
                        const float64 expected = isVisibility == true ? dense.Visibility : dense.Diffuse;
                        const float64 tolerance = isVisibility == true ? 0.10 : 0.10 * control.Diffuse;
                        CAPTURE(degrees,
                                shutter,
                                i,
                                hasAtmosphere,
                                isVisibility,
                                actual,
                                matched.Visibility,
                                matched.Diffuse,
                                expected,
                                tolerance);
                        CHECK(math::Abs(actual - expected) <= tolerance);
                    }
                }
            }
        }
    }
    // A changed environment rebuilds its diffuse binding while the submitted shadow colour stays shared.
    camera.Atmosphere.IsEnabled = false;
    camera.LightingView = DeferredLightingView::SkyDiffuse;
    inputs.Environment = {1, 1, 1};
    RenderGraphReport changedSky;
    REQUIRE(PollAtmosphereGraph(fixture,
                                fixture.Graph.Update({meshes.GetData(), PANEL_COUNT},
                                                     {items.GetData(), PANEL_COUNT},
                                                     {&camera, 1},
                                                     ++revision,
                                                     RenderGraphCoveragePolicy::RejectIncomplete,
                                                     changedSky,
                                                     &inputs,
                                                     content,
                                                     {1, content, content, 1, 2})) == rhi::RasterStatus::Ready);
    CHECK(fixture.Graph.GetSkyVisibilityReport().Revision == previousSkyRevision);
    CHECK(fixture.Graph.GetSkyVisibilityReport().LocalMaps == SKY_VISIBILITY_DIRECTION_COUNT);
    ReadAtmosphereFrame(fixture);
    // A rejected replacement leaves both the drawable atlas and the last valid floor probes usable.
    items[0].MeshIndex = PANEL_COUNT;
    RenderGraphReport rejection;
    const rhi::RasterStatus rejected = fixture.Graph.Update({meshes.GetData(), PANEL_COUNT},
                                                            {items.GetData(), PANEL_COUNT},
                                                            {&camera, 1},
                                                            ++revision,
                                                            RenderGraphCoveragePolicy::RejectIncomplete,
                                                            rejection,
                                                            &inputs,
                                                            ++content);
    REQUIRE(rejected != rhi::RasterStatus::Ready);
    REQUIRE(rejected != rhi::RasterStatus::Pending);
    CHECK(fixture.Graph.GetSkyVisibilityReport().Revision == previousSkyRevision);
    const StaticArray<uint8, PIXEL_BYTES> accepted = fixture.Pixels;
    ReadAtmosphereFrame(fixture);
    CHECK(std::equal(accepted.GetData(), accepted.GetData() + PIXEL_BYTES, fixture.Pixels.GetData()));
    // Keep the receiver, shutter and local window fixed. The ridge is outside the camera
    // and the local square but intersects sky rays before they reach the floor.
    items[0].MeshIndex = 0;
    constexpr uint32 RIDGE_INDEX = 1;
    constexpr uint32 OUTSIDE_PROBE = 2;
    camera.LightingView = DeferredLightingView::SkyVisibility;
    const math::Vector3d outsideWorld{static_cast<float64>(PROBE_X[OUTSIDE_PROBE]),
                                      0,
                                      static_cast<float64>(PROBE_Z[OUTSIDE_PROBE])};
    Panels withoutRidge = panels;
    withoutRidge[RIDGE_INDEX].HalfUWorld = {};
    const float64 oracleChange =
        Integrate(withoutRidge, Shutter::RoofRemoved, outsideWorld, 0, nullptr, {}).Visibility -
        Integrate(panels, Shutter::RoofRemoved, outsideWorld, 0, nullptr, {}).Visibility;
    float64 withRidge = 0;
    for (bool isRidgeExcluded : {false, true})
    {
        items[RIDGE_INDEX].IsIntentionallyExcluded = isRidgeExcluded;
        RenderGraphReport ridgeReport;
        REQUIRE(PollAtmosphereGraph(fixture,
                                    fixture.Graph.Update({meshes.GetData(), PANEL_COUNT},
                                                         {items.GetData(), PANEL_COUNT},
                                                         {&camera, 1},
                                                         ++revision,
                                                         RenderGraphCoveragePolicy::RejectIncomplete,
                                                         ridgeReport,
                                                         &inputs,
                                                         ++content)) == rhi::RasterStatus::Ready);
        ReadAtmosphereFrame(fixture);
        ReadAtmosphereFrame(fixture);
        const math::Vector4 clip =
            camera.WorldToClip * math::Vector4{PROBE_X[OUTSIDE_PROBE], 0, PROBE_Z[OUTSIDE_PROBE], 1};
        const usize column = static_cast<usize>((0.5F + 0.5F * clip.X / clip.W) * static_cast<float32>(WIDTH));
        const usize row = static_cast<usize>((0.5F - 0.5F * clip.Y / clip.W) * static_cast<float32>(HEIGHT));
        const float64 value = static_cast<float64>(fixture.Pixels[GetPixel(column, row)]) / 255;
        if (isRidgeExcluded == false)
        {
            withRidge = value;
        }
        else
        {
            CAPTURE(value, withRidge, oracleChange);
            REQUIRE(oracleChange > 2.0 / 255);
            CHECK(math::Abs(value - withRidge - oracleChange) <= 2.0 / 255);
        }
    }
}
} // namespace
} // namespace ludus::graphics::renderer::testing
