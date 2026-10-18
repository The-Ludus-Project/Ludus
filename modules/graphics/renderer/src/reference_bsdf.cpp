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
namespace
{
bool Unit(math::Vector3 v) noexcept
{
    if (math::IsFinite(v) == false)
    {
        return false;
    }
    const auto square = math::Dot(v, v);
    return square >= 0.99999F && square <= 1.00001F;
}
bool Valid(const ReferenceBsdf& b) noexcept
{
    return math::IsFinite(b.BaseColor) && b.BaseColor.X >= 0 && b.BaseColor.X <= 1 && b.BaseColor.Y >= 0 &&
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
    return b.Metallic == 1 ? 1.0F : 0.5F;
}
ReferenceBsdfEvaluation Evaluate(const ReferenceBsdf& b, math::Vector3 n, math::Vector3 v, math::Vector3 l) noexcept
{
    const auto nv = math::Dot(n, v), nl = math::Dot(n, l);
    if (nv <= 0 || nl <= 0)
    {
        return {};
    }
    const auto h = Normalized(v + l);
    const auto nh = math::Clamp(math::Dot(n, h), 0.0F, 1.0F), vh = math::Clamp(math::Dot(v, h), 0.0F, 1.0F);
    const auto alpha = b.Roughness * b.Roughness, alphaSquared = alpha * alpha;
    // This stable equivalent avoids cancellation at low roughness and nh~=1.
    const auto denominator = (1 - nh * nh) + nh * nh * alphaSquared;
    const auto distribution = alphaSquared / (math::kPiF * denominator * denominator);
    const auto lambdaView = math::Sqrt(nv * nv * (1 - alphaSquared) + alphaSquared);
    const auto lambdaLight = math::Sqrt(nl * nl * (1 - alphaSquared) + alphaSquared);
    const auto visibility = 0.5F / math::Max(nl * lambdaView + nv * lambdaLight, 1e-6F);
    const auto f0 = math::Vector3{0.04F, 0.04F, 0.04F} * (1 - b.Metallic) + b.BaseColor * b.Metallic;
    const auto oneMinus = 1 - vh, square = oneMinus * oneMinus;
    const auto fresnel = f0 + (math::Vector3{1, 1, 1} - f0) * (square * square * oneMinus);
    const auto value = fresnel * (distribution * visibility) +
                       math::Hadamard(b.BaseColor, math::Vector3{1, 1, 1} - fresnel) * ((1 - b.Metallic) / math::kPiF);
    const auto specularPdf = distribution / (2 * (nv + lambdaView));
    const auto probability = SpecularProbability(b);
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
    const auto result = Evaluate(b, normal, view, light);
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
    const auto radius = math::Sqrt(sample.Y), angle = math::kTauF * sample.Z;
    const auto diskX = radius * math::Cos(angle), diskY = radius * math::Sin(angle);
    if (sample.X >= SpecularProbability(b))
    {
        direction = frame.World({diskX, diskY, math::Sqrt(math::Max(0.0F, 1 - sample.Y))});
    }
    else
    {
        const auto localView = frame.Local(view);
        const auto alpha = b.Roughness * b.Roughness;
        const auto stretched = Normalized({alpha * localView.X, alpha * localView.Y, localView.Z});
        const auto tangentLength = stretched.X * stretched.X + stretched.Y * stretched.Y;
        const auto tangent = tangentLength > 0 ? math::Vector3{-stretched.Y, stretched.X, 0} / math::Sqrt(tangentLength)
                                               : math::Vector3{1, 0, 0};
        const auto bitangent = math::Cross(stretched, tangent);
        const auto blend = 0.5F * (1 + stretched.Z);
        const auto warpedY = (1 - blend) * math::Sqrt(math::Max(0.0F, 1 - diskX * diskX)) + blend * diskY;
        const auto hemisphere = tangent * diskX + bitangent * warpedY +
                                stretched * math::Sqrt(math::Max(0.0F, 1 - diskX * diskX - warpedY * warpedY));
        const auto half =
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
