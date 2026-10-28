// Offline LTC fitter for Ludus's specular BSDF (see ltc_fit.hpp for the
// references). Everything is evaluated in float64.
//
// The fitted BRDF is the specular lobe of EvaluateReferenceBsdf
// (modules/graphics/renderer/src/reference_bsdf.cpp) and of the GPU preview's
// Evaluate (shaders/renderer_gpu_preview.slang) with a Fresnel factor of one:
// GGX with alpha = roughness^2, the height-correlated Smith visibility
// 0.5 / (nl * lambdaView + nv * lambdaLight), and Schlick's Fresnel handled
// through FresnelNorm. The fitter repeats that formula rather than linking the
// renderer, as cookers must not link their consumers.
// Thanks to Eric Heitz, "Sampling the GGX Distribution of Visible Normals",
// JCGT 7(4), 2018, sections 3-4, https://jcgt.org/published/0007/04/01/paper.pdf,
// for the visible-normal sampling of the BRDF moments and objective samples;
// it is the construction the renderer's reference BSDF uses.

#include "ltc_fit.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::tools::ltc_fit
{
namespace
{
namespace math = foundation::math;
using foundation::Array;
using foundation::StaticArray;
using foundation::core::usize;
using math::Vector3d;

constexpr uint32 PARAMETER_COUNT = 4;
constexpr uint32 SIMPLEX_SIZE = PARAMETER_COUNT + 1;
constexpr uint32 MATRIX_COLUMN_COUNT = 3;
// Standard Nelder-Mead coefficients (Lagarias, Reeds, Wright and Wright 1998).
constexpr float64 REFLECTION = 1.0;
constexpr float64 EXPANSION = 2.0;
constexpr float64 CONTRACTION = 0.5;
constexpr float64 SHRINK = 0.5;
// Initial simplex edge in the optimizer's coordinates (log A, B, log C, D), relative to the lobe's width.
constexpr float64 INITIAL_STEP = 0.05;
// Widths, in multiples of alpha, of the initial guesses aligned with the lobe's average direction.
constexpr StaticArray<float64, 4> ALIGNED_WIDTH_SCALES{0.5, 1.0, 2.0, 4.0};
// A start ends when the simplex values agree to this relative tolerance.
constexpr float64 RELATIVE_TOLERANCE = 1.0e-10;
// Stand-in objective for matrices without a positive determinant.
constexpr float64 INVALID_OBJECTIVE = 1.0e300;
// Schlick's Fresnel uses (1 - v.h)^5.
constexpr uint32 SCHLICK_POWER = 5;
// Independent jitter streams of the PCG generator for the fitting and residual sample sets.
constexpr uint64 FITTING_STREAM = 1;
constexpr uint64 RESIDUAL_STREAM = 2;
constexpr uint64 MOMENT_STREAM = 3;

using Parameters = StaticArray<float64, PARAMETER_COUNT>;
using Matrix = StaticArray<Vector3d, MATRIX_COLUMN_COUNT>;

[[nodiscard]] float64 GetLength(Vector3d value) noexcept
{
    return math::Sqrt(math::Dot(value, value));
}
[[nodiscard]] Vector3d Normalize(Vector3d value) noexcept
{
    return value * (1.0 / GetLength(value));
}
[[nodiscard]] float64 Clamp01(float64 value) noexcept
{
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}
[[nodiscard]] Vector3d Apply(const Matrix& matrix, Vector3d value) noexcept
{
    return matrix[0] * value.X + matrix[1] * value.Y + matrix[2] * value.Z;
}

/// The specular lobe for one view direction in the fitting frame.
struct Lobe final
{
    float64 Alpha = 1.0;
    Vector3d View{0.0, 0.0, 1.0};
    /// sqrt(nv^2 (1 - alpha^2) + alpha^2), the renderer's lambdaView.
    float64 ViewLambda = 1.0;
};

[[nodiscard]] Lobe MakeLobe(float64 roughness, float64 viewCosine) noexcept
{
    Lobe result;
    result.Alpha = roughness * roughness;
    result.View = Vector3d{math::Sqrt(1.0 - viewCosine * viewCosine), 0.0, viewCosine};
    const float64 alphaSquared = result.Alpha * result.Alpha;
    result.ViewLambda = math::Sqrt(viewCosine * viewCosine * (1.0 - alphaSquared) + alphaSquared);
    return result;
}
/// GGX normal distribution in the renderer's cancellation-free form.
[[nodiscard]] float64 EvaluateDistribution(float64 alpha, float64 normalHalfCosine) noexcept
{
    const float64 alphaSquared = alpha * alpha;
    const float64 cosineSquared = normalHalfCosine * normalHalfCosine;
    const float64 denominator = (1.0 - cosineSquared) + cosineSquared * alphaSquared;
    return alphaSquared / (math::kPi * denominator * denominator);
}
/// rho(v, l) * cos(theta_l) with a Fresnel factor of one, and Schlick's (1 - v.h)^5 weight.
[[nodiscard]] float64 EvaluateCosineWeightedBrdf(const Lobe& lobe, Vector3d light, float64& outFresnelWeight) noexcept
{
    outFresnelWeight = 0.0;
    const float64 normalLightCosine = light.Z;
    const float64 normalViewCosine = lobe.View.Z;
    if (normalLightCosine <= 0.0 || normalViewCosine <= 0.0)
    {
        return 0.0;
    }
    const Vector3d half = Normalize(lobe.View + light);
    const float64 normalHalfCosine = Clamp01(half.Z);
    const float64 viewHalfCosine = Clamp01(math::Dot(lobe.View, half));
    const float64 alphaSquared = lobe.Alpha * lobe.Alpha;
    const float64 lightLambda = math::Sqrt(normalLightCosine * normalLightCosine * (1.0 - alphaSquared) + alphaSquared);
    const float64 visibility = 0.5 / (normalLightCosine * lobe.ViewLambda + normalViewCosine * lightLambda);
    float64 fresnelWeight = 1.0;
    for (uint32 i = 0; i < SCHLICK_POWER; ++i)
    {
        fresnelWeight *= 1.0 - viewHalfCosine;
    }
    outFresnelWeight = fresnelWeight;
    return EvaluateDistribution(lobe.Alpha, normalHalfCosine) * visibility * normalLightCosine;
}
/// Solid-angle density of SampleVisibleNormalReflection, including directions below the horizon.
[[nodiscard]] float64 EvaluateSamplingDensity(const Lobe& lobe, Vector3d light) noexcept
{
    const Vector3d sum = lobe.View + light;
    const float64 length = GetLength(sum);
    if (length <= 0.0)
    {
        return 0.0;
    }
    const Vector3d half = sum * (1.0 / length);
    if (half.Z <= 0.0 || math::Dot(lobe.View, half) <= 0.0)
    {
        return 0.0;
    }
    // D_v(h) / (4 v.h) = G1(v) D(h) / (4 nv), with G1(v) = 2 nv / (nv + lambdaView).
    return EvaluateDistribution(lobe.Alpha, half.Z) / (2.0 * (lobe.View.Z + lobe.ViewLambda));
}
/// Heitz 2018: reflect the view about a visible normal sampled from the stretched hemisphere.
[[nodiscard]] Vector3d SampleVisibleNormalReflection(const Lobe& lobe, float64 u, float64 v) noexcept
{
    const float64 alpha = lobe.Alpha;
    const Vector3d stretched = Normalize(Vector3d{alpha * lobe.View.X, alpha * lobe.View.Y, lobe.View.Z});
    const float64 tangentLengthSquared = stretched.X * stretched.X + stretched.Y * stretched.Y;
    const Vector3d tangent = tangentLengthSquared > 0.0
                                 ? Vector3d{-stretched.Y, stretched.X, 0.0} * (1.0 / math::Sqrt(tangentLengthSquared))
                                 : Vector3d{1.0, 0.0, 0.0};
    const Vector3d bitangent = math::Cross(stretched, tangent);
    const float64 radius = math::Sqrt(u);
    const float64 angle = math::kTau * v;
    const float64 diskX = radius * math::Cos(angle);
    const float64 diskY = radius * math::Sin(angle);
    const float64 blend = 0.5 * (1.0 + stretched.Z);
    const float64 warpedY = (1.0 - blend) * math::Sqrt(1.0 - diskX * diskX) + blend * diskY;
    const float64 heightSquared = 1.0 - diskX * diskX - warpedY * warpedY;
    const Vector3d hemisphere =
        tangent * diskX + bitangent * warpedY + stretched * math::Sqrt(heightSquared > 0.0 ? heightSquared : 0.0);
    const Vector3d half =
        Normalize(Vector3d{alpha * hemisphere.X, alpha * hemisphere.Y, hemisphere.Z > 0.0 ? hemisphere.Z : 0.0});
    return half * (2.0 * math::Dot(lobe.View, half)) - lobe.View;
}
[[nodiscard]] Vector3d SampleCosine(float64 u, float64 v) noexcept
{
    const float64 radius = math::Sqrt(u);
    const float64 angle = math::kTau * v;
    return Vector3d{radius * math::Cos(angle), radius * math::Sin(angle), math::Sqrt(1.0 - u)};
}

/// One stratified, jittered point per stratum of a strata x strata grid.
bool TryMakeStratifiedPoints(uint32 strata, uint64 seed, uint64 stream, Array<float64>& outPoints) noexcept
{
    math::RandomStream random;
    if (random.TryReseed(seed, stream) == false || outPoints.TryResize(usize{strata} * strata * 2) == false)
    {
        return false;
    }
    for (uint32 row = 0; row < strata; ++row)
    {
        for (uint32 column = 0; column < strata; ++column)
        {
            const usize index = (usize{row} * strata + column) * 2;
            outPoints[index] = (column + static_cast<float64>(random.NextFloat01())) / strata;
            outPoints[index + 1] = (row + static_cast<float64>(random.NextFloat01())) / strata;
        }
    }
    return true;
}

/// The LTC of an Equation 6 matrix, evaluated through its scaled inverse.
struct Distribution final
{
    Matrix Transform{};
    Matrix InverseTransform{};
    float64 InverseDeterminant = 1.0;
};

[[nodiscard]] bool TryMakeDistribution(const Parameters& parameters, Distribution& outDistribution) noexcept
{
    const float64 a = math::Exp(parameters[0]);
    const float64 b = parameters[1];
    const float64 c = math::Exp(parameters[2]);
    const float64 d = parameters[3];
    const float64 skewDeterminant = a - b * d;
    if ((skewDeterminant > 0.0) == false)
    {
        return false;
    }
    outDistribution.Transform[0] = Vector3d{a, 0.0, d};
    outDistribution.Transform[1] = Vector3d{0.0, c, 0.0};
    outDistribution.Transform[2] = Vector3d{b, 0.0, 1.0};
    // (a - b d) M^-1 = [[1, 0, -b], [0, (a - b d) / c, 0], [-d, 0, a]].
    outDistribution.InverseTransform[0] = Vector3d{1.0, 0.0, -d};
    outDistribution.InverseTransform[1] = Vector3d{0.0, skewDeterminant / c, 0.0};
    outDistribution.InverseTransform[2] = Vector3d{-b, 0.0, a};
    outDistribution.InverseDeterminant = skewDeterminant * skewDeterminant / c;
    return true;
}
/// Heitz et al. 2016, Equation 1 with the clamped cosine of Equation 4.
[[nodiscard]] float64 EvaluateDistributionDensity(const Distribution& distribution, Vector3d direction) noexcept
{
    const Vector3d cosineDirection = Apply(distribution.InverseTransform, direction);
    if (cosineDirection.Z <= 0.0)
    {
        return 0.0;
    }
    const float64 length = GetLength(cosineDirection);
    return cosineDirection.Z / length * distribution.InverseDeterminant / (length * length * length) / math::kPi;
}

/// Fixed samples of the target, rho cos / Norm, and of the clamped cosine for one cell.
struct ObjectiveSamples final
{
    Lobe Target{};
    float64 Norm = 1.0;
    Array<Vector3d> BrdfDirections;
    Array<float64> BrdfTargets;
    Array<float64> BrdfDensities;
    Array<Vector3d> CosineDirections;
};

bool TryMakeObjectiveSamples(const Lobe& lobe,
                             float64 norm,
                             uint32 strata,
                             uint64 seed,
                             uint64 stream,
                             ObjectiveSamples& outSamples) noexcept
{
    Array<float64> points;
    if (TryMakeStratifiedPoints(strata, seed, stream, points) == false)
    {
        return false;
    }
    const usize count = points.GetSize() / 2;
    outSamples.Target = lobe;
    outSamples.Norm = norm;
    if (outSamples.BrdfDirections.TryResize(count) == false || outSamples.BrdfTargets.TryResize(count) == false ||
        outSamples.BrdfDensities.TryResize(count) == false || outSamples.CosineDirections.TryResize(count) == false)
    {
        return false;
    }
    for (usize i = 0; i < count; ++i)
    {
        const Vector3d light = SampleVisibleNormalReflection(lobe, points[2 * i], points[2 * i + 1]);
        float64 fresnelWeight = 0.0;
        outSamples.BrdfDirections[i] = light;
        outSamples.BrdfTargets[i] = EvaluateCosineWeightedBrdf(lobe, light, fresnelWeight) / norm;
        outSamples.BrdfDensities[i] = EvaluateSamplingDensity(lobe, light);
        outSamples.CosineDirections[i] = SampleCosine(points[2 * i], points[2 * i + 1]);
    }
    return true;
}

/// The integral of |LTC - target|^power over the sphere, estimated from both sample sets with the balance heuristic.
[[nodiscard]] float64
EstimateError(const ObjectiveSamples& samples, const Parameters& parameters, uint32 power) noexcept
{
    Distribution distribution;
    if (TryMakeDistribution(parameters, distribution) == false)
    {
        return INVALID_OBJECTIVE;
    }
    float64 sum = 0.0;
    const usize count = samples.BrdfDirections.GetSize();
    for (usize i = 0; i < count; ++i)
    {
        // Sampled from the BRDF.
        const float64 density = EvaluateDistributionDensity(distribution, samples.BrdfDirections[i]);
        const float64 denominator = samples.BrdfDensities[i] + density;
        if (denominator > 0.0)
        {
            const float64 difference = math::Abs(density - samples.BrdfTargets[i]);
            sum += (power == 1 ? difference : difference * difference * difference) / denominator;
        }
        // Sampled from the LTC: w = M w_o / |M w_o| (Heitz et al. 2016, Algorithm 1).
        const Vector3d light = Normalize(Apply(distribution.Transform, samples.CosineDirections[i]));
        float64 fresnelWeight = 0.0;
        const float64 target = EvaluateCosineWeightedBrdf(samples.Target, light, fresnelWeight) / samples.Norm;
        const float64 lightDensity = EvaluateDistributionDensity(distribution, light);
        const float64 lightDenominator = EvaluateSamplingDensity(samples.Target, light) + lightDensity;
        if (lightDenominator > 0.0)
        {
            const float64 difference = math::Abs(lightDensity - target);
            sum += (power == 1 ? difference : difference * difference * difference) / lightDenominator;
        }
    }
    return sum / static_cast<float64>(count);
}

[[nodiscard]] Parameters MakeParameters(const FittedCell& cell) noexcept
{
    Parameters result{};
    result[0] = math::Log(cell.A);
    result[1] = cell.B;
    result[2] = math::Log(cell.C);
    result[3] = cell.D;
    return result;
}

/// One Nelder-Mead start from inoutBest; replaces it when a better point is found.
void MinimizeFromStart(const ObjectiveSamples& samples,
                       uint32 maxIterations,
                       Parameters& inoutBest,
                       float64& inoutBestValue,
                       uint32& inoutEvaluationCount) noexcept
{
    StaticArray<Parameters, SIMPLEX_SIZE> points{};
    StaticArray<float64, SIMPLEX_SIZE> values{};
    // Steps relative to the lobe: B tilts the lobe by about a width per A (1 + B^2), and D skews it by about A (1 +
    // |B|).
    const float64 a = math::Exp(inoutBest[0]);
    const float64 b = inoutBest[1];
    Parameters steps{};
    steps[0] = INITIAL_STEP;
    steps[1] = INITIAL_STEP * a * (1.0 + b * b);
    steps[2] = INITIAL_STEP;
    steps[3] = INITIAL_STEP * a * (1.0 + math::Abs(b));
    for (uint32 i = 0; i < SIMPLEX_SIZE; ++i)
    {
        points[i] = inoutBest;
        if (i > 0)
        {
            points[i][i - 1] += steps[i - 1];
        }
        values[i] = i == 0 ? inoutBestValue : EstimateError(samples, points[i], 3);
    }
    inoutEvaluationCount += PARAMETER_COUNT;
    for (uint32 iteration = 0; iteration < maxIterations; ++iteration)
    {
        // Insertion sort: best first.
        for (uint32 i = 1; i < SIMPLEX_SIZE; ++i)
        {
            for (uint32 j = i; j > 0 && values[j] < values[j - 1]; --j)
            {
                const float64 value = values[j];
                values[j] = values[j - 1];
                values[j - 1] = value;
                const Parameters point = points[j];
                points[j] = points[j - 1];
                points[j - 1] = point;
            }
        }
        const uint32 worst = SIMPLEX_SIZE - 1;
        if (values[worst] - values[0] <= RELATIVE_TOLERANCE * math::Abs(values[0]))
        {
            break;
        }
        Parameters centroid{};
        for (uint32 i = 0; i < worst; ++i)
        {
            for (uint32 k = 0; k < PARAMETER_COUNT; ++k)
            {
                centroid[k] += points[i][k] / PARAMETER_COUNT;
            }
        }
        const auto makePoint = [&](float64 coefficient) noexcept {
            Parameters result{};
            for (uint32 k = 0; k < PARAMETER_COUNT; ++k)
            {
                result[k] = centroid[k] + coefficient * (points[worst][k] - centroid[k]);
            }
            return result;
        };
        const Parameters reflected = makePoint(-REFLECTION);
        const float64 reflectedValue = EstimateError(samples, reflected, 3);
        ++inoutEvaluationCount;
        bool shouldShrink = false;
        if (reflectedValue < values[0])
        {
            const Parameters expanded = makePoint(-REFLECTION * EXPANSION);
            const float64 expandedValue = EstimateError(samples, expanded, 3);
            ++inoutEvaluationCount;
            points[worst] = expandedValue < reflectedValue ? expanded : reflected;
            values[worst] = expandedValue < reflectedValue ? expandedValue : reflectedValue;
        }
        else if (reflectedValue < values[worst - 1])
        {
            points[worst] = reflected;
            values[worst] = reflectedValue;
        }
        else
        {
            const bool isOutside = reflectedValue < values[worst];
            const Parameters contracted = makePoint(isOutside == true ? -REFLECTION * CONTRACTION : CONTRACTION);
            const float64 contractedValue = EstimateError(samples, contracted, 3);
            ++inoutEvaluationCount;
            if (contractedValue < (isOutside == true ? reflectedValue : values[worst]))
            {
                points[worst] = contracted;
                values[worst] = contractedValue;
            }
            else
            {
                shouldShrink = true;
            }
        }
        if (shouldShrink == true)
        {
            for (uint32 i = 1; i < SIMPLEX_SIZE; ++i)
            {
                for (uint32 k = 0; k < PARAMETER_COUNT; ++k)
                {
                    points[i][k] = points[0][k] + SHRINK * (points[i][k] - points[0][k]);
                }
                values[i] = EstimateError(samples, points[i], 3);
            }
            inoutEvaluationCount += PARAMETER_COUNT;
        }
    }
    for (uint32 i = 0; i < SIMPLEX_SIZE; ++i)
    {
        if (values[i] < inoutBestValue)
        {
            inoutBestValue = values[i];
            inoutBest = points[i];
        }
    }
}
} // namespace

float64 GetTableRoughness(const FitSettings& settings, uint32 index) noexcept
{
    const float64 step = (1.0 - settings.MinimumRoughness) / static_cast<float64>(settings.RoughnessCount - 1);
    return index + 1 == settings.RoughnessCount ? 1.0 : settings.MinimumRoughness + step * index;
}

float64 GetTableViewCosine(const FitSettings& settings, uint32 index) noexcept
{
    const float64 theta = math::kHalfPi * index / static_cast<float64>(settings.ViewCount - 1);
    const float64 cosine = index == 0 ? 1.0 : math::Cos(theta);
    return cosine > settings.MinimumViewCosine ? cosine : settings.MinimumViewCosine;
}

bool FitCell(const FitSettings& settings,
             float64 roughness,
             float64 viewCosine,
             foundation::BorrowedView<const FittedCell> initialGuesses,
             FittedCell& outCell) noexcept
{
    const Lobe lobe = MakeLobe(roughness, viewCosine);
    // Moments of the exact lobe by visible-normal importance sampling: rho cos / pdf = 2 V nl (nv + lambdaView).
    Array<float64> points;
    if (TryMakeStratifiedPoints(settings.MomentStrata, settings.Seed, MOMENT_STREAM, points) == false)
    {
        return false;
    }
    FittedCell result;
    Vector3d averageDirection{};
    const usize momentCount = points.GetSize() / 2;
    for (usize i = 0; i < momentCount; ++i)
    {
        const Vector3d light = SampleVisibleNormalReflection(lobe, points[2 * i], points[2 * i + 1]);
        float64 fresnelWeight = 0.0;
        const float64 value = EvaluateCosineWeightedBrdf(lobe, light, fresnelWeight);
        const float64 density = EvaluateSamplingDensity(lobe, light);
        if (value > 0.0 && density > 0.0)
        {
            result.Norm += value / density;
            result.FresnelNorm += value / density * fresnelWeight;
            averageDirection = averageDirection + light * (value / density);
        }
    }
    result.Norm /= static_cast<float64>(momentCount);
    result.FresnelNorm /= static_cast<float64>(momentCount);
    if ((result.Norm > 0.0) == false)
    {
        return false;
    }

    ObjectiveSamples fitting;
    if (TryMakeObjectiveSamples(lobe, result.Norm, settings.ObjectiveStrata, settings.Seed, FITTING_STREAM, fitting) ==
        false)
    {
        return false;
    }
    Parameters best = MakeParameters(FittedCell{});
    float64 bestValue = EstimateError(fitting, best, 3);
    result.EvaluationCount = 1;
    for (usize i = 0; i < initialGuesses.GetSize(); ++i)
    {
        const Parameters guess = MakeParameters(initialGuesses[i]);
        const float64 value = EstimateError(fitting, guess, 3);
        ++result.EvaluationCount;
        if (value < bestValue)
        {
            best = guess;
            bestValue = value;
        }
    }
    // Lobes aligned with the target's average direction: M = R diag(s1, s2, 1) scaled to M(2, 2) = 1, where R rotates
    // z towards that direction about y, gives A = s1, B = tan(phi), C = s2 / cos(phi) and D = -s1 tan(phi).
    const float64 tilt = averageDirection.X / averageDirection.Z;
    const float64 secant = math::Sqrt(1.0 + tilt * tilt);
    for (usize i = 0; i < ALIGNED_WIDTH_SCALES.GetSize(); ++i)
    {
        const float64 width = lobe.Alpha * ALIGNED_WIDTH_SCALES[i];
        FittedCell aligned;
        aligned.A = width;
        aligned.B = tilt;
        aligned.C = width * secant;
        aligned.D = -width * tilt;
        const Parameters guess = MakeParameters(aligned);
        const float64 value = EstimateError(fitting, guess, 3);
        ++result.EvaluationCount;
        if (value < bestValue)
        {
            best = guess;
            bestValue = value;
        }
    }
    if ((bestValue < INVALID_OBJECTIVE) == false)
    {
        return false;
    }
    for (uint32 start = 0; start < settings.StartCount; ++start)
    {
        MinimizeFromStart(fitting, settings.MaxIterations, best, bestValue, result.EvaluationCount);
    }
    result.A = math::Exp(best[0]);
    result.B = best[1];
    result.C = math::Exp(best[2]);
    result.D = best[3];
    result.Objective = bestValue;

    ObjectiveSamples residual;
    if (TryMakeObjectiveSamples(lobe, result.Norm, settings.ResidualStrata, settings.Seed, RESIDUAL_STREAM, residual) ==
        false)
    {
        return false;
    }
    result.RelativeL1Error = EstimateError(residual, best, 1);
    outCell = result;
    return true;
}

bool FitTable(const FitSettings& settings, Array<FittedCell>& outCells) noexcept
{
    Array<FittedCell> cells;
    if (settings.RoughnessCount < 2 || settings.ViewCount < 2 ||
        cells.TryResize(usize{settings.RoughnessCount} * settings.ViewCount) == false)
    {
        return false;
    }
    for (uint32 row = settings.RoughnessCount; row-- > 0;)
    {
        for (uint32 column = 0; column < settings.ViewCount; ++column)
        {
            // Start from the rougher neighbor and the less grazing neighbor; the table varies smoothly.
            StaticArray<FittedCell, 2> guesses{};
            usize guessCount = 0;
            if (row + 1 < settings.RoughnessCount)
            {
                guesses[guessCount] = cells[usize{row + 1} * settings.ViewCount + column];
                ++guessCount;
            }
            if (column > 0)
            {
                guesses[guessCount] = cells[usize{row} * settings.ViewCount + column - 1];
                ++guessCount;
            }
            FittedCell cell;
            if (FitCell(settings,
                        GetTableRoughness(settings, row),
                        GetTableViewCosine(settings, column),
                        foundation::BorrowedView<const FittedCell>(guesses.GetData(), guessCount),
                        cell) == false)
            {
                return false;
            }
            cells[usize{row} * settings.ViewCount + column] = cell;
        }
    }
    outCells = foundation::Move(cells);
    return true;
}

StoredCell MakeStoredCell(const FittedCell& cell) noexcept
{
    // (a - b d) M^-1 = [[1, 0, -b], [0, (a - b d) / c, 0], [-d, 0, a]].
    StoredCell result;
    result.M02 = -cell.B;
    result.M11 = (cell.A - cell.B * cell.D) / cell.C;
    result.M20 = -cell.D;
    result.M22 = cell.A;
    result.Norm = cell.Norm;
    result.FresnelNorm = cell.FresnelNorm;
    return result;
}
} // namespace ludus::tools::ltc_fit
