#pragma once
// Natural light sources and the spatial coherence of the light they emit.
// Thanks to Shlomi Steinberg and Ling-Qi Yan, "A Generic Framework for Physical Light Transport", ACM
// Transactions on Graphics 40(4), article 139, SIGGRAPH 2021, https://doi.org/10.1145/3450626.3459791 .
// Section 5 models a natural source as a sphere of radius rho filled with independently radiating elementary
// radiators. Its incoherent sourcing operator (Definition 5.1, Eq. (25)) makes the degree of coherence between
// two points at distance r from the source a function of rho k |r1 - r2| / r only, through
// J_{3/2}(x) / x^{3/2}. Its first zero, x = 4.4934, gives the coherence radius r_c = 4.4934 r / (k rho): about
// 77 um for sunlight on earth at 0.5 um, which the paper compares with measurements (section 5.1, Analysis).
//
// Section 8.1 (Eqs. (41) and (42)) scatters that light from a surface through the Fourier transform of this
// coherence function. By the van Cittert-Zernike theorem, which the paper also cites, that transform is the
// source's angular brightness distribution. For the uniformly radiating sphere it follows from the projection-
// slice theorem: the brightness is proportional to the chord through the sphere, sqrt(1 - (q / q0)^2) at angle q
// from the centre of a source of angular radius q0 (EvaluateNaturalSourceProfile). This closed form is our
// derivation, not an equation printed in the paper. The paper evaluates Eq. (42) in the far field (rho << r); the
// profile's normalization keeps the next term of sin(q) and is accurate for q0 up to
// NATURAL_SOURCE_MAXIMUM_ANGULAR_RADIUS.

/// First positive zero of J_{3/2}(x), the root of tan(x) = x; the coherence radius of Definition 5.1.
static const float NATURAL_SOURCE_COHERENCE_ZERO = 4.49340945790906;
/// Largest angular radius, in radians, at which a source is evaluated; nearer sources are clamped to it.
static const float NATURAL_SOURCE_MAXIMUM_ANGULAR_RADIUS = 0.5;
/// World positions are in metres.
static const float NANOMETERS_PER_WORLD_UNIT = 1e9;
static const float MICROMETERS_PER_WORLD_UNIT = 1e6;
static const float WORLD_UNITS_PER_MILLIMETER = 1e-3;

/// Angular radius, in radians, of a natural source of radiusWorld whose centre is distanceWorld away.
float ComputeNaturalSourceAngularRadius(float radiusWorld, float distanceWorld)
{
    if (radiusWorld <= 0)
        return 0;
    if (distanceWorld <= radiusWorld)
        return NATURAL_SOURCE_MAXIMUM_ANGULAR_RADIUS;
    return min(asin(radiusWorld / distanceWorld), NATURAL_SOURCE_MAXIMUM_ANGULAR_RADIUS);
}

/// Spatial coherence radius, in world units, of natural light from a source of the given angular radius:
/// the first zero of Definition 5.1's degree of coherence, NATURAL_SOURCE_COHERENCE_ZERO / (k sin(q0)).
float ComputeCoherenceRadiusWorld(float angularRadius, float wavelengthNanometers)
{
    float wavenumberPerWorld = 2 * PI * NANOMETERS_PER_WORLD_UNIT / wavelengthNanometers;
    return NATURAL_SOURCE_COHERENCE_ZERO / (wavenumberPerWorld * sin(angularRadius));
}

/// Angle, in radians, between two unit directions; accurate for small angles, unlike acos of their dot product.
float ComputeAngleBetween(float3 firstWorld, float3 secondWorld)
{
    return 2 * asin(min(length(firstWorld - secondWorld) * 0.5, 1));
}

/// Angular brightness distribution of a natural source per steradian, at angle angleFromCenter from the
/// direction of its centre: sqrt(1 - (q / q0)^2) inside the angular radius q0, zero outside. It integrates to
/// one over the sphere of directions, so multiplying it by the source's solid-angle-integrated radiance
/// (a LightSample's IncidentCoefficient) gives the radiance arriving from that direction.
float EvaluateNaturalSourceProfile(float angleFromCenter, float angularRadius)
{
    if (angularRadius <= 0 || angleFromCenter >= angularRadius)
        return 0;
    float ratio = angleFromCenter / angularRadius;
    float radiusSquared = angularRadius * angularRadius;
    // 2 pi integral of sqrt(1 - (q / q0)^2) sin(q) dq over [0, q0], to fourth order in q0.
    float normalization = 2 * PI * radiusSquared * (1.0 / 3.0 - radiusSquared / 45.0);
    return sqrt(1 - ratio * ratio) / normalization;
}
