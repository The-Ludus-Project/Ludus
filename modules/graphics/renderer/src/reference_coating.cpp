#include "internal/reference_coating.h"
#include "../shaders/coated_diffuse_layout.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>

// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, Physically Based Rendering,
// 4th edition (2023), sections 9.7 and 14.3, for rough dielectric transmission and
// layered BSDF random walks:
// https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Scattering_from_Layered_Materials. Adapted from pbrt-v4
// revision b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f, src/pbrt/bxdfs.{h,cpp} and util/scattering.h, Copyright 1998-2020
// the authors, licensed under Apache-2.0 (see third_party/pbrt/LICENSE.txt). Specialization: isotropic rough dielectric
// over opaque Lambertian, zero medium scattering, RGB throughput, ten internal events. Philox addresses replace PCG;
// path ratios are accumulated directly to avoid products of conditional densities.
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace math = foundation::math;
using content::scene_format::LUDUS_COATING_MAX_DEPTH;
using content::scene_format::LUDUS_COATING_MIN_ALPHA;
namespace
{
math::Vector3 NormalizeCoatingVector(math::Vector3 value) noexcept
{
    math::Vector3 result{};
    static_cast<void>(math::TryNormalize(value, result));
    return result;
}
struct CoatingFrame final
{
    math::Vector3 AxisXWorld;
    math::Vector3 AxisYWorld;
    math::Vector3 NormalWorld;
};
CoatingFrame MakeCoatingFrame(math::Vector3 normalWorld) noexcept
{
    const math::Vector3 axisXWorld = NormalizeCoatingVector(math::Cross(
        math::Abs(normalWorld.Z) < COATING_BASIS_SWITCH_COSINE ? math::Vector3{0, 0, 1} : math::Vector3{0, 1, 0},
        normalWorld));
    return {axisXWorld, math::Cross(normalWorld, axisXWorld), normalWorld};
}
math::Vector3 TransformCoatingToTangent(const CoatingFrame& frame, math::Vector3 directionWorld) noexcept
{
    return {math::Dot(directionWorld, frame.AxisXWorld),
            math::Dot(directionWorld, frame.AxisYWorld),
            math::Dot(directionWorld, frame.NormalWorld)};
}
math::Vector3 TransformCoatingToWorld(const CoatingFrame& frame, math::Vector3 directionTangent) noexcept
{
    return NormalizeCoatingVector(frame.AxisXWorld * directionTangent.X + frame.AxisYWorld * directionTangent.Y +
                                  frame.NormalWorld * directionTangent.Z);
}
class CoatingRandomStream final
{
public:
    LUDUS_INLINE CoatingRandomStream(ReferenceBsdfRandomContext random, uint32 domain) noexcept
        : mKey{random.Key.Value ^ (uint64{domain} << COATING_KEY_WORD_SHIFT)}, mAddress(random.Address)
    {
        mAddress.Dimension = static_cast<uint16>(uint32{mAddress.Dimension} * COATING_DIMENSIONS_PER_EVENT);
    }
    LUDUS_INLINE float32 NextFloat() noexcept
    {
        const float32 result = math::SampleFloat01(mKey, mAddress);
        ++mAddress.Dimension;
        return result;
    }
    LUDUS_INLINE math::Vector3 NextThree() noexcept
    {
        const float32 first = NextFloat();
        const float32 second = NextFloat();
        const float32 third = NextFloat();
        return {first, second, third};
    }

private:
    math::RandomKey mKey;
    math::RandomAddress mAddress;
};
float32 ComputeCoatingFresnel(float32 cosine, float32 eta) noexcept
{
    cosine = math::Clamp(cosine, -1.0F, 1.0F);
    if (cosine < 0)
    {
        eta = 1 / eta;
        cosine = -cosine;
    }
    const float32 transmittedSineSquared = (1 - cosine * cosine) / (eta * eta);
    if (transmittedSineSquared >= 1)
    {
        return 1;
    }
    const float32 transmittedCosine = math::Sqrt(1 - transmittedSineSquared);
    const float32 parallel = (eta * cosine - transmittedCosine) / (eta * cosine + transmittedCosine);
    const float32 perpendicular = (cosine - eta * transmittedCosine) / (cosine + eta * transmittedCosine);
    return (parallel * parallel + perpendicular * perpendicular) * 0.5F;
}
float32 ComputeCoatingDistribution(float32 alpha, math::Vector3 halfTangent) noexcept
{
    const float32 cosineSquared = halfTangent.Z * halfTangent.Z;
    if (cosineSquared * cosineSquared < COATING_MIN_NORMAL_COSINE_FOURTH)
    {
        return 0;
    }
    const float32 alphaSquared = alpha * alpha;
    const float32 denominator =
        halfTangent.X * halfTangent.X + halfTangent.Y * halfTangent.Y + alphaSquared * halfTangent.Z * halfTangent.Z;
    return alphaSquared / (math::kPiF * denominator * denominator);
}
float32 ComputeCoatingProjectedLength(float32 alpha, math::Vector3 directionTangent) noexcept
{
    return math::Sqrt(directionTangent.Z * directionTangent.Z +
                      alpha * alpha *
                          (directionTangent.X * directionTangent.X + directionTangent.Y * directionTangent.Y));
}
float32 ComputeCoatingGeometry(float32 alpha, math::Vector3 viewTangent, math::Vector3 lightTangent) noexcept
{
    const float32 viewCosine = math::Abs(viewTangent.Z);
    const float32 lightCosine = math::Abs(lightTangent.Z);
    return 2 * viewCosine * lightCosine /
           (ComputeCoatingProjectedLength(alpha, viewTangent) * lightCosine +
            ComputeCoatingProjectedLength(alpha, lightTangent) * viewCosine);
}
float32 ComputeCoatingNormalPdf(float32 alpha, math::Vector3 viewTangent, math::Vector3 halfTangent) noexcept
{
    return 2 * ComputeCoatingDistribution(alpha, halfTangent) * math::Abs(math::Dot(viewTangent, halfTangent)) /
           (math::Abs(viewTangent.Z) + ComputeCoatingProjectedLength(alpha, viewTangent));
}
math::Vector3 SampleCoatingNormal(float32 alpha, math::Vector3 viewTangent, math::Vector2 samples) noexcept
{
    math::Vector3 stretchedTangent =
        NormalizeCoatingVector({alpha * viewTangent.X, alpha * viewTangent.Y, viewTangent.Z});
    if (stretchedTangent.Z < 0)
    {
        stretchedTangent = -stretchedTangent;
    }
    const math::Vector3 firstTangent = stretchedTangent.Z < COATING_BASIS_SWITCH_COSINE
                                           ? NormalizeCoatingVector(math::Cross({0, 0, 1}, stretchedTangent))
                                           : math::Vector3{1, 0, 0};
    const math::Vector3 secondTangent = math::Cross(stretchedTangent, firstTangent);
    const float32 radius = math::Sqrt(samples.X);
    const float32 angle = math::kTauF * samples.Y;
    const float32 diskX = radius * math::Cos(angle);
    const float32 diskY = radius * math::Sin(angle);
    const float32 blend = (1 + stretchedTangent.Z) * 0.5F;
    const float32 warpedY = (1 - blend) * math::Sqrt(math::Max(0.0F, 1 - diskX * diskX)) + blend * diskY;
    const float32 projectedZ = math::Sqrt(math::Max(0.0F, 1 - diskX * diskX - warpedY * warpedY));
    const math::Vector3 projectedTangent =
        firstTangent * diskX + secondTangent * warpedY + stretchedTangent * projectedZ;
    return NormalizeCoatingVector(
        {alpha * projectedTangent.X, alpha * projectedTangent.Y, math::Max(COATING_MIN_NORMAL_Z, projectedTangent.Z)});
}
float32 ComputeCoatingInterfaceWithNormal(float32 alpha,
                                          float32 eta,
                                          math::Vector3 viewTangent,
                                          math::Vector3 lightTangent,
                                          math::Vector3 halfTangent,
                                          bool isRadiance,
                                          uint32 choices,
                                          float32& outPdf) noexcept
{
    outPdf = 0;
    if (viewTangent.Z == 0 || lightTangent.Z == 0)
    {
        return 0;
    }
    const bool isReflection = viewTangent.Z * lightTangent.Z > 0;
    const float32 relativeEta = isReflection == true ? 1 : (viewTangent.Z > 0 ? eta : 1 / eta);
    if (halfTangent.Z < 0)
    {
        halfTangent = -halfTangent;
    }
    const float32 viewDotHalf = math::Dot(viewTangent, halfTangent);
    const float32 lightDotHalf = math::Dot(lightTangent, halfTangent);
    if (viewDotHalf * viewTangent.Z < 0 || lightDotHalf * lightTangent.Z < 0 || halfTangent.Z == 0)
    {
        return 0;
    }
    const float32 reflectance = ComputeCoatingFresnel(viewDotHalf, eta);
    const float32 reflectionProbability = (choices & COATING_REFLECTION) != 0 ? reflectance : 0;
    const float32 transmissionProbability = (choices & COATING_TRANSMISSION) != 0 ? 1 - reflectance : 0;
    const float32 totalProbability = reflectionProbability + transmissionProbability;
    if (totalProbability == 0)
    {
        return 0;
    }
    const float32 distribution = ComputeCoatingDistribution(alpha, halfTangent);
    const float32 geometry = ComputeCoatingGeometry(alpha, viewTangent, lightTangent);
    const float32 normalPdf = ComputeCoatingNormalPdf(alpha, viewTangent, halfTangent);
    if (isReflection == true)
    {
        outPdf = normalPdf * reflectionProbability /
                 (COATING_REFLECTION_JACOBIAN * math::Abs(viewDotHalf) * totalProbability);
        return distribution * geometry * reflectance /
               math::Abs(COATING_REFLECTION_JACOBIAN * viewTangent.Z * lightTangent.Z);
    }
    const float32 halfSum = lightDotHalf + viewDotHalf / relativeEta;
    const float32 denominator = halfSum * halfSum;
    if (denominator == 0)
    {
        return 0;
    }
    outPdf = normalPdf * math::Abs(lightDotHalf) / denominator * transmissionProbability / totalProbability;
    const float32 value = (1 - reflectance) * distribution * geometry *
                          math::Abs(viewDotHalf * lightDotHalf / (viewTangent.Z * lightTangent.Z * denominator));
    return isRadiance == true ? value / (relativeEta * relativeEta) : value;
}
float32 ComputeCoatingInterface(float32 alpha,
                                float32 eta,
                                math::Vector3 viewTangent,
                                math::Vector3 lightTangent,
                                bool isRadiance,
                                uint32 choices,
                                float32& outPdf) noexcept
{
    const bool isReflection = viewTangent.Z * lightTangent.Z > 0;
    const float32 relativeEta = isReflection == true ? 1 : (viewTangent.Z > 0 ? eta : 1 / eta);
    const math::Vector3 halfTangent = NormalizeCoatingVector(viewTangent + lightTangent * relativeEta);
    return ComputeCoatingInterfaceWithNormal(alpha,
                                             eta,
                                             viewTangent,
                                             lightTangent,
                                             halfTangent,
                                             isRadiance,
                                             choices,
                                             outPdf);
}
struct CoatingInterfaceSample final
{
    math::Vector3 DirectionTangent{};
    float32 Value = 0;
    float32 Pdf = 0;
    bool IsTransmission = false;
    bool HasSample = false;
};
CoatingInterfaceSample SampleCoatingInterface(float32 alpha,
                                              float32 eta,
                                              math::Vector3 viewTangent,
                                              math::Vector3 samples,
                                              bool isRadiance,
                                              uint32 choices) noexcept
{
    CoatingInterfaceSample result;
    if (viewTangent.Z == 0)
    {
        return result;
    }
    math::Vector3 halfTangent = SampleCoatingNormal(alpha, viewTangent, {samples.Y, samples.Z});
    const math::Vector3 distributionNormalTangent = halfTangent;
    const float32 reflectance = ComputeCoatingFresnel(math::Dot(viewTangent, halfTangent), eta);
    const float32 reflectionProbability = (choices & COATING_REFLECTION) != 0 ? reflectance : 0;
    const float32 transmissionProbability = (choices & COATING_TRANSMISSION) != 0 ? 1 - reflectance : 0;
    if (reflectionProbability + transmissionProbability == 0)
    {
        return result;
    }
    const bool isReflection = samples.X < reflectionProbability / (reflectionProbability + transmissionProbability);
    if (isReflection == true)
    {
        result.DirectionTangent = halfTangent * (2 * math::Dot(viewTangent, halfTangent)) - viewTangent;
        if (result.DirectionTangent.Z * viewTangent.Z <= 0)
        {
            return result;
        }
    }
    else
    {
        float32 cosine = math::Dot(viewTangent, halfTangent);
        float32 relativeEta = eta;
        if (cosine < 0)
        {
            relativeEta = 1 / relativeEta;
            halfTangent = -halfTangent;
            cosine = -cosine;
        }
        const float32 sineSquared = math::Max(0.0F, 1 - cosine * cosine) / (relativeEta * relativeEta);
        if (sineSquared >= 1)
        {
            return result;
        }
        result.DirectionTangent =
            -viewTangent / relativeEta + halfTangent * (cosine / relativeEta - math::Sqrt(1 - sineSquared));
        if (result.DirectionTangent.Z * viewTangent.Z >= 0)
        {
            return result;
        }
        result.IsTransmission = true;
    }
    result.Value = ComputeCoatingInterfaceWithNormal(alpha,
                                                     eta,
                                                     viewTangent,
                                                     result.DirectionTangent,
                                                     distributionNormalTangent,
                                                     isRadiance,
                                                     choices,
                                                     result.Pdf);
    result.HasSample = result.Value > 0 && result.Pdf > 0 && result.DirectionTangent.Z != 0;
    return result;
}
math::Vector3 SampleCoatingBase(math::Vector3 samples) noexcept
{
    const float32 radius = math::Sqrt(samples.Y);
    const float32 angle = math::kTauF * samples.Z;
    return {radius * math::Cos(angle), radius * math::Sin(angle), math::Sqrt(math::Max(0.0F, 1 - samples.Y))};
}
float32 ComputeCoatingTransmittance(float32 thickness, math::Vector3 directionTangent) noexcept
{
    return directionTangent.Z != 0
               ? static_cast<float32>(math::Exp(static_cast<float64>(-math::Abs(thickness / directionTangent.Z))))
               : 0;
}
float32 ComputeCoatingPowerWeight(float32 first, float32 second) noexcept
{
    return first * first / (first * first + second * second);
}
float32 GetCoatingMaximum(math::Vector3 value) noexcept
{
    return math::Max(value.X, math::Max(value.Y, value.Z));
}
float32 EvaluateCoatingPdf(const ReferenceBsdf& bsdf,
                           math::Vector3 viewTangent,
                           math::Vector3 lightTangent,
                           ReferenceBsdfRandomContext random) noexcept
{
    CoatingRandomStream stream(random, COATING_PDF_DOMAIN);
    const float32 alpha = bsdf.Scattering.AlphaU;
    const float32 eta = bsdf.Scattering.CoatingEta;
    float32 sum = 0;
    static_cast<void>(ComputeCoatingInterface(alpha, eta, viewTangent, lightTangent, true, COATING_REFLECTION, sum));
    const CoatingInterfaceSample entrance =
        SampleCoatingInterface(alpha, eta, viewTangent, stream.NextThree(), true, COATING_TRANSMISSION);
    const CoatingInterfaceSample exit =
        SampleCoatingInterface(alpha, eta, lightTangent, stream.NextThree(), false, COATING_TRANSMISSION);
    if (entrance.HasSample == true && exit.HasSample == true && GetCoatingMaximum(bsdf.BaseColor) > 0)
    {
        const math::Vector3 baseDirectionTangent = SampleCoatingBase(stream.NextThree());
        const float32 basePdf = baseDirectionTangent.Z / math::kPiF;
        const float32 reverseBasePdf = math::Abs(exit.DirectionTangent.Z) / math::kPiF;
        sum += ComputeCoatingPowerWeight(exit.Pdf, reverseBasePdf) * reverseBasePdf;
        float32 exitPdf = 0;
        static_cast<void>(ComputeCoatingInterface(alpha,
                                                  eta,
                                                  -baseDirectionTangent,
                                                  lightTangent,
                                                  true,
                                                  COATING_TRANSMISSION,
                                                  exitPdf));
        sum += ComputeCoatingPowerWeight(basePdf, exitPdf) * exitPdf;
    }
    return COATING_PDF_ESTIMATE_WEIGHT * sum +
           (1 - COATING_PDF_ESTIMATE_WEIGHT) / (COATING_REFLECTION_JACOBIAN * math::kPiF);
}
math::Vector3 EvaluateCoatingValue(const ReferenceBsdf& bsdf,
                                   math::Vector3 viewTangent,
                                   math::Vector3 lightTangent,
                                   ReferenceBsdfRandomContext random) noexcept
{
    CoatingRandomStream stream(random, COATING_EVALUATION_DOMAIN);
    const float32 alpha = bsdf.Scattering.AlphaU;
    const float32 eta = bsdf.Scattering.CoatingEta;
    const float32 thickness = bsdf.Scattering.CoatingThickness;
    float32 unusedPdf = 0;
    const float32 reflection =
        ComputeCoatingInterface(alpha, eta, viewTangent, lightTangent, true, COATING_ALL, unusedPdf);
    math::Vector3 result{reflection, reflection, reflection};
    const CoatingInterfaceSample entrance =
        SampleCoatingInterface(alpha, eta, viewTangent, stream.NextThree(), true, COATING_TRANSMISSION);
    const CoatingInterfaceSample exit =
        SampleCoatingInterface(alpha, eta, lightTangent, stream.NextThree(), false, COATING_TRANSMISSION);
    if (entrance.HasSample == false || exit.HasSample == false)
    {
        return result;
    }
    const float32 entranceWeight = entrance.Value * math::Abs(entrance.DirectionTangent.Z) / entrance.Pdf;
    math::Vector3 weight{entranceWeight, entranceWeight, entranceWeight};
    math::Vector3 directionTangent = entrance.DirectionTangent;
    bool isAtTop = true;
    for (uint32 depth = 0; depth < LUDUS_COATING_MAX_DEPTH; ++depth)
    {
        const float32 maximum = GetCoatingMaximum(weight);
        if (maximum == 0)
        {
            break;
        }
        if (depth > COATING_ROULETTE_START && maximum < COATING_ROULETTE_THRESHOLD)
        {
            if (stream.NextFloat() >= maximum)
            {
                break;
            }
            weight = weight / maximum;
        }
        weight = weight * ComputeCoatingTransmittance(thickness, directionTangent);
        isAtTop = isAtTop == false;
        if (isAtTop == true)
        {
            const CoatingInterfaceSample reflected =
                SampleCoatingInterface(alpha, eta, -directionTangent, stream.NextThree(), true, COATING_REFLECTION);
            if (reflected.HasSample == false)
            {
                break;
            }
            weight = weight * (reflected.Value * math::Abs(reflected.DirectionTangent.Z) / reflected.Pdf);
            directionTangent = reflected.DirectionTangent;
        }
        else
        {
            const float32 reverseBasePdf = math::Abs(exit.DirectionTangent.Z) / math::kPiF;
            const float32 connectionWeight = ComputeCoatingPowerWeight(exit.Pdf, reverseBasePdf);
            result =
                result + math::Hadamard(weight, bsdf.BaseColor) *
                             (reverseBasePdf * connectionWeight *
                              ComputeCoatingTransmittance(thickness, exit.DirectionTangent) * exit.Value / exit.Pdf);
            directionTangent = SampleCoatingBase(stream.NextThree());
            weight = math::Hadamard(weight, bsdf.BaseColor);
            float32 exitPdf = 0;
            const float32 exitValue = ComputeCoatingInterface(alpha,
                                                              eta,
                                                              -directionTangent,
                                                              lightTangent,
                                                              true,
                                                              COATING_TRANSMISSION,
                                                              exitPdf);
            if (exitValue > 0)
            {
                const float32 connectionPdf = directionTangent.Z / math::kPiF;
                result = result + weight * (ComputeCoatingTransmittance(thickness, directionTangent) * exitValue *
                                            ComputeCoatingPowerWeight(connectionPdf, exitPdf));
            }
        }
    }
    return result;
}
} // namespace
bool IsReferenceCoatingSupported(const content::scene_format::MaterialScattering& scattering) noexcept
{
    return scattering.Model != content::scene_format::MaterialModel::CoatedDiffuse ||
           (scattering.AlphaU == scattering.AlphaV && scattering.AlphaU >= LUDUS_COATING_MIN_ALPHA &&
            scattering.CoatingEta >= 1);
}
bool IsCoatingRandomContextValid(ReferenceBsdfRandomContext random) noexcept
{
    return random.Address.Dimension <= COATING_MAX_EVENT;
}
ReferenceBsdfEvaluation EvaluateCoatedDiffuse(const ReferenceBsdf& bsdf,
                                              math::Vector3 normalWorld,
                                              math::Vector3 viewWorld,
                                              math::Vector3 lightWorld,
                                              ReferenceBsdfRandomContext random) noexcept
{
    const CoatingFrame frame = MakeCoatingFrame(normalWorld);
    const math::Vector3 viewTangent = TransformCoatingToTangent(frame, viewWorld);
    const math::Vector3 lightTangent = TransformCoatingToTangent(frame, lightWorld);
    if (viewTangent.Z <= 0 || lightTangent.Z <= 0)
    {
        return {};
    }
    if (bsdf.Scattering.CoatingEta == 1)
    {
        return {bsdf.BaseColor *
                    (ComputeCoatingTransmittance(bsdf.Scattering.CoatingThickness, viewTangent) *
                     ComputeCoatingTransmittance(bsdf.Scattering.CoatingThickness, lightTangent) / math::kPiF),
                lightTangent.Z / math::kPiF};
    }
    return {EvaluateCoatingValue(bsdf, viewTangent, lightTangent, random),
            EvaluateCoatingPdf(bsdf, viewTangent, lightTangent, random)};
}
ReferenceBsdfSample SampleCoatedDiffuse(const ReferenceBsdf& bsdf,
                                        math::Vector3 normalWorld,
                                        math::Vector3 viewWorld,
                                        math::Vector3 samples,
                                        ReferenceBsdfRandomContext random) noexcept
{
    ReferenceBsdfSample result;
    const CoatingFrame frame = MakeCoatingFrame(normalWorld);
    const math::Vector3 viewTangent = TransformCoatingToTangent(frame, viewWorld);
    if (viewTangent.Z <= 0)
    {
        return result;
    }
    const float32 alpha = bsdf.Scattering.AlphaU;
    const float32 eta = bsdf.Scattering.CoatingEta;
    const float32 thickness = bsdf.Scattering.CoatingThickness;
    if (eta == 1)
    {
        const math::Vector3 directionTangent = SampleCoatingBase(samples);
        result.Direction = TransformCoatingToWorld(frame, directionTangent);
        result.Evaluation = EvaluateCoatedDiffuse(bsdf, normalWorld, viewWorld, result.Direction, random);
        result.Weight = bsdf.BaseColor * (ComputeCoatingTransmittance(thickness, viewTangent) *
                                          ComputeCoatingTransmittance(thickness, directionTangent));
        result.MisPdf = result.Evaluation.Pdf;
        result.Accepted = result.MisPdf > 0;
        return result;
    }
    CoatingRandomStream stream(random, COATING_SAMPLE_DOMAIN);
    CoatingInterfaceSample sampled = SampleCoatingInterface(alpha, eta, viewTangent, samples, true, COATING_ALL);
    if (sampled.HasSample == false)
    {
        return result;
    }
    const float32 entranceWeight = sampled.Value * math::Abs(sampled.DirectionTangent.Z) / sampled.Pdf;
    math::Vector3 weight{entranceWeight, entranceWeight, entranceWeight};
    math::Vector3 directionTangent = sampled.DirectionTangent;
    bool hasExited = sampled.IsTransmission == false;
    bool isAtTop = true;
    for (uint32 depth = 0; hasExited == false && depth < LUDUS_COATING_MAX_DEPTH; ++depth)
    {
        const float32 maximum = GetCoatingMaximum(weight);
        if (maximum == 0 || directionTangent.Z == 0)
        {
            return result;
        }
        if (depth > COATING_ROULETTE_START && maximum < COATING_ROULETTE_THRESHOLD)
        {
            if (stream.NextFloat() >= maximum)
            {
                return result;
            }
            weight = weight / maximum;
        }
        weight = weight * ComputeCoatingTransmittance(thickness, directionTangent);
        isAtTop = isAtTop == false;
        if (isAtTop == false)
        {
            directionTangent = SampleCoatingBase(stream.NextThree());
            weight = math::Hadamard(weight, bsdf.BaseColor);
        }
        else
        {
            sampled = SampleCoatingInterface(alpha, eta, -directionTangent, stream.NextThree(), true, COATING_ALL);
            if (sampled.HasSample == false)
            {
                return result;
            }
            weight = weight * (sampled.Value * math::Abs(sampled.DirectionTangent.Z) / sampled.Pdf);
            directionTangent = sampled.DirectionTangent;
            hasExited = sampled.IsTransmission;
        }
    }
    if (hasExited == true && directionTangent.Z > 0)
    {
        result.Direction = TransformCoatingToWorld(frame, directionTangent);
        result.Weight = weight;
        result.Evaluation = {weight / math::Dot(normalWorld, result.Direction), 1};
        result.MisPdf = EvaluateCoatingPdf(bsdf, viewTangent, directionTangent, random);
        result.HasProportionalPdf = true;
        result.Accepted = true;
    }
    return result;
}
} // namespace ludus::graphics::renderer::internal
