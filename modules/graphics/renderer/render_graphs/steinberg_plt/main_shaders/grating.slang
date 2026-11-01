#pragma once
#include "spectrum.slang"
// Reflection by a sinusoidal surface grating, in wave optics and in its geometric-optics limit.
// Thanks to Shlomi Steinberg and Ling-Qi Yan, "A Generic Framework for Physical Light Transport", ACM
// Transactions on Graphics 40(4), article 139, SIGGRAPH 2021, https://doi.org/10.1145/3450626.3459791 .
// Section 8.1 writes the light that a surface of height h scatters toward a viewer as the convolution of the
// incident light's coherence, through its Fourier transform, with the frequency response
// T^2 = |F{exp(i k (w_i + w_o) . (r + n h(r)))}|^2 (Eqs. (42) and (43)). For h = a sin(2 pi x / period) along
// the grating vector, the Jacobi-Anger expansion exp(i A sin t) = sum_m J_m(A) exp(i m t) (Abramowitz and
// Stegun 9.1.41) turns T^2 into one peak per diffraction order m, where the parallel direction components obey
// the grating equation w_i,par + w_o,par = m (wavelength / period) g, with weight J_m(A_m)^2 and
// A_m = k a (cos theta_i + cos theta_o). Convolving those peaks with the source's angular profile
// (natural_light.slang) gives Eq. (42) for this surface; we derived this closed form from the paper's equations.
// Departures: the paper applies the Church polarization factors through H and Q in Eq. (42); this template is
// scalar and unpolarized, and it multiplies each order by the material's Fresnel reflectance at the order's
// half vector. Orders are not renormalized, so like Kirchhoff scattering the efficiencies sum to one only
// approximately, and evanescent orders carry no energy.
//
// The geometric-optics comparison is the short-wavelength limit that Appendix B establishes for the framework:
// the same sinusoidal surface as an ensemble of mirror facets. Facet slopes s = S sin(theta), with theta uniform
// in [-pi/2, pi/2] and S = 2 pi a / period, have the arcsine density of the slope of a sinusoid at a uniformly
// random position. Shadowing and masking by neighbouring facets are ignored, as Kirchhoff scattering ignores them.

/// Dielectric specular reflectance of the metallic-roughness model, matching the renderer's preview kernel.
static const float GRATING_DIELECTRIC_SPECULAR_REFLECTANCE = 0.04;
/// Below this squared length a projected tangent cannot define the grooves; the next fallback is tried.
static const float GRATING_MINIMUM_GROOVE_LENGTH_SQUARED = 0.01;
/// Bessel arguments below this use the leading terms of the series; J_m(0) is 1 for m = 0 and 0 otherwise.
static const float BESSEL_SMALL_ARGUMENT = 1e-4;
/// Accuracy constant of Miller's starting order, n + sqrt(BESSEL_MILLER_ACCURACY n) (Numerical Recipes' IACC).
static const float BESSEL_MILLER_ACCURACY = 40;
/// Backward recurrence rescaling, which keeps unnormalized values within binary32 range.
static const float BESSEL_RESCALE_THRESHOLD = 1e10;
static const float BESSEL_RESCALE_FACTOR = 1e-10;
/// Starting-order bound of the recurrence; the settings' ranges need fewer than 100 terms.
static const uint BESSEL_MAXIMUM_START_ORDER = 256;
/// Most orders the delta-source loop visits for one wavelength; the settings' ranges need fewer.
static const int GRATING_MAXIMUM_ORDER_SPAN = 128;
/// Probability of the uniform component of the defensive order proposal.
static const float GRATING_ORDER_UNIFORM_PROBABILITY = 0.1;
/// Stratified quadrature points per facet-window integral.
static const uint GRATING_FACET_QUADRATURE_POINTS = 16;
/// Facet-window half-width per source angular radius; covers the source when the window centre is not the
/// exact closest facet.
static const float GRATING_FACET_WINDOW_SCALE = 3;
/// Below this angular speed of the reflected direction, the facet window covers every slope.
static const float GRATING_MINIMUM_REFLECTION_SPEED = 1e-3;

/// Orientation of a grating at a surface point.
struct GratingFrame
{
    float3 NormalWorld;        ///< Mean surface normal, unit length.
    float3 GrooveWorld;        ///< Unit groove direction; the height does not vary along it.
    float3 GratingVectorWorld; ///< Unit in-plane direction of periodicity, NormalWorld x GrooveWorld.
};

/// Grating parameters from the Graph settings, in nanometres.
struct GratingProfile
{
    float PeriodNanometers;
    float AmplitudeNanometers;
};

GratingProfile GetGratingProfile()
{
    GratingProfile result;
    result.PeriodNanometers = PLT_GRATING_PERIOD_NANOMETERS;
    result.AmplitudeNanometers = PLT_GRATING_AMPLITUDE_NANOMETERS;
    return result;
}

float3 ProjectOntoPlane(float3 directionWorld, float3 normalWorld)
{
    return directionWorld - normalWorld * dot(directionWorld, normalWorld);
}

/// Grooves follow the surface tangent when the mesh has one, otherwise world +X projected onto the surface,
/// or world +Z where +X is nearly normal to it.
GratingFrame MakeGratingFrame(float3 normalWorld, float4 tangentWorld)
{
    GratingFrame result;
    result.NormalWorld = normalWorld;
    float3 grooveWorld = float3(0);
    if (abs(tangentWorld.w) > 0.5)
        grooveWorld = ProjectOntoPlane(tangentWorld.xyz, normalWorld);
    if (dot(grooveWorld, grooveWorld) < GRATING_MINIMUM_GROOVE_LENGTH_SQUARED)
        grooveWorld = ProjectOntoPlane(float3(1, 0, 0), normalWorld);
    if (dot(grooveWorld, grooveWorld) < GRATING_MINIMUM_GROOVE_LENGTH_SQUARED)
        grooveWorld = ProjectOntoPlane(float3(0, 0, 1), normalWorld);
    result.GrooveWorld = normalize(grooveWorld);
    result.GratingVectorWorld = cross(normalWorld, result.GrooveWorld);
    return result;
}

/// Metals at or below PLT_GRATING_ROUGHNESS_MAXIMUM are gratings: the conductor model, or the metallic-roughness
/// model with Metallic of at least one half. Roughness is compared through the material's GGX alpha.
bool IsGratingMaterial(MaterialSample material)
{
    bool isMetal = material.Model == LUDUS_MATERIAL_MODEL_CONDUCTOR ||
                   (material.Model == LUDUS_MATERIAL_MODEL_METALLIC_ROUGHNESS && material.Metallic >= 0.5);
    float alpha = ComputeMaterialAlpha(material.Model, material.Roughness, max(material.AlphaU, material.AlphaV));
    return isMetal == true && alpha <= PLT_GRATING_ROUGHNESS_MAXIMUM * PLT_GRATING_ROUGHNESS_MAXIMUM;
}

/// Unpolarized linear RGB Fresnel reflectance of a grating's metal at one cosine.
float3 EvaluateGratingFresnel(MaterialSample material, float cosine)
{
    float clampedCosine = saturate(cosine);
    if (material.Model == LUDUS_MATERIAL_MODEL_CONDUCTOR)
        return ComputeConductorFresnelRgb(clampedCosine, material.Eta, material.K);
    float3 normalReflectance =
        lerp(float3(GRATING_DIELECTRIC_SPECULAR_REFLECTANCE), material.BaseColor, material.Metallic);
    float complement = 1 - clampedCosine;
    float complementSquared = complement * complement;
    return normalReflectance + (1 - normalReflectance) * (complementSquared * complementSquared * complement);
}

// Thanks to William H. Press, Saul A. Teukolsky, William T. Vetterling and Brian P. Flannery, Numerical Recipes in
// C, 2nd ed., section 6.5 (bessj): Miller's backward recurrence J_{n-1} = (2n / x) J_n - J_{n+1}
// (Abramowitz and Stegun 9.1.27), started at an arbitrary scale above the wanted order and normalized with
// J_0 + 2 sum_k J_2k = 1 (Abramowitz and Stegun 9.1.46). Unlike bessj, it also recurs backward when x exceeds the
// order, starting above both, so one loop serves every argument.
/// Bessel function of the first kind J_order(x) for x >= 0.
float EvaluateBesselJ(uint order, float x)
{
    if (x < BESSEL_SMALL_ARGUMENT)
        return order == 0 ? 1 - 0.25 * x * x : (order == 1 ? 0.5 * x : 0);
    float largest = max(float(order), x);
    uint start = 2 * ((uint(largest + sqrt(BESSEL_MILLER_ACCURACY * largest)) + 2) / 2);
    start = min(start, BESSEL_MAXIMUM_START_ORDER);
    float twoOverArgument = 2 / x;
    float higher = 0;  // J_{n+1}, at the running scale
    float current = 1; // J_n
    float evenSum = 0; // sum of J_2k for k >= 1
    float result = 0;
    for (uint n = start; n > 0; --n)
    {
        float lower = float(n) * twoOverArgument * current - higher;
        higher = current;
        current = lower;
        if (abs(current) > BESSEL_RESCALE_THRESHOLD)
        {
            current *= BESSEL_RESCALE_FACTOR;
            higher *= BESSEL_RESCALE_FACTOR;
            result *= BESSEL_RESCALE_FACTOR;
            evenSum *= BESSEL_RESCALE_FACTOR;
        }
        uint lowerOrder = n - 1;
        if (lowerOrder == order)
            result = current;
        if (lowerOrder > 0 && (lowerOrder & 1) == 0)
            evenSum += current;
    }
    return result / (current + 2 * evenSum);
}

/// Incident direction whose order-th diffracted reflection leaves along outgoingWorld, from the grating equation.
/// Returns false for an evanescent order, whose parallel component would exceed one.
bool TryGetOrderIncidentDirection(GratingFrame frame,
                                  float3 outgoingWorld,
                                  int order,
                                  float wavelengthOverPeriod,
                                  out float3 outIncidentWorld)
{
    float3 parallelWorld = -ProjectOntoPlane(outgoingWorld, frame.NormalWorld) +
                           float(order) * wavelengthOverPeriod * frame.GratingVectorWorld;
    float parallelSquared = dot(parallelWorld, parallelWorld);
    if (parallelSquared >= 1)
    {
        outIncidentWorld = float3(0);
        return false;
    }
    outIncidentWorld = parallelWorld + frame.NormalWorld * sqrt(1 - parallelSquared);
    return true;
}

/// Orders whose incident directions propagate for one outgoing direction, as an inclusive range.
/// Returns false when none does, which cannot happen for an outgoing direction above the surface.
bool TryGetPropagatingOrders(GratingFrame frame,
                             float3 outgoingWorld,
                             float wavelengthOverPeriod,
                             out int outFirstOrder,
                             out int outLastOrder)
{
    float3 mirrorParallelWorld = -ProjectOntoPlane(outgoingWorld, frame.NormalWorld);
    float alongVector = dot(mirrorParallelWorld, frame.GratingVectorWorld);
    float alongGrooves = dot(mirrorParallelWorld, frame.GrooveWorld);
    float halfWidthSquared = 1 - alongGrooves * alongGrooves;
    outFirstOrder = 0;
    outLastOrder = -1;
    if (halfWidthSquared <= 0)
        return false;
    float halfWidth = sqrt(halfWidthSquared);
    outFirstOrder = int(ceil((-alongVector - halfWidth) / wavelengthOverPeriod));
    outLastOrder = int(floor((-alongVector + halfWidth) / wavelengthOverPeriod));
    // The bounds are open; drop an endpoint that lies exactly on them.
    float3 incidentWorld;
    if (TryGetOrderIncidentDirection(frame, outgoingWorld, outFirstOrder, wavelengthOverPeriod, incidentWorld) == false)
        outFirstOrder += 1;
    if (TryGetOrderIncidentDirection(frame, outgoingWorld, outLastOrder, wavelengthOverPeriod, incidentWorld) == false)
        outLastOrder -= 1;
    return outFirstOrder <= outLastOrder;
}

/// Efficiency J_m(A_m)^2 of one order, A_m = k a (cos theta_i + cos theta_o); wavenumberAmplitude is k a.
float EvaluateOrderEfficiency(GratingFrame frame,
                              float3 incidentWorld,
                              float3 outgoingWorld,
                              int order,
                              float wavenumberAmplitude)
{
    float argument =
        wavenumberAmplitude * (dot(incidentWorld, frame.NormalWorld) + dot(outgoingWorld, frame.NormalWorld));
    float amplitude = EvaluateBesselJ(uint(abs(order)), argument);
    return amplitude * amplitude;
}

// Order proposal for the traced estimate. For a large argument A, J_m(A)^2 oscillates about the arcsine density
// 1 / (pi sqrt(A^2 - m^2)) on |m| < A, which is also the geometric-optics slope distribution. The proposal draws
// a continuous arcsine variable on [-A0, A0], A0 = 2 k a cos theta_o (the mirror order's argument), restricted to
// the propagating orders, and rounds it to the nearest order. Thanks to Tim Hesterberg, "Weighted Average
// Importance Sampling and Defensive Mixture Distributions", Technometrics 37(2), 1995, pp. 185-194 : a
// uniform component with probability GRATING_ORDER_UNIFORM_PROBABILITY bounds
// the weight of orders the arcsine misses. A flat grating needs no proposal: its mirror order carries all the light.

/// Cumulative arcsine distribution on [-argument, argument] at t; a step at zero for a zero argument.
float EvaluateArcsineDistribution(float t, float argument)
{
    if (argument <= 0)
        return t >= 0 ? 1 : 0;
    return 0.5 + asin(clamp(t / argument, -1.0, 1.0)) / PI;
}

/// Order proposal over [firstOrder, lastOrder], for one outgoing direction and wavelength.
struct OrderProposal
{
    int FirstOrder;
    int LastOrder;
    float Argument;           ///< A0, the arcsine half-width in orders.
    float LowerMass;          ///< Arcsine distribution at firstOrder - 1/2.
    float Mass;               ///< Arcsine probability of the propagating orders.
    float UniformProbability; ///< Weight of the uniform component.
};

OrderProposal MakeOrderProposal(int firstOrder, int lastOrder, float argument)
{
    OrderProposal result;
    result.FirstOrder = firstOrder;
    result.LastOrder = lastOrder;
    result.Argument = argument;
    result.LowerMass = EvaluateArcsineDistribution(float(firstOrder) - 0.5, argument);
    result.Mass = EvaluateArcsineDistribution(float(lastOrder) + 0.5, argument) - result.LowerMass;
    result.UniformProbability = GRATING_ORDER_UNIFORM_PROBABILITY;
    // The mirror order always propagates and holds arcsine mass, so Mass is positive; guard it regardless.
    if (result.Mass <= 0)
        result.UniformProbability = 1;
    return result;
}

/// Probability that SampleOrder returns order.
float EvaluateOrderProbability(OrderProposal proposal, int order)
{
    if (order < proposal.FirstOrder || order > proposal.LastOrder)
        return 0;
    float count = float(proposal.LastOrder - proposal.FirstOrder + 1);
    float arcsine = proposal.Mass > 0 ? (EvaluateArcsineDistribution(float(order) + 0.5, proposal.Argument) -
                                         EvaluateArcsineDistribution(float(order) - 0.5, proposal.Argument)) /
                                            proposal.Mass
                                      : 0;
    return (1 - proposal.UniformProbability) * arcsine + proposal.UniformProbability / count;
}

/// Draw an order from the proposal with one uniform value in [0, 1).
int SampleOrder(OrderProposal proposal, float randomValue)
{
    int count = proposal.LastOrder - proposal.FirstOrder + 1;
    if (randomValue < proposal.UniformProbability)
    {
        int offset = int(randomValue / proposal.UniformProbability * float(count));
        return proposal.FirstOrder + min(offset, count - 1);
    }
    float rescaled = (randomValue - proposal.UniformProbability) / (1 - proposal.UniformProbability);
    float cumulative = proposal.LowerMass + rescaled * proposal.Mass;
    float t = proposal.Argument * sin(PI * (cumulative - 0.5));
    return clamp(int(round(t)), proposal.FirstOrder, proposal.LastOrder);
}

/// Maximum slope 2 pi a / period of the sinusoidal height profile.
float ComputeMaximumFacetSlope(GratingProfile profile)
{
    return 2 * PI * profile.AmplitudeNanometers / profile.PeriodNanometers;
}

/// Unit normal of the facet with the given slope dh/dx along the grating vector.
float3 MakeFacetNormal(GratingFrame frame, float slope)
{
    return normalize(frame.NormalWorld - slope * frame.GratingVectorWorld);
}

/// Mirror reflection of outgoingWorld about a unit normal.
float3 ReflectAbout(float3 outgoingWorld, float3 normalWorld)
{
    return 2 * dot(outgoingWorld, normalWorld) * normalWorld - outgoingWorld;
}

/// Radiance weight of a facet seen from outgoingWorld: its projected area per projected area of the mean surface,
/// (w_o . n_f) / ((w_o . n) (n_f . n)). Its mean over the slope distribution is one; a facet facing away is zero.
float ComputeFacetWeight(GratingFrame frame, float3 facetNormalWorld, float3 outgoingWorld)
{
    float facing = dot(outgoingWorld, facetNormalWorld);
    if (facing <= 0)
        return 0;
    return facing / (dot(outgoingWorld, frame.NormalWorld) * dot(facetNormalWorld, frame.NormalWorld));
}
