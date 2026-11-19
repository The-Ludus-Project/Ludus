#include "radiance.slang"
// Thanks to Sébastien Lagarde, "Physically Based Material, Where Are We?"
// (SIGGRAPH 2017, pp. 31–35, 67–74), for framing the unresolved-normal experiment.
// The opaque white fixture carries slope moments in its base texture. RGB is sampled
// in linear light. The packed surface texture keeps its usual roughness and metalness.
float4 PSMain(in Varying input)
{
    SurfaceSample surface = SampleMaterial(input);
    // The coordinate basis of the fixture is the authored +U,+V tangent frame.
    float2 meanSlopeTangent = surface.BaseColor.rg - 0.5;
    float secondMoment = surface.BaseColor.b;
    float variance = max(0, secondMoment - dot(meanSlopeTangent, meanSlopeTangent));
    // Worst single-texel sRGB8 rounding discrepancy over the admitted fixture slopes.
    static const float MOMENT_QUANTIZATION_TOLERANCE = 0.003;
    float strength = material.SurfaceMap.x;
    // A constant direction or a disabled normal map uses the decoded material exactly.
    if (variance > MOMENT_QUANTIZATION_TOLERANCE && strength != 0)
    {
        float3 normalWorld = NormalizeOrZero(input.NormalWorld);
        float3 tangentWorld =
            NormalizeOrZero(input.TangentWorld.xyz - normalWorld * dot(input.TangentWorld.xyz, normalWorld));
        float3 bitangentWorld = cross(normalWorld, tangentWorld) * input.TangentWorld.w;
        surface.NormalWorld = NormalizeOrZero(
            normalWorld + strength * (meanSlopeTangent.x * tangentWorld + meanSlopeTangent.y * bitangentWorld));
        float alpha = surface.Roughness * surface.Roughness;
        surface.Roughness = sqrt(sqrt(saturate(alpha * alpha + strength * strength * variance)));
    }
    surface.BaseColor = float4(1);
    return EncodeRadiance(ShadeMaterial(surface, input).r);
}
