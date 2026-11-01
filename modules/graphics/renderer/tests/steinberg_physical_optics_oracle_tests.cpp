#include "steinberg_physical_optics_oracle.h"

#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <catch2/catch_test_macros.hpp>

// Physics of the steinberg-physical-optics template's scattering core, on its CPU oracle. Thanks to Shlomi Steinberg,
// Pradeep Sen and Ling-Qi Yan, "Towards Practical Physical-Optics Rendering", ACM Transactions on Graphics 41(4),
// article 132, SIGGRAPH 2022, https://doi.org/10.1145/3528223.3530119 : the Fresnel Mueller matrix of Eq. (29), the
// coherence of natural light (Eq. (25) and section 4), the energy of the normalized diffraction operator (Eq. (28) and
// Fig. 5), Theorem 3.2's conserved coherence area under Eq. (20), Eq. (34)'s Bessel weights and the sampling envelope
// of Eq. (31).
namespace
{
using namespace ludus::foundation;
using namespace ludus::graphics::renderer::testing;

constexpr float64 NANOMETERS_PER_METER = 1e9;
constexpr float64 MICROMETERS_PER_METER = 1e6;
constexpr float64 GREEN_NANOMETERS = 550;

float64 ComputeWavenumber(float64 wavelengthNanometers)
{
    return math::kTau * NANOMETERS_PER_METER / wavelengthNanometers;
}

/// A rough dielectric at normal incidence lit by natural light from a radiator distanceWorld away, the paper's 1 mm^2
/// and 1 usr, or by coherent light when distanceWorld is zero (the radiometric coherent limit).
OpticsCase MakeNormalIncidenceCase(float64 distanceWorld)
{
    constexpr float64 RADIATOR_AREA = 1e-6;
    constexpr float64 SOLID_ANGLE = 1e-6;
    constexpr float64 CORRELATION_LENGTH = 1e-6;
    OpticsCase result;
    result.Incident = MakeOpticsNaturalBeam({0, 0, -1},
                                            1,
                                            ComputeOpticsRadiatorShape(RADIATOR_AREA, SOLID_ANGLE),
                                            distanceWorld > 0 ? distanceWorld : 1);
    result.IsWave = distanceWorld > 0;
    result.Lobe = OpticsLobe::Scattered;
    result.CorrelationLengthWorld = CORRELATION_LENGTH;
    return result;
}

/// The integral of D cos theta_o over the outgoing hemisphere: over the unit disk of projected directions, since
/// d^2 r_perp = cos theta_o d omega_o, by the midpoint rule.
float64 IntegrateDiffraction(OpticsCase opticsCase)
{
    constexpr usize STEPS = 1000;
    constexpr float64 STEP = 2.0 / STEPS;
    float64 sum = 0;
    for (usize row = 0; row < STEPS; ++row)
    {
        const float64 y = -1 + (static_cast<float64>(row) + .5) * STEP;
        for (usize column = 0; column < STEPS; ++column)
        {
            const float64 x = -1 + (static_cast<float64>(column) + .5) * STEP;
            const float64 projectedSquared = x * x + y * y;
            if (projectedSquared >= 1)
            {
                continue;
            }
            opticsCase.OutgoingWorld = {x, y, math::Sqrt(1 - projectedSquared)};
            sum += EvaluateOpticsCase(opticsCase).DiffractionX;
        }
    }
    return sum * STEP * STEP;
}
} // namespace

TEST_CASE("The Steinberg physical-optics oracle's Fresnel matrix polarizes at Brewster's angle",
          "[renderer][steinberg-physical-optics][oracle]")
{
    // Eq. (29) at normal incidence reflects ((n - 1)^2 + kappa^2) / ((n + 1)^2 + kappa^2) of either polarization.
    constexpr OpticsComplex GOLD{.2, 3.4};
    const float64 normal = ((GOLD.Real - 1) * (GOLD.Real - 1) + GOLD.Imaginary * GOLD.Imaginary) /
                           ((GOLD.Real + 1) * (GOLD.Real + 1) + GOLD.Imaginary * GOLD.Imaginary);
    constexpr float64 TOLERANCE = 1e-12;
    CHECK(math::Abs(ComputeOpticsUnpolarizedReflectance(1, GOLD) - normal) < TOLERANCE);
    // At Brewster's angle, tan theta = n, a dielectric reflects no p light: unpolarized light becomes s-polarized,
    // and a polarizer along the p axis (the frame's x axis) passes nothing of it.
    constexpr float64 GLASS = 1.5;
    const float64 brewster = math::Atan2(GLASS, 1.0);
    OpticsCase reflection;
    reflection.Incident = MakeOpticsNaturalBeam({math::Sin(brewster), 0, -math::Cos(brewster)}, 1, 1, 1);
    reflection.OutgoingWorld = {math::Sin(brewster), 0, math::Cos(brewster)};
    reflection.Lobe = OpticsLobe::Direct;
    reflection.Eta = {GLASS, 0};
    const OpticsResult result = EvaluateOpticsCase(reflection);
    OpticsComplex perpendicular;
    OpticsComplex parallel;
    ComputeOpticsFresnel(math::Cos(brewster), {GLASS, 0}, perpendicular, parallel);
    const float64 perpendicularReflectance =
        perpendicular.Real * perpendicular.Real + perpendicular.Imaginary * perpendicular.Imaginary;
    INFO("S = " << result.Scattered.Stokes.S0 << ", " << result.Scattered.Stokes.S1 << ", "
                << result.Scattered.Stokes.S2 << ", " << result.Scattered.Stokes.S3);
    constexpr float64 POLARIZATION_TOLERANCE = 1e-9;
    CHECK(math::Abs(result.Scattered.Stokes.S0 - .5 * perpendicularReflectance) < POLARIZATION_TOLERANCE);
    CHECK(ApplyOpticsPolarizer(result.Scattered.Stokes, 0) < POLARIZATION_TOLERANCE);
    CHECK(math::Abs(ApplyOpticsPolarizer(result.Scattered.Stokes, math::kPi / 2) - result.Scattered.Stokes.S0) <
          POLARIZATION_TOLERANCE);
}

TEST_CASE("The Steinberg physical-optics oracle reproduces the coherence of natural light",
          "[renderer][steinberg-physical-optics][oracle]")
{
    // Section 4: sunlight is coherent over about sigma_sun = 50 um on earth, which the template fixes at 550 nm; Eq.
    // (25)'s 1 / k^2 makes it proportional to the wavelength.
    constexpr float64 SUN_COHERENCE = 50e-6;
    const OpticsBeam sun =
        MakeOpticsNaturalBeam({0, 0, -1}, 1, ComputeOpticsSunShape(SUN_COHERENCE), OPTICS_SUN_DISTANCE_WORLD);
    constexpr float64 RELATIVE_TOLERANCE = 1e-9;
    CHECK(math::Abs(ComputeOpticsCoherenceLength(sun, GREEN_NANOMETERS) / SUN_COHERENCE - 1) < RELATIVE_TOLERANCE);
    constexpr float64 RED_NANOMETERS = 660;
    CHECK(math::Abs(ComputeOpticsCoherenceLength(sun, RED_NANOMETERS) / SUN_COHERENCE -
                    RED_NANOMETERS / GREEN_NANOMETERS) < RELATIVE_TOLERANCE);
    // A 1 mm^2 radiator emitting into 1 usr, 1.9 m away: (r / k) sqrt(2 pi Omega / A), about 0.42 um.
    constexpr float64 RADIATOR_AREA = 1e-6;
    constexpr float64 SOLID_ANGLE = 1e-6;
    constexpr float64 DISTANCE = 1.9;
    const OpticsBeam radiator =
        MakeOpticsNaturalBeam({0, 0, -1}, 1, ComputeOpticsRadiatorShape(RADIATOR_AREA, SOLID_ANGLE), DISTANCE);
    const float64 expected = DISTANCE / ComputeWavenumber(GREEN_NANOMETERS) * math::Sqrt(math::kTau);
    const float64 length = ComputeOpticsCoherenceLength(radiator, GREEN_NANOMETERS);
    INFO("radiator coherence " << length * MICROMETERS_PER_METER << " um");
    CHECK(math::Abs(length / expected - 1) < RELATIVE_TOLERANCE);
    constexpr float64 MINIMUM_MICROMETERS = .41;
    constexpr float64 MAXIMUM_MICROMETERS = .43;
    CHECK(length * MICROMETERS_PER_METER > MINIMUM_MICROMETERS);
    CHECK(length * MICROMETERS_PER_METER < MAXIMUM_MICROMETERS);
}

TEST_CASE("The Steinberg physical-optics oracle's diffraction operator conserves energy and blurs with incoherence",
          "[renderer][steinberg-physical-optics][oracle]")
{
    // The normalized Eq. (28) integrates to one over the propagating half vectors, d^2 h~ = cos theta_o d omega_o,
    // whatever the coherence, as long as the blurred lobe stays within them (Fig. 5's caption). Less coherent light
    // widens the lobe (Eq. (15) is a Gaussian blur) and lowers its peak. A radiator 0.3 m away is coherent over about
    // 0.07 um: its lobe reaches the evanescent half vectors, which carry its missing energy away.
    const OpticsCase coherent = MakeNormalIncidenceCase(0);
    constexpr float64 PARTIAL_DISTANCE = 2;
    const OpticsCase partial = MakeNormalIncidenceCase(PARTIAL_DISTANCE);
    constexpr float64 INCOHERENT_DISTANCE = .3;
    const OpticsCase incoherent = MakeNormalIncidenceCase(INCOHERENT_DISTANCE);
    const float64 coherentEnergy = IntegrateDiffraction(coherent);
    const float64 partialEnergy = IntegrateDiffraction(partial);
    const float64 incoherentEnergy = IntegrateDiffraction(incoherent);
    INFO("energy: coherent " << coherentEnergy << ", partial " << partialEnergy << ", incoherent " << incoherentEnergy);
    constexpr float64 QUADRATURE_TOLERANCE = 1e-3;
    CHECK(math::Abs(coherentEnergy - 1) < QUADRATURE_TOLERANCE);
    CHECK(math::Abs(partialEnergy - 1) < QUADRATURE_TOLERANCE);
    constexpr float64 EVANESCENT_LOSS = .05;
    CHECK(incoherentEnergy < 1 - EVANESCENT_LOSS);
    OpticsCase mirror = coherent;
    mirror.OutgoingWorld = {0, 0, 1};
    OpticsCase partialMirror = partial;
    partialMirror.OutgoingWorld = {0, 0, 1};
    CHECK(EvaluateOpticsCase(partialMirror).DiffractionX < EvaluateOpticsCase(mirror).DiffractionX);
    // Eq. (22): the scattered lobe carries 1 - alpha_direct, with alpha_direct = exp(-(k cos theta_i q)^2).
    OpticsCase rough = partialMirror;
    constexpr float64 RMS_HEIGHT = 50e-9;
    rough.RmsHeightWorld = RMS_HEIGHT;
    const float64 phase = ComputeWavenumber(rough.WavelengthNanometers) * RMS_HEIGHT;
    constexpr float64 TOLERANCE = 1e-12;
    CHECK(math::Abs(EvaluateOpticsCase(rough).DirectFraction - math::Exp(-phase * phase)) < TOLERANCE);
}

TEST_CASE("The Steinberg physical-optics oracle keeps the coherence area on scattering",
          "[renderer][steinberg-physical-optics][oracle]")
{
    // Theorem 3.2: the coherence area |theta| is conserved. Eq. (20) rotates the shapes and deforms them by U of unit
    // determinant (Eq. (18)); with theta_x = theta_y the mixing by the frame rotation changes nothing.
    OpticsCase opticsCase = MakeNormalIncidenceCase(2);
    constexpr float64 TILT = .6;
    opticsCase.Incident = MakeOpticsNaturalBeam({math::Sin(TILT), 0, -math::Cos(TILT)}, 1, 3, 2);
    opticsCase.Incident.ShapeX = {3, .5, 2};
    opticsCase.Incident.ShapeY = opticsCase.Incident.ShapeX;
    opticsCase.OutgoingWorld = {-.3, .4, math::Sqrt(1 - .25)};
    opticsCase.Eta = {.2, 3.4};
    const OpticsResult result = EvaluateOpticsCase(opticsCase);
    const float64 incidentArea = ComputeOpticsDeterminant(opticsCase.Incident.ShapeX);
    constexpr float64 RELATIVE_TOLERANCE = 1e-9;
    CHECK(math::Abs(ComputeOpticsDeterminant(result.Scattered.ShapeX) / incidentArea - 1) < RELATIVE_TOLERANCE);
    CHECK(math::Abs(ComputeOpticsDeterminant(result.Scattered.ShapeY) / incidentArea - 1) < RELATIVE_TOLERANCE);
    // The scattered frame is orthonormal and transverse to the scattered direction.
    const math::Vector3d x = result.Scattered.TransverseXWorld;
    CHECK(math::Abs(math::Dot(x, x) - 1) < RELATIVE_TOLERANCE);
    CHECK(math::Abs(math::Dot(x, result.Scattered.PropagationWorld)) < RELATIVE_TOLERANCE);
}

TEST_CASE("The Steinberg physical-optics oracle's grating weights are Bessel functions",
          "[renderer][steinberg-physical-optics][oracle]")
{
    // Eq. (34) weighs order n by J_n(upsilon)^2, and sum_n J_n(x)^2 = 1 (Abramowitz and Stegun 9.1.76). Reference
    // values from Abramowitz and Stegun, Table 9.1. Miller's recurrence with Numerical Recipes' starting order is
    // accurate to about 1e-7, enough for the template's float32 shaders.
    constexpr float64 TABLE_TOLERANCE = 1e-6;
    constexpr float64 J0_OF_1 = .7651976866;
    constexpr float64 J1_OF_1 = .4400505857;
    constexpr float64 J2_OF_2_5 = .4460590584;
    constexpr float64 ARGUMENT = 2.5;
    CHECK(math::Abs(EvaluateOpticsBessel(0, 1) - J0_OF_1) < TABLE_TOLERANCE);
    CHECK(math::Abs(EvaluateOpticsBessel(1, 1) - J1_OF_1) < TABLE_TOLERANCE);
    CHECK(math::Abs(EvaluateOpticsBessel(2, ARGUMENT) - J2_OF_2_5) < TABLE_TOLERANCE);
    constexpr uint32 ORDER_COUNT = 40;
    float64 sum = 0;
    for (uint32 order = 0; order < ORDER_COUNT; ++order)
    {
        const float64 value = EvaluateOpticsBessel(order, ARGUMENT);
        sum += order == 0 ? value * value : 2 * value * value;
    }
    CHECK(math::Abs(sum - 1) < TABLE_TOLERANCE);
}

TEST_CASE("Sampling the Steinberg physical-optics oracle's envelope estimates the scattered lobe without bias",
          "[renderer][steinberg-physical-optics][oracle]")
{
    // Eq. (31): half vectors drawn from the isotropic envelope N(0, w I), mapped to directions with the Jacobian
    // d^2 h~ = cos theta d omega. The estimate of D cos theta over the hemisphere matches the quadrature.
    OpticsCase opticsCase = MakeNormalIncidenceCase(2);
    constexpr float64 ENVELOPE_VARIANCE = .1;
    opticsCase.EnvelopeVariance = ENVELOPE_VARIANCE;
    const float64 reference = IntegrateDiffraction(opticsCase);
    constexpr usize SAMPLE_COUNT = 200000;
    constexpr uint64 SEED = 0x50485953;
    math::RandomStream random;
    REQUIRE(random.TryReseed(SEED, 0) == true);
    float64 sum = 0;
    float64 squares = 0;
    const float64 deviation = math::Sqrt(ENVELOPE_VARIANCE);
    for (usize sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        // NextFloat01 lies on a 2^-24 grid in [0, 1); half a step moves it into the open interval.
        constexpr float64 HALF_STEP = 0x1p-25;
        const float64 first = static_cast<float64>(random.NextFloat01()) + HALF_STEP;
        const float64 second = static_cast<float64>(random.NextFloat01()) + HALF_STEP;
        // The incident direction is the normal, so the outgoing direction's projection is the half vector.
        const float64 x = deviation * math::ComputeStandardNormalQuantile(first);
        const float64 y = deviation * math::ComputeStandardNormalQuantile(second);
        const float64 projectedSquared = x * x + y * y;
        if (projectedSquared >= 1)
        {
            continue;
        }
        const float64 cosine = math::Sqrt(1 - projectedSquared);
        opticsCase.OutgoingWorld = {x, y, cosine};
        const float64 density =
            math::Exp(-.5 * projectedSquared / ENVELOPE_VARIANCE) / (math::kTau * ENVELOPE_VARIANCE) * cosine;
        const float64 value = EvaluateOpticsCase(opticsCase).DiffractionX * cosine / density;
        sum += value;
        squares += value * value;
    }
    const float64 mean = sum / SAMPLE_COUNT;
    const float64 standardError = math::Sqrt(math::Max(squares / SAMPLE_COUNT - mean * mean, 0.0) / SAMPLE_COUNT);
    INFO("quadrature " << reference << ", estimate " << mean << " +/- " << standardError);
    constexpr float64 STANDARD_ERRORS = 5;
    CHECK(math::Abs(mean - reference) < STANDARD_ERRORS * standardError);
}
