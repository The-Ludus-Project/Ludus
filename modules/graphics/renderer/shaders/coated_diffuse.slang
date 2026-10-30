#pragma once
#include "coated_diffuse_layout.h"
#include "material.h"
#include "math.slang"

// Copyright 1998-2020 Matt Pharr, Wenzel Jakob and Greg Humphreys.
// SPDX-License-Identifier: Apache-2.0
// Thanks to the authors of Physically Based Rendering, 4th ed., sections 9.7 and 14.3.
// Adapted from pbrt-v4 b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f; the same
// non-scattering isotropic specialization as reference_coating.cpp. Philox domains
// and normalized path weights are Ludus choices. License: third_party/pbrt/LICENSE.txt.
struct CoatingRandomContext
{
    uint2 Key;
    RandomAddress Address;
};
struct CoatingMaterial
{
    float3 BaseColor;
    float Alpha;
    float Eta;
    float Thickness;
};
struct CoatingEvaluation
{
    float3 Value;
    float Pdf;
};
struct CoatingSample
{
    float3 DirectionWorld;
    float3 Weight;
    float MisPdf;
    bool Accepted;
};
float3 NormalizeCoatingVector(float3 value)
{
    float squaredLength = dot(value, value);
    return squaredLength > 0 ? value / sqrt(squaredLength) : float3(0);
}
struct CoatingFrame
{
    float3 AxisXWorld;
    float3 AxisYWorld;
    float3 NormalWorld;
};
CoatingFrame MakeCoatingFrame(float3 normalWorld)
{
    float3 axisXWorld = NormalizeCoatingVector(
        cross(abs(normalWorld.z) < COATING_BASIS_SWITCH_COSINE ? float3(0, 0, 1) : float3(0, 1, 0), normalWorld));
    CoatingFrame result;
    result.AxisXWorld = axisXWorld;
    result.AxisYWorld = cross(normalWorld, axisXWorld);
    result.NormalWorld = normalWorld;
    return result;
}
float3 TransformCoatingToTangent(CoatingFrame frame, float3 directionWorld)
{
    return float3(dot(directionWorld, frame.AxisXWorld),
                  dot(directionWorld, frame.AxisYWorld),
                  dot(directionWorld, frame.NormalWorld));
}
float3 TransformCoatingToWorld(CoatingFrame frame, float3 directionTangent)
{
    return NormalizeCoatingVector(frame.AxisXWorld * directionTangent.x + frame.AxisYWorld * directionTangent.y +
                                  frame.NormalWorld * directionTangent.z);
}
struct CoatingRandomStream
{
    uint2 Key;
    RandomAddress Address;
};
CoatingRandomStream MakeCoatingRandomStream(CoatingRandomContext random, uint domain)
{
    CoatingRandomStream result;
    result.Key = uint2(random.Key.x, random.Key.y ^ domain);
    result.Address = random.Address;
    result.Address.Dimension *= COATING_DIMENSIONS_PER_EVENT;
    return result;
}
float NextCoatingFloat(inout CoatingRandomStream stream)
{
    float result = SampleRandomFloat01(stream.Key, stream.Address);
    ++stream.Address.Dimension;
    return result;
}
float3 NextCoatingThree(inout CoatingRandomStream stream)
{
    float first = NextCoatingFloat(stream);
    float second = NextCoatingFloat(stream);
    float third = NextCoatingFloat(stream);
    return float3(first, second, third);
}
float ComputeCoatingFresnel(float cosine, float eta)
{
    cosine = clamp(cosine, -1.0, 1.0);
    if (cosine < 0)
    {
        eta = 1 / eta;
        cosine = -cosine;
    }
    float transmittedSineSquared = (1 - cosine * cosine) / (eta * eta);
    if (transmittedSineSquared >= 1)
    {
        return 1;
    }
    float transmittedCosine = sqrt(1 - transmittedSineSquared);
    float parallel = (eta * cosine - transmittedCosine) / (eta * cosine + transmittedCosine);
    float perpendicular = (cosine - eta * transmittedCosine) / (cosine + eta * transmittedCosine);
    return (parallel * parallel + perpendicular * perpendicular) * 0.5;
}
float ComputeCoatingDistribution(float alpha, float3 halfTangent)
{
    float cosineSquared = halfTangent.z * halfTangent.z;
    if (cosineSquared * cosineSquared < COATING_MIN_NORMAL_COSINE_FOURTH)
        return 0;
    float alphaSquared = alpha * alpha;
    float denominator =
        halfTangent.x * halfTangent.x + halfTangent.y * halfTangent.y + alphaSquared * halfTangent.z * halfTangent.z;
    return alphaSquared / (PI * denominator * denominator);
}
float ComputeCoatingProjectedLength(float alpha, float3 directionTangent)
{
    return sqrt(directionTangent.z * directionTangent.z +
                alpha * alpha * (directionTangent.x * directionTangent.x + directionTangent.y * directionTangent.y));
}
float ComputeCoatingGeometry(float alpha, float3 viewTangent, float3 lightTangent)
{
    float viewCosine = abs(viewTangent.z);
    float lightCosine = abs(lightTangent.z);
    return 2 * viewCosine * lightCosine /
           (ComputeCoatingProjectedLength(alpha, viewTangent) * lightCosine +
            ComputeCoatingProjectedLength(alpha, lightTangent) * viewCosine);
}
float ComputeCoatingNormalPdf(float alpha, float3 viewTangent, float3 halfTangent)
{
    return 2 * ComputeCoatingDistribution(alpha, halfTangent) * abs(dot(viewTangent, halfTangent)) /
           (abs(viewTangent.z) + ComputeCoatingProjectedLength(alpha, viewTangent));
}
float3 SampleCoatingNormal(float alpha, float3 viewTangent, float2 samples)
{
    float3 stretchedTangent =
        NormalizeCoatingVector(float3(alpha * viewTangent.x, alpha * viewTangent.y, viewTangent.z));
    if (stretchedTangent.z < 0)
    {
        stretchedTangent = -stretchedTangent;
    }
    float3 firstTangent = stretchedTangent.z < COATING_BASIS_SWITCH_COSINE
                              ? NormalizeCoatingVector(cross(float3(0, 0, 1), stretchedTangent))
                              : float3(1, 0, 0);
    float3 secondTangent = cross(stretchedTangent, firstTangent);
    float radius = sqrt(samples.x);
    float angle = (2 * PI) * samples.y;
    float diskX = radius * cos(angle);
    float diskY = radius * sin(angle);
    float blend = (1 + stretchedTangent.z) * 0.5;
    float warpedY = (1 - blend) * sqrt(max(0.0, 1 - diskX * diskX)) + blend * diskY;
    float projectedZ = sqrt(max(0.0, 1 - diskX * diskX - warpedY * warpedY));
    float3 projectedTangent = firstTangent * diskX + secondTangent * warpedY + stretchedTangent * projectedZ;
    return NormalizeCoatingVector(
        float3(alpha * projectedTangent.x, alpha * projectedTangent.y, max(COATING_MIN_NORMAL_Z, projectedTangent.z)));
}
float ComputeCoatingInterfaceWithNormal(float alpha,
                                        float eta,
                                        float3 viewTangent,
                                        float3 lightTangent,
                                        float3 halfTangent,
                                        bool isRadiance,
                                        uint choices,
                                        out float outPdf)
{
    outPdf = 0;
    if (viewTangent.z == 0 || lightTangent.z == 0)
    {
        return 0;
    }
    bool isReflection = viewTangent.z * lightTangent.z > 0;
    float relativeEta = isReflection == true ? 1 : (viewTangent.z > 0 ? eta : 1 / eta);
    if (halfTangent.z < 0)
    {
        halfTangent = -halfTangent;
    }
    float viewDotHalf = dot(viewTangent, halfTangent);
    float lightDotHalf = dot(lightTangent, halfTangent);
    if (viewDotHalf * viewTangent.z < 0 || lightDotHalf * lightTangent.z < 0 || halfTangent.z == 0)
    {
        return 0;
    }
    float reflectance = ComputeCoatingFresnel(viewDotHalf, eta);
    float reflectionProbability = (choices & COATING_REFLECTION) != 0 ? reflectance : 0;
    float transmissionProbability = (choices & COATING_TRANSMISSION) != 0 ? 1 - reflectance : 0;
    float totalProbability = reflectionProbability + transmissionProbability;
    if (totalProbability == 0)
    {
        return 0;
    }
    float distribution = ComputeCoatingDistribution(alpha, halfTangent);
    float geometry = ComputeCoatingGeometry(alpha, viewTangent, lightTangent);
    float normalPdf = ComputeCoatingNormalPdf(alpha, viewTangent, halfTangent);
    if (isReflection == true)
    {
        outPdf =
            normalPdf * reflectionProbability / (COATING_REFLECTION_JACOBIAN * abs(viewDotHalf) * totalProbability);
        return distribution * geometry * reflectance /
               abs(COATING_REFLECTION_JACOBIAN * viewTangent.z * lightTangent.z);
    }
    float halfSum = lightDotHalf + viewDotHalf / relativeEta;
    float denominator = halfSum * halfSum;
    if (denominator == 0)
    {
        return 0;
    }
    outPdf = normalPdf * abs(lightDotHalf) / denominator * transmissionProbability / totalProbability;
    float value = (1 - reflectance) * distribution * geometry *
                  abs(viewDotHalf * lightDotHalf / (viewTangent.z * lightTangent.z * denominator));
    return isRadiance == true ? value / (relativeEta * relativeEta) : value;
}
float ComputeCoatingInterface(float alpha,
                              float eta,
                              float3 viewTangent,
                              float3 lightTangent,
                              bool isRadiance,
                              uint choices,
                              out float outPdf)
{
    bool isReflection = viewTangent.z * lightTangent.z > 0;
    float relativeEta = isReflection == true ? 1 : (viewTangent.z > 0 ? eta : 1 / eta);
    float3 halfTangent = NormalizeCoatingVector(viewTangent + lightTangent * relativeEta);
    return ComputeCoatingInterfaceWithNormal(alpha,
                                             eta,
                                             viewTangent,
                                             lightTangent,
                                             halfTangent,
                                             isRadiance,
                                             choices,
                                             outPdf);
}
struct CoatingInterfaceSample
{
    float3 DirectionTangent;
    float Value;
    float Pdf;
    bool IsTransmission;
    bool HasSample;
};
CoatingInterfaceSample
SampleCoatingInterface(float alpha, float eta, float3 viewTangent, float3 samples, bool isRadiance, uint choices)
{
    CoatingInterfaceSample result = (CoatingInterfaceSample)0;
    if (viewTangent.z == 0)
    {
        return result;
    }
    float3 halfTangent = SampleCoatingNormal(alpha, viewTangent, float2(samples.y, samples.z));
    float3 distributionNormalTangent = halfTangent;
    float reflectance = ComputeCoatingFresnel(dot(viewTangent, halfTangent), eta);
    float reflectionProbability = (choices & COATING_REFLECTION) != 0 ? reflectance : 0;
    float transmissionProbability = (choices & COATING_TRANSMISSION) != 0 ? 1 - reflectance : 0;
    if (reflectionProbability + transmissionProbability == 0)
    {
        return result;
    }
    bool isReflection = samples.x < reflectionProbability / (reflectionProbability + transmissionProbability);
    if (isReflection == true)
    {
        result.DirectionTangent = halfTangent * (2 * dot(viewTangent, halfTangent)) - viewTangent;
        if (result.DirectionTangent.z * viewTangent.z <= 0)
        {
            return result;
        }
    }
    else
    {
        float cosine = dot(viewTangent, halfTangent);
        float relativeEta = eta;
        if (cosine < 0)
        {
            relativeEta = 1 / relativeEta;
            halfTangent = -halfTangent;
            cosine = -cosine;
        }
        float sineSquared = max(0.0, 1 - cosine * cosine) / (relativeEta * relativeEta);
        if (sineSquared >= 1)
        {
            return result;
        }
        result.DirectionTangent =
            -viewTangent / relativeEta + halfTangent * (cosine / relativeEta - sqrt(1 - sineSquared));
        if (result.DirectionTangent.z * viewTangent.z >= 0)
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
    result.HasSample = result.Value > 0 && result.Pdf > 0 && result.DirectionTangent.z != 0;
    return result;
}
float3 SampleCoatingBase(float3 samples)
{
    float radius = sqrt(samples.y);
    float angle = (2 * PI) * samples.z;
    return float3(radius * cos(angle), radius * sin(angle), sqrt(max(0.0, 1 - samples.y)));
}
float ComputeCoatingTransmittance(float thickness, float3 directionTangent)
{
    return directionTangent.z != 0 ? exp(-abs(thickness / directionTangent.z)) : 0;
}
float ComputeCoatingPowerWeight(float first, float second)
{
    return first * first / (first * first + second * second);
}
float GetCoatingMaximum(float3 value)
{
    return max(value.x, max(value.y, value.z));
}
float EvaluateCoatingPdf(CoatingMaterial bsdf, float3 viewTangent, float3 lightTangent, CoatingRandomContext random)
{
    CoatingRandomStream stream = MakeCoatingRandomStream(random, COATING_PDF_DOMAIN);
    float alpha = bsdf.Alpha;
    float eta = bsdf.Eta;
    float sum = 0;
    (ComputeCoatingInterface(alpha, eta, viewTangent, lightTangent, true, COATING_REFLECTION, sum));
    CoatingInterfaceSample entrance =
        SampleCoatingInterface(alpha, eta, viewTangent, NextCoatingThree(stream), true, COATING_TRANSMISSION);
    CoatingInterfaceSample exit =
        SampleCoatingInterface(alpha, eta, lightTangent, NextCoatingThree(stream), false, COATING_TRANSMISSION);
    if (entrance.HasSample == true && exit.HasSample == true && GetCoatingMaximum(bsdf.BaseColor) > 0)
    {
        float3 baseDirectionTangent = SampleCoatingBase(NextCoatingThree(stream));
        float basePdf = baseDirectionTangent.z / PI;
        float reverseBasePdf = abs(exit.DirectionTangent.z) / PI;
        sum += ComputeCoatingPowerWeight(exit.Pdf, reverseBasePdf) * reverseBasePdf;
        float exitPdf = 0;
        (ComputeCoatingInterface(alpha, eta, -baseDirectionTangent, lightTangent, true, COATING_TRANSMISSION, exitPdf));
        sum += ComputeCoatingPowerWeight(basePdf, exitPdf) * exitPdf;
    }
    return COATING_PDF_ESTIMATE_WEIGHT * sum + (1 - COATING_PDF_ESTIMATE_WEIGHT) / (COATING_REFLECTION_JACOBIAN * PI);
}
float3 EvaluateCoatingValue(CoatingMaterial bsdf, float3 viewTangent, float3 lightTangent, CoatingRandomContext random)
{
    CoatingRandomStream stream = MakeCoatingRandomStream(random, COATING_EVALUATION_DOMAIN);
    float alpha = bsdf.Alpha;
    float eta = bsdf.Eta;
    float thickness = bsdf.Thickness;
    float unusedPdf = 0;
    float reflection = ComputeCoatingInterface(alpha, eta, viewTangent, lightTangent, true, COATING_ALL, unusedPdf);
    float3 result = float3(reflection, reflection, reflection);
    CoatingInterfaceSample entrance =
        SampleCoatingInterface(alpha, eta, viewTangent, NextCoatingThree(stream), true, COATING_TRANSMISSION);
    CoatingInterfaceSample exit =
        SampleCoatingInterface(alpha, eta, lightTangent, NextCoatingThree(stream), false, COATING_TRANSMISSION);
    if (entrance.HasSample == false || exit.HasSample == false)
    {
        return result;
    }
    float entranceWeight = entrance.Value * abs(entrance.DirectionTangent.z) / entrance.Pdf;
    float3 weight = float3(entranceWeight, entranceWeight, entranceWeight);
    float3 directionTangent = entrance.DirectionTangent;
    bool isAtTop = true;
    for (uint depth = 0; depth < LUDUS_COATING_MAX_DEPTH; ++depth)
    {
        float maximum = GetCoatingMaximum(weight);
        if (maximum == 0)
        {
            break;
        }
        if (depth > COATING_ROULETTE_START && maximum < COATING_ROULETTE_THRESHOLD)
        {
            if (NextCoatingFloat(stream) >= maximum)
            {
                break;
            }
            weight = weight / maximum;
        }
        weight = weight * ComputeCoatingTransmittance(thickness, directionTangent);
        isAtTop = isAtTop == false;
        if (isAtTop == true)
        {
            CoatingInterfaceSample reflected = SampleCoatingInterface(alpha,
                                                                      eta,
                                                                      -directionTangent,
                                                                      NextCoatingThree(stream),
                                                                      true,
                                                                      COATING_REFLECTION);
            if (reflected.HasSample == false)
            {
                break;
            }
            weight = weight * (reflected.Value * abs(reflected.DirectionTangent.z) / reflected.Pdf);
            directionTangent = reflected.DirectionTangent;
        }
        else
        {
            float reverseBasePdf = abs(exit.DirectionTangent.z) / PI;
            float connectionWeight = ComputeCoatingPowerWeight(exit.Pdf, reverseBasePdf);
            result =
                result + (weight * bsdf.BaseColor) *
                             (reverseBasePdf * connectionWeight *
                              ComputeCoatingTransmittance(thickness, exit.DirectionTangent) * exit.Value / exit.Pdf);
            directionTangent = SampleCoatingBase(NextCoatingThree(stream));
            weight = (weight * bsdf.BaseColor);
            float exitPdf = 0;
            float exitValue = ComputeCoatingInterface(alpha,
                                                      eta,
                                                      -directionTangent,
                                                      lightTangent,
                                                      true,
                                                      COATING_TRANSMISSION,
                                                      exitPdf);
            if (exitValue > 0)
            {
                float connectionPdf = directionTangent.z / PI;
                result = result + weight * (ComputeCoatingTransmittance(thickness, directionTangent) * exitValue *
                                            ComputeCoatingPowerWeight(connectionPdf, exitPdf));
            }
        }
    }
    return result;
}

CoatingEvaluation EvaluateCoatedDiffuse(CoatingMaterial bsdf,
                                        float3 normalWorld,
                                        float3 viewWorld,
                                        float3 lightWorld,
                                        CoatingRandomContext random)
{
    CoatingEvaluation result = (CoatingEvaluation)0;
    if (random.Address.Dimension > COATING_MAX_EVENT)
        return result;
    CoatingFrame frame = MakeCoatingFrame(normalWorld);
    float3 viewTangent = TransformCoatingToTangent(frame, viewWorld);
    float3 lightTangent = TransformCoatingToTangent(frame, lightWorld);
    if (viewTangent.z <= 0 || lightTangent.z <= 0)
        return result;
    if (bsdf.Eta == 1)
    {
        result.Value = bsdf.BaseColor * (ComputeCoatingTransmittance(bsdf.Thickness, viewTangent) *
                                         ComputeCoatingTransmittance(bsdf.Thickness, lightTangent) / PI);
        result.Pdf = lightTangent.z / PI;
        return result;
    }
    result.Value = EvaluateCoatingValue(bsdf, viewTangent, lightTangent, random);
    result.Pdf = EvaluateCoatingPdf(bsdf, viewTangent, lightTangent, random);
    return result;
}
CoatingSample SampleCoatedDiffuse(CoatingMaterial bsdf,
                                  float3 normalWorld,
                                  float3 viewWorld,
                                  float3 samples,
                                  CoatingRandomContext random)
{
    CoatingSample result = (CoatingSample)0;
    if (random.Address.Dimension > COATING_MAX_EVENT)
        return result;
    CoatingFrame frame = MakeCoatingFrame(normalWorld);
    float3 viewTangent = TransformCoatingToTangent(frame, viewWorld);
    if (viewTangent.z <= 0)
        return result;
    float alpha = bsdf.Alpha;
    float eta = bsdf.Eta;
    float thickness = bsdf.Thickness;
    if (eta == 1)
    {
        float3 directionTangent = SampleCoatingBase(samples);
        result.DirectionWorld = TransformCoatingToWorld(frame, directionTangent);
        result.Weight = bsdf.BaseColor * (ComputeCoatingTransmittance(thickness, viewTangent) *
                                          ComputeCoatingTransmittance(thickness, directionTangent));
        result.MisPdf = directionTangent.z / PI;
        result.Accepted = result.MisPdf > 0;
        return result;
    }
    CoatingRandomStream stream = MakeCoatingRandomStream(random, COATING_SAMPLE_DOMAIN);
    CoatingInterfaceSample sampled = SampleCoatingInterface(alpha, eta, viewTangent, samples, true, COATING_ALL);
    if (sampled.HasSample == false)
        return result;
    float entranceWeight = sampled.Value * abs(sampled.DirectionTangent.z) / sampled.Pdf;
    float3 weight = float3(entranceWeight);
    float3 directionTangent = sampled.DirectionTangent;
    bool hasExited = sampled.IsTransmission == false;
    bool isAtTop = true;
    for (uint depth = 0; hasExited == false && depth < LUDUS_COATING_MAX_DEPTH; ++depth)
    {
        float maximum = GetCoatingMaximum(weight);
        if (maximum == 0 || directionTangent.z == 0)
            return result;
        if (depth > COATING_ROULETTE_START && maximum < COATING_ROULETTE_THRESHOLD)
        {
            if (NextCoatingFloat(stream) >= maximum)
                return result;
            weight = weight / maximum;
        }
        weight *= ComputeCoatingTransmittance(thickness, directionTangent);
        isAtTop = isAtTop == false;
        if (isAtTop == false)
        {
            directionTangent = SampleCoatingBase(NextCoatingThree(stream));
            weight *= bsdf.BaseColor;
        }
        else
        {
            sampled =
                SampleCoatingInterface(alpha, eta, -directionTangent, NextCoatingThree(stream), true, COATING_ALL);
            if (sampled.HasSample == false)
                return result;
            weight *= sampled.Value * abs(sampled.DirectionTangent.z) / sampled.Pdf;
            directionTangent = sampled.DirectionTangent;
            hasExited = sampled.IsTransmission;
        }
    }
    if (hasExited == true && directionTangent.z > 0)
    {
        result.DirectionWorld = TransformCoatingToWorld(frame, directionTangent);
        result.Weight = weight;
        result.MisPdf = EvaluateCoatingPdf(bsdf, viewTangent, directionTangent, random);
        result.Accepted = true;
    }
    return result;
}
