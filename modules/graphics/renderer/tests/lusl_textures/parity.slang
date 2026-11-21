#include "raster_bindings.slang"

static const uint IMAGE_COUNT = 2;
static const uint SECOND_IMAGE = IMAGE_COUNT - 1;
static const uint IMAGE_SLOT = 0;
static const uint VOLUME_SLOT = 2;
static const uint CUBE_SLOT = VOLUME_SLOT + 1;
static const uint LAYERS_SLOT = CUBE_SLOT + 1;
static const uint DEPTH_SLOT = LAYERS_SLOT + 1;
static const uint COLOR_SAMPLER_SLOT = DEPTH_SLOT + 1;
static const uint DEPTH_SAMPLER_SLOT = COLOR_SAMPLER_SLOT + 1;
static const float GATHER_WEIGHT = 0.25;
LUDUS_SHADER_BINDING(IMAGE_SLOT) Texture2D<float4> images[IMAGE_COUNT] : register(t0);
LUDUS_SHADER_BINDING(VOLUME_SLOT) Texture3D<float4> volume : register(t2);
LUDUS_SHADER_BINDING(CUBE_SLOT) TextureCube<float4> cube : register(t3);
LUDUS_SHADER_BINDING(LAYERS_SLOT) Texture2DArray<float4> layers : register(t4);
LUDUS_SHADER_BINDING(DEPTH_SLOT) DepthTexture2D depthImage : register(t5);
LUDUS_SHADER_BINDING(COLOR_SAMPLER_SLOT) SamplerState colorSampler : register(s6);
LUDUS_SHADER_BINDING(DEPTH_SAMPLER_SLOT) SamplerComparisonState depthSampler : register(s7);

struct Readback
{
    float4 Samples : SV_Target0;
    float4 Comparison : SV_Target1;
};
[shader("fragment")] Readback Main() {
    float4 gathered = images[SECOND_IMAGE].Gather(colorSampler, float2(0.5));
    float arrayValue = images[0].SampleLevel(colorSampler, float2(0.5), 0).x;
    float volumeValue = volume.SampleLevel(colorSampler, float3(0.5), 0).x;
    float cubeValue = cube.SampleLevel(colorSampler, float3(1, 0, 0), 0).x;
    float layerValue = layers.SampleLevel(colorSampler, float3(0.5, 0.5, 1), 0).x;
    float passes = depthImage.SampleCmp(depthSampler, float2(0.5), 0.5);
    float fails = depthImage.SampleCmp(depthSampler, float2(0.5), 1);
    Readback output;
    output.Samples = float4(arrayValue, volumeValue, cubeValue, layerValue);
    output.Comparison = float4(dot(gathered, float4(GATHER_WEIGHT)), passes, fails, 1);
    return output;
}
