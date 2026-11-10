#include <ludus/graphics/renderer/reference.hpp>

#include "internal/reference_coating.h"
#include "internal/reference_dielectric.h"

#include <ludus/content/scene_format/luscn.h>
#include <ludus/content/scene_format/material_program.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>

// Thanks to Google/Filament, "Physically Based Rendering in Filament", sections
// 4.4 and 5.2.2, https://google.github.io/filament/main/filament.html: preserve L4's
// single-scattering GGX/correlated Smith/Schlick and Fresnel-weighted Lambert.
// Thanks to Eric Heitz, "Sampling the GGX Distribution of Visible Normals",
// JCGT 7(4), 2018, sections 3-4 and Appendix B,
// https://jcgt.org/published/0007/04/01/paper.pdf: use the projected-hemisphere
// construction and its matching reflection density, specialized to isotropic alpha.
// The diffuse/specular mixture and explicit null-event contract are Ludus choices.
// Thanks to Pharr, Jakob and Humphreys, Physically Based Rendering, 4th ed., section 2.2.3,
// https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency: evaluate the weighted
// sum of both proposal densities after sampling one lobe of the authored Mix.
namespace ludus::graphics::renderer
{
using namespace foundation;
namespace math = foundation::math;
namespace format = content::scene_format;
namespace
{
bool Unit(math::Vector3 v) noexcept
{
    if (math::IsFinite(v) == false)
    {
        return false;
    }
    const float32 square = math::Dot(v, v);
    return square >= 0.99999F && square <= 1.00001F;
}
bool Valid(const ReferenceBsdf& b) noexcept
{
    return format::IsMaterialScatteringValid(b.Scattering) == true &&
           internal::IsReferenceCoatingSupported(b.Scattering) == true &&
           internal::IsReferenceDielectricSupported(b.Scattering) == true &&
           (b.Scattering.Model != format::MaterialModel::Conductor || b.Scattering.AlphaU == b.Scattering.AlphaV) &&
           (b.Scattering.Model != format::MaterialModel::Program ||
            (b.ProgramOrNull != nullptr && format::IsMaterialProgramValid(*b.ProgramOrNull) == true)) &&
           math::IsFinite(b.BaseColor) && b.BaseColor.X >= 0 && b.BaseColor.X <= 1 && b.BaseColor.Y >= 0 &&
           b.BaseColor.Y <= 1 && b.BaseColor.Z >= 0 && b.BaseColor.Z <= 1 && math::IsFinite(b.Metallic) &&
           b.Metallic >= 0 && b.Metallic <= 1 && math::IsFinite(b.Roughness) && b.Roughness >= 0.045F &&
           b.Roughness <= 1;
}
math::Vector3 Normalized(math::Vector3 v) noexcept
{
    math::Vector3 result;
    static_cast<void>(math::TryNormalize(v, result));
    return result;
}
struct Basis final
{
    math::Vector3 X, Y, Z;
    explicit Basis(math::Vector3 normal) noexcept
        : X(Normalized(
              math::Cross(math::Abs(normal.Z) < 0.999F ? math::Vector3{0, 0, 1} : math::Vector3{0, 1, 0}, normal))),
          Y(math::Cross(normal, X)), Z(normal)
    {
    }
    [[nodiscard]] math::Vector3 Local(math::Vector3 v) const noexcept
    {
        return {math::Dot(v, X), math::Dot(v, Y), math::Dot(v, Z)};
    }
    [[nodiscard]] math::Vector3 World(math::Vector3 v) const noexcept
    {
        return X * v.X + Y * v.Y + Z * v.Z;
    }
};
float32 SpecularProbability(const ReferenceBsdf& b) noexcept
{
    if (b.Scattering.Model == format::MaterialModel::Diffuse)
    {
        return 0;
    }
    if (b.Scattering.Model == format::MaterialModel::DiffuseConductorMix)
    {
        return b.Metallic;
    }
    return b.Scattering.Model == format::MaterialModel::Conductor || b.Metallic == 1 ? 1.0F : 0.5F;
}
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, Physically Based Rendering,
// 4th ed., section 9.3, https://pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission:
// evaluate unpolarized complex-IOR Fresnel; RGB optical constants remain a declared proxy.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): incident cosine and optical constants of the Fresnel equation.
float32 ComputeConductorFresnel(float32 cosine, float32 eta, float32 extinction) noexcept
{
    if (eta == 1 && extinction == 0)
    {
        return 0;
    }
    const float32 cosineSquared = cosine * cosine;
    const float32 sineSquared = 1 - cosineSquared;
    const float32 etaSquared = eta * eta;
    const float32 extinctionSquared = extinction * extinction;
    const float32 difference = etaSquared - extinctionSquared - sineSquared;
    const float32 magnitude = math::Sqrt(difference * difference + 4 * etaSquared * extinctionSquared);
    const float32 realPart = math::Sqrt(math::Max(0.0F, (magnitude + difference) * 0.5F));
    const float32 first = magnitude + cosineSquared;
    const float32 second = 2 * cosine * realPart;
    const float32 perpendicular = (first - second) / (first + second);
    const float32 third = cosineSquared * magnitude + sineSquared * sineSquared;
    const float32 fourth = second * sineSquared;
    const float32 parallel = perpendicular * (third - fourth) / (third + fourth);
    return (perpendicular + parallel) * 0.5F;
}
float32 GetMicrofacetAlpha(const ReferenceBsdf& bsdf) noexcept
{
    return format::HasRoughConductorLobe(bsdf.Scattering.Model) == true ? bsdf.Scattering.AlphaU
                                                                        : bsdf.Roughness * bsdf.Roughness;
}
/// Lower bound of the height-correlated Smith visibility denominator; dimensionless, avoids division by zero at
/// grazing view and light.
constexpr float32 SMITH_DENOMINATOR_FLOOR = 1e-6F;
/// GGX distribution, height-correlated Smith visibility and visible-normal reflection density.
struct GgxReflection final
{
    float32 Distribution = 0;
    float32 Visibility = 0;
    float32 Pdf = 0;
};
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): GGX alpha and the view, light and half-vector cosines.
[[nodiscard]] GgxReflection EvaluateGgxReflection(float32 alpha, float32 nv, float32 nl, float32 nh) noexcept
{
    const float32 alphaSquared = alpha * alpha;
    // This stable equivalent avoids cancellation at low roughness and nh~=1.
    const float32 denominator = (1 - nh * nh) + nh * nh * alphaSquared;
    const float32 distribution = alphaSquared / (math::kPiF * denominator * denominator);
    const float32 lambdaView = math::Sqrt(nv * nv * (1 - alphaSquared) + alphaSquared);
    const float32 lambdaLight = math::Sqrt(nl * nl * (1 - alphaSquared) + alphaSquared);
    return
    {
        .Distribution = distribution,
        .Visibility = 0.5F / math::Max(nl * lambdaView + nv * lambdaLight, SMITH_DENOMINATOR_FLOOR),
        .Pdf = distribution / (2 * (nv + lambdaView)),
    };
}
/// Reflect the view about a visible GGX normal sampled from a unit-disk point; unnormalized.
[[nodiscard]] math::Vector3
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): GGX alpha, then the unit-disk point's two coordinates.
SampleGgxReflection(const Basis& frame, math::Vector3 view, float32 alpha, float32 diskX, float32 diskY) noexcept
{
    const math::Vector3 localView = frame.Local(view);
    const math::Vector3 stretched = Normalized({alpha * localView.X, alpha * localView.Y, localView.Z});
    const float32 tangentLength = stretched.X * stretched.X + stretched.Y * stretched.Y;
    const math::Vector3 tangent = tangentLength > 0
                                      ? math::Vector3{-stretched.Y, stretched.X, 0} / math::Sqrt(tangentLength)
                                      : math::Vector3{1, 0, 0};
    const math::Vector3 bitangent = math::Cross(stretched, tangent);
    const float32 blend = 0.5F * (1 + stretched.Z);
    const float32 warpedY = (1 - blend) * math::Sqrt(math::Max(0.0F, 1 - diskX * diskX)) + blend * diskY;
    const math::Vector3 hemisphere = tangent * diskX + bitangent * warpedY +
                                     stretched * math::Sqrt(math::Max(0.0F, 1 - diskX * diskX - warpedY * warpedY));
    const math::Vector3 half =
        frame.World(Normalized({alpha * hemisphere.X, alpha * hemisphere.Y, math::Max(0.0F, hemisphere.Z)}));
    return half * (2 * math::Dot(view, half)) - view;
}
// Thanks to Pharr, Jakob and Humphreys, Physically Based Rendering, 4th ed., sections 2.2.3 and A.5.2,
// https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency and
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions:
// apply mixture PDFs and hemisphere CDF inversion to a reciprocal power basis.
[[nodiscard]] ReferenceBsdfEvaluation
EvaluateProjectBrdf(const ReferenceBsdf& bsdf, float32 viewCosine, float32 lightCosine) noexcept
{
    const float32 viewLightCosine = math::Min(1.0F, viewCosine * lightCosine);
    const float32 clampedLightCosine = math::Min(1.0F, lightCosine);
    float32 value = 0;
    float32 pdf = 0;
    float32 total = 0;
    for (usize lobe = 0; lobe < format::PROJECT_BRDF_LOBE_COUNT; ++lobe)
    {
        const float32 weight = bsdf.Scattering.Eta[lobe];
        const float32 power = (1 - bsdf.Roughness) * bsdf.Scattering.K[lobe];
        const float32 scale = weight * (power + 2) / math::kTauF;
        const float32 viewLightPower =
            power == 0
                ? 1.0F
                : static_cast<float32>(math::Pow(static_cast<float64>(viewLightCosine), static_cast<float64>(power)));
        value += scale * viewLightPower;
        pdf += scale * static_cast<float32>(
                           math::Pow(static_cast<float64>(clampedLightCosine), static_cast<float64>(power + 1)));
        total += weight;
    }
    return {bsdf.BaseColor * (value / total), pdf / total};
}

[[nodiscard]] math::Vector3
SampleProjectBrdf(const ReferenceBsdf& bsdf, const Basis& frame, math::Vector3 sample) noexcept
{
    const float32 firstWeight = bsdf.Scattering.Eta[0];
    const float32 secondWeight = bsdf.Scattering.Eta[1];
    const float32 thirdWeight = bsdf.Scattering.Eta[2];
    const float32 firstTwoWeights = firstWeight + secondWeight;
    const float32 selector = sample.X * (firstTwoWeights + thirdWeight);
    const bool shouldSelectFirst = selector < firstWeight || (secondWeight == 0 && thirdWeight == 0);
    const usize lobe = shouldSelectFirst == true ? 0 : (selector < firstTwoWeights || thirdWeight == 0 ? 1 : 2);
    const float32 power = (1 - bsdf.Roughness) * bsdf.Scattering.K[lobe];
    const float32 cosine =
        static_cast<float32>(math::Pow(static_cast<float64>(1 - sample.Y), 1.0 / static_cast<float64>(power + 2)));
    const float32 radius = math::Sqrt(math::Max(0.0F, 1 - cosine * cosine));
    const float32 angle = math::kTauF * sample.Z;
    return frame.World({radius * math::Cos(angle), radius * math::Sin(angle), cosine});
}

// Closure-program interpreter (content::scene_format::MaterialProgram), the CPU oracle of
// material_program.slang. Thanks to Pharr, Jakob and Humphreys, Physically Based Rendering, 4th ed.,
// section 2.2.3, https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency: the weighted
// lobe mixture f = sum(w_i f_i) is sampled by choosing one lobe in proportion to its selection weight
// and returns the full mixture density sum(s_i p_i) / sum(s_i). The register program and lobe
// vocabulary are Ludus choices; see docs/architecture/renderer-systems.md#material-closure-programs.
/// Lobe weights, roughness and selection weights of one shading point.
struct ProgramMixture final
{
    StaticArray<math::Vector3, format::MAX_MATERIAL_PROGRAM_LOBES> Weights{};
    StaticArray<float32, format::MAX_MATERIAL_PROGRAM_LOBES> Roughness{};
    StaticArray<float32, format::MAX_MATERIAL_PROGRAM_LOBES> Selection{};
    float32 SelectionTotal = 0;
};
[[nodiscard]] math::Vector3 ClampProgramValue(math::Vector3 value) noexcept
{
    const float32 limit = format::MAX_MATERIAL_PROGRAM_CONSTANT;
    return {math::Clamp(value.X, -limit, limit),
            math::Clamp(value.Y, -limit, limit),
            math::Clamp(value.Z, -limit, limit)};
}
[[nodiscard]] ProgramMixture RunMaterialProgram(const ReferenceBsdf& bsdf) noexcept
{
    using format::MaterialProgramOpcode;
    const format::MaterialProgram& program = *bsdf.ProgramOrNull;
    StaticArray<math::Vector3, format::MAX_MATERIAL_PROGRAM_REGISTERS> registers{};
    for (usize index = 0; index < program.InstructionCount; ++index)
    {
        const format::MaterialProgramInstruction& instruction = program.Instructions[index];
        math::Vector3 result{};
        switch (instruction.Opcode)
        {
            case MaterialProgramOpcode::Constant: {
                const StaticArray<float32, format::MATERIAL_PROGRAM_COMPONENTS>& constant =
                    program.Constants[instruction.OperandA];
                result = {constant[0], constant[1], constant[2]};
                break;
            }
            case MaterialProgramOpcode::SurfaceInput:
                result = instruction.OperandA == static_cast<uint8>(format::MaterialProgramInput::BaseColor)
                             ? bsdf.BaseColor
                         : instruction.OperandA == static_cast<uint8>(format::MaterialProgramInput::Roughness)
                             ? math::Vector3{bsdf.Roughness, bsdf.Roughness, bsdf.Roughness}
                             : math::Vector3{bsdf.Metallic, bsdf.Metallic, bsdf.Metallic};
                break;
            case MaterialProgramOpcode::Add:
                result = registers[instruction.OperandA] + registers[instruction.OperandB];
                break;
            case MaterialProgramOpcode::Multiply:
                result = math::Hadamard(registers[instruction.OperandA], registers[instruction.OperandB]);
                break;
            case MaterialProgramOpcode::Lerp:
                result = registers[instruction.OperandA] +
                         math::Hadamard(registers[instruction.OperandB] - registers[instruction.OperandA],
                                        registers[instruction.OperandC]);
                break;
            case MaterialProgramOpcode::OneMinus:
                result = math::Vector3{1, 1, 1} - registers[instruction.OperandA];
                break;
            case MaterialProgramOpcode::Clamp: {
                const math::Vector3 value = registers[instruction.OperandA];
                result = {math::Clamp(value.X, 0.0F, 1.0F),
                          math::Clamp(value.Y, 0.0F, 1.0F),
                          math::Clamp(value.Z, 0.0F, 1.0F)};
                break;
            }
            case MaterialProgramOpcode::None:
                break;
        }
        registers[instruction.Destination] = ClampProgramValue(result);
    }
    ProgramMixture mixture;
    for (usize lobe = 0; lobe < program.LobeCount; ++lobe)
    {
        const format::MaterialProgramLobe& source = program.Lobes[lobe];
        const math::Vector3 weight = registers[source.WeightRegister];
        mixture.Weights[lobe] = {math::Max(weight.X, 0.0F), math::Max(weight.Y, 0.0F), math::Max(weight.Z, 0.0F)};
        mixture.Selection[lobe] = (mixture.Weights[lobe].X + mixture.Weights[lobe].Y + mixture.Weights[lobe].Z) /
                                  static_cast<float32>(format::MATERIAL_RGB_CHANNELS);
        mixture.Roughness[lobe] = source.RoughnessRegister == format::NO_MATERIAL_PROGRAM_REGISTER
                                      ? 1.0F
                                      : math::Clamp(registers[source.RoughnessRegister].X, format::MIN_ROUGHNESS, 1.0F);
        mixture.SelectionTotal += mixture.Selection[lobe];
    }
    return mixture;
}
/// One lobe's value without its weight, and its own solid-angle density.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): normal, view and light directions, then their cosines,
// in the order of every BSDF evaluation in this file.
[[nodiscard]] ReferenceBsdfEvaluation EvaluateProgramLobe(const format::MaterialProgramLobe& lobe,
                                                          float32 roughness,
                                                          math::Vector3 n,
                                                          math::Vector3 v,
                                                          math::Vector3 l,
                                                          float32 nv,
                                                          float32 nl) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (lobe.Operation == format::MaterialLobeOperation::Diffuse)
    {
        const float32 value = 1 / math::kPiF;
        return {{value, value, value}, nl / math::kPiF};
    }
    if (lobe.Operation == format::MaterialLobeOperation::Conductor)
    {
        const math::Vector3 h = Normalized(v + l);
        const float32 nh = math::Clamp(math::Dot(n, h), 0.0F, 1.0F);
        const float32 vh = math::Clamp(math::Dot(v, h), 0.0F, 1.0F);
        const GgxReflection ggx = EvaluateGgxReflection(roughness * roughness, nv, nl, nh);
        const math::Vector3 fresnel = {ComputeConductorFresnel(vh, lobe.Eta[0], lobe.K[0]),
                                       ComputeConductorFresnel(vh, lobe.Eta[1], lobe.K[1]),
                                       ComputeConductorFresnel(vh, lobe.Eta[2], lobe.K[2])};
        return {fresnel * (ggx.Distribution * ggx.Visibility), ggx.Pdf};
    }
    const float32 exponent = (1 - roughness) * lobe.Power;
    const float32 viewLightCosine = math::Min(1.0F, nv * nl);
    const float32 lightCosine = math::Min(1.0F, nl);
    const float32 scale = (exponent + 2) / math::kTauF;
    const float32 value = exponent == 0 ? scale
                                        : scale * static_cast<float32>(math::Pow(static_cast<float64>(viewLightCosine),
                                                                                 static_cast<float64>(exponent)));
    const float32 pdf =
        scale * static_cast<float32>(math::Pow(static_cast<float64>(lightCosine), static_cast<float64>(exponent + 1)));
    return {{value, value, value}, pdf};
}
[[nodiscard]] ReferenceBsdfEvaluation EvaluateMaterialProgram(const ReferenceBsdf& bsdf,
                                                              math::Vector3 n,
                                                              math::Vector3 v,
                                                              math::Vector3 l,
                                                              float32 nv,
                                                              float32 nl) noexcept
{
    const ProgramMixture mixture = RunMaterialProgram(bsdf);
    if (mixture.SelectionTotal <= 0)
    {
        return {};
    }
    ReferenceBsdfEvaluation result;
    for (usize lobe = 0; lobe < bsdf.ProgramOrNull->LobeCount; ++lobe)
    {
        const ReferenceBsdfEvaluation evaluated =
            EvaluateProgramLobe(bsdf.ProgramOrNull->Lobes[lobe], mixture.Roughness[lobe], n, v, l, nv, nl);
        result.Value = result.Value + math::Hadamard(mixture.Weights[lobe], evaluated.Value);
        result.Pdf += mixture.Selection[lobe] * evaluated.Pdf;
    }
    result.Pdf /= mixture.SelectionTotal;
    return result;
}
/// Select one lobe with sample.X in proportion to its selection weight and sample its direction
/// with the remaining two values; unnormalized, and zero for a program without positive weight.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the view direction, then the three uniform values.
[[nodiscard]] math::Vector3 SampleMaterialProgram(const ReferenceBsdf& bsdf,
                                                  const Basis& frame,
                                                  math::Vector3 view,
                                                  math::Vector3 sample,
                                                  float32 diskX,
                                                  float32 diskY) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const ProgramMixture mixture = RunMaterialProgram(bsdf);
    if (mixture.SelectionTotal <= 0)
    {
        return {};
    }
    const format::MaterialProgram& program = *bsdf.ProgramOrNull;
    const float32 selector = sample.X * mixture.SelectionTotal;
    float32 cumulative = 0;
    usize selected = program.LobeCount;
    // Rounding can leave the selector at the total; the last positive lobe then takes it.
    for (usize lobe = 0; lobe < program.LobeCount; ++lobe)
    {
        if (mixture.Selection[lobe] > 0)
        {
            selected = lobe;
            cumulative += mixture.Selection[lobe];
            if (selector < cumulative)
            {
                break;
            }
        }
    }
    const format::MaterialProgramLobe& lobe = program.Lobes[selected];
    const float32 roughness = mixture.Roughness[selected];
    if (lobe.Operation == format::MaterialLobeOperation::Diffuse)
    {
        return frame.World({diskX, diskY, math::Sqrt(math::Max(0.0F, 1 - sample.Y))});
    }
    if (lobe.Operation == format::MaterialLobeOperation::Conductor)
    {
        return SampleGgxReflection(frame, view, roughness * roughness, diskX, diskY);
    }
    const float32 exponent = (1 - roughness) * lobe.Power;
    const float32 cosine =
        static_cast<float32>(math::Pow(static_cast<float64>(1 - sample.Y), 1.0 / static_cast<float64>(exponent + 2)));
    const float32 radius = math::Sqrt(math::Max(0.0F, 1 - cosine * cosine));
    const float32 angle = math::kTauF * sample.Z;
    return frame.World({radius * math::Cos(angle), radius * math::Sin(angle), cosine});
}

ReferenceBsdfEvaluation Evaluate(const ReferenceBsdf& b, math::Vector3 n, math::Vector3 v, math::Vector3 l) noexcept
{
    const float32 nv = math::Dot(n, v);
    const float32 nl = math::Dot(n, l);
    if (nv <= 0 || nl <= 0 || internal::IsReferenceDielectric(b.Scattering.Model) == true)
    {
        return {};
    }
    if (b.Scattering.Model == format::MaterialModel::ProjectBrdf)
    {
        return EvaluateProjectBrdf(b, nv, nl);
    }
    if (b.Scattering.Model == format::MaterialModel::Program)
    {
        return EvaluateMaterialProgram(b, n, v, l, nv, nl);
    }
    const bool isMix = b.Scattering.Model == format::MaterialModel::DiffuseConductorMix;
    if (b.Scattering.Model == format::MaterialModel::Diffuse || (isMix == true && b.Metallic == 0))
    {
        return {b.BaseColor / math::kPiF, nl / math::kPiF};
    }
    const math::Vector3 h = Normalized(v + l);
    const float32 nh = math::Clamp(math::Dot(n, h), 0.0F, 1.0F);
    const float32 vh = math::Clamp(math::Dot(v, h), 0.0F, 1.0F);
    const GgxReflection ggx = EvaluateGgxReflection(GetMicrofacetAlpha(b), nv, nl, nh);
    const float32 distribution = ggx.Distribution;
    const float32 visibility = ggx.Visibility;
    const math::Vector3 f0 = math::Vector3{0.04F, 0.04F, 0.04F} * (1 - b.Metallic) + b.BaseColor * b.Metallic;
    const float32 oneMinus = 1 - vh;
    const float32 square = oneMinus * oneMinus;
    math::Vector3 fresnel = f0 + (math::Vector3{1, 1, 1} - f0) * (square * square * oneMinus);
    if (format::HasRoughConductorLobe(b.Scattering.Model) == true)
    {
        fresnel = {ComputeConductorFresnel(vh, b.Scattering.Eta[0], b.Scattering.K[0]),
                   ComputeConductorFresnel(vh, b.Scattering.Eta[1], b.Scattering.K[1]),
                   ComputeConductorFresnel(vh, b.Scattering.Eta[2], b.Scattering.K[2])};
    }
    const float32 specularPdf = ggx.Pdf;
    const float32 probability = SpecularProbability(b);
    const float32 pdf = probability * specularPdf + (1 - probability) * nl / math::kPiF;
    if (isMix == true)
    {
        return {fresnel * (distribution * visibility * probability) + b.BaseColor * ((1 - probability) / math::kPiF),
                pdf};
    }
    const float32 diffuseFraction = b.Scattering.Model == format::MaterialModel::Conductor ? 0 : 1 - b.Metallic;
    const math::Vector3 value =
        fresnel * (distribution * visibility) +
        math::Hadamard(b.BaseColor, math::Vector3{1, 1, 1} - fresnel) * (diffuseFraction / math::kPiF);
    return {value, pdf};
}
} // namespace
rhi::RasterStatus EvaluateReferenceBsdf(const ReferenceBsdf& b,
                                        math::Vector3 normal,
                                        math::Vector3 view,
                                        math::Vector3 light,
                                        ReferenceBsdfEvaluation& outEvaluation,
                                        ReferenceBsdfRandomContext random) noexcept
{
    if (Valid(b) == false || Unit(normal) == false || Unit(view) == false || Unit(light) == false ||
        (b.Scattering.Model == format::MaterialModel::CoatedDiffuse &&
         internal::IsCoatingRandomContextValid(random) == false))
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const ReferenceBsdfEvaluation result = b.Scattering.Model == format::MaterialModel::CoatedDiffuse
                                               ? internal::EvaluateCoatedDiffuse(b, normal, view, light, random)
                                               : Evaluate(b, normal, view, light);
    if (math::IsFinite(result.Value) == false || math::IsFinite(result.Pdf) == false || result.Pdf < 0)
    {
        return rhi::RasterStatus::Failed;
    }
    outEvaluation = result;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus SampleReferenceBsdf(const ReferenceBsdf& b,
                                      math::Vector3 normal,
                                      math::Vector3 view,
                                      math::Vector3 sample,
                                      ReferenceBsdfSample& outSample,
                                      ReferenceBsdfRandomContext random) noexcept
{
    if (Valid(b) == false || Unit(normal) == false || Unit(view) == false || math::IsFinite(sample) == false ||
        sample.X < 0 || sample.X >= 1 || sample.Y < 0 || sample.Y >= 1 || sample.Z < 0 || sample.Z >= 1 ||
        (b.Scattering.Model == format::MaterialModel::CoatedDiffuse &&
         internal::IsCoatingRandomContextValid(random) == false))
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    if (b.Scattering.Model == format::MaterialModel::CoatedDiffuse)
    {
        const ReferenceBsdfSample result = internal::SampleCoatedDiffuse(b, normal, view, sample, random);
        if (math::IsFinite(result.Weight) == false || math::IsFinite(result.MisPdf) == false || result.MisPdf < 0 ||
            math::IsFinite(result.Evaluation.Value) == false)
        {
            return rhi::RasterStatus::Failed;
        }
        outSample = result;
        return rhi::RasterStatus::Ready;
    }
    ReferenceBsdfSample result;
    if (math::Dot(normal, view) <= 0)
    {
        outSample = result;
        return rhi::RasterStatus::Ready;
    }
    if (internal::IsReferenceDielectric(b.Scattering.Model) == true)
    {
        result = internal::SampleDielectric(b, normal, view, sample.X);
        if (Unit(result.Direction) == false || math::IsFinite(result.Weight) == false)
        {
            return rhi::RasterStatus::Failed;
        }
        outSample = result;
        return rhi::RasterStatus::Ready;
    }
    const Basis frame(normal);
    math::Vector3 direction;
    const float32 radius = math::Sqrt(sample.Y);
    const float32 angle = math::kTauF * sample.Z;
    const float32 diskX = radius * math::Cos(angle);
    const float32 diskY = radius * math::Sin(angle);
    if (b.Scattering.Model == format::MaterialModel::ProjectBrdf)
    {
        direction = SampleProjectBrdf(b, frame, sample);
    }
    else if (b.Scattering.Model == format::MaterialModel::Program)
    {
        direction = SampleMaterialProgram(b, frame, view, sample, diskX, diskY);
    }
    else if (sample.X >= SpecularProbability(b))
    {
        direction = frame.World({diskX, diskY, math::Sqrt(math::Max(0.0F, 1 - sample.Y))});
    }
    else
    {
        direction = SampleGgxReflection(frame, view, GetMicrofacetAlpha(b), diskX, diskY);
    }
    direction = Normalized(direction);
    if (math::Dot(normal, direction) > 0)
    {
        result.Direction = direction;
        result.Evaluation = Evaluate(b, normal, view, direction);
        if (math::IsFinite(result.Evaluation.Value) == false || math::IsFinite(result.Evaluation.Pdf) == false ||
            result.Evaluation.Pdf <= 0)
        {
            return rhi::RasterStatus::Failed;
        }
        result.Weight = result.Evaluation.Value * (math::Dot(normal, direction) / result.Evaluation.Pdf);
        result.MisPdf = result.Evaluation.Pdf;
        result.Accepted = true;
    }
    outSample = result;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::graphics::renderer
