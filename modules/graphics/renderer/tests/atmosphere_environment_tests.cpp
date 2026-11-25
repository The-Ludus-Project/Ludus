// CPU reference sky: the raster's clear-sky tables evaluated on the CPU (atmosphere_environment.cpp)
// against independent quadrature, analytic optical depth, and the public resolution contract.

#include "../shaders/renderer_atmosphere_layout.h"
#include "../src/internal/atmosphere_sky_tables.h"
#include "../src/internal/render_graph_atmosphere.h"
#include "earth_sky_quadrature.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/tone_mapping.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;
namespace renderer = ludus::graphics::renderer;

namespace
{
constexpr usize CHANNEL_COUNT = 3;
constexpr float64 METRES_PER_KILOMETRE = 1000;

float64 GetChannel(math::Vector3d value, usize channel)
{
    return channel == 0 ? value.X : (channel == 1 ? value.Y : value.Z);
}

float32 GetChannel(math::Vector3 value, usize channel)
{
    return channel == 0 ? value.X : (channel == 1 ? value.Y : value.Z);
}

renderer::AtmosphereDescription MakeAtmosphere(math::Vector3 directionToSunWorld)
{
    constexpr float32 SOLAR_ILLUMINANCE = 3;
    renderer::AtmosphereDescription atmosphere;
    atmosphere.IsEnabled = true;
    atmosphere.ShouldLightScene = true;
    atmosphere.DirectionToSunWorld = directionToSunWorld;
    atmosphere.SolarIlluminance = {SOLAR_ILLUMINANCE, SOLAR_ILLUMINANCE, SOLAR_ILLUMINANCE};
    return atmosphere;
}
} // namespace

TEST_CASE("CPU reference sky matches independent single-scattering quadrature", "[atmosphere]")
{
    // The configurations of the GPU clear-sky pixel test, which checks the raster tables against the same quadrature.
    const float64 altitudeMetres = GENERATE(0.0, 2000.0);
    const float64 solarElevation = GENERATE(1.0, 0.10, -0.04);
    const float64 heightKilometres = altitudeMetres / METRES_PER_KILOMETRE;
    renderer::internal::AtmosphereSkyTables tables;
    REQUIRE(tables.TryBuild(heightKilometres, solarElevation, false) == true);
    const math::Vector3d upWorld{0, 1, 0};
    const math::Vector3d sunWorld{std::sqrt(1 - solarElevation * solarElevation), solarElevation, 0};
    constexpr float64 OBLIQUE_SINE = 0.5;
    const float64 obliqueCosine = std::sqrt(1 - OBLIQUE_SINE * OBLIQUE_SINE);
    for (const math::Vector3d directionWorld : {math::Vector3d{0, 0, -1},
                                                math::Vector3d{0, OBLIQUE_SINE, -obliqueCosine},
                                                math::Vector3d{-obliqueCosine, OBLIQUE_SINE, 0},
                                                math::Vector3d{0, 1, 0}})
    {
        const math::Vector3d actual = tables.SampleSky(directionWorld, upWorld, sunWorld);
        for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
        {
            const float64 expected = IntegrateEarthSky(heightKilometres, directionWorld, sunWorld, channel).Radiance;
            INFO("altitude " << altitudeMetres << " solar elevation " << solarElevation << " direction "
                             << directionWorld.X << ", " << directionWorld.Y << ", " << directionWorld.Z << " channel "
                             << channel);
            // Table parameterization, bilinear lookup and the 80-step ray march, as for the GPU tables.
            constexpr float64 RELATIVE_TOLERANCE = 0.08;
            constexpr float64 ABSOLUTE_TOLERANCE = 1e-6;
            CHECK(std::abs(GetChannel(actual, channel) - expected) <=
                  ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * expected);
        }
    }
}

TEST_CASE("CPU reference sky's multiple scattering matches independent higher orders", "[atmosphere]")
{
    renderer::internal::AtmosphereSkyTables single;
    renderer::internal::AtmosphereSkyTables multiple;
    REQUIRE(single.TryBuild(0, 1, false) == true);
    REQUIRE(multiple.TryBuild(0, 1, true) == true);
    const math::Vector3d horizontalWorld{0, 0, -1};
    const math::Vector3d upWorld{0, 1, 0};
    const float64 higherOrders =
        multiple.SampleSky(horizontalWorld, upWorld, upWorld).Z - single.SampleSky(horizontalWorld, upWorld, upWorld).Z;
    const float64 expected = IntegrateEarthBlueHigherOrders();
    CAPTURE(higherOrders, expected);
    CHECK(higherOrders > 0);
    // Hillaire's isotropic local closure, with the GPU pixel test's tolerance.
    constexpr float64 RELATIVE_TOLERANCE = 0.20;
    CHECK(std::abs(higherOrders - expected) <= RELATIVE_TOLERANCE * expected);
}

TEST_CASE("CPU reference Sun transmittance matches analytic zenith optical depth", "[atmosphere]")
{
    renderer::internal::AtmosphereSkyTables tables;
    REQUIRE(tables.TryBuild(0, 1, true) == true);
    const math::Vector3d transmittance = tables.SampleTransmittance(0, 1);
    constexpr StaticArray<float64, CHANNEL_COUNT> RAYLEIGH{{LUDUS_ATMOSPHERE_RAYLEIGH_SCATTERING_RED,
                                                            LUDUS_ATMOSPHERE_RAYLEIGH_SCATTERING_GREEN,
                                                            LUDUS_ATMOSPHERE_RAYLEIGH_SCATTERING_BLUE}};
    constexpr StaticArray<float64, CHANNEL_COUNT> OZONE{{LUDUS_ATMOSPHERE_OZONE_ABSORPTION_RED,
                                                         LUDUS_ATMOSPHERE_OZONE_ABSORPTION_GREEN,
                                                         LUDUS_ATMOSPHERE_OZONE_ABSORPTION_BLUE}};
    constexpr float64 TOP = LUDUS_ATMOSPHERE_HEIGHT_KILOMETRES;
    constexpr float64 RAYLEIGH_HEIGHT = LUDUS_ATMOSPHERE_RAYLEIGH_SCALE_HEIGHT_KILOMETRES;
    constexpr float64 AEROSOL_HEIGHT = LUDUS_ATMOSPHERE_AEROSOL_SCALE_HEIGHT_KILOMETRES;
    // The ozone tent has unit peak and a base of twice its half width, so its area is the half width.
    constexpr float64 OZONE_COLUMN = LUDUS_ATMOSPHERE_OZONE_HALF_WIDTH_KILOMETRES;
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        const float64 depth =
            RAYLEIGH[channel] * RAYLEIGH_HEIGHT * (1 - std::exp(-TOP / RAYLEIGH_HEIGHT)) +
            LUDUS_ATMOSPHERE_AEROSOL_EXTINCTION * AEROSOL_HEIGHT * (1 - std::exp(-TOP / AEROSOL_HEIGHT)) +
            OZONE[channel] * OZONE_COLUMN;
        CAPTURE(channel, GetChannel(transmittance, channel), std::exp(-depth));
        constexpr float64 RELATIVE_TOLERANCE = 0.002;
        CHECK(std::abs(GetChannel(transmittance, channel) - std::exp(-depth)) <= RELATIVE_TOLERANCE * std::exp(-depth));
    }
}

TEST_CASE("Atmosphere environment resolution rejects inputs and preserves outputs", "[atmosphere]")
{
    constexpr uint32 WIDTH = 8;
    constexpr uint32 HEIGHT = WIDTH / 2;
    constexpr usize PIXEL_COUNT = static_cast<usize>(WIDTH) * HEIGHT;
    constexpr float32 SENTINEL = -7;
    StaticArray<math::Vector4, PIXEL_COUNT> pixels{};
    for (math::Vector4& pixel : pixels)
    {
        pixel = {SENTINEL, SENTINEL, SENTINEL, SENTINEL};
    }
    math::Vector3 sun{SENTINEL, SENTINEL, SENTINEL};
    const renderer::AtmosphereDescription valid = MakeAtmosphere({0, 1, 0});
    const auto resolve =
        [&](const renderer::AtmosphereDescription& atmosphere, math::Vector3 eyeWorld, uint32 width, usize count) {
            return renderer::ResolveAtmosphereEnvironment(atmosphere,
                                                          eyeWorld,
                                                          width,
                                                          HEIGHT,
                                                          {pixels.GetData(), count},
                                                          sun);
        };
    using Status = renderer::AtmosphereEnvironmentStatus;
    renderer::AtmosphereDescription disabled = valid;
    disabled.IsEnabled = false;
    CHECK(resolve(disabled, {}, WIDTH, PIXEL_COUNT) == Status::NotLit);
    renderer::AtmosphereDescription unlit = valid;
    unlit.ShouldLightScene = false;
    CHECK(resolve(unlit, {}, WIDTH, PIXEL_COUNT) == Status::NotLit);
    // The raster applies no atmosphere surface lighting below the ground or above the shell.
    constexpr float32 BELOW_GROUND_METRES = -1;
    CHECK(resolve(valid, {0, BELOW_GROUND_METRES, 0}, WIDTH, PIXEL_COUNT) == Status::NotLit);
    constexpr float32 ORBIT_METRES = 200'000;
    CHECK(resolve(valid, {0, ORBIT_METRES, 0}, WIDTH, PIXEL_COUNT) == Status::NotLit);
    CHECK(resolve(valid, {}, 0, 0) == Status::InvalidDescription);
    CHECK(resolve(valid, {}, WIDTH, PIXEL_COUNT - 1) == Status::InvalidDescription);
    CHECK(resolve(valid, {}, renderer::MAX_ATMOSPHERE_ENVIRONMENT_SIDE + 1, PIXEL_COUNT) == Status::InvalidDescription);
    renderer::AtmosphereDescription noUp = valid;
    noUp.SurfaceUpWorld = {};
    CHECK(resolve(noUp, {}, WIDTH, PIXEL_COUNT) == Status::InvalidDescription);
    renderer::AtmosphereDescription noSun = valid;
    noSun.DirectionToSunWorld = {};
    CHECK(resolve(noSun, {}, WIDTH, PIXEL_COUNT) == Status::InvalidDescription);
    renderer::AtmosphereDescription negative = valid;
    negative.SolarIlluminance.Y = -1;
    CHECK(resolve(negative, {}, WIDTH, PIXEL_COUNT) == Status::InvalidDescription);
    for (const math::Vector4& pixel : pixels)
    {
        CHECK(pixel.X == SENTINEL);
        CHECK(pixel.W == SENTINEL);
    }
    CHECK(sun.X == SENTINEL);
}

TEST_CASE("Atmosphere environment holds the observer's sky and transmitted Sun", "[atmosphere]")
{
    constexpr uint32 WIDTH = 32;
    constexpr uint32 HEIGHT = WIDTH / 2;
    StaticArray<math::Vector4, static_cast<usize>(WIDTH) * HEIGHT> pixels{};
    // The Sun is thirty degrees up toward +X; the image's longitude zero faces it.
    constexpr float32 SOLAR_SINE = 0.5F;
    const float32 solarCosine = std::sqrt(1 - SOLAR_SINE * SOLAR_SINE);
    const renderer::AtmosphereDescription atmosphere = MakeAtmosphere({solarCosine, SOLAR_SINE, 0});
    math::Vector3 sun{};
    REQUIRE(renderer::ResolveAtmosphereEnvironment(atmosphere,
                                                   {},
                                                   WIDTH,
                                                   HEIGHT,
                                                   {pixels.GetData(), pixels.GetSize()},
                                                   sun) == renderer::AtmosphereEnvironmentStatus::Ready);
    for (const math::Vector4& pixel : pixels)
    {
        REQUIRE(math::IsFinite(pixel) == true);
        CHECK(pixel.X >= 0);
        CHECK(pixel.Y >= 0);
        CHECK(pixel.Z >= 0);
        CHECK(pixel.W == 1);
    }
    renderer::internal::AtmosphereSkyTables tables;
    REQUIRE(tables.TryBuild(0, static_cast<float64>(SOLAR_SINE), true) == true);
    const math::Vector3d transmittance = tables.SampleTransmittance(0, static_cast<float64>(SOLAR_SINE));
    for (usize channel = 0; channel < CHANNEL_COUNT; ++channel)
    {
        const float64 expected =
            static_cast<float64>(GetChannel(atmosphere.SolarIlluminance, channel)) * GetChannel(transmittance, channel);
        constexpr float64 RELATIVE_TOLERANCE = 1e-5;
        CHECK(std::abs(static_cast<float64>(GetChannel(sun, channel)) - expected) <= RELATIVE_TOLERANCE * expected);
    }
    // Red is attenuated least along the slant path.
    CHECK(sun.X > sun.Z);
    // The sky is symmetric about the Sun's vertical plane, the XY plane: longitude u mirrors to 1 - u.
    for (uint32 y = 0; y < HEIGHT; ++y)
    {
        for (uint32 x = 0; x < WIDTH; ++x)
        {
            const math::Vector4 pixel = pixels[static_cast<usize>(y) * WIDTH + x];
            const math::Vector4 mirror = pixels[static_cast<usize>(y) * WIDTH + (WIDTH - 1 - x)];
            constexpr float32 MIRROR_TOLERANCE = 1e-4F;
            CHECK(math::Abs(pixel.Z - mirror.Z) <= MIRROR_TOLERANCE * math::Max(pixel.Z, mirror.Z));
        }
    }
    // Aerosol forward scattering brightens the sky toward the Sun at its elevation.
    constexpr uint32 SUN_ROW = HEIGHT / 3;
    CHECK(pixels[static_cast<usize>(SUN_ROW) * WIDTH].X > pixels[static_cast<usize>(SUN_ROW) * WIDTH + WIDTH / 2].X);
    // Clear daylight is blue overhead.
    CHECK(pixels[0].Z > pixels[0].X);

    // Twilight: the Sun is below the horizon and supplies no direct light, while the sky still scatters.
    constexpr float32 TWILIGHT_SINE = -0.05F;
    const renderer::AtmosphereDescription twilight =
        MakeAtmosphere({std::sqrt(1 - TWILIGHT_SINE * TWILIGHT_SINE), TWILIGHT_SINE, 0});
    REQUIRE(renderer::ResolveAtmosphereEnvironment(twilight,
                                                   {},
                                                   WIDTH,
                                                   HEIGHT,
                                                   {pixels.GetData(), pixels.GetSize()},
                                                   sun) == renderer::AtmosphereEnvironmentStatus::Ready);
    CHECK(sun == math::Vector3{});
    CHECK(pixels[0].Z > 0);
}

TEST_CASE("Atmosphere meter matches an independent integral of the resolved sky", "[atmosphere]")
{
    // The resolved latitude/longitude sky with +Y up, integrated by its texels' cosine-weighted solid angles.
    constexpr uint32 WIDTH = 256;
    constexpr uint32 HEIGHT = WIDTH / 2;
    Array<math::Vector4> pixels;
    REQUIRE(pixels.TryResize(static_cast<usize>(WIDTH) * HEIGHT) == true);
    const float32 solarSine = GENERATE(1.0F, 0.5F, 0.1F, -0.05F);
    const renderer::AtmosphereDescription atmosphere =
        MakeAtmosphere({std::sqrt(1 - solarSine * solarSine), solarSine, 0});
    math::Vector3 sun{};
    REQUIRE(renderer::ResolveAtmosphereEnvironment(atmosphere,
                                                   {},
                                                   WIDTH,
                                                   HEIGHT,
                                                   {pixels.GetData(), pixels.GetSize()},
                                                   sun) == renderer::AtmosphereEnvironmentStatus::Ready);
    const float64 texelAngle = math::kPi / HEIGHT;
    math::Vector3d expected{};
    for (uint32 y = 0; y < HEIGHT / 2; ++y)
    {
        const float64 polar = (static_cast<float64>(y) + 0.5) * texelAngle;
        const float64 weight = std::cos(polar) * std::sin(polar) * texelAngle * (2 * math::kPi / WIDTH);
        for (uint32 x = 0; x < WIDTH; ++x)
        {
            const math::Vector4 pixel = pixels[static_cast<usize>(y) * WIDTH + x];
            expected = expected + math::Vector3d{static_cast<float64>(pixel.X),
                                                 static_cast<float64>(pixel.Y),
                                                 static_cast<float64>(pixel.Z)} *
                                      weight;
        }
    }
    const float64 sunCosine = math::Max(static_cast<float64>(solarSine), 0.0);
    expected = expected +
               math::Vector3d{static_cast<float64>(sun.X), static_cast<float64>(sun.Y), static_cast<float64>(sun.Z)} *
                   sunCosine;
    const float64 expectedLux = 0.2126 * expected.X + 0.7152 * expected.Y + 0.0722 * expected.Z;
    float64 actualLux = -1;
    REQUIRE(renderer::MeterAtmosphereIlluminance(atmosphere, actualLux) ==
            renderer::AtmosphereEnvironmentStatus::Ready);
    INFO("solar sine " << solarSine << " expected " << expectedLux << " actual " << actualLux);
    // Both are midpoint rules over the same tables; they differ in where they sample the horizon band.
    constexpr float64 RELATIVE_TOLERANCE = 0.03;
    CHECK(std::abs(actualLux - expectedLux) <= RELATIVE_TOLERANCE * expectedLux);
}

TEST_CASE("Atmosphere meter reads daylight, darkness and invalid inputs", "[atmosphere]")
{
    constexpr float32 EARTH_SUN_LUX = 133'800;
    constexpr float64 WIDE_EARTH_SUN_LUX = static_cast<float64>(EARTH_SUN_LUX);
    renderer::AtmosphereDescription noon = MakeAtmosphere({0, 1, 0});
    noon.SolarIlluminance = {EARTH_SUN_LUX, EARTH_SUN_LUX, EARTH_SUN_LUX};
    float64 noonLux = 0;
    REQUIRE(renderer::MeterAtmosphereIlluminance(noon, noonLux) == renderer::AtmosphereEnvironmentStatus::Ready);
    renderer::internal::AtmosphereSkyTables tables;
    REQUIRE(tables.TryBuild(0, 1, true) == true);
    const math::Vector3d transmittance = tables.SampleTransmittance(0, 1);
    const float64 directLux =
        WIDE_EARTH_SUN_LUX * (0.2126 * transmittance.X + 0.7152 * transmittance.Y + 0.0722 * transmittance.Z);
    INFO("noon " << noonLux << " direct " << directLux);
    // The clear sky adds diffuse light to the transmitted Sun: global horizontal illuminance at a zenith Sun is
    // on the order of 100 klx, so the meter reads about EV100 15.
    CHECK(noonLux > directLux);
    CHECK(noonLux < WIDE_EARTH_SUN_LUX);
    CHECK(renderer::ComputeIncidentExposureValue100(noonLux) > 14);
    CHECK(renderer::ComputeIncidentExposureValue100(noonLux) < 16);

    // Far below the horizon the clear sky delivers nothing, so the natural night sky sets the exposure.
    constexpr float32 NIGHT_SINE = -0.5F;
    renderer::AtmosphereDescription night = MakeAtmosphere({std::sqrt(1 - NIGHT_SINE * NIGHT_SINE), NIGHT_SINE, 0});
    night.SolarIlluminance = noon.SolarIlluminance;
    float64 nightLux = -1;
    REQUIRE(renderer::MeterAtmosphereIlluminance(night, nightLux) == renderer::AtmosphereEnvironmentStatus::Ready);
    CHECK(nightLux >= 0);
    CHECK(nightLux < renderer::NATURAL_NIGHT_SKY_ILLUMINANCE_LUX);

    // The displayed sky without multiple scattering is still metered.
    renderer::AtmosphereDescription unlit = noon;
    unlit.ShouldLightScene = false;
    float64 unlitLux = 0;
    REQUIRE(renderer::MeterAtmosphereIlluminance(unlit, unlitLux) == renderer::AtmosphereEnvironmentStatus::Ready);
    CHECK(unlitLux > directLux);
    CHECK(unlitLux < noonLux);

    constexpr float64 SENTINEL = 7;
    float64 preserved = SENTINEL;
    renderer::AtmosphereDescription disabled = noon;
    disabled.IsEnabled = false;
    CHECK(renderer::MeterAtmosphereIlluminance(disabled, preserved) == renderer::AtmosphereEnvironmentStatus::NotLit);
    renderer::AtmosphereDescription invalid = noon;
    invalid.DirectionToSunWorld = {};
    CHECK(renderer::MeterAtmosphereIlluminance(invalid, preserved) ==
          renderer::AtmosphereEnvironmentStatus::InvalidDescription);
    invalid = noon;
    invalid.SolarIlluminance = {-1, 0, 0};
    CHECK(renderer::MeterAtmosphereIlluminance(invalid, preserved) ==
          renderer::AtmosphereEnvironmentStatus::InvalidDescription);
    invalid = noon;
    invalid.SurfaceUpWorld = {};
    CHECK(renderer::MeterAtmosphereIlluminance(invalid, preserved) ==
          renderer::AtmosphereEnvironmentStatus::InvalidDescription);
    CHECK(preserved == SENTINEL);
}

TEST_CASE("Cloud layer packing validates its fields and places the eye in the layer frame", "[atmosphere][cloud-layer]")
{
    renderer::AtmosphereDescription atmosphere;
    atmosphere.IsEnabled = true;
    atmosphere.SurfaceAnchorWorld = {100, 0, -50};
    atmosphere.Clouds.IsEnabled = true;
    // A quarter turn about up: the layer's +X is world -Z, so its +Z (X cross Y) is world +X.
    atmosphere.Clouds.AxisXWorld = {0, 0.25F, -2};
    renderer::ViewDescription camera;
    camera.CameraPosition = {130, 2, -70};
    renderer::internal::CloudLayerUniform uniform;
    REQUIRE(renderer::internal::TryPackCloudLayer(atmosphere, camera, uniform) == true);
    CHECK(uniform.AxisXWorld.Z == -1);
    CHECK(uniform.AxisXWorld.W == 1);
    CHECK(uniform.AxisZWorld.X == 1);
    CHECK(uniform.CameraLocal.X == 20);
    CHECK(uniform.CameraLocal.Y == 2);
    CHECK(uniform.CameraLocal.Z == 30);
    const renderer::CloudLayerDescription& clouds = atmosphere.Clouds;
    CHECK(uniform.UpWorld.W == clouds.BaseHeightMetres);
    CHECK(uniform.AxisZWorld.W == clouds.BaseHeightMetres + clouds.ThicknessMetres);
    CHECK(uniform.CameraLocal.W == clouds.EdgeFadeMetres);
    CHECK(uniform.Medium.X == clouds.SizeXMetres / 2);
    CHECK(uniform.Medium.Z == clouds.ExtinctionPerMetre);
    CHECK(uniform.Medium.W == clouds.ExtinctionPerMetre * clouds.SingleScatteringAlbedo);
    CHECK(uniform.Phase.X == clouds.PhaseAsymmetry);

    // An edge fade wider than half the footprint is limited to it.
    atmosphere.Clouds.EdgeFadeMetres = renderer::CloudLayerDescription::MAX_SIZE_METRES;
    REQUIRE(renderer::internal::TryPackCloudLayer(atmosphere, camera, uniform) == true);
    CHECK(uniform.CameraLocal.W == clouds.SizeXMetres / 2);

    // Orthographic views, a disabled layer and an eye above the shell omit the layer without failing.
    renderer::internal::CloudLayerUniform omitted;
    omitted.AxisXWorld.W = 1;
    camera.OrthographicCamera = true;
    REQUIRE(renderer::internal::TryPackCloudLayer(atmosphere, camera, omitted) == true);
    CHECK(omitted.AxisXWorld.W == 0);
    camera.OrthographicCamera = false;
    omitted.AxisXWorld.W = 1;
    camera.CameraPosition.Y = static_cast<float32>(renderer::GetAtmosphereTopAltitude() * 2);
    REQUIRE(renderer::internal::TryPackCloudLayer(atmosphere, camera, omitted) == true);
    CHECK(omitted.AxisXWorld.W == 0);

    // Invalid fields fail and keep the previous uniform.
    const renderer::internal::CloudLayerUniform previous = uniform;
    const auto reject = [&](renderer::CloudLayerDescription invalid) {
        renderer::AtmosphereDescription candidate = atmosphere;
        candidate.Clouds = invalid;
        renderer::internal::CloudLayerUniform output = previous;
        CHECK(renderer::internal::TryPackCloudLayer(candidate, camera, output) == false);
        CHECK(output.AxisXWorld == previous.AxisXWorld);
        CHECK(output.Medium == previous.Medium);
    };
    renderer::CloudLayerDescription invalid = atmosphere.Clouds;
    invalid.ThicknessMetres = 0;
    reject(invalid);
    invalid = atmosphere.Clouds;
    invalid.BaseHeightMetres = static_cast<float32>(renderer::GetAtmosphereTopAltitude());
    reject(invalid);
    invalid = atmosphere.Clouds;
    invalid.SizeZMetres = renderer::CloudLayerDescription::MAX_SIZE_METRES * 2;
    reject(invalid);
    invalid = atmosphere.Clouds;
    invalid.Density = -1;
    reject(invalid);
    invalid = atmosphere.Clouds;
    invalid.SingleScatteringAlbedo = 2;
    reject(invalid);
    invalid = atmosphere.Clouds;
    invalid.PhaseAsymmetry = 1;
    reject(invalid);
    invalid = atmosphere.Clouds;
    invalid.AxisXWorld = {0, 3, 0};
    reject(invalid);

    // A homogeneous layer packs no test field.
    CHECK(uniform.Phase.Y == 0);
    CHECK(uniform.Phase.Z == clouds.SingleScatteringAlbedo);
    CHECK(uniform.TestSplit == math::Vector4{});

    // The two-region fixture packs a unit normal, the offset and each region with the density applied.
    camera.CameraPosition = {130, 2, -70};
    atmosphere.Clouds.Density = 2;
    renderer::CloudLayerTestField& field = atmosphere.Clouds.TestField;
    field.IsEnabled = true;
    field.SplitNormalLocal = {0, 0, -4};
    field.SplitOffsetMetres = -250;
    field.Regions[0] = { .ExtinctionPerMetre = 0.001F, .SourceRadiance = {0.5F, 0.25F, 0} };
    field.Regions[1] = { .ExtinctionPerMetre = 0.003F, .SourceRadiance = {0, 0, 1} };
    REQUIRE(renderer::internal::TryPackCloudLayer(atmosphere, camera, uniform) == true);
    CHECK(uniform.Phase.Y == 1);
    CHECK(uniform.TestSplit == math::Vector4{0, 0, -1, -250});
    CHECK(uniform.TestRegions[0] == math::Vector4{0.5F, 0.25F, 0, 0.002F});
    CHECK(uniform.TestRegions[1] == math::Vector4{0, 0, 1, 0.006F});
    const renderer::CloudLayerDescription valid = atmosphere.Clouds;
    invalid = valid;
    invalid.TestField.SplitNormalLocal = {};
    reject(invalid);
    invalid = valid;
    invalid.TestField.SplitOffsetMetres = std::nanf("");
    reject(invalid);
    invalid = valid;
    invalid.TestField.Regions[1].ExtinctionPerMetre = -1;
    reject(invalid);
    invalid = valid;
    invalid.TestField.Regions[0].SourceRadiance.Y = -1;
    reject(invalid);
    // A disabled field ignores its values.
    invalid.TestField.IsEnabled = false;
    atmosphere.Clouds = invalid;
    REQUIRE(renderer::internal::TryPackCloudLayer(atmosphere, camera, uniform) == true);
    CHECK(uniform.Phase.Y == 0);
}
