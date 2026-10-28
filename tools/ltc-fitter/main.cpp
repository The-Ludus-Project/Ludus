// Offline fitter for the renderer's GGX linearly transformed cosine (LTC) table.
//
// Thanks to Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
// "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", ACM
// Transactions on Graphics 35(4), Article 41, 2016, section 4
// (https://doi.org/10.1145/2897824.2925895): an LTC with the isotropic matrix of
// Equation (6) is fitted to the cosine-weighted BRDF of each view inclination and
// roughness by minimizing the L3 error, and the table stores the inverse matrix on
// axes theta in [0, pi/2] and sqrt(alpha) in [0, 1]. Thanks to Christoph Peters,
// "BRDF Importance Sampling for Polygonal Lights", ACM Transactions on Graphics
// 40(4), Article 140, 2021, section 4.1 (https://doi.org/10.1145/3450626.3459672),
// for the third axis over Schlick's F0 and the 64 x 64 x 51 resolution. Neither
// paper specifies the optimizer: this tool uses Nelder and Mead, "A Simplex Method
// for Function Minimization", The Computer Journal 7(4), 1965, warm-started from
// neighboring fits, and estimates the error with multiple importance sampling.
// The fit target is the renderer's own reference BSDF (GGX, height-correlated
// Smith, Schlick), not the Frostbite BRDF of Peters' table, so no third-party table
// is used. Fitting is reimplemented from the papers' text.
//
// Usage: ludus_ltc_fitter <output-directory> [--workers <count>] [--slice <F0 index>]
// --slice fits one F0 slice and leaves the others at the identity, for diagnosing the fit; never check in its output.
// Writes ggx_ltc_table.bin and ggx_ltc_table.json; see the renderer's ltc_table.hpp.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/math/linearly_transformed_cosine.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/ltc_table.hpp>
#include <ludus/graphics/renderer/reference.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace
{
using namespace ludus::foundation;
using namespace ludus::foundation::threading;
namespace math = ludus::foundation::math;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;

// Stratified samples per technique and dimension for the L3 error; fixed sets keep the objective deterministic.
constexpr usize ERROR_SAMPLE_GRID = 32;
// Stratified samples per dimension for the albedo integrals.
constexpr usize ALBEDO_SAMPLE_GRID = 256;
// Nelder-Mead budget and convergence: the relative spread of the simplex's errors.
constexpr usize MAX_SIMPLEX_ITERATIONS = 1200;
constexpr float64 SIMPLEX_RELATIVE_TOLERANCE = 1.0e-7;
// The free entries a, c and s of M = R [[a, 0, s], [0, c, 0], [0, 0, 1]]; R follows the lobe's average direction.
constexpr usize PARAMETER_COUNT = 3;
constexpr usize SIMPLEX_VERTEX_COUNT = PARAMETER_COUNT + 1;
// Standard Nelder-Mead coefficients: reflection, expansion, contraction, shrink.
constexpr float64 REFLECTION = 1.0;
constexpr float64 EXPANSION = 2.0;
constexpr float64 CONTRACTION = 0.5;
constexpr float64 SHRINK = 0.5;
// Initial simplex steps relative to each parameter, with a floor relative to the matrix's diagonal scale.
constexpr float64 SIMPLEX_RELATIVE_STEP = 0.05;
constexpr float64 SIMPLEX_STEP_FLOOR = 0.05;
// The renderer's reference BSDF admits perceptual roughness from 0.045; smaller table rows reuse that fit.
constexpr float32 MINIMUM_FIT_ROUGHNESS = 0.045F;
// The BRDF vanishes for a view on the horizon; the last inclination row is fitted just above it.
constexpr float32 MINIMUM_FIT_COSINE = 1.0e-3F;
// Error reported for parameters whose matrix is not orientation preserving or leaves the admitted range.
constexpr float64 INVALID_ERROR = 1.0e30;
// Admitted magnitude of the diagonal entries a and c and of the skew s; the extreme fits at roughness 0.045 need
// entries near alpha = 0.002, and larger values only arise from degenerate runaway fits.
constexpr float64 MINIMUM_DIAGONAL = 1.0e-6;
constexpr float64 MAXIMUM_ENTRY = 1.0e3;
// Nelder-Mead restarts from its best vertex, which escapes a prematurely collapsed simplex.
constexpr usize SIMPLEX_RESTART_COUNT = 1;
constexpr uint32 DEFAULT_WORKER_COUNT = 8;
constexpr uint32 MAX_WORKER_COUNT = 64;
constexpr uint32 DECIMAL_BASE = 10;
constexpr usize DECIMAL_BUFFER_BYTES = 24;
constexpr usize FLOAT32_BYTES = 4;
constexpr uint32 BYTE_BITS = 8;
constexpr uint32 BYTE_MASK = 0xffU;

using Parameters = StaticArray<float64, PARAMETER_COUNT>;

void WriteText(std::string_view text) noexcept
{
    static_cast<void>(std::fwrite(text.data(), 1, text.size(), stdout));
}
void WriteUnsigned(uint64 value) noexcept
{
    StaticArray<char, DECIMAL_BUFFER_BYTES> digits{};
    usize cursor = DECIMAL_BUFFER_BYTES;
    do
    {
        --cursor;
        digits[cursor] = static_cast<char>('0' + value % DECIMAL_BASE);
        value /= DECIMAL_BASE;
    } while (value != 0);
    WriteText(std::string_view(digits.GetData() + cursor, DECIMAL_BUFFER_BYTES - cursor));
}
[[nodiscard]] bool TryParseUnsigned(std::string_view text, uint32& outValue) noexcept
{
    uint64 value = 0;
    if (text.empty() == true || text.size() > DECIMAL_BUFFER_BYTES)
    {
        return false;
    }
    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        value = value * DECIMAL_BASE + static_cast<uint64>(character - '0');
    }
    if (value > MAX_WORKER_COUNT)
    {
        return false;
    }
    outValue = static_cast<uint32>(value);
    return true;
}

[[nodiscard]] float32 GetRoughness(usize index) noexcept
{
    return static_cast<float32>(index) / static_cast<float32>(renderer::LTC_TABLE_ROUGHNESS_COUNT - 1);
}
[[nodiscard]] float32 GetInclination(usize index) noexcept
{
    return static_cast<float32>(index) / static_cast<float32>(renderer::LTC_TABLE_INCLINATION_COUNT - 1) *
           math::kHalfPiF;
}
[[nodiscard]] float32 GetFresnel(usize index) noexcept
{
    return static_cast<float32>(index) / static_cast<float32>(renderer::LTC_TABLE_FRESNEL_COUNT - 1);
}
/// The view direction (sin theta, 0, cos theta) of Heitz et al. section 4, kept just above the horizon.
[[nodiscard]] math::Vector3 MakeView(float32 inclination) noexcept
{
    const float32 cosine = math::Max(math::Cos(inclination), MINIMUM_FIT_COSINE);
    return math::Vector3{math::Sqrt(1.0F - cosine * cosine), 0.0F, cosine};
}
[[nodiscard]] renderer::ReferenceBsdf MakeSpecularBsdf(float32 roughness, float32 fresnel) noexcept
{
    // A pure metal with base color F0 is exactly the specular lobe, with Schlick's F0 equal to the base color.
    return renderer::ReferenceBsdf
    {
        .BaseColor = math::Vector3{fresnel, fresnel, fresnel},
        .Metallic = 1.0F,
        .Roughness = math::Max(roughness, MINIMUM_FIT_ROUGHNESS),
    };
}
/// A stratified pair of random numbers in [0, 1).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): cell indices then the grid size, as in every loop here.
[[nodiscard]] math::Vector3 MakeStratifiedSample(usize i, usize j, usize grid) noexcept
{
    const float32 size = static_cast<float32>(grid);
    return math::Vector3{0.0F,
                         math::Min((static_cast<float32>(i) + 0.5F) / size, 1.0F - 1.0e-7F),
                         math::Min((static_cast<float32>(j) + 0.5F) / size, 1.0F - 1.0e-7F)};
}

constexpr math::Vector3 NORMAL{0.0F, 0.0F, 1.0F};

/// The specular albedo of Schlick's Fresnel splits into F0 * scale + bias (Peters section 4.2 needs it per
/// channel). Both terms are integrated with the BSDF's own visible-normal sampling.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the table's axis order, roughness then inclination.
[[nodiscard]] renderer::LtcAlbedoEntry ComputeAlbedo(float32 roughness, float32 inclination) noexcept
{
    const math::Vector3 view = MakeView(inclination);
    float64 withFresnelOne = 0.0;
    float64 withFresnelZero = 0.0;
    for (usize i = 0; i < ALBEDO_SAMPLE_GRID; ++i)
    {
        for (usize j = 0; j < ALBEDO_SAMPLE_GRID; ++j)
        {
            renderer::ReferenceBsdfSample sample;
            if (renderer::SampleReferenceBsdf(MakeSpecularBsdf(roughness, 1.0F),
                                              NORMAL,
                                              view,
                                              MakeStratifiedSample(i, j, ALBEDO_SAMPLE_GRID),
                                              sample) != rhi::RasterStatus::Ready ||
                sample.Accepted == false)
            {
                continue;
            }
            const float64 weight =
                static_cast<float64>(sample.Direction.Z) / static_cast<float64>(sample.Evaluation.Pdf);
            withFresnelOne += static_cast<float64>(sample.Evaluation.Value.X) * weight;
            renderer::ReferenceBsdfEvaluation zero;
            if (renderer::EvaluateReferenceBsdf(MakeSpecularBsdf(roughness, 0.0F),
                                                NORMAL,
                                                view,
                                                sample.Direction,
                                                zero) == rhi::RasterStatus::Ready)
            {
                withFresnelZero += static_cast<float64>(zero.Value.X) * weight;
            }
        }
    }
    const float64 count = static_cast<float64>(ALBEDO_SAMPLE_GRID * ALBEDO_SAMPLE_GRID);
    return renderer::LtcAlbedoEntry
    {
        .FresnelScale = static_cast<float32>((withFresnelOne - withFresnelZero) / count),
        .FresnelBias = static_cast<float32>(withFresnelZero / count),
    };
}

/// A fixed set of samples of the normalized cosine-weighted BRDF t, with its values and sampling density.
struct TargetSample final
{
    math::Vector3 Direction{};
    float64 Target = 0.0;
    float64 Density = 0.0;
};

/// The L3 error of Heitz et al. section 4 for one (roughness, inclination, F0), estimated with the balance
/// heuristic over samples of the BRDF and of the candidate LTC.
class FitProblem final
{
public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the table's axis order, then the lobe's albedo.
    [[nodiscard]] bool TryInitialize(float32 roughness, float32 inclination, float32 fresnel, float64 norm) noexcept
    {
        mBsdf = MakeSpecularBsdf(roughness, fresnel);
        mView = MakeView(inclination);
        mNorm = norm;
        if ((norm > 0.0) == false || mTargetSamples.TryResize(ERROR_SAMPLE_GRID * ERROR_SAMPLE_GRID) == false)
        {
            return false;
        }
        float64 averageX = 0.0;
        float64 averageZ = 0.0;
        for (usize i = 0; i < ERROR_SAMPLE_GRID; ++i)
        {
            for (usize j = 0; j < ERROR_SAMPLE_GRID; ++j)
            {
                // The problem is reused from fit to fit: clear the slot, so that a rejected sample contributes nothing
                // instead of a stale sample of an earlier lobe.
                TargetSample& target = mTargetSamples[i * ERROR_SAMPLE_GRID + j];
                target = TargetSample{};
                renderer::ReferenceBsdfSample sample;
                if (renderer::SampleReferenceBsdf(mBsdf,
                                                  NORMAL,
                                                  mView,
                                                  MakeStratifiedSample(i, j, ERROR_SAMPLE_GRID),
                                                  sample) == rhi::RasterStatus::Ready &&
                    sample.Accepted == true)
                {
                    target.Direction = sample.Direction;
                    target.Target = static_cast<float64>(sample.Evaluation.Value.X) *
                                    static_cast<float64>(sample.Direction.Z) / mNorm;
                    target.Density = static_cast<float64>(sample.Evaluation.Pdf);
                    averageX += static_cast<float64>(sample.Direction.X) * target.Target / target.Density;
                    averageZ += static_cast<float64>(sample.Direction.Z) * target.Target / target.Density;
                }
            }
        }
        // The median vector of an LTC is M (0, 0, 1)^T (Heitz et al. section 3.1). M = R S rotates z onto the
        // cosine-weighted lobe's average direction (sin phi, 0, cos phi) and leaves S = [[a, 0, s], [0, c, 0],
        // [0, 0, 1]] to the optimizer. det(M) = a c, so no fit can approach a singular matrix, whose collapsed
        // distribution a finite sample set cannot penalize; M keeps the zero pattern of Equation (6).
        const float64 length = std::sqrt(averageX * averageX + averageZ * averageZ);
        mAverageSine = length > 0.0 ? averageX / length : 0.0;
        mAverageCosine = length > 0.0 ? averageZ / length : 1.0;
        return length > 0.0 && mAverageCosine > 0.0;
    }

    /// M = R [[a, 0, s], [0, c, 0], [0, 0, 1]] with R = [[cos phi, 0, sin phi], [0, 1, 0], [-sin phi, 0, cos phi]].
    [[nodiscard]] math::Matrix3 MakeTransform(const Parameters& parameters) const noexcept
    {
        const float64 a = parameters[0];
        const float64 c = parameters[1];
        const float64 skew = parameters[2];
        return math::Matrix3{{
            math::Vector3{static_cast<float32>(a * mAverageCosine), 0.0F, static_cast<float32>(-a * mAverageSine)},
            math::Vector3{0.0F, static_cast<float32>(c), 0.0F},
            math::Vector3{static_cast<float32>(skew * mAverageCosine + mAverageSine),
                          0.0F,
                          static_cast<float32>(mAverageCosine - skew * mAverageSine)},
        }};
    }
    /// M^-1 = S^-1 R^T, scaled so its last diagonal entry, cos phi, becomes one; LTCs are invariant to scaling
    /// (Heitz et al. section 3.1). The result has the zero pattern of Equation (6).
    [[nodiscard]] renderer::LtcTableEntry MakeEntry(const Parameters& parameters) const noexcept
    {
        const float64 a = parameters[0];
        const float64 c = parameters[1];
        const float64 skew = parameters[2];
        const float64 cosine = mAverageCosine;
        const float64 sine = mAverageSine;
        return renderer::LtcTableEntry
        {
            .InverseM00 = static_cast<float32>((cosine - skew * sine) / (a * cosine)),
            .InverseM02 = static_cast<float32>((-sine - skew * cosine) / (a * cosine)),
            .InverseM11 = static_cast<float32>(1.0 / (c * cosine)),
            .InverseM20 = static_cast<float32>(sine / cosine),
        };
    }

    [[nodiscard]] float64 ComputeError(const Parameters& parameters) const noexcept
    {
        const float64 a = parameters[0];
        const float64 c = parameters[1];
        const float64 skew = parameters[2];
        if ((a >= MINIMUM_DIAGONAL && a <= MAXIMUM_ENTRY) == false ||
            (c >= MINIMUM_DIAGONAL && c <= MAXIMUM_ENTRY) == false || (std::abs(skew) <= MAXIMUM_ENTRY) == false)
        {
            return INVALID_ERROR;
        }
        const math::Matrix3 transform = MakeTransform(parameters);
        const renderer::LtcTableEntry entry = MakeEntry(parameters);
        const math::Matrix3 inverse{{
            math::Vector3{entry.InverseM00, 0.0F, entry.InverseM20},
            math::Vector3{0.0F, entry.InverseM11, 0.0F},
            math::Vector3{entry.InverseM02, 0.0F, 1.0F},
        }};
        float64 error = 0.0;
        for (const TargetSample& target : mTargetSamples)
        {
            if ((target.Density > 0.0) == false)
            {
                continue;
            }
            const float64 ltc = static_cast<float64>(math::EvaluateLtcDensity(inverse, target.Direction));
            const float64 difference = std::abs(ltc - target.Target);
            error += difference * difference * difference / (0.5 * target.Density + 0.5 * ltc);
        }
        for (usize i = 0; i < ERROR_SAMPLE_GRID; ++i)
        {
            for (usize j = 0; j < ERROR_SAMPLE_GRID; ++j)
            {
                // Sample the clamped cosine, then transform it by M (Heitz et al. Algorithm 1).
                const math::Vector3 random = MakeStratifiedSample(i, j, ERROR_SAMPLE_GRID);
                const float32 radius = math::Sqrt(random.Y);
                const float32 angle = math::kTauF * random.Z;
                const math::Vector3 cosine{radius * math::Cos(angle),
                                           radius * math::Sin(angle),
                                           math::Sqrt(math::Max(0.0F, 1.0F - random.Y))};
                math::Vector3 direction;
                if (math::TryNormalize(transform * cosine, direction) != math::MathStatus::Success)
                {
                    continue;
                }
                const float64 ltc = static_cast<float64>(math::EvaluateLtcDensity(inverse, direction));
                float64 target = 0.0;
                float64 density = 0.0;
                renderer::ReferenceBsdfEvaluation evaluation;
                if (direction.Z > 0.0F &&
                    renderer::EvaluateReferenceBsdf(mBsdf, NORMAL, mView, direction, evaluation) ==
                        rhi::RasterStatus::Ready)
                {
                    target = static_cast<float64>(evaluation.Value.X) * static_cast<float64>(direction.Z) / mNorm;
                    density = static_cast<float64>(evaluation.Pdf);
                }
                const float64 difference = std::abs(ltc - target);
                if ((ltc + density > 0.0) == true)
                {
                    error += difference * difference * difference / (0.5 * density + 0.5 * ltc);
                }
            }
        }
        return error / static_cast<float64>(2 * ERROR_SAMPLE_GRID * ERROR_SAMPLE_GRID);
    }

private:
    renderer::ReferenceBsdf mBsdf{};
    math::Vector3 mView{};
    float64 mNorm = 0.0;
    float64 mAverageSine = 0.0;
    float64 mAverageCosine = 1.0;
    Array<TargetSample> mTargetSamples;
};

/// Nelder-Mead minimization of the problem's error from an initial guess.
[[nodiscard]] Parameters Minimize(const FitProblem& problem, const Parameters& initial) noexcept
{
    StaticArray<Parameters, SIMPLEX_VERTEX_COUNT> simplex{};
    StaticArray<float64, SIMPLEX_VERTEX_COUNT> errors{};
    const float64 scale = SIMPLEX_STEP_FLOOR * (std::abs(initial[0]) + std::abs(initial[1]));
    for (usize vertex = 0; vertex < SIMPLEX_VERTEX_COUNT; ++vertex)
    {
        simplex[vertex] = initial;
        if (vertex > 0)
        {
            const usize parameter = vertex - 1;
            simplex[vertex][parameter] += math::Max(SIMPLEX_RELATIVE_STEP * std::abs(initial[parameter]), scale);
        }
        errors[vertex] = problem.ComputeError(simplex[vertex]);
    }
    for (usize iteration = 0; iteration < MAX_SIMPLEX_ITERATIONS; ++iteration)
    {
        // Order the vertices by error.
        for (usize i = 1; i < SIMPLEX_VERTEX_COUNT; ++i)
        {
            for (usize j = i; j > 0 && errors[j] < errors[j - 1]; --j)
            {
                const Parameters vertex = simplex[j];
                simplex[j] = simplex[j - 1];
                simplex[j - 1] = vertex;
                const float64 error = errors[j];
                errors[j] = errors[j - 1];
                errors[j - 1] = error;
            }
        }
        const usize worst = SIMPLEX_VERTEX_COUNT - 1;
        if (errors[worst] - errors[0] <= SIMPLEX_RELATIVE_TOLERANCE * errors[0])
        {
            break;
        }
        Parameters centroid{};
        for (usize vertex = 0; vertex < worst; ++vertex)
        {
            for (usize p = 0; p < PARAMETER_COUNT; ++p)
            {
                centroid[p] += simplex[vertex][p] / static_cast<float64>(worst);
            }
        }
        const auto moveFrom = [&](float64 coefficient) noexcept {
            Parameters point{};
            for (usize p = 0; p < PARAMETER_COUNT; ++p)
            {
                point[p] = centroid[p] + coefficient * (simplex[worst][p] - centroid[p]);
            }
            return point;
        };
        const Parameters reflected = moveFrom(-REFLECTION);
        const float64 reflectedError = problem.ComputeError(reflected);
        if (reflectedError < errors[0])
        {
            const Parameters expanded = moveFrom(-EXPANSION);
            const float64 expandedError = problem.ComputeError(expanded);
            simplex[worst] = expandedError < reflectedError ? expanded : reflected;
            errors[worst] = math::Min(expandedError, reflectedError);
        }
        else if (reflectedError < errors[worst - 1])
        {
            simplex[worst] = reflected;
            errors[worst] = reflectedError;
        }
        else
        {
            const Parameters contracted = moveFrom(CONTRACTION);
            const float64 contractedError = problem.ComputeError(contracted);
            if (contractedError < errors[worst])
            {
                simplex[worst] = contracted;
                errors[worst] = contractedError;
            }
            else
            {
                for (usize vertex = 1; vertex < SIMPLEX_VERTEX_COUNT; ++vertex)
                {
                    for (usize p = 0; p < PARAMETER_COUNT; ++p)
                    {
                        simplex[vertex][p] = simplex[0][p] + SHRINK * (simplex[vertex][p] - simplex[0][p]);
                    }
                    errors[vertex] = problem.ComputeError(simplex[vertex]);
                }
            }
        }
    }
    usize best = 0;
    for (usize vertex = 1; vertex < SIMPLEX_VERTEX_COUNT; ++vertex)
    {
        best = errors[vertex] < errors[best] ? vertex : best;
    }
    return simplex[best];
}

/// Fits one F0 slice: inclinations in increasing order, roughness from rough to smooth, each fit warm-started
/// from its neighbor.
struct SliceJob final
{
    usize Fresnel = 0;
    const Array<renderer::LtcAlbedoEntry>* AlbedoOrNull = nullptr; ///< Set before submission.
    renderer::LtcTableEntry* OutputOrNull = nullptr;               ///< This slice's transforms; set before submission.
    bool HasFailed = false;
};

[[nodiscard]] JobOutcome FitSlice(void* context) noexcept
{
    SliceJob& job = *static_cast<SliceJob*>(context);
    if (job.AlbedoOrNull == nullptr || job.OutputOrNull == nullptr)
    {
        return JobOutcome::Failed;
    }
    const float32 fresnel = GetFresnel(job.Fresnel);
    Array<Parameters> fits;
    if (fits.TryResize(renderer::LTC_TABLE_ROUGHNESS_COUNT * renderer::LTC_TABLE_INCLINATION_COUNT) == false)
    {
        job.HasFailed = true;
        return JobOutcome::Failed;
    }
    FitProblem problem;
    for (usize inclination = 0; inclination < renderer::LTC_TABLE_INCLINATION_COUNT; ++inclination)
    {
        for (usize step = 0; step < renderer::LTC_TABLE_ROUGHNESS_COUNT; ++step)
        {
            const usize roughness = renderer::LTC_TABLE_ROUGHNESS_COUNT - 1 - step;
            const usize index = inclination * renderer::LTC_TABLE_ROUGHNESS_COUNT + roughness;
            const float32 fitRoughness = math::Max(GetRoughness(roughness), MINIMUM_FIT_ROUGHNESS);
            if (step > 0 && fitRoughness == math::Max(GetRoughness(roughness + 1), MINIMUM_FIT_ROUGHNESS))
            {
                fits[index] = fits[index + 1];
                job.OutputOrNull[index] = job.OutputOrNull[index + 1];
                continue;
            }
            else
            {
                Parameters initial{1.0, 1.0, 0.0};
                if (step > 0)
                {
                    initial = fits[index + 1];
                }
                else if (inclination > 0)
                {
                    initial = fits[index - renderer::LTC_TABLE_ROUGHNESS_COUNT];
                }
                const renderer::LtcAlbedoEntry albedo = (*job.AlbedoOrNull)[index];
                const float64 norm = static_cast<float64>(fresnel) * static_cast<float64>(albedo.FresnelScale) +
                                     static_cast<float64>(albedo.FresnelBias);
                if (problem.TryInitialize(fitRoughness, GetInclination(inclination), fresnel, norm) == false)
                {
                    job.HasFailed = true;
                    return JobOutcome::Failed;
                }
                fits[index] = Minimize(problem, initial);
                for (usize restart = 0; restart < SIMPLEX_RESTART_COUNT; ++restart)
                {
                    fits[index] = Minimize(problem, fits[index]);
                }
            }
            job.OutputOrNull[index] = problem.MakeEntry(fits[index]);
        }
    }
    return JobOutcome::Succeeded;
}

void AppendFloat32(Array<uint8>& inoutBytes, float32 value) noexcept
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (usize i = 0; i < FLOAT32_BYTES; ++i)
    {
        static_cast<void>(inoutBytes.TryAdd(static_cast<uint8>((bits >> (BYTE_BITS * i)) & BYTE_MASK)));
    }
}

void AppendText(Array<uint8>& inoutBytes, std::string_view text) noexcept
{
    for (const char character : text)
    {
        static_cast<void>(inoutBytes.TryAdd(static_cast<uint8>(character)));
    }
}

void AppendUnsigned(Array<uint8>& inoutBytes, uint64 value) noexcept
{
    StaticArray<char, DECIMAL_BUFFER_BYTES> digits{};
    usize cursor = DECIMAL_BUFFER_BYTES;
    do
    {
        --cursor;
        digits[cursor] = static_cast<char>('0' + value % DECIMAL_BASE);
        value /= DECIMAL_BASE;
    } while (value != 0);
    AppendText(inoutBytes, std::string_view(digits.GetData() + cursor, DECIMAL_BUFFER_BYTES - cursor));
}
} // namespace

int main(int argumentCount, char** arguments)
{
    uint32 workerCount = DEFAULT_WORKER_COUNT;
    uint32 slice = static_cast<uint32>(renderer::LTC_TABLE_FRESNEL_COUNT);
    bool areArgumentsValid = argumentCount >= 2 && argumentCount % 2 == 0;
    for (int i = 2; areArgumentsValid == true && i + 1 < argumentCount; i += 2)
    {
        const std::string_view option(arguments[i]);
        const std::string_view value(arguments[i + 1]);
        areArgumentsValid = (option == "--workers" && TryParseUnsigned(value, workerCount) == true) ||
                            (option == "--slice" && TryParseUnsigned(value, slice) == true &&
                             slice < renderer::LTC_TABLE_FRESNEL_COUNT);
    }
    if (areArgumentsValid == false)
    {
        WriteText("usage: ludus_ltc_fitter <output-directory> [--workers <0-64>] [--slice <F0 index 0-50>]\n");
        return 2;
    }
    const bool isSingleSlice = slice < renderer::LTC_TABLE_FRESNEL_COUNT;
    filesystem::WriteDirectory directory;
    if (directory.Open(std::string_view(arguments[1])).Succeeded() == false)
    {
        WriteText("ludus_ltc_fitter: cannot open the output directory\n");
        return 1;
    }
    const uint64 start = time::NowTicks();

    // The albedo split depends only on roughness and inclination.
    Array<renderer::LtcAlbedoEntry> albedo;
    if (albedo.TryResize(renderer::LTC_TABLE_ROUGHNESS_COUNT * renderer::LTC_TABLE_INCLINATION_COUNT) == false)
    {
        return 1;
    }
    for (usize inclination = 0; inclination < renderer::LTC_TABLE_INCLINATION_COUNT; ++inclination)
    {
        for (usize roughness = 0; roughness < renderer::LTC_TABLE_ROUGHNESS_COUNT; ++roughness)
        {
            albedo[inclination * renderer::LTC_TABLE_ROUGHNESS_COUNT + roughness] =
                ComputeAlbedo(math::Max(GetRoughness(roughness), MINIMUM_FIT_ROUGHNESS), GetInclination(inclination));
        }
    }
    WriteText("albedo done\n");

    Array<renderer::LtcTableEntry> transforms;
    Array<SliceJob> jobs;
    if (transforms.TryResize(renderer::LTC_TABLE_TRANSFORM_COUNT) == false ||
        jobs.TryResize(renderer::LTC_TABLE_FRESNEL_COUNT) == false)
    {
        return 1;
    }
    JobSystem system;
    JobGraph graph;
    if (system.Initialize({ .WorkerCount = workerCount }) != JobStatus::Ok ||
        graph.Initialize({ .MaxJobs = static_cast<uint32>(renderer::LTC_TABLE_FRESNEL_COUNT) }) != JobStatus::Ok)
    {
        WriteText("ludus_ltc_fitter: cannot start the job system\n");
        return 1;
    }
    for (usize fresnel = 0; fresnel < renderer::LTC_TABLE_FRESNEL_COUNT; ++fresnel)
    {
        jobs[fresnel] = SliceJob
        {
            .Fresnel = fresnel,
            .AlbedoOrNull = &albedo,
            .OutputOrNull = transforms.GetData() +
                            fresnel * renderer::LTC_TABLE_ROUGHNESS_COUNT * renderer::LTC_TABLE_INCLINATION_COUNT,
        };
        JobHandle handle;
        if ((isSingleSlice == false || fresnel == slice) &&
            graph.Add(&FitSlice, &jobs[fresnel], handle) != JobStatus::Ok)
        {
            return 1;
        }
    }
    JobOutcome outcome = JobOutcome::Pending;
    if (graph.Seal() != JobStatus::Ok || system.Submit(graph) != JobStatus::Ok ||
        system.Wait(outcome) != JobStatus::Ok || outcome != JobOutcome::Succeeded)
    {
        WriteText("ludus_ltc_fitter: fitting failed\n");
        return 1;
    }
    static_cast<void>(system.Shutdown());

    // Little-endian float32: every transform entry, then every albedo entry (ltc_table.hpp).
    Array<uint8> table;
    if (table.TryEnsureCapacity(renderer::LTC_TABLE_BYTES) == false)
    {
        return 1;
    }
    for (const renderer::LtcTableEntry& entry : transforms)
    {
        AppendFloat32(table, entry.InverseM00);
        AppendFloat32(table, entry.InverseM02);
        AppendFloat32(table, entry.InverseM11);
        AppendFloat32(table, entry.InverseM20);
    }
    for (const renderer::LtcAlbedoEntry& entry : albedo)
    {
        AppendFloat32(table, entry.FresnelScale);
        AppendFloat32(table, entry.FresnelBias);
    }
    Array<uint8> provenance;
    AppendText(provenance, "{\n  \"version\": ");
    AppendUnsigned(provenance, renderer::LTC_TABLE_VERSION);
    AppendText(provenance, ",\n  \"bytes\": ");
    AppendUnsigned(provenance, table.GetSize());
    AppendText(provenance, ",\n  \"roughness_count\": ");
    AppendUnsigned(provenance, renderer::LTC_TABLE_ROUGHNESS_COUNT);
    AppendText(provenance, ",\n  \"inclination_count\": ");
    AppendUnsigned(provenance, renderer::LTC_TABLE_INCLINATION_COUNT);
    AppendText(provenance, ",\n  \"fresnel_count\": ");
    AppendUnsigned(provenance, renderer::LTC_TABLE_FRESNEL_COUNT);
    AppendText(provenance,
               ",\n  \"target\": \"Ludus reference BSDF specular lobe: GGX, height-correlated Smith, Schlick F0\",\n"
               "  \"error\": \"L3, balance-heuristic MIS of visible-normal BSDF and LTC samples\",\n"
               "  \"error_sample_grid\": ");
    AppendUnsigned(provenance, ERROR_SAMPLE_GRID);
    AppendText(provenance, ",\n  \"albedo_sample_grid\": ");
    AppendUnsigned(provenance, ALBEDO_SAMPLE_GRID);
    AppendText(provenance,
               ",\n  \"optimizer\": \"Nelder-Mead over a, c, s of M = R(average direction) [[a, 0, s], [0, c, 0], [0, "
               "0, 1]], warm-started, one "
               "restart\",\n  \"max_simplex_iterations\": ");
    AppendUnsigned(provenance, MAX_SIMPLEX_ITERATIONS);
    AppendText(provenance, ",\n  \"generator\": \"tools/ltc-fitter\"\n}\n");
    if (directory.Publish("ggx_ltc_table.bin", {table.GetData(), table.GetSize()}).Outcome.Succeeded() == false ||
        directory.Publish("ggx_ltc_table.json", {provenance.GetData(), provenance.GetSize()}).Outcome.Succeeded() ==
            false)
    {
        WriteText("ludus_ltc_fitter: cannot write the table\n");
        return 1;
    }
    WriteText("fitted in ");
    WriteUnsigned((time::NowTicks() - start) / time::NANOSECONDS_PER_SECOND);
    WriteText(" s\n");
    return 0;
}
