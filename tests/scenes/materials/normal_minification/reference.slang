#include "radiance.slang"

// Integrate the original periodic texel field over this projected pixel. Always
// use the full-resolution cooked fixture for this graph, including comparisons
// with a budget-reduced candidate. Load preserves the discrete input normals.
float4 PSMain(in Varying input)
{
    static const int SAMPLE_AXIS = 16;
    float2 uv = input.TexcoordUv;
    float2 dx = ddx(uv);
    float2 dy = ddy(uv);
    uint width;
    uint height;
    surfaceTexture.GetDimensions(width, height);
    float3 normalWorld = NormalizeOrZero(input.NormalWorld);
    float3 tangentWorld = NormalizeOrZero(input.TangentWorld.xyz);
    float3 bitangentWorld = cross(normalWorld, tangentWorld) * input.TangentWorld.w;
    SurfaceSample surface = SampleMaterial(input);
    surface.BaseColor = float4(1);
    float3 dxWorld = ddx(input.PositionWorld);
    float3 dyWorld = ddy(input.PositionWorld);
    float radiance = 0;
    for (int y = 0; y < SAMPLE_AXIS; ++y)
    {
        for (int x = 0; x < SAMPLE_AXIS; ++x)
        {
            float2 offset = (float2(x, y) + 0.5) / float(SAMPLE_AXIS) - 0.5;
            int2 texel = int2(frac(uv + offset.x * dx + offset.y * dy) * float2(width, height));
            float4 packed = surfaceTexture.Load(int3(texel, 0));
            SurfaceParameters resolved =
                DecodeSurfaceParameters(packed, material.Pbr.x, material.Pbr.y, material.SurfaceMap.x);
            float3 mappedWorld =
                NormalizeOrZero(tangentWorld * resolved.NormalTangent.x + bitangentWorld * resolved.NormalTangent.y +
                                normalWorld * resolved.NormalTangent.z);
            surface.NormalWorld = mappedWorld;
            surface.Metallic = resolved.Metallic;
            surface.Roughness = resolved.Roughness;
            surface.ViewDirectionWorld =
                NormalizeOrZero(input.EyeWorld.xyz - (input.PositionWorld + dxWorld * offset.x + dyWorld * offset.y));
            radiance += ShadeMaterial(surface, input).r;
        }
    }
    return EncodeRadiance(radiance / float(SAMPLE_AXIS * SAMPLE_AXIS));
}
