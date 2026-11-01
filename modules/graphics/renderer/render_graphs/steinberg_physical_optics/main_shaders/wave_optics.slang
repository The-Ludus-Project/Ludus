#pragma once
#include "spectrum.slang"
// Physical-optics scattering of partially coherent, polarized light, following Shlomi Steinberg, Pradeep Sen and
// Ling-Qi Yan, "Towards Practical Physical-Optics Rendering", ACM Transactions on Graphics 41(4), article 132,
// SIGGRAPH 2022, https://doi.org/10.1145/3528223.3530119 , and its supplemental derivations. Scene-free: shade.slang
// supplies surfaces and lights, and the parity kernel (tests/steinberg_physical_optics_parity.slang) runs these
// functions against the CPU oracle.
//
// A beam (wave packet, section 3.2) carries the generalized radiance of Definition 3.1 at xi = 0: a classical
// radiance-carrying Stokes vector in its local frame [mu] = (x, y, propagation), plus the anisotropic Gaussian
// spatial coherence of each transverse component, the shape matrices Theta_{x,y} of Eq. (12). Theta's transverse
// block is (r / k)^2 theta_{x,y}: theta stays constant on propagation (Property 3.1) and only the mean propagation
// distance r grows. Its longitudinal variance is sigma_zz^2 (section 4: 1 mm). Theta_1/2 uses (theta_x + theta_y) / 2.
//
// Emission (section 4, Eq. (25)): a small radiator of area A emitting into a solid angle Omega has
// theta = 2 pi Omega / A, isotropic and unpolarized (L_x = L_y). The paper's defaults are A = 1 mm^2 and
// Omega = 1e-6 sr. For sunlight it sets Omega / A so that the characteristic coherence length on earth is about
// sigma_sun = 50 um; we fix that length at OPTICS_REFERENCE_WAVELENGTH_NANOMETERS and keep Eq. (25)'s 1 / k^2.
//
// Scattering (section 3.5): the wave BSDF of Eq. (22) partitions a surface's response into a direct (specular)
// part, weighted by alpha_direct = exp(-(k cos theta_i q)^2) with q_rel = q (section 4.1.1), and the scattering wBSDF
// of Eq. (16). Eq. (16) applies the diffraction operator D (Eq. (15)) to each transverse component with that
// component's shape matrix, rotated to the matter's frame and restricted to the surface plane (section 4.1.1), and
// then the polarimetric BSDF: the Fresnel Mueller matrix of Eq. (29) in the s/p frame, after the Mueller rotation
// of Eq. (59). For a Gaussian surface D is Eq. (28); for a sinusoidal grating it is Eq. (34) convolved with the
// coherence Gaussian (section 4.1.3); for a perfectly diffuse surface D = 1 (Eq. (32)). The scattered beam's
// shape matrices follow Eq. (20): mixed and rotated into the outgoing frame, then deformed by U (Eqs. (18) and
// (30)), whose Hessians we take by finite differences with five extra Fresnel evaluations, as the paper does.
// Importance sampling uses the maximally incoherent envelope of Eqs. (23), (24) and (31): D with Theta = sigma_min I,
// two independent Gaussians in the half-vector analogue h, drawn through the inverse normal distribution (the paper
// cites Winitzki's inverse error function; FoundationMath's ComputeStandardNormalQuantile is used instead).
//
// Units and conventions, and where they depart from the printed equations:
// - Dimensionless frequencies: h~ = h / k = (r^ + s^), with s^ pointing towards the light (section 4.1.3's DC lobe
//   scatters at theta_o = theta_i, so h vanishes at the mirror direction) and the coherence variance k^2 Theta.
//   Its transverse block, r^2 theta, does not depend on the wavelength. D then needs no 1 / k^2 rescaling.
// - Scale of D: Eq. (28)'s prefactor q^2 and Eq. (29)'s 1 / lambda^2 do not combine into a dimensionless BRDF. We
//   normalize D to the unit-integral Gaussian N(h~; Sigma~^-1 + (k sigma_s)^-2 I) in h~, so that the scattered lobe
//   carries the fraction 1 - alpha_direct of Eq. (22) over the propagating half vectors (d^2 h~ = cos theta_o
//   d omega_o). The convolution of Eq. (15) preserves that integral, so coherence redistributes energy without
//   creating it, as Fig. 5's caption requires. The same normalization makes Eq. (32)'s D = 1 the Lambertian BRDF
//   m00 / pi.
// - Radiance: the paper's W carries cos theta_o (Fig. 5 plots it against (1 / pi) cos theta_o); the radiance
//   that enters Eq. (26) is W / cos theta_o, the BRDF. EvaluateScatteredRadiance returns that radiance.
// - s/p frame: Eqs. (29), (30) and (57) give the x component |f_pp|^2, so the frame's x axis lies in the plane of
//   scattering (p) and y is perpendicular to it (s). Section 4.1.1's text names the axes the other way round.
// - Eq. (18) states that U has unit determinant; the printed formula divides by |U~^-1| and not its square root. We
//   normalize to unit determinant. Binary32 cannot resolve the curvature of a nearly flat Fresnel intensity: U~'s
//   eigenvalues below OPTICS_HESSIAN_RESOLUTION of the intensity per squared step are raised to that bound (U is the
//   identity when both are below it), and their ratio is capped at OPTICS_MAXIMUM_DEFORMATION_ANISOTROPY; a clearly
//   indefinite U~ also gives the identity. The paper does not discuss these cases.
// - Eq. (59) rotates the Stokes vector by 2 phi; the sign here is the frame rotation that carries the incident x
//   axis onto the p axis, so that linearly polarized light keeps its orientation in space.
// - Eq. (33)'s grating phase amplitude k b / (2 cos theta_i cos theta_o) is used as printed; its cosines are
//   clamped at OPTICS_MINIMUM_COSINE.

/// Wavelength at which the sunlight coherence length is given and the surface correlation length is fitted.
static const float OPTICS_REFERENCE_WAVELENGTH_NANOMETERS = 550;
/// World positions are in metres.
static const float NANOMETERS_PER_WORLD_UNIT = 1e9;
static const float MICROMETERS_PER_WORLD_UNIT = 1e6;
static const float WORLD_UNITS_PER_MILLIMETER = 1e-3;
static const float WORLD_UNITS_PER_CENTIMETER = 1e-2;
static const float WORLD_UNITS_PER_MICROMETER = 1e-6;
static const float SQUARE_WORLD_UNITS_PER_SQUARE_MILLIMETER = 1e-6;
static const float STERADIANS_PER_MICROSTERADIAN = 1e-6;
/// Mean distance from the earth to the sun, in metres: the propagation distance of directional light.
static const float OPTICS_SUN_DISTANCE_WORLD = 1.496e11;
/// Smallest cosine used by the grating phase amplitude and by frame constructions.
static const float OPTICS_MINIMUM_COSINE = 1e-3;
/// Below this squared length a projected axis cannot define a frame and the next fallback is used.
static const float OPTICS_MINIMUM_AXIS_LENGTH_SQUARED = 1e-6;
/// Finite-difference step of the Hessians in Eq. (19), in radians of the outgoing direction.
static const float OPTICS_HESSIAN_STEP = 0.05;
/// Smallest second difference, relative to the intensity at the centre, that binary32 resolves.
static const float OPTICS_HESSIAN_RESOLUTION = 1e-4;
/// Largest ratio of U~'s eigenvalues; U's axes then differ by at most its square root.
static const float OPTICS_MAXIMUM_DEFORMATION_ANISOTROPY = 100;
/// Smallest probability passed to the inverse normal distribution, which needs the open interval (0, 1).
static const float OPTICS_MINIMUM_NORMAL_PROBABILITY = 1e-7;
/// Bessel arguments below this use the series' leading terms.
static const float BESSEL_SMALL_ARGUMENT = 1e-4;
/// Above this argument J_n(x) uses its leading asymptotic term (Abramowitz and Stegun 9.2.1); it is accurate to
/// about 1e-3 there for the orders a grating renders.
static const float BESSEL_ASYMPTOTIC_ARGUMENT = 64;
/// Accuracy constant of Miller's starting order, n + sqrt(BESSEL_MILLER_ACCURACY n) (Numerical Recipes' IACC).
static const float BESSEL_MILLER_ACCURACY = 40;
/// Backward recurrence rescaling, which keeps unnormalized values within binary32 range.
static const float BESSEL_RESCALE_THRESHOLD = 1e10;
static const float BESSEL_RESCALE_FACTOR = 1e-10;
/// Largest grating order the template renders; the Graph setting is clamped to it.
static const uint OPTICS_GRATING_MAXIMUM_ORDER = 8;

// ---------------------------------------------------------------------------------------------------------------
// Complex numbers for the Fresnel coefficients.

struct Complex
{
    float Real;
    float Imaginary;
};

Complex MakeComplex(float real, float imaginary)
{
    Complex result;
    result.Real = real;
    result.Imaginary = imaginary;
    return result;
}

Complex AddComplex(Complex first, Complex second)
{
    return MakeComplex(first.Real + second.Real, first.Imaginary + second.Imaginary);
}

Complex SubtractComplex(Complex first, Complex second)
{
    return MakeComplex(first.Real - second.Real, first.Imaginary - second.Imaginary);
}

Complex MultiplyComplex(Complex first, Complex second)
{
    return MakeComplex(first.Real * second.Real - first.Imaginary * second.Imaginary,
                       first.Real * second.Imaginary + first.Imaginary * second.Real);
}

Complex ConjugateComplex(Complex value)
{
    return MakeComplex(value.Real, -value.Imaginary);
}

float ComputeSquaredMagnitude(Complex value)
{
    return value.Real * value.Real + value.Imaginary * value.Imaginary;
}

/// numerator / denominator; zero for a zero denominator.
Complex DivideComplex(Complex numerator, Complex denominator)
{
    float squaredMagnitude = ComputeSquaredMagnitude(denominator);
    if (squaredMagnitude <= 0)
        return MakeComplex(0, 0);
    Complex product = MultiplyComplex(numerator, ConjugateComplex(denominator));
    return MakeComplex(product.Real / squaredMagnitude, product.Imaginary / squaredMagnitude);
}

/// Principal square root, whose real part is never negative.
Complex ComputeComplexSquareRoot(Complex value)
{
    float magnitude = sqrt(ComputeSquaredMagnitude(value));
    float real = sqrt(max((magnitude + value.Real) * 0.5, 0.0));
    float imaginary = sqrt(max((magnitude - value.Real) * 0.5, 0.0));
    return MakeComplex(real, value.Imaginary < 0 ? -imaginary : imaginary);
}

// ---------------------------------------------------------------------------------------------------------------
// Fresnel coefficients and the polarimetric BSDF (Eq. (29)).

/// Complex amplitude reflection coefficients of an interface with relative index eta = n + i kappa.
struct FresnelCoefficients
{
    Complex Perpendicular; ///< f_ss
    Complex Parallel;      ///< f_pp
};

/// Reflection coefficients at the cosine between the incident direction and the relative normal (the half vector
/// for reflective scattering, section 4.1.1).
FresnelCoefficients ComputeFresnelCoefficients(float cosine, Complex eta)
{
    float clampedCosine = clamp(cosine, 0.0, 1.0);
    Complex incidentCosine = MakeComplex(clampedCosine, 0);
    Complex sineOverEtaSquared =
        DivideComplex(MakeComplex(1 - clampedCosine * clampedCosine, 0), MultiplyComplex(eta, eta));
    Complex transmittedCosine = ComputeComplexSquareRoot(SubtractComplex(MakeComplex(1, 0), sineOverEtaSquared));
    Complex etaTransmittedCosine = MultiplyComplex(eta, transmittedCosine);
    Complex etaIncidentCosine = MultiplyComplex(eta, incidentCosine);
    FresnelCoefficients result;
    result.Perpendicular = DivideComplex(SubtractComplex(incidentCosine, etaTransmittedCosine),
                                         AddComplex(incidentCosine, etaTransmittedCosine));
    result.Parallel = DivideComplex(SubtractComplex(etaIncidentCosine, transmittedCosine),
                                    AddComplex(etaIncidentCosine, transmittedCosine));
    return result;
}

/// Unpolarized reflectance (|f_ss|^2 + |f_pp|^2) / 2, Eq. (29)'s m00 without its 1 / lambda^2.
float ComputeUnpolarizedReflectance(float cosine, Complex eta)
{
    FresnelCoefficients fresnel = ComputeFresnelCoefficients(cosine, eta);
    return 0.5 * (ComputeSquaredMagnitude(fresnel.Perpendicular) + ComputeSquaredMagnitude(fresnel.Parallel));
}

/// Eq. (29)'s Mueller matrix applied to a Stokes vector given in the s/p frame (x = p, y = s).
float4 ApplyFresnelMueller(FresnelCoefficients fresnel, float4 stokes)
{
    float parallelSquared = ComputeSquaredMagnitude(fresnel.Parallel);
    float perpendicularSquared = ComputeSquaredMagnitude(fresnel.Perpendicular);
    float m00 = 0.5 * (parallelSquared + perpendicularSquared);
    float m01 = 0.5 * (parallelSquared - perpendicularSquared);
    Complex cross = MultiplyComplex(fresnel.Perpendicular, ConjugateComplex(fresnel.Parallel));
    float m22 = cross.Real;
    float m23 = cross.Imaginary;
    return float4(m00 * stokes.x + m01 * stokes.y,
                  m01 * stokes.x + m00 * stokes.y,
                  m22 * stokes.z + m23 * stokes.w,
                  -m23 * stokes.z + m22 * stokes.w);
}

/// Express a Stokes vector in a frame whose x axis is turned by angle (radians, from x towards y) in the same
/// transverse plane (Eq. (59)).
float4 RotateStokesFrame(float4 stokes, float angle)
{
    float cosine = cos(2 * angle);
    float sine = sin(2 * angle);
    return float4(stokes.x, cosine * stokes.y + sine * stokes.z, -sine * stokes.y + cosine * stokes.z, stokes.w);
}

/// Enforce Eq. (9), S0^2 >= S1^2 + S2^2 + S3^2: Eq. (16) scales S2 and S3 by D of the 1/2 component and S0, S1 by
/// D of the x and y components, so anisotropic coherence can leave a degree of polarization above one. The polarized
/// part is shortened to S0; a negative S0 becomes zero.
float4 EnforceRealizableStokes(float4 stokes)
{
    float intensity = max(stokes.x, 0.0);
    float polarized = length(stokes.yzw);
    if (polarized <= intensity)
        return float4(intensity, stokes.yzw);
    return float4(intensity, stokes.yzw * (intensity / polarized));
}

/// Intensity behind an ideal linear polarizer whose transmission axis is at angle (radians, from x towards y).
/// Clamped at zero: a Stokes vector that violates Eq. (9) by rounding must not transmit negative power.
float ApplyLinearPolarizer(float4 stokes, float angle)
{
    return max(0.5 * (stokes.x + cos(2 * angle) * stokes.y + sin(2 * angle) * stokes.z), 0.0);
}

// ---------------------------------------------------------------------------------------------------------------
// Symmetric 2 x 2 matrices, stored as (xx, xy, yy).

float ComputeSymmetricDeterminant(float3 matrix)
{
    return matrix.x * matrix.z - matrix.y * matrix.y;
}

/// R m R^T for the coordinate change into axes turned by angle from x towards y; R(phi)^T m R(phi) in Eq. (20).
float3 RotateSymmetric(float3 matrix, float angle)
{
    float cosine = cos(angle);
    float sine = sin(angle);
    float cosineSquared = cosine * cosine;
    float sineSquared = sine * sine;
    float product = cosine * sine;
    return float3(cosineSquared * matrix.x + 2 * product * matrix.y + sineSquared * matrix.z,
                  -product * matrix.x + (cosineSquared - sineSquared) * matrix.y + product * matrix.z,
                  sineSquared * matrix.x - 2 * product * matrix.y + cosineSquared * matrix.z);
}

/// U m U^T for a general 2 x 2 matrix U given by rows (u00, u01) and (u10, u11).
float3 TransformSymmetric(float3 matrix, float4 transform)
{
    float2 firstRow =
        float2(transform.x * matrix.x + transform.y * matrix.y, transform.x * matrix.y + transform.y * matrix.z);
    float2 secondRow =
        float2(transform.z * matrix.x + transform.w * matrix.y, transform.z * matrix.y + transform.w * matrix.z);
    return float3(firstRow.x * transform.x + firstRow.y * transform.y,
                  firstRow.x * transform.z + firstRow.y * transform.w,
                  secondRow.x * transform.z + secondRow.y * transform.w);
}

/// Smaller eigenvalue of a symmetric matrix.
float ComputeSmallerEigenvalue(float3 matrix)
{
    float mean = 0.5 * (matrix.x + matrix.z);
    float halfDifference = 0.5 * (matrix.x - matrix.z);
    return mean - sqrt(halfDifference * halfDifference + matrix.y * matrix.y);
}

/// The bivariate normal density N(offset; 0, covariance); zero for a covariance that is not positive definite.
float EvaluateBivariateNormal(float2 offset, float3 covariance)
{
    float determinant = ComputeSymmetricDeterminant(covariance);
    if (determinant <= 0 || covariance.x <= 0)
        return 0;
    float3 inverse = float3(covariance.z, -covariance.y, covariance.x) / determinant;
    float exponent =
        inverse.x * offset.x * offset.x + 2 * inverse.y * offset.x * offset.y + inverse.z * offset.y * offset.y;
    return exp(-0.5 * exponent) / (2 * PI * sqrt(determinant));
}

// ---------------------------------------------------------------------------------------------------------------
// Beams (Definition 3.1) and their emission (Eq. (25)).

/// A wave packet at one wavelength, before or after an interaction.
struct Beam
{
    float3 PropagationWorld; ///< Unit mean direction of propagation, the local z axis.
    float3 TransverseXWorld; ///< Unit local x axis; y = cross(PropagationWorld, TransverseXWorld).
    float4 Stokes;           ///< Radiance-carrying classical Stokes vector (L0, L1, L2, L3) in the local frame.
    float3 ShapeX;           ///< theta_x of Eq. (12) as (xx, xy, yy), in m^-2.
    float3 ShapeY;           ///< theta_y.
    float DistanceWorld;     ///< Mean propagation distance r.
};

float3 GetTransverseYWorld(Beam beam)
{
    return cross(beam.PropagationWorld, beam.TransverseXWorld);
}

/// A unit vector perpendicular to directionWorld.
float3 MakePerpendicularWorld(float3 directionWorld)
{
    float3 helperWorld = abs(directionWorld.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0);
    return normalize(cross(helperWorld, directionWorld));
}

/// theta of a small radiator of area A (square metres) emitting into the solid angle Omega (steradians), Eq. (25).
float ComputeRadiatorShape(float radiatorAreaWorld, float beamSolidAngle)
{
    return 2 * PI * beamSolidAngle / radiatorAreaWorld;
}

/// theta of sunlight whose characteristic coherence length at the reference wavelength is sunCoherenceWorld.
float ComputeSunShape(float sunCoherenceWorld)
{
    float wavenumber = 2 * PI * NANOMETERS_PER_WORLD_UNIT / OPTICS_REFERENCE_WAVELENGTH_NANOMETERS;
    float ratio = wavenumber * sunCoherenceWorld / OPTICS_SUN_DISTANCE_WORLD;
    return ratio * ratio;
}

/// Unpolarized natural light (L_x = L_y, chi = varsigma = 0, section 4) with isotropic coherence shape theta.
Beam MakeNaturalBeam(float3 propagationWorld, float radiance, float shape, float distanceWorld)
{
    Beam result;
    result.PropagationWorld = propagationWorld;
    result.TransverseXWorld = MakePerpendicularWorld(propagationWorld);
    result.Stokes = float4(radiance, 0, 0, 0);
    result.ShapeX = float3(shape, 0, shape);
    result.ShapeY = result.ShapeX;
    result.DistanceWorld = distanceWorld;
    return result;
}

/// Free propagation over distanceWorld: only r grows (Property 3.1).
Beam PropagateBeam(Beam beam, float distanceWorld)
{
    Beam result = beam;
    result.DistanceWorld += distanceWorld;
    return result;
}

/// Characteristic transverse coherence length, sqrt of Theta's smallest transverse eigenvalue, in world units:
/// (r / k) sqrt(min eig theta) over both components.
float ComputeCoherenceLengthWorld(Beam beam, float wavelengthNanometers)
{
    float wavenumber = 2 * PI * NANOMETERS_PER_WORLD_UNIT / wavelengthNanometers;
    float smallest = min(ComputeSmallerEigenvalue(beam.ShapeX), ComputeSmallerEigenvalue(beam.ShapeY));
    return beam.DistanceWorld * sqrt(max(smallest, 0.0)) / wavenumber;
}

// ---------------------------------------------------------------------------------------------------------------
// Surfaces and the diffraction operator.

/// Orthonormal frame of a scattering surface.
struct SurfaceFrame
{
    float3 NormalWorld;
    float3 TangentWorld;
    float3 BitangentWorld;
};

SurfaceFrame MakeSurfaceFrame(float3 normalWorld, float3 tangentHintWorld)
{
    SurfaceFrame result;
    result.NormalWorld = normalWorld;
    float3 tangentWorld = tangentHintWorld - normalWorld * dot(tangentHintWorld, normalWorld);
    if (dot(tangentWorld, tangentWorld) < OPTICS_MINIMUM_AXIS_LENGTH_SQUARED)
        tangentWorld = MakePerpendicularWorld(normalWorld);
    result.TangentWorld = normalize(tangentWorld);
    result.BitangentWorld = cross(normalWorld, result.TangentWorld);
    return result;
}

/// h~ = r^ + s^ projected onto the surface, in (tangent, bitangent) coordinates.
float2 ComputeSurfaceHalfVector(SurfaceFrame frame, float3 incidentWorld, float3 outgoingWorld)
{
    float3 sumWorld = incidentWorld + outgoingWorld;
    return float2(dot(sumWorld, frame.TangentWorld), dot(sumWorld, frame.BitangentWorld));
}

/// Sigma~^-1, the inverse of k^2 Xi restricted to the surface plane (section 4.1.1), for one beam component. It is
/// held in the basis (Axis, perpendicular) whose first axis is the beam's propagation direction projected onto the
/// surface: there the longitudinal variance (k sigma_zz)^2, about 1e8 against a transverse r^2 theta of tens, adds to
/// one element only, and the inverse needs no difference of large numbers. In the surface's (tangent, bitangent) axes
/// binary32 would lose the narrow direction of the lobe to cancellation.
struct SurfaceCoherence
{
    float2 AxisSurface; ///< Unit first axis in (tangent, bitangent) coordinates.
    float3 Inverse;     ///< Sigma~^-1 as (axis-axis, axis-perpendicular, perpendicular-perpendicular).
};

/// The coherent limit, Sigma~^-1 = 0: the radiometric comparison's D.
SurfaceCoherence MakeCoherentLimit()
{
    SurfaceCoherence result;
    result.AxisSurface = float2(1, 0);
    result.Inverse = float3(0);
    return result;
}

/// (tangent, bitangent) coordinates expressed in the coherence's basis.
float2 RotateIntoCoherenceBasis(SurfaceCoherence coherence, float2 surface)
{
    float2 axis = coherence.AxisSurface;
    return float2(dot(surface, axis), dot(surface, float2(-axis.y, axis.x)));
}

/// k^2 Xi of the beam component with shape theta (Eq. (14)), restricted to the surface plane and inverted. k^2 Theta
/// is r^2 theta transversally and (k sigma_zz)^2 along the propagation direction.
[noinline] SurfaceCoherence ComputeSurfaceCoherence(Beam beam,
                                                    float3 shape,
                                                    SurfaceFrame frame,
                                                    float wavenumberPerWorld,
                                                    float longitudinalCoherenceWorld) {
    SurfaceCoherence result;
    float2 projected =
        float2(dot(frame.TangentWorld, beam.PropagationWorld), dot(frame.BitangentWorld, beam.PropagationWorld));
    float projectedLength = length(projected);
    result.AxisSurface =
        projectedLength > OPTICS_MINIMUM_AXIS_LENGTH_SQUARED ? projected / projectedLength : float2(1, 0);
    float3 axisWorld = result.AxisSurface.x * frame.TangentWorld + result.AxisSurface.y * frame.BitangentWorld;
    float3 perpendicularWorld = cross(frame.NormalWorld, axisWorld);
    float3 yWorld = GetTransverseYWorld(beam);
    // The transverse axes in the basis: rows (axis, perpendicular), columns (beam x, beam y).
    float2 axisLocal = float2(dot(axisWorld, beam.TransverseXWorld), dot(axisWorld, yWorld));
    float2 perpendicularLocal = float2(dot(perpendicularWorld, beam.TransverseXWorld), dot(perpendicularWorld, yWorld));
    float3 transverse = shape * (beam.DistanceWorld * beam.DistanceWorld);
    float axisAxis = axisLocal.x * axisLocal.x * transverse.x + 2 * axisLocal.x * axisLocal.y * transverse.y +
                     axisLocal.y * axisLocal.y * transverse.z;
    float axisPerpendicular = axisLocal.x * perpendicularLocal.x * transverse.x +
                              (axisLocal.x * perpendicularLocal.y + axisLocal.y * perpendicularLocal.x) * transverse.y +
                              axisLocal.y * perpendicularLocal.y * transverse.z;
    float perpendicularPerpendicular = perpendicularLocal.x * perpendicularLocal.x * transverse.x +
                                       2 * perpendicularLocal.x * perpendicularLocal.y * transverse.y +
                                       perpendicularLocal.y * perpendicularLocal.y * transverse.z;
    float longitudinal = wavenumberPerWorld * longitudinalCoherenceWorld * projectedLength;
    // det of the transverse block is det(r^2 theta) det(P)^2, both without cancellation for the shapes beams carry.
    float projectionDeterminant = axisLocal.x * perpendicularLocal.y - axisLocal.y * perpendicularLocal.x;
    float transverseDeterminant =
        ComputeSymmetricDeterminant(transverse) * projectionDeterminant * projectionDeterminant;
    float determinant = longitudinal * longitudinal * perpendicularPerpendicular + transverseDeterminant;
    if (determinant <= 0)
    {
        result.Inverse = float3(0);
        return result;
    }
    result.Inverse =
        float3(perpendicularPerpendicular, -axisPerpendicular, axisAxis + longitudinal * longitudinal) / determinant;
    return result;
}

/// Eq. (28), normalized: N(h~; Sigma~^-1 + (k sigma_s)^-2 I), with inverseCorrelationSquared = (k sigma_s)^-2. The
/// correlation term is isotropic, so it adds in the coherence's basis unchanged.
float EvaluateGaussianSurfaceDiffraction(float2 halfVector, SurfaceCoherence coherence, float inverseCorrelationSquared)
{
    return EvaluateBivariateNormal(RotateIntoCoherenceBasis(coherence, halfVector),
                                   coherence.Inverse + float3(inverseCorrelationSquared, 0, inverseCorrelationSquared));
}

// Thanks to William H. Press, Saul A. Teukolsky, William T. Vetterling and Brian P. Flannery, Numerical Recipes in
// C, 2nd ed., section 6.5 (bessj): Miller's backward recurrence J_{n-1} = (2n / x) J_n - J_{n+1}
// (Abramowitz and Stegun 9.1.27), started above both the order and the argument and normalized with
// J_0 + 2 sum_k J_2k = 1 (Abramowitz and Stegun 9.1.46), as in the steinberg-plt template.
/// Bessel function of the first kind J_order(x) for x >= 0.
float EvaluateBesselJ(uint order, float x)
{
    if (x < BESSEL_SMALL_ARGUMENT)
        return order == 0 ? 1 - 0.25 * x * x : (order == 1 ? 0.5 * x : 0);
    if (x > BESSEL_ASYMPTOTIC_ARGUMENT)
        return sqrt(2 / (PI * x)) * cos(x - 0.5 * PI * float(order) - 0.25 * PI);
    float largest = max(float(order), x);
    uint start = 2 * ((uint(largest + sqrt(BESSEL_MILLER_ACCURACY * largest)) + 2) / 2);
    float twoOverArgument = 2 / x;
    float higher = 0;
    float current = 1;
    float evenSum = 0;
    float result = 0;
    [loop] for (uint n = start; n > 0; --n)
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

/// A sinusoidal phase grating (section 4.1.3) on a surface frame.
struct Grating
{
    float2 AxisSurface; ///< Unit direction of periodicity in (tangent, bitangent) coordinates.
    float PeriodWorld;  ///< Pitch Lambda.
    float HeightWorld;  ///< Grating height b.
    uint MaximumOrder;  ///< N: orders |n| <= N are rendered.
};

/// Eq. (33)'s phase amplitude k b / (2 cos theta_i cos theta_o), the argument of the Bessel weights of Eq. (34).
float ComputeGratingPhaseAmplitude(Grating grating,
                                   float wavenumberPerWorld,
                                   float incidentCosine,
                                   float outgoingCosine)
{
    float cosineProduct = max(incidentCosine, OPTICS_MINIMUM_COSINE) * max(outgoingCosine, OPTICS_MINIMUM_COSINE);
    return wavenumberPerWorld * grating.HeightWorld / (2 * cosineProduct);
}

/// One order of Eq. (34) convolved with the coherence Gaussian: J_n(upsilon)^2 N(h~ - n (lambda / Lambda) g;
/// Sigma~^-1).
float EvaluateGratingOrder(Grating grating,
                           float2 halfVector,
                           SurfaceCoherence coherence,
                           float wavelengthOverPeriod,
                           float phaseAmplitude,
                           int order)
{
    float bessel = EvaluateBesselJ(uint(abs(order)), phaseAmplitude);
    float2 offset = halfVector - grating.AxisSurface * (float(order) * wavelengthOverPeriod);
    return bessel * bessel * EvaluateBivariateNormal(RotateIntoCoherenceBasis(coherence, offset), coherence.Inverse);
}

/// Eq. (34) convolved with the coherence Gaussian (Eq. (15)): the sum of EvaluateGratingOrder over the orders
/// firstOrder to lastOrder, -N to N for the whole PSD. Its weights sum to one over all orders, so with the
/// normalization of D the whole PSD conserves energy up to the truncated and evanescent orders.
float EvaluateGratingDiffraction(Grating grating,
                                 float2 halfVector,
                                 SurfaceCoherence coherence,
                                 float wavelengthOverPeriod,
                                 float phaseAmplitude,
                                 int firstOrder,
                                 int lastOrder)
{
    float result = 0;
    [loop] for (int order = firstOrder; order <= lastOrder; ++order)
    {
        result += EvaluateGratingOrder(grating, halfVector, coherence, wavelengthOverPeriod, phaseAmplitude, order);
    }
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// The scattering interaction: Eq. (16) and Eq. (20).

/// The s/p frames of one scattering event: the incident beam travels along -incidentWorld, the scattered beam
/// along outgoingWorld. x = p lies in the plane of scattering, y = s is perpendicular to it.
struct ScatteringFrames
{
    float3 PerpendicularWorld;    ///< s, shared by both frames.
    float3 IncidentParallelWorld; ///< p of the incident beam.
    float3 OutgoingParallelWorld; ///< p of the scattered beam.
};

ScatteringFrames MakeScatteringFrames(Beam incident, float3 outgoingWorld)
{
    ScatteringFrames result;
    float3 perpendicularWorld = cross(incident.PropagationWorld, outgoingWorld);
    // Forward or backward scattering has no plane of scattering; keep the incident y axis as s.
    if (dot(perpendicularWorld, perpendicularWorld) < OPTICS_MINIMUM_AXIS_LENGTH_SQUARED)
        perpendicularWorld = GetTransverseYWorld(incident);
    result.PerpendicularWorld = normalize(perpendicularWorld);
    result.IncidentParallelWorld = cross(result.PerpendicularWorld, incident.PropagationWorld);
    result.OutgoingParallelWorld = cross(result.PerpendicularWorld, outgoingWorld);
    return result;
}

/// Angle that turns the beam's x axis onto the incident p axis, from x towards y.
float ComputeFrameRotation(Beam incident, ScatteringFrames frames)
{
    return atan2(dot(frames.IncidentParallelWorld, GetTransverseYWorld(incident)),
                 dot(frames.IncidentParallelWorld, incident.TransverseXWorld));
}

/// The diffraction operator's values for the x, y and 1/2 components of one beam (Eq. (16)).
struct ComponentDiffraction
{
    float X;
    float Y;
    float Half;
};

/// Eq. (16)'s bracket: each transverse component of the incident Stokes vector scaled by its own D. The result is a
/// Stokes vector in the incident beam's frame, before the polarimetric BSDF acts on it.
float4 ApplyComponentDiffraction(float4 stokes, ComponentDiffraction diffraction)
{
    float irradianceX = 0.5 * (stokes.x + stokes.y);
    float irradianceY = 0.5 * (stokes.x - stokes.y);
    return float4(irradianceX * diffraction.X + irradianceY * diffraction.Y,
                  irradianceX * diffraction.X - irradianceY * diffraction.Y,
                  stokes.z * diffraction.Half,
                  stokes.w * diffraction.Half);
}

/// Eq. (20) without U: the incident shapes mixed by the frame rotation and expressed in the s/p axes. Returns the
/// x (p) shape in outShapeX and the y (s) shape in outShapeY.
void RotateShapesIntoScatteringFrame(Beam incident, float rotation, out float3 outShapeX, out float3 outShapeY)
{
    float cosineSquared = cos(rotation) * cos(rotation);
    float sineSquared = 1 - cosineSquared;
    outShapeX = RotateSymmetric(cosineSquared * incident.ShapeX + sineSquared * incident.ShapeY, rotation);
    outShapeY = RotateSymmetric(sineSquared * incident.ShapeX + cosineSquared * incident.ShapeY, rotation);
}

/// (|f_pp|^2, |f_ss|^2) for light from incidentWorld scattered towards outgoingWorld, at their half vector.
float2 EvaluateFresnelIntensities(float3 incidentWorld, float3 outgoingWorld, Complex eta)
{
    float3 halfWorld = normalize(incidentWorld + outgoingWorld);
    FresnelCoefficients fresnel = ComputeFresnelCoefficients(dot(incidentWorld, halfWorld), eta);
    return float2(ComputeSquaredMagnitude(fresnel.Parallel), ComputeSquaredMagnitude(fresnel.Perpendicular));
}

/// U of Eq. (18) from the Hessian U~ (xx, xy, yy) of one component's intensity centerIntensity: U~^-1 scaled to
/// unit determinant, as rows (u00, u01, u10, u11), with the resolution and anisotropy limits above. U m U^T is the same
/// for U and -U, so a negative-definite U~ gives the same deformation as its negation.
float4 ComputeShapeDeformation(float3 hessian, float centerIntensity)
{
    float mean = 0.5 * (hessian.x + hessian.z);
    float radius = sqrt(0.25 * (hessian.x - hessian.z) * (hessian.x - hessian.z) + hessian.y * hessian.y);
    float first = mean + radius;
    float second = mean - radius;
    float largest = max(abs(first), abs(second));
    // Eigenvalues below the resolvable curvature are rounding noise: they are raised to it, or to the anisotropy cap.
    float resolvable = OPTICS_HESSIAN_RESOLUTION * abs(centerIntensity) / (OPTICS_HESSIAN_STEP * OPTICS_HESSIAN_STEP);
    if (largest <= resolvable)
        return float4(1, 0, 0, 1);
    float smallest = max(largest / OPTICS_MAXIMUM_DEFORMATION_ANISOTROPY, resolvable);
    // Opposite signs that both exceed the anisotropy limit are a genuinely indefinite U~.
    if (first * second < 0 && min(abs(first), abs(second)) > smallest)
        return float4(1, 0, 0, 1);
    float firstMagnitude = max(abs(first), smallest);
    float secondMagnitude = max(abs(second), smallest);
    // U~^-1 has eigenvalues 1 / |lambda|; at unit determinant they become sqrt(|lambda_other| / |lambda|).
    float firstScale = sqrt(secondMagnitude / firstMagnitude);
    float secondScale = sqrt(firstMagnitude / secondMagnitude);
    float angle = 0.5 * atan2(2 * hessian.y, hessian.x - hessian.z);
    float cosine = cos(angle);
    float sine = sin(angle);
    float offDiagonal = (firstScale - secondScale) * cosine * sine;
    return float4(firstScale * cosine * cosine + secondScale * sine * sine,
                  offDiagonal,
                  offDiagonal,
                  firstScale * sine * sine + secondScale * cosine * cosine);
}

/// Samples of the Hessians' finite differences: the centre, +/- the first axis, +/- the second axis and both.
static const uint OPTICS_HESSIAN_SAMPLE_COUNT = 6;

/// Offset of one Hessian sample, in steps along the (p, s) axes.
float2 GetHessianSampleOffset(uint sampleIndex)
{
    if (sampleIndex == 1)
        return float2(1, 0);
    if (sampleIndex == 2)
        return float2(-1, 0);
    if (sampleIndex == 3)
        return float2(0, 1);
    if (sampleIndex == 4)
        return float2(0, -1);
    if (sampleIndex == 5)
        return float2(1, 1);
    return float2(0, 0);
}

/// One sample's weights in the second differences (pp, ps, ss), before division by the squared step:
/// f(+p) - 2 f(0) + f(-p), f(+p+s) - f(+p) - f(+s) + f(0) and f(+s) - 2 f(0) + f(-s).
float3 GetHessianSampleWeights(uint sampleIndex)
{
    if (sampleIndex == 1)
        return float3(1, -1, 0);
    if (sampleIndex == 2)
        return float3(1, 0, 0);
    if (sampleIndex == 3)
        return float3(0, -1, 1);
    if (sampleIndex == 4)
        return float3(0, 0, 1);
    if (sampleIndex == 5)
        return float3(0, 1, 0);
    return float3(-2, 1, -2);
}

/// U_x and U_y of Eq. (30) by finite differences in the scattered beam's transverse axes (p, s): five Fresnel
/// evaluations besides the centre around outgoingWorld, with incidentWorld fixed. One loop, so the kernel inlines one
/// Fresnel evaluation.
[noinline] void ComputeFresnelShapeDeformations(float3 incidentWorld,
                                                float3 outgoingWorld,
                                                ScatteringFrames frames,
                                                Complex eta,
                                                out float4 outDeformationX,
                                                out float4 outDeformationY) {
    float3 firstAxisWorld = frames.OutgoingParallelWorld * OPTICS_HESSIAN_STEP;
    float3 secondAxisWorld = frames.PerpendicularWorld * OPTICS_HESSIAN_STEP;
    float2 center = float2(0);
    float3 parallelHessian = float3(0);
    float3 perpendicularHessian = float3(0);
    [loop] for (uint sampleIndex = 0; sampleIndex < OPTICS_HESSIAN_SAMPLE_COUNT; ++sampleIndex)
    {
        float2 offset = GetHessianSampleOffset(sampleIndex);
        float3 sampleWorld = normalize(outgoingWorld + offset.x * firstAxisWorld + offset.y * secondAxisWorld);
        float2 intensities = EvaluateFresnelIntensities(incidentWorld, sampleWorld, eta);
        if (sampleIndex == 0)
            center = intensities;
        float3 weights = GetHessianSampleWeights(sampleIndex);
        parallelHessian += weights * intensities.x;
        perpendicularHessian += weights * intensities.y;
    }
    float stepSquared = OPTICS_HESSIAN_STEP * OPTICS_HESSIAN_STEP;
    // (pp, ps, ss) is already (xx, xy, yy) in the (p, s) = (x, y) axes.
    outDeformationX = ComputeShapeDeformation(parallelHessian / stepSquared, center.x);
    outDeformationY = ComputeShapeDeformation(perpendicularHessian / stepSquared, center.y);
}

/// What a scattering event does to one beam at one wavelength.
struct Interaction
{
    ComponentDiffraction Diffraction; ///< D of each transverse component; one where the lobe has no D.
    bool HasFresnel;                  ///< Apply Eq. (29) with Eta; otherwise the depolarizer-free Eq. (32).
    Complex Eta;                      ///< Relative index of the interface.
    float Weight;                     ///< Scalar factor: the lobe's partition and albedo.
    bool HasShapeDeformation;         ///< Apply Eq. (30)'s U (rough surfaces); the identity otherwise.
};

/// Scatter a beam that arrives along -incidentWorld towards outgoingWorld: Eq. (16) (or Eq. (21), (32)) for the
/// Stokes vector, Eq. (20) for the shapes. The returned beam's Stokes vector is the scattered radiance per unit
/// incident radiance and solid angle times the incident Stokes vector, in the s/p frame (x = p) of the scattering.
[noinline] Beam ScatterBeam(Beam incident, float3 outgoingWorld, Interaction interaction) {
    float3 incidentWorld = -incident.PropagationWorld;
    ScatteringFrames frames = MakeScatteringFrames(incident, outgoingWorld);
    float rotation = ComputeFrameRotation(incident, frames);
    float4 diffracted =
        RotateStokesFrame(ApplyComponentDiffraction(incident.Stokes, interaction.Diffraction), rotation);
    Beam result;
    result.PropagationWorld = outgoingWorld;
    result.TransverseXWorld = frames.OutgoingParallelWorld;
    result.DistanceWorld = incident.DistanceWorld;
    float3 halfWorld = normalize(incidentWorld + outgoingWorld);
    if (interaction.HasFresnel == true)
        result.Stokes =
            ApplyFresnelMueller(ComputeFresnelCoefficients(dot(incidentWorld, halfWorld), interaction.Eta), diffracted);
    else
        result.Stokes = diffracted;
    result.Stokes = EnforceRealizableStokes(result.Stokes * interaction.Weight);
    float3 shapeX;
    float3 shapeY;
    RotateShapesIntoScatteringFrame(incident, rotation, shapeX, shapeY);
    if (interaction.HasShapeDeformation == true)
    {
        float4 deformationX;
        float4 deformationY;
        ComputeFresnelShapeDeformations(incidentWorld,
                                        outgoingWorld,
                                        frames,
                                        interaction.Eta,
                                        deformationX,
                                        deformationY);
        shapeX = TransformSymmetric(shapeX, deformationX);
        shapeY = TransformSymmetric(shapeY, deformationY);
    }
    result.ShapeX = shapeX;
    result.ShapeY = shapeY;
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Partition and importance sampling: Eqs. (22), (23), (24) and (31).

/// alpha_direct of Eq. (22) with q_rel = q (section 4.1.1).
float ComputeDirectFraction(float wavenumberPerWorld, float incidentCosine, float rmsHeightWorld)
{
    float phase = wavenumberPerWorld * incidentCosine * rmsHeightWorld;
    return exp(-phase * phase);
}

/// k^2 sigma_min: the variance of the least coherent light rendered (section 4), radiator light after
/// floorDistanceWorld at the shortest wavelength, rescaled to this wavelength.
float ComputeEnvelopeCoherence(float wavelengthNanometers, float floorDistanceWorld, float radiatorShape)
{
    float distance = floorDistanceWorld * SPECTRUM_WAVELENGTH_MINIMUM_NANOMETERS / wavelengthNanometers;
    return distance * distance * radiatorShape;
}

/// Draw the dimensionless half vector h~ from the isotropic envelope N(0, variance I) of Eq. (31).
float2 SampleEnvelopeHalfVector(float variance, float2 randomValues)
{
    float2 clamped = clamp(randomValues, OPTICS_MINIMUM_NORMAL_PROBABILITY, 1 - OPTICS_MINIMUM_NORMAL_PROBABILITY);
    return sqrt(variance) * float2(ComputeStandardNormalQuantile(clamped.x), ComputeStandardNormalQuantile(clamped.y));
}

/// The direction on the frame's upper hemisphere whose half vector with knownWorld is halfVector:
/// projected = halfVector - known projected. Returns false when the projection leaves the unit disk.
bool TryMakeDirectionFromHalfVector(SurfaceFrame frame,
                                    float3 knownWorld,
                                    float2 halfVector,
                                    out float3 outDirectionWorld)
{
    float2 projected = halfVector - float2(dot(knownWorld, frame.TangentWorld), dot(knownWorld, frame.BitangentWorld));
    float projectedSquared = dot(projected, projected);
    outDirectionWorld = float3(0);
    if (projectedSquared >= 1)
        return false;
    outDirectionWorld = projected.x * frame.TangentWorld + projected.y * frame.BitangentWorld +
                        sqrt(1 - projectedSquared) * frame.NormalWorld;
    return true;
}

/// Solid-angle density of a direction drawn through a half-vector density: the Jacobian d^2 h~ = cos d omega.
float ConvertHalfVectorDensity(float halfVectorDensity, SurfaceFrame frame, float3 directionWorld)
{
    return halfVectorDensity * max(dot(directionWorld, frame.NormalWorld), 0.0);
}
