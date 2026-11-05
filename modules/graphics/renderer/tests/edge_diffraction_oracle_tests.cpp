#include "edge_diffraction_oracle.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

// The edge-diffraction oracle against independent evidence: tabulated Fresnel integrals and transition-function
// values (computed with mpmath at 30 digits), Sommerfeld's exact half-plane solution, Keller's non-uniform limit and
// the physical invariants of the uniform coefficient (reciprocity, no diffraction without an edge, a continuous total
// field across the shadow and reflection boundaries, conserved power). The scene checks then fix what the single-slit
// test scene must show.
namespace
{
using namespace ludus::foundation;
namespace testing = ludus::graphics::renderer::testing;

constexpr float64 DESIGN_WAVENUMBER = 2 * math::kPi / 0.005;
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

[[nodiscard]] math::Vector3d MakeWallPoint(const testing::SingleSlitScene& scene, float64 x)
{
    return {x, 0, scene.WallHeightWorld};
}
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

TEST_CASE("Single-slit oracle reproduces the scene's geometric optics", "[edge-diffraction]")
{
    const testing::SingleSlitScene scene;
    constexpr usize SUBDIVISIONS = 64;
    // Without the jaws, the 3 cm light 1.5 m above the wall: L rho / pi * A / d^2.
    const float64 lightArea = 4 * scene.LightHalfSizeWorld * scene.LightHalfSizeWorld;
    const float64 height = scene.LightHeightWorld - scene.WallHeightWorld;
    const float64 freeCentre = testing::ComputeSlitFreeWallRadiance(scene, MakeWallPoint(scene, 0), SUBDIVISIONS);
    CHECK(math::Abs(freeCentre - scene.LightRadiance * scene.WallAlbedo / math::kPi * lightArea / (height * height)) <
          1e-3 * freeCentre);
    // The wall sees all of the light through the slit within 4.5 cm of the centre, half of it on the point-source
    // shadow boundary at 7.5 cm, and none of it beyond 10.5 cm.
    for (const float64 x : {0.0, 0.03, 0.045})
    {
        INFO("X = " << x);
        CHECK(math::Abs(testing::ComputeSlitWallRadiance(scene,
                                                         testing::SlitModel::Geometric,
                                                         MakeWallPoint(scene, x),
                                                         SUBDIVISIONS) -
                        testing::ComputeSlitFreeWallRadiance(scene, MakeWallPoint(scene, x), SUBDIVISIONS)) < 1e-12);
    }
    const float64 boundary = testing::ComputeSlitWallRadiance(scene,
                                                              testing::SlitModel::Geometric,
                                                              MakeWallPoint(scene, 0.075),
                                                              SUBDIVISIONS);
    CHECK(math::Abs(boundary / testing::ComputeSlitFreeWallRadiance(scene, MakeWallPoint(scene, 0.075), SUBDIVISIONS) -
                    0.5) < 0.02);
    CHECK(testing::ComputeSlitWallRadiance(scene,
                                           testing::SlitModel::Geometric,
                                           MakeWallPoint(scene, 0.106),
                                           SUBDIVISIONS) == 0);
}

TEST_CASE("Single-slit UTD is continuous across the slit's shadow boundaries", "[edge-diffraction]")
{
    // A point source at the light's centre: the edges' shadow boundaries meet the wall at X = +-3 a.
    const testing::SingleSlitScene scene;
    const math::Vector3d sourceWorld{0, 0, scene.LightHeightWorld};
    const float64 boundaryX = 3 * scene.SlitHalfWidthWorld;
    for (const testing::SlitModel model : {testing::SlitModel::UtdSoft, testing::SlitModel::UtdHard})
    {
        for (const float64 side : {1.0, -1.0})
        {
            const float64 before = testing::ComputeSlitTransmission(scene,
                                                                    model,
                                                                    sourceWorld,
                                                                    MakeWallPoint(scene, side * (boundaryX - 1e-9)));
            const float64 after = testing::ComputeSlitTransmission(scene,
                                                                   model,
                                                                   sourceWorld,
                                                                   MakeWallPoint(scene, side * (boundaryX + 1e-9)));
            INFO("model = " << static_cast<uint32>(model) << ", side = " << side);
            CHECK(math::Abs(before - after) < 1e-5);
            // Near a single edge's shadow boundary the intensity is about a quarter of the unobstructed one.
            CHECK((before > 0.1 && before < 0.3));
        }
    }
}

TEST_CASE("Single-slit UTD agrees with Fresnel-Kirchhoff near the beam and conserves power", "[edge-diffraction]")
{
    const testing::SingleSlitScene scene;
    const math::Vector3d sourceWorld{0, 0, scene.LightHeightWorld};
    // Within the beam and its first fringes, where both are accurate, the black-screen Kirchhoff integral and
    // unpolarized UTD for the 30 degree wedge give nearly the same intensity.
    for (const float64 x : {0.0, 0.02, 0.04, 0.06, 0.075, 0.09, 0.105, 0.12, 0.15})
    {
        const float64 kirchhoff = testing::ComputeSlitTransmission(scene,
                                                                   testing::SlitModel::Kirchhoff,
                                                                   sourceWorld,
                                                                   MakeWallPoint(scene, x));
        const float64 uniform = testing::ComputeSlitTransmission(scene,
                                                                 testing::SlitModel::UtdUnpolarized,
                                                                 sourceWorld,
                                                                 MakeWallPoint(scene, x));
        INFO("X = " << x << ", Kirchhoff " << kirchhoff << ", UTD " << uniform);
        CHECK(math::Abs(uniform - kirchhoff) < 0.03);
    }
    // Diffraction redistributes the light that passes the slit; it does not create or remove it. Over the wall's
    // central 1.2 m row, the 3 cm light's radiance integrates to the geometric value within a few percent (the rest
    // falls further out, and UTD is only asymptotically power conserving).
    constexpr usize SUBDIVISIONS = 8;
    constexpr usize SAMPLES = 241;
    float64 geometric = 0;
    float64 uniform = 0;
    for (usize i = 0; i < SAMPLES; ++i)
    {
        const float64 x = -0.6 + 1.2 * (static_cast<float64>(i) + 0.5) / static_cast<float64>(SAMPLES);
        geometric += testing::ComputeSlitWallRadiance(scene,
                                                      testing::SlitModel::Geometric,
                                                      MakeWallPoint(scene, x),
                                                      SUBDIVISIONS);
        uniform += testing::ComputeSlitWallRadiance(scene,
                                                    testing::SlitModel::UtdUnpolarized,
                                                    MakeWallPoint(scene, x),
                                                    SUBDIVISIONS);
    }
    INFO("geometric " << geometric << ", UTD " << uniform);
    CHECK(math::Abs(uniform / geometric - 1) < 0.05);
}

TEST_CASE("Print the single-slit reference profile", "[.single-slit-profile]")
{
    // ludus_renderer_tests "[.single-slit-profile]" prints the wall radiance along y = 0 that each model predicts for
    // the scene's 3 cm light, as comma-separated values in W / (sr m^2).
    const testing::SingleSlitScene scene;
    constexpr usize SUBDIVISIONS = 24;
    constexpr usize SAMPLES = 121;
    std::string table = "x_m,free,geometric,kirchhoff,utd_soft,utd_hard,utd_unpolarized\n";
    for (usize i = 0; i < SAMPLES; ++i)
    {
        const float64 x = -0.6 + 1.2 * static_cast<float64>(i) / static_cast<float64>(SAMPLES - 1) + 1e-6;
        const math::Vector3d wallWorld = MakeWallPoint(scene, x);
        table += std::to_string(x);
        table += "," + std::to_string(testing::ComputeSlitFreeWallRadiance(scene, wallWorld, SUBDIVISIONS));
        for (const testing::SlitModel model : {testing::SlitModel::Geometric,
                                               testing::SlitModel::Kirchhoff,
                                               testing::SlitModel::UtdSoft,
                                               testing::SlitModel::UtdHard,
                                               testing::SlitModel::UtdUnpolarized})
        {
            table += "," + std::to_string(testing::ComputeSlitWallRadiance(scene, model, wallWorld, SUBDIVISIONS));
        }
        table += '\n';
    }
    WARN(table);
}
