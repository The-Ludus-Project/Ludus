#include "edge_diffraction_oracle.h"
#include "edge_diffraction_scene_loading.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

// The edge-diffraction oracle against independent evidence: tabulated Fresnel integrals and transition-function
// values (computed with mpmath at 30 digits), Sommerfeld's exact half-plane solution, Keller's non-uniform limit and
// the physical invariants of the uniform coefficient (reciprocity, no diffraction without an edge, a continuous total
// field across the shadow and reflection boundaries, conserved power). The scene checks then read the diffraction
// test scene files (tests/scenes/diffraction) and fix what they must show.
namespace
{
using namespace ludus::foundation;
namespace testing = ludus::graphics::renderer::testing;

/// The diffraction scenes' design wavelength, metres.
constexpr float64 DESIGN_WAVELENGTH = 0.005;
constexpr float64 DESIGN_WAVENUMBER = 2 * math::kPi / DESIGN_WAVELENGTH;
constexpr float64 WEDGE_N_30_DEGREES = 11.0 / 6.0;
/// The angular offset either side of a boundary at which continuity is checked, radians.
constexpr float64 BOUNDARY_OFFSET = 1e-7;

struct FresnelValue final
{
    float64 X = 0;
    float64 CosineIntegral = 0;
    float64 SineIntegral = 0;
};

struct TransitionValue final
{
    float64 X = 0;
    float64 Real = 0;
    float64 Imaginary = 0;
};

[[nodiscard]] float64 ComputeDistance(testing::DiffractionComplex first, testing::DiffractionComplex second)
{
    return math::Sqrt(testing::ComputeSquaredMagnitude({first.Real - second.Real, first.Imaginary - second.Imaginary}));
}

// Angles in the order of D(phi, phi'), then the wedge.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] testing::WedgeDiffraction
MakePlaneWave(float64 observationAngle, float64 incidentAngle, float64 wedgeN, testing::EdgeCondition condition)
{
    testing::WedgeDiffraction diffraction;
    diffraction.ObservationAngle = observationAngle;
    diffraction.IncidentAngle = incidentAngle;
    diffraction.DistanceParameterWorld = 1;
    diffraction.WedgeN = wedgeN;
    diffraction.Wavenumber = DESIGN_WAVENUMBER;
    diffraction.Condition = condition;
    return diffraction;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] math::Vector3d MakeWallPoint(const testing::EdgeDiffractionScene& scene, float64 x)
{
    return {x, 0, scene.WallHeightWorld};
}

/// Loads a diffraction test scene below tests/scenes/diffraction.
[[nodiscard]] testing::EdgeDiffractionScene LoadScene(std::string_view name)
{
    const std::string path = std::string(LUDUS_TEST_SCENES_DIR) + "/diffraction/" + std::string(name);
    testing::EdgeDiffractionScene scene;
    INFO(path);
    REQUIRE(testing::TryLoadDiffractionScene(path, scene) == true);
    return scene;
}

/// The model's transmission, which must apply to the scene.
[[nodiscard]] float64 ComputeTransmission(const testing::EdgeDiffractionScene& scene,
                                          testing::DiffractionModel model,
                                          math::Vector3d sourceWorld,
                                          math::Vector3d wallWorld)
{
    float64 transmission = -1;
    REQUIRE(testing::TryComputeDiffractionTransmission(scene,
                                                       model,
                                                       DESIGN_WAVELENGTH,
                                                       sourceWorld,
                                                       wallWorld,
                                                       transmission) == true);
    return transmission;
}

// A wall position and a node count.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
/// The wall radiance the model predicts, which must apply to the scene.
[[nodiscard]] float64
ComputeRadiance(const testing::EdgeDiffractionScene& scene, testing::DiffractionModel model, float64 x, usize nodes)
{
    testing::DiffractionSettings settings;
    settings.Model = model;
    settings.WavelengthWorld = DESIGN_WAVELENGTH;
    settings.LightQuadratureNodes = nodes;
    float64 radiance = -1;
    REQUIRE(testing::TryComputeDiffractionWallRadiance(scene, settings, MakeWallPoint(scene, x), radiance) == true);
    return radiance;
}
// NOLINTEND(bugprone-easily-swappable-parameters)
} // namespace

TEST_CASE("Edge diffraction oracle evaluates the Fresnel integrals", "[edge-diffraction]")
{
    constexpr usize VALUE_COUNT = 7;
    constexpr StaticArray<FresnelValue, VALUE_COUNT> VALUES{{
        { .X = 0.5, .CosineIntegral = 0.49234422587144639, .SineIntegral = 0.064732432859999278 },
        { .X = 1, .CosineIntegral = 0.77989340037682283, .SineIntegral = 0.43825914739035477 },
        { .X = 2, .CosineIntegral = 0.48825340607534075, .SineIntegral = 0.34341567836369824 },
        { .X = 3.5, .CosineIntegral = 0.53257243502800085, .SineIntegral = 0.41524801197243752 },
        { .X = 5, .CosineIntegral = 0.56363118870401223, .SineIntegral = 0.49919138191711689 },
        { .X = 10, .CosineIntegral = 0.49989869420551572, .SineIntegral = 0.46816997858488224 },
        { .X = -1, .CosineIntegral = -0.77989340037682283, .SineIntegral = -0.43825914739035477 },
    }};
    for (const FresnelValue& value : VALUES)
    {
        INFO("x = " << value.X);
        const testing::FresnelIntegrals integrals = testing::ComputeFresnelIntegrals(value.X);
        CHECK(math::Abs(integrals.Cosine - value.CosineIntegral) < 1e-13);
        CHECK(math::Abs(integrals.Sine - value.SineIntegral) < 1e-13);
    }

    // The tail switches from the power series to the continued fraction at |u| = 2.5 without a step.
    for (const float64 u : {2.5, -2.5})
    {
        INFO("u = " << u);
        CHECK(ComputeDistance(testing::ComputeFresnelTail(u - 1e-12), testing::ComputeFresnelTail(u + 1e-12)) < 1e-11);
    }
    // G(0) = (sqrt(pi) / 2) exp(-j pi / 4), and G(u) + G(-u) = 2 G(0).
    const float64 half = math::Sqrt(math::kPi / 2) / 2;
    CHECK(ComputeDistance(testing::ComputeFresnelTail(0), {half, -half}) < 1e-15);
    const testing::DiffractionComplex positive = testing::ComputeFresnelTail(4);
    const testing::DiffractionComplex negative = testing::ComputeFresnelTail(-4);
    CHECK(ComputeDistance({positive.Real + negative.Real, positive.Imaginary + negative.Imaginary},
                          {2 * half, -2 * half}) < 1e-13);
}

TEST_CASE("Edge diffraction oracle evaluates the UTD transition function", "[edge-diffraction]")
{
    constexpr usize VALUE_COUNT = 3;
    constexpr StaticArray<TransitionValue, VALUE_COUNT> VALUES{{
        { .X = 0.01, .Real = 0.12420518577376368, .Imaginary = 0.10657897379188278 },
        { .X = 1, .Real = 0.80952548174740884, .Imaginary = 0.23219939005526461 },
        { .X = 10, .Real = 0.99304112701162634, .Imaginary = 0.048351495561654347 },
    }};
    for (const TransitionValue& value : VALUES)
    {
        INFO("x = " << value.X);
        CHECK(ComputeDistance(testing::ComputeUtdTransition(value.X), {value.Real, value.Imaginary}) < 1e-13);
    }
    // F tends to 1 far from every boundary, so UTD becomes Keller's GTD there.
    CHECK(ComputeDistance(testing::ComputeUtdTransition(1e6), {1, 0}) < 1e-6);
    CHECK(testing::ComputeSquaredMagnitude(testing::ComputeUtdTransition(0)) == 0);
}

TEST_CASE("UTD reproduces Sommerfeld's exact half-plane solution", "[edge-diffraction]")
{
    // For a plane wave on a half-plane (n = 2), geometric optics plus the uniform diffracted field is the exact
    // solution, everywhere outside the edge itself.
    constexpr usize ANGLE_COUNT = 2000;
    for (const float64 incidentDegrees : {30.0, 60.0, 120.0, 170.0})
    {
        for (const float64 distance : {0.01, 1.0})
        {
            for (const testing::EdgeCondition condition : {testing::EdgeCondition::Soft, testing::EdgeCondition::Hard})
            {
                float64 largestError = 0;
                for (usize i = 0; i < ANGLE_COUNT; ++i)
                {
                    // Avoid the faces themselves (0 and 2 pi) and an exact boundary hit.
                    const float64 observation =
                        0.01 + (2 * math::kPi - 0.02) * (static_cast<float64>(i) + 0.37) / ANGLE_COUNT;
                    testing::WedgeDiffraction diffraction =
                        MakePlaneWave(observation, math::DegreesToRadians(incidentDegrees), 2, condition);
                    diffraction.DistanceParameterWorld = distance;
                    largestError = math::Max(largestError,
                                             ComputeDistance(testing::ComputeUtdWedgeField(diffraction),
                                                             testing::ComputeHalfPlaneField(diffraction)));
                }
                INFO("phi' = " << incidentDegrees << " degrees, rho = " << distance
                               << " m, hard = " << (condition == testing::EdgeCondition::Hard));
                CHECK(largestError < 1e-9);
            }
        }
    }
}

TEST_CASE("UTD coefficient is reciprocal and vanishes without an edge", "[edge-diffraction]")
{
    for (const testing::EdgeCondition condition : {testing::EdgeCondition::Soft, testing::EdgeCondition::Hard})
    {
        INFO("hard = " << (condition == testing::EdgeCondition::Hard));
        // Exchanging source and observer leaves D unchanged.
        const testing::DiffractionComplex forward = testing::ComputeUtdCoefficient(
            MakePlaneWave(math::DegreesToRadians(250.0), math::DegreesToRadians(70.0), WEDGE_N_30_DEGREES, condition));
        const testing::DiffractionComplex backward = testing::ComputeUtdCoefficient(
            MakePlaneWave(math::DegreesToRadians(70.0), math::DegreesToRadians(250.0), WEDGE_N_30_DEGREES, condition));
        CHECK(ComputeDistance(forward, backward) < 1e-15);
        // n = 1 is a flat plane: there is no edge to diffract.
        for (const float64 observationDegrees : {20.0, 95.0, 160.0})
        {
            const testing::DiffractionComplex flat = testing::ComputeUtdCoefficient(
                MakePlaneWave(math::DegreesToRadians(observationDegrees), math::DegreesToRadians(50.0), 1, condition));
            CHECK(testing::ComputeSquaredMagnitude(flat) < 1e-28);
        }
    }
}

TEST_CASE("UTD coefficient approaches Keller's GTD away from the boundaries", "[edge-diffraction]")
{
    // A 30 degree wedge lit from 60 degrees: the shadow boundary is at 240 degrees, the reflection boundary at 120.
    for (const testing::EdgeCondition condition : {testing::EdgeCondition::Soft, testing::EdgeCondition::Hard})
    {
        for (const float64 observationDegrees : {20.0, 180.0, 300.0})
        {
            testing::WedgeDiffraction diffraction = MakePlaneWave(math::DegreesToRadians(observationDegrees),
                                                                  math::DegreesToRadians(60.0),
                                                                  WEDGE_N_30_DEGREES,
                                                                  condition);
            diffraction.DistanceParameterWorld = 1000;
            const testing::DiffractionComplex uniform = testing::ComputeUtdCoefficient(diffraction);
            const testing::DiffractionComplex keller = testing::ComputeGtdCoefficient(diffraction);
            INFO("phi = " << observationDegrees << " degrees, hard = " << (condition == testing::EdgeCondition::Hard));
            CHECK(ComputeDistance(uniform, keller) < 1e-3 * math::Sqrt(testing::ComputeSquaredMagnitude(keller)));
        }
    }
}

TEST_CASE("UTD total field of a 30 degree wedge is continuous across every boundary", "[edge-diffraction]")
{
    struct Boundary final
    {
        float64 IncidentDegrees = 0;
        float64 BoundaryDegrees = 0;
    };
    // Lit from 60 degrees: shadow boundary 240, face 0 reflection boundary 120. Lit from 200 degrees: shadow boundary
    // 20, face n reflection boundary 480 - 200 = 280.
    constexpr usize BOUNDARY_COUNT = 4;
    constexpr StaticArray<Boundary, BOUNDARY_COUNT> BOUNDARIES{{
        { .IncidentDegrees = 60, .BoundaryDegrees = 240 },
        { .IncidentDegrees = 60, .BoundaryDegrees = 120 },
        { .IncidentDegrees = 200, .BoundaryDegrees = 20 },
        { .IncidentDegrees = 200, .BoundaryDegrees = 280 },
    }};
    for (const Boundary& boundary : BOUNDARIES)
    {
        for (const testing::EdgeCondition condition : {testing::EdgeCondition::Soft, testing::EdgeCondition::Hard})
        {
            const float64 incident = math::DegreesToRadians(boundary.IncidentDegrees);
            const float64 at = math::DegreesToRadians(boundary.BoundaryDegrees);
            INFO("phi' = " << boundary.IncidentDegrees << ", boundary = " << boundary.BoundaryDegrees
                           << ", hard = " << (condition == testing::EdgeCondition::Hard));
            // Across the boundary, where geometric optics jumps by a whole incident or reflected wave.
            const testing::DiffractionComplex before = testing::ComputeUtdWedgeField(
                MakePlaneWave(at - BOUNDARY_OFFSET, incident, WEDGE_N_30_DEGREES, condition));
            const testing::DiffractionComplex after = testing::ComputeUtdWedgeField(
                MakePlaneWave(at + BOUNDARY_OFFSET, incident, WEDGE_N_30_DEGREES, condition));
            CHECK(ComputeDistance(before, after) < 1e-3);
            // Across the oracle's switch to the coefficient's boundary limit, 1e-5 radians from the boundary.
            const testing::DiffractionComplex inside =
                testing::ComputeUtdWedgeField(MakePlaneWave(at + 0.99e-5, incident, WEDGE_N_30_DEGREES, condition));
            const testing::DiffractionComplex outside =
                testing::ComputeUtdWedgeField(MakePlaneWave(at + 1.01e-5, incident, WEDGE_N_30_DEGREES, condition));
            CHECK(ComputeDistance(inside, outside) < 1e-3);
        }
    }
}

TEST_CASE("Diffraction test scenes read as diffraction scenes", "[edge-diffraction]")
{
    struct ExpectedScene final
    {
        std::string_view File;
        usize Obstacles = 0;
        bool HasAperture = false;
    };
    constexpr usize SCENE_COUNT = 3;
    constexpr StaticArray<ExpectedScene, SCENE_COUNT> SCENES{{
        { .File = "single-edge.luscene.json", .Obstacles = 1, .HasAperture = false },
        { .File = "single-slit.luscene.json", .Obstacles = 2, .HasAperture = true },
        { .File = "slit-partial-occluder.luscene.json", .Obstacles = 3, .HasAperture = false },
    }};
    for (const ExpectedScene& expected : SCENES)
    {
        INFO(expected.File);
        const testing::EdgeDiffractionScene scene = LoadScene(expected.File);
        CHECK(scene.ObstacleCount == expected.Obstacles);
        for (usize o = 0; o < scene.ObstacleCount; ++o)
        {
            CHECK(scene.Obstacles[o].CornerCount == 4);
        }
        // The 3 cm light 0.5 m above the jaws and the white wall 1 m below them, seen from between them.
        CHECK(scene.LightCenterWorld == math::Vector3d{0, 0, 0.5});
        CHECK(scene.LightHalfWidthWorld == 0.015);
        CHECK(scene.LightHalfHeightWorld == 0.015);
        CHECK(scene.LightRadiance == 10000);
        CHECK(scene.WallHeightWorld == -1);
        CHECK(scene.WallAlbedo == 0.8);
        CHECK(scene.EyeWorld == math::Vector3d{0, 0, -0.1});
        // Kirchhoff needs one aperture between obstacles that share a top plane: only the plain slit has it.
        float64 transmission = 0;
        CHECK(testing::TryComputeDiffractionTransmission(scene,
                                                         testing::DiffractionModel::Kirchhoff,
                                                         DESIGN_WAVELENGTH,
                                                         scene.LightCenterWorld,
                                                         MakeWallPoint(scene, 0),
                                                         transmission) == expected.HasAperture);
    }
}

TEST_CASE("Diffraction oracle reproduces the slit's geometric optics", "[edge-diffraction]")
{
    const testing::EdgeDiffractionScene scene = LoadScene("single-slit.luscene.json");
    constexpr usize LIGHT_NODES = 12;
    // Without the jaws, the 3 cm light 1.5 m above the wall: L rho / pi * A / d^2.
    const float64 lightArea = 4 * scene.LightHalfWidthWorld * scene.LightHalfHeightWorld;
    const float64 height = scene.LightCenterWorld.Z - scene.WallHeightWorld;
    const float64 freeCentre = testing::ComputeDiffractionFreeWallRadiance(scene, MakeWallPoint(scene, 0), LIGHT_NODES);
    CHECK(math::Abs(freeCentre - scene.LightRadiance * scene.WallAlbedo / math::kPi * lightArea / (height * height)) <
          1e-3 * freeCentre);
    // The wall sees all of the light through the slit within 4.5 cm of the centre, half of it on the point-source
    // shadow boundary at 7.5 cm, and none of it beyond 10.5 cm.
    for (const float64 x : {0.0, 0.03, 0.045})
    {
        INFO("X = " << x);
        CHECK(math::Abs(ComputeRadiance(scene, testing::DiffractionModel::Geometric, x, LIGHT_NODES) -
                        testing::ComputeDiffractionFreeWallRadiance(scene, MakeWallPoint(scene, x), LIGHT_NODES)) <
              1e-12);
    }
    const float64 boundary = ComputeRadiance(scene, testing::DiffractionModel::Geometric, 0.075, LIGHT_NODES);
    CHECK(math::Abs(boundary /
                        testing::ComputeDiffractionFreeWallRadiance(scene, MakeWallPoint(scene, 0.075), LIGHT_NODES) -
                    0.5) < 0.02);
    CHECK(ComputeRadiance(scene, testing::DiffractionModel::Geometric, 0.106, LIGHT_NODES) == 0);
}

TEST_CASE("Diffraction oracle UTD is continuous across the shadow boundaries", "[edge-diffraction]")
{
    struct Boundary final
    {
        std::string_view File;
        float64 X = 0; ///< Where the light centre's shadow boundary meets the wall.
    };
    // The single edge at x = 0 casts its boundary at 0; the slit's edges at +-2.5 cm cast theirs at +-7.5 cm.
    constexpr usize BOUNDARY_COUNT = 3;
    constexpr StaticArray<Boundary, BOUNDARY_COUNT> BOUNDARIES{{
        { .File = "single-edge.luscene.json", .X = 0 },
        { .File = "single-slit.luscene.json", .X = 0.075 },
        { .File = "single-slit.luscene.json", .X = -0.075 },
    }};
    for (const Boundary& boundary : BOUNDARIES)
    {
        const testing::EdgeDiffractionScene scene = LoadScene(boundary.File);
        for (const testing::DiffractionModel model : {testing::DiffractionModel::UtdAbsorbing,
                                                      testing::DiffractionModel::UtdSoft,
                                                      testing::DiffractionModel::UtdHard})
        {
            // 0.1 um either side: outside the visibility test's 1 nm tolerance, which would otherwise count the
            // direct light on both sides of the boundary.
            const float64 before =
                ComputeTransmission(scene, model, scene.LightCenterWorld, MakeWallPoint(scene, boundary.X - 1e-7));
            const float64 after =
                ComputeTransmission(scene, model, scene.LightCenterWorld, MakeWallPoint(scene, boundary.X + 1e-7));
            INFO(boundary.File << " X = " << boundary.X << ", model = " << static_cast<uint32>(model));
            CHECK(math::Abs(before - after) < 1e-5);
            // On a single edge's shadow boundary the intensity is about a quarter of the unobstructed one.
            CHECK((before > 0.1 && before < 0.35));
        }
    }
}

TEST_CASE("Diffraction oracle UTD agrees with Fresnel-Kirchhoff near the slit's beam", "[edge-diffraction]")
{
    const testing::EdgeDiffractionScene scene = LoadScene("single-slit.luscene.json");
    // Within the beam and its first fringes, where both are accurate, the black-screen Kirchhoff integral, absorbing
    // UTD and unpolarized conducting UTD for the 30 degree wedges give nearly the same intensity.
    for (const float64 x : {0.0, 0.02, 0.04, 0.06, 0.075, 0.09, 0.105, 0.12, 0.15})
    {
        const float64 kirchhoff = ComputeTransmission(scene,
                                                      testing::DiffractionModel::Kirchhoff,
                                                      scene.LightCenterWorld,
                                                      MakeWallPoint(scene, x));
        const float64 absorbing = ComputeTransmission(scene,
                                                      testing::DiffractionModel::UtdAbsorbing,
                                                      scene.LightCenterWorld,
                                                      MakeWallPoint(scene, x));
        const float64 unpolarized = ComputeTransmission(scene,
                                                        testing::DiffractionModel::UtdUnpolarized,
                                                        scene.LightCenterWorld,
                                                        MakeWallPoint(scene, x));
        INFO("X = " << x << ", Kirchhoff " << kirchhoff << ", absorbing " << absorbing << ", unpolarized "
                    << unpolarized);
        CHECK(math::Abs(absorbing - kirchhoff) < 0.03);
        CHECK(math::Abs(unpolarized - kirchhoff) < 0.03);
    }
}

TEST_CASE("Diffraction oracle UTD conserves the power through the slit", "[edge-diffraction]")
{
    // Diffraction redistributes the light that passes the slit; it does not create or remove it. Over the wall's
    // central 1.2 m row the light's radiance integrates to the geometric value within a few percent (the rest falls
    // further out, and UTD is only asymptotically power conserving).
    const testing::EdgeDiffractionScene scene = LoadScene("single-slit.luscene.json");
    constexpr usize LIGHT_NODES = 4;
    constexpr usize SAMPLES = 241;
    for (const testing::DiffractionModel model :
         {testing::DiffractionModel::UtdAbsorbing, testing::DiffractionModel::UtdUnpolarized})
    {
        float64 geometric = 0;
        float64 uniform = 0;
        for (usize i = 0; i < SAMPLES; ++i)
        {
            const float64 x = -0.6 + 1.2 * (static_cast<float64>(i) + 0.5) / static_cast<float64>(SAMPLES);
            geometric += ComputeRadiance(scene, testing::DiffractionModel::Geometric, x, LIGHT_NODES);
            uniform += ComputeRadiance(scene, model, x, LIGHT_NODES);
        }
        INFO("model " << static_cast<uint32>(model) << ", geometric " << geometric << ", UTD " << uniform);
        CHECK(math::Abs(uniform / geometric - 1) < 0.05);
    }
}

TEST_CASE("Diffraction oracle lights the single edge's shadow and fringes its lit side", "[edge-diffraction]")
{
    const testing::EdgeDiffractionScene scene = LoadScene("single-edge.luscene.json");
    const math::Vector3d sourceWorld = scene.LightCenterWorld;
    // Deep in the lit side the intensity oscillates about the unobstructed one; deep in the shadow it decays.
    float64 lowest = 2;
    float64 highest = 0;
    for (usize i = 0; i < 100; ++i)
    {
        const float64 x = -0.05 - 0.4 * static_cast<float64>(i) / 100;
        const float64 lit =
            ComputeTransmission(scene, testing::DiffractionModel::UtdAbsorbing, sourceWorld, MakeWallPoint(scene, x));
        lowest = math::Min(lowest, lit);
        highest = math::Max(highest, lit);
    }
    CHECK((lowest > 0.7 && lowest < 1));
    CHECK((highest > 1 && highest < 1.4));
    const float64 shallow =
        ComputeTransmission(scene, testing::DiffractionModel::UtdAbsorbing, sourceWorld, MakeWallPoint(scene, 0.05));
    const float64 deep =
        ComputeTransmission(scene, testing::DiffractionModel::UtdAbsorbing, sourceWorld, MakeWallPoint(scene, 0.4));
    CHECK((shallow > deep && deep > 0 && deep < 0.01));
}

TEST_CASE("Diffraction oracle partially occludes the light behind the rod", "[edge-diffraction]")
{
    // The rod hides the right half of the light from the slit's centre: geometric optics halves the beam's centre.
    const testing::EdgeDiffractionScene slit = LoadScene("single-slit.luscene.json");
    const testing::EdgeDiffractionScene occluded = LoadScene("slit-partial-occluder.luscene.json");
    constexpr usize LIGHT_NODES = 8;
    const float64 openGeometric = ComputeRadiance(slit, testing::DiffractionModel::Geometric, 0, LIGHT_NODES);
    const float64 occludedGeometric = ComputeRadiance(occluded, testing::DiffractionModel::Geometric, 0, LIGHT_NODES);
    CHECK(math::Abs(occludedGeometric / openGeometric - 0.5) < 0.02);
    // First-order UTD: the rod also hides the right jaw's edge from the whole left half of the light, so that edge
    // diffracts only the light of the right half that passes beside the rod. What it would receive by diffraction
    // around the rod is second order and absent; the edge lies 1.5 wavelengths inside the rod's shadow, so in reality
    // that light is not small. A first-order implementation must reproduce this value; a physical one need not.
    const float64 openUniform = ComputeRadiance(slit, testing::DiffractionModel::UtdAbsorbing, 0, LIGHT_NODES);
    const float64 occludedUniform = ComputeRadiance(occluded, testing::DiffractionModel::UtdAbsorbing, 0, LIGHT_NODES);
    INFO("open " << openUniform << ", occluded " << occludedUniform);
    CHECK((occludedUniform > 0.1 * openUniform && occludedUniform < 0.5 * openUniform));
}
