#include <ludus/graphics/renderer/reference.hpp>

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
           (b.Scattering.Model != format::MaterialModel::Conductor || b.Scattering.AlphaU == b.Scattering.AlphaV) &&
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
    return b.Scattering.Model == format::MaterialModel::Conductor || b.Metallic == 1 ? 1.0F : 0.5F;
}
// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, Physically Based Rendering,
// 4th ed., section 9.3, https://pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission:
// evaluate unpolarized complex-IOR Fresnel; RGB optical constants remain a declared proxy.
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
    return bsdf.Scattering.Model == format::MaterialModel::Conductor ? bsdf.Scattering.AlphaU
                                                                     : bsdf.Roughness * bsdf.Roughness;
}
ReferenceBsdfEvaluation Evaluate(const ReferenceBsdf& b, math::Vector3 n, math::Vector3 v, math::Vector3 l) noexcept
{
    const float32 nv = math::Dot(n, v);
    const float32 nl = math::Dot(n, l);
    if (nv <= 0 || nl <= 0)
    {
        return {};
    }
    if (b.Scattering.Model == format::MaterialModel::Diffuse)
    {
        return {b.BaseColor / math::kPiF, nl / math::kPiF};
    }
    const math::Vector3 h = Normalized(v + l);
    const float32 nh = math::Clamp(math::Dot(n, h), 0.0F, 1.0F);
    const float32 vh = math::Clamp(math::Dot(v, h), 0.0F, 1.0F);
    const float32 alpha = GetMicrofacetAlpha(b);
    const float32 alphaSquared = alpha * alpha;
    // This stable equivalent avoids cancellation at low roughness and nh~=1.
    const float32 denominator = (1 - nh * nh) + nh * nh * alphaSquared;
    const float32 distribution = alphaSquared / (math::kPiF * denominator * denominator);
    const float32 lambdaView = math::Sqrt(nv * nv * (1 - alphaSquared) + alphaSquared);
    const float32 lambdaLight = math::Sqrt(nl * nl * (1 - alphaSquared) + alphaSquared);
    const float32 visibility = 0.5F / math::Max(nl * lambdaView + nv * lambdaLight, 1e-6F);
    const math::Vector3 f0 = math::Vector3{0.04F, 0.04F, 0.04F} * (1 - b.Metallic) + b.BaseColor * b.Metallic;
    const float32 oneMinus = 1 - vh;
    const float32 square = oneMinus * oneMinus;
    math::Vector3 fresnel = f0 + (math::Vector3{1, 1, 1} - f0) * (square * square * oneMinus);
    const bool isConductor = b.Scattering.Model == format::MaterialModel::Conductor;
    if (isConductor == true)
    {
        fresnel = {ComputeConductorFresnel(vh, b.Scattering.Eta[0], b.Scattering.K[0]),
                   ComputeConductorFresnel(vh, b.Scattering.Eta[1], b.Scattering.K[1]),
                   ComputeConductorFresnel(vh, b.Scattering.Eta[2], b.Scattering.K[2])};
    }
    const float32 diffuseFraction = isConductor == true ? 0 : 1 - b.Metallic;
    const math::Vector3 value =
        fresnel * (distribution * visibility) +
        math::Hadamard(b.BaseColor, math::Vector3{1, 1, 1} - fresnel) * (diffuseFraction / math::kPiF);
    const float32 specularPdf = distribution / (2 * (nv + lambdaView));
    const float32 probability = SpecularProbability(b);
    return {value, probability * specularPdf + (1 - probability) * nl / math::kPiF};
}
} // namespace
rhi::RasterStatus EvaluateReferenceBsdf(const ReferenceBsdf& b,
                                        math::Vector3 normal,
                                        math::Vector3 view,
                                        math::Vector3 light,
                                        ReferenceBsdfEvaluation& output) noexcept
{
    if (Valid(b) == false || Unit(normal) == false || Unit(view) == false || Unit(light) == false)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    const ReferenceBsdfEvaluation result = Evaluate(b, normal, view, light);
    if (math::IsFinite(result.Value) == false || math::IsFinite(result.Pdf) == false || result.Pdf < 0)
    {
        return rhi::RasterStatus::Failed;
    }
    output = result;
    return rhi::RasterStatus::Ready;
}
rhi::RasterStatus SampleReferenceBsdf(const ReferenceBsdf& b,
                                      math::Vector3 normal,
                                      math::Vector3 view,
                                      math::Vector3 sample,
                                      ReferenceBsdfSample& output) noexcept
{
    if (Valid(b) == false || Unit(normal) == false || Unit(view) == false || math::IsFinite(sample) == false ||
        sample.X < 0 || sample.X >= 1 || sample.Y < 0 || sample.Y >= 1 || sample.Z < 0 || sample.Z >= 1)
    {
        return rhi::RasterStatus::InvalidDescription;
    }
    ReferenceBsdfSample result;
    if (math::Dot(normal, view) <= 0)
    {
        output = result;
        return rhi::RasterStatus::Ready;
    }
    const Basis frame(normal);
    math::Vector3 direction;
    const float32 radius = math::Sqrt(sample.Y);
    const float32 angle = math::kTauF * sample.Z;
    const float32 diskX = radius * math::Cos(angle);
    const float32 diskY = radius * math::Sin(angle);
    if (sample.X >= SpecularProbability(b))
    {
        direction = frame.World({diskX, diskY, math::Sqrt(math::Max(0.0F, 1 - sample.Y))});
    }
    else
    {
        const math::Vector3 localView = frame.Local(view);
        const float32 alpha = GetMicrofacetAlpha(b);
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
        direction = half * (2 * math::Dot(view, half)) - view;
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
        result.Accepted = true;
    }
    output = result;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::graphics::renderer
