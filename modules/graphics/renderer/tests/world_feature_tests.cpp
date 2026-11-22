// World features: the single owner of each feature's text and detection (world_feature.hpp), the authoring guide's
// table that repeats it, and the CPU tables ray kernels read (render_graph_world_features.cpp).

#include "../shaders/renderer_gpu_preview_layout.h"
#include "../src/internal/atmosphere_sky_tables.h"
#include "../src/internal/render_graph_world_features.h"
#include "earth_sky_quadrature.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/world_feature.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;

namespace
{
constexpr usize CHANNEL_COUNT = 3;
constexpr float64 METRES_PER_KILOMETRE = 1000;
constexpr StaticArray<renderer::WorldFeature, renderer::WORLD_FEATURE_COUNT> FEATURES{
    {renderer::WorldFeature::AtmosphereSky, renderer::WorldFeature::AerialPerspective, renderer::WorldFeature::Aurora}};

renderer::AtmosphereDescription MakeAtmosphere()
{
    constexpr float32 SOLAR_ILLUMINANCE = 3;
    constexpr float32 SUN_ELEVATION_SINE = 0.6F;
    constexpr float32 SUN_HORIZONTAL = 0.8F;
    renderer::AtmosphereDescription atmosphere;
    atmosphere.IsEnabled = true;
    atmosphere.ShouldLightScene = true;
    atmosphere.DirectionToSunWorld = {SUN_HORIZONTAL, SUN_ELEVATION_SINE, 0};
    atmosphere.SolarIlluminance = {SOLAR_ILLUMINANCE, SOLAR_ILLUMINANCE, SOLAR_ILLUMINANCE};
    atmosphere.SunSourceId = 7;
    return atmosphere;
}

float32 GetChannel(math::Vector4 value, usize channel)
{
    return channel == 0 ? value.X : (channel == 1 ? value.Y : value.Z);
}

std::string ReadGuide()
{
    std::string text;
    std::FILE* fileOrNull = std::fopen(LUDUS_RENDER_GRAPH_AUTHORING_GUIDE, "rb");
    if (fileOrNull == nullptr)
    {
        return text;
    }
    constexpr usize CHUNK_BYTES = 4096;
    StaticArray<char, CHUNK_BYTES> chunk{};
    usize read = 0;
    while ((read = std::fread(chunk.GetData(), 1, chunk.GetSize(), fileOrNull)) != 0)
    {
        text.append(chunk.GetData(), read);
    }
    static_cast<void>(std::fclose(fileOrNull));
    return text;
}
} // namespace

TEST_CASE("World features describe themselves and their available scene functions", "[render-graph][world-features]")
{
    for (usize i = 0; i < FEATURES.GetSize(); ++i)
    {
        const renderer::WorldFeatureDescription description = renderer::DescribeWorldFeature(FEATURES[i]);
        INFO("feature " << i);
        CHECK(std::string_view(description.Name).empty() == false);
        const bool hasRayImplementation = FEATURES[i] != renderer::WorldFeature::Aurora;
        CHECK((std::string_view(description.Function).find('(') != std::string_view::npos) == hasRayImplementation);
        CHECK(std::string_view(description.Callback).empty() == false);
        CHECK(std::string_view(description.Omission).empty() == false);
        CHECK(std::string_view(description.Approximation).empty() == (hasRayImplementation == false));
        CHECK(renderer::GetWorldFeatureBit(FEATURES[i]) == (renderer::WorldFeatureMask{1} << i));
        for (usize j = 0; j < i; ++j)
        {
            CHECK(std::string_view(description.Name) != renderer::DescribeWorldFeature(FEATURES[j]).Name);
        }
    }
    CHECK(std::string_view(renderer::DescribeWorldFeature(static_cast<renderer::WorldFeature>(255)).Name).empty() ==
          true);
    CHECK(std::string_view(renderer::DescribeWorldFeatureSupport(renderer::WorldFeatureSupport::NotRendered)) ==
          "Not rendered");
    CHECK(std::string_view(renderer::WORLD_FEATURE_WARNING_CODE) == "RenderGraphFeatureNotRendered");
}

TEST_CASE("World-feature support follows the compiled kernel's calls", "[render-graph][world-features]")
{
    renderer::RenderGraphPrograms programs;
    for (const renderer::WorldFeature feature : FEATURES)
    {
        CHECK(renderer::GetWorldFeatureSupport(programs, feature) == renderer::WorldFeatureSupport::NotRendered);
    }
    // Reflection kept the sky table: the kernel calls EvaluateEnvironment, but not ApplyAerialPerspective.
    programs.HasSceneShaderInterface = true;
    programs.HasAtmosphereSky = true;
    CHECK(renderer::GetWorldFeatureSupport(programs, renderer::WorldFeature::AtmosphereSky) ==
          renderer::WorldFeatureSupport::Approximated);
    CHECK(renderer::GetWorldFeatureSupport(programs, renderer::WorldFeature::AerialPerspective) ==
          renderer::WorldFeatureSupport::NotRendered);
    programs.HasAerialPerspective = true;
    CHECK(renderer::GetWorldFeatureSupport(programs, renderer::WorldFeature::AerialPerspective) ==
          renderer::WorldFeatureSupport::Approximated);
    // The clustered deferred resolve composes the whole Atmosphere.
    renderer::RenderGraphPrograms deferred;
    deferred.HasClusteredDeferred = true;
    for (const renderer::WorldFeature feature : FEATURES)
    {
        CHECK(renderer::GetWorldFeatureSupport(deferred, feature) == renderer::WorldFeatureSupport::Supported);
    }
    CHECK(renderer::GetWorldFeatureSupport(programs, static_cast<renderer::WorldFeature>(255)) ==
          renderer::WorldFeatureSupport::NotRendered);
}

TEST_CASE("The authoring guide's World features table repeats the renderer's descriptions",
          "[render-graph][world-features]")
{
    const std::string guide = ReadGuide();
    REQUIRE(guide.empty() == false);
    CHECK(guide.find(renderer::WORLD_FEATURE_WARNING_CODE) != std::string::npos);
    for (const renderer::WorldFeature feature : FEATURES)
    {
        const renderer::WorldFeatureDescription description = renderer::DescribeWorldFeature(feature);
        const std::string function =
            *description.Function != '\0' ? std::string("`") + description.Function + "`" : "—";
        const std::string row = std::string("| ") + description.Name + " | " + function + " | ";
        INFO("expected a table row starting " << row);
        const usize start = guide.find(row);
        REQUIRE(start != std::string::npos);
        const std::string line = guide.substr(start, guide.find('\n', start) - start);
        CHECK(line.find(description.Callback) != std::string::npos);
        CHECK(line.find(description.Omission) != std::string::npos);
        CHECK(line.find(description.Approximation) != std::string::npos);
    }
}

TEST_CASE("The aerial perspective table starts clear and matches analytic optical depth",
          "[render-graph][world-features][atmosphere]")
{
    const renderer::AtmosphereDescription atmosphere = MakeAtmosphere();
    Array<math::Vector4> table;
    REQUIRE(table.TryResize(renderer::internal::AERIAL_PERSPECTIVE_TABLE_WORDS) == true);
    // One metre above the anchor, so the observer is inside the shell.
    const math::Vector3 eyeWorld{0, 1, 0};
    REQUIRE(renderer::internal::ResolveAtmosphereAerialPerspective(atmosphere,
                                                                   eyeWorld,
                                                                   {table.GetData(), table.GetSize()}) ==
            renderer::AtmosphereEnvironmentStatus::Ready);
    CHECK(table[0].X == 1);
    CHECK(table[1].Y == 1);
    CHECK(std::abs(table[2].Y - atmosphere.DirectionToSunWorld.Y) < 1e-6F);
    constexpr usize AZIMUTHS = LUDUS_AERIAL_PERSPECTIVE_AZIMUTH_COUNT;
    constexpr usize ZENITHS = LUDUS_AERIAL_PERSPECTIVE_ZENITH_COUNT;
    constexpr usize SLICES = LUDUS_AERIAL_PERSPECTIVE_SLICE_COUNT;
    const auto load = [&table](usize slice, usize row, usize column, usize word) {
        return table[LUDUS_AERIAL_PERSPECTIVE_HEADER_WORDS +
                     ((slice * ZENITHS + row) * AZIMUTHS + column) * LUDUS_AERIAL_PERSPECTIVE_ENTRY_WORDS + word];
    };
    // A zero-length segment changes nothing; longer ones scatter more and transmit less.
    for (usize row = 0; row < ZENITHS; ++row)
    {
        const math::Vector4 scattered = load(0, row, 0, 0);
        const math::Vector4 transmitted = load(0, row, 0, 1);
        CHECK(scattered.X == 0);
        CHECK(transmitted.X == 1);
        for (usize slice = 1; slice < SLICES; ++slice)
        {
            for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
            {
                CHECK(GetChannel(load(slice, row, 0, 0), channel) >= GetChannel(load(slice - 1, row, 0, 0), channel));
                CHECK(GetChannel(load(slice, row, 0, 1), channel) <= GetChannel(load(slice - 1, row, 0, 1), channel));
            }
        }
    }
    // Row zero looks straight up: integrate the extinction of the independent quadrature directly.
    constexpr usize QUADRATURE_STEPS = 20000;
    const float64 heightKilometres = 1 / METRES_PER_KILOMETRE;
    for (const usize slice : {usize{4}, usize{12}, usize{SLICES - 1}})
    {
        const float64 fraction = static_cast<float64>(slice) / (SLICES - 1);
        const float64 distance = LUDUS_AERIAL_PERSPECTIVE_MAX_METRES / METRES_PER_KILOMETRE * fraction * fraction;
        for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
        {
            float64 opticalDepth = 0;
            const float64 step = distance / QUADRATURE_STEPS;
            for (usize i = 0; i < QUADRATURE_STEPS; ++i)
            {
                opticalDepth +=
                    renderer::testing::ComputeEarthExtinction(heightKilometres + (static_cast<float64>(i) + 0.5) * step,
                                                              channel) *
                    step;
            }
            INFO("slice " << slice << " channel " << channel);
            constexpr float64 RELATIVE_TOLERANCE = 0.01;
            const float64 expected = std::exp(-opticalDepth);
            const float64 actual = static_cast<float64>(GetChannel(load(slice, 0, 0, 1), channel));
            CHECK(std::abs(actual - expected) <= RELATIVE_TOLERANCE * expected);
        }
    }
}

TEST_CASE("Ray kernels see the Atmosphere sky in place of the World environment and its dimmed Sun",
          "[render-graph][world-features][atmosphere]")
{
    const renderer::AtmosphereDescription atmosphere = MakeAtmosphere();
    StaticArray<renderer::DirectLight, 2> lights{};
    lights[0].SourceId = atmosphere.SunSourceId;
    lights[0].Kind = renderer::LightKind::Directional;
    lights[0].Intensity = atmosphere.SolarIlluminance.X;
    lights[1].SourceId = atmosphere.SunSourceId + 1;
    lights[1].Kind = renderer::LightKind::Point;
    constexpr float32 POINT_INTENSITY = 5;
    lights[1].Intensity = POINT_INTENSITY;
    renderer::SceneShaderInputs inputs;
    inputs.Lights = {lights.GetData(), lights.GetSize()};
    constexpr float32 WORLD_ENVIRONMENT = 0.25F;
    inputs.Environment = {WORLD_ENVIRONMENT, WORLD_ENVIRONMENT, WORLD_ENVIRONMENT};
    const math::Vector3 eyeWorld{0, 1, 0};

    renderer::internal::RenderGraphWorldTables tables;
    REQUIRE(tables.Resolve(atmosphere, eyeWorld, true, true, inputs) == rhi::RasterStatus::Ready);
    REQUIRE(tables.HasSky() == true);
    const BorrowedView<const math::Vector4> sky = tables.GetSkyTable();
    REQUIRE(sky.GetSize() == renderer::internal::ATMOSPHERE_SKY_TABLE_WORDS);
    CHECK(sky[0].X == LUDUS_ATMOSPHERE_SKY_TABLE_WIDTH);
    CHECK(sky[0].Y == LUDUS_ATMOSPHERE_SKY_TABLE_HEIGHT);
    // The table holds exactly what the RT1 reference's sky holds.
    Array<math::Vector4> expected;
    REQUIRE(expected.TryResize(sky.GetSize() - LUDUS_ATMOSPHERE_SKY_HEADER_WORDS) == true);
    math::Vector3 sunIlluminance;
    REQUIRE(renderer::ResolveAtmosphereEnvironment(atmosphere,
                                                   eyeWorld,
                                                   LUDUS_ATMOSPHERE_SKY_TABLE_WIDTH,
                                                   LUDUS_ATMOSPHERE_SKY_TABLE_HEIGHT,
                                                   {expected.GetData(), expected.GetSize()},
                                                   sunIlluminance) == renderer::AtmosphereEnvironmentStatus::Ready);
    constexpr usize SAMPLE_STRIDE = 997;
    for (usize i = 0; i < expected.GetSize(); i += SAMPLE_STRIDE)
    {
        CHECK(sky[LUDUS_ATMOSPHERE_SKY_HEADER_WORDS + i].X == expected[i].X);
    }
    const renderer::SceneShaderInputs& substituted = tables.GetInputs();
    REQUIRE(substituted.Lights.GetSize() == lights.GetSize());
    const renderer::DirectLight& sun = substituted.Lights[0];
    CHECK(sun.Intensity == math::Max(sunIlluminance.X, math::Max(sunIlluminance.Y, sunIlluminance.Z)));
    CHECK(sun.Intensity < atmosphere.SolarIlluminance.X);
    CHECK(std::abs(sun.Color.X * sun.Intensity - sunIlluminance.X) < 1e-5F);
    CHECK(substituted.Lights[1].Intensity == POINT_INTENSITY);
    CHECK(substituted.EnvironmentPixels.IsEmpty() == true);
    CHECK(substituted.Environment.Z > 0);
    CHECK(substituted.Environment != inputs.Environment);
    CHECK(tables.GetAerialPerspectiveTable()[0].X == 1);

    // A camera above the shell keeps the World environment and an undimmed Sun, as the raster does.
    const math::Vector3 orbitWorld{0, static_cast<float32>(renderer::GetAtmosphereTopAltitude() * 2), 0};
    REQUIRE(tables.Resolve(atmosphere, orbitWorld, true, true, inputs) == rhi::RasterStatus::Ready);
    CHECK(tables.HasSky() == false);
    CHECK(tables.GetSkyTable()[0].X == 0);
    CHECK(tables.GetAerialPerspectiveTable()[0].X == 0);
    CHECK(tables.GetInputs().Environment == inputs.Environment);
    CHECK(tables.GetInputs().Lights[0].Intensity == lights[0].Intensity);
    // Kernels that read neither table get none.
    REQUIRE(tables.Resolve(atmosphere, eyeWorld, false, false, inputs) == rhi::RasterStatus::Ready);
    CHECK(tables.GetSkyTable().IsEmpty() == true);
    CHECK(tables.GetAerialPerspectiveTable().IsEmpty() == true);
    // Invalid Atmosphere fields reject the scene, as the raster does.
    renderer::AtmosphereDescription invalid = atmosphere;
    invalid.DirectionToSunWorld = {};
    CHECK(tables.Resolve(invalid, eyeWorld, true, false, inputs) == rhi::RasterStatus::InvalidDescription);
}
