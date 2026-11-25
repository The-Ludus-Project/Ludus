#include "raster_bindings.slang"

// The R11 binding-array variants that LUSL cannot express, kept as RHI tests of Slang input
// (config/lusl_migration.json). r11arrays (FragmentMain) indexes four 2D images with a
// non-uniform runtime index; r11runtime (LUDUS_R11C_RUNTIME, ConstantArrayMain) samples a
// runtime-sized array through the RHI's runtime binding slot. The scalar and constant-index
// variants are r11.lusl, r11_constant.lusl and r11_wider.lusl.
struct Settings
{
    float4 Selection;
};
LUDUS_SHADER_BINDING(0) ConstantBuffer<Settings> SettingsBuffer : register(b0);
#if defined(LUDUS_R11C_RUNTIME)
#    include "runtime_bindings.slang"
LUDUS_SHADER_BINDING(LUDUS_RUNTIME_BINDING_ARRAY_SLOT)
Texture2D<float4> Images[] : register(LUDUS_RUNTIME_REGISTER(t, LUDUS_RUNTIME_BINDING_ARRAY_SLOT));
#else
LUDUS_SHADER_BINDING(1) Texture2D<float4> Images[4] : register(t1);
#endif
LUDUS_SHADER_BINDING(2) SamplerState ImageSampler : register(s2);
struct Input
{
    LUDUS_SHADER_LOCATION(0) float2 PositionClip : POSITION;
};
struct Varying
{
    float4 PositionClip : SV_Position;
    float2 Uv : TEXCOORD0;
};
[shader("vertex")] Varying VertexMain(Input input) {
    Varying output;
    output.PositionClip = float4(input.PositionClip, 0, 1);
    output.Uv = input.PositionClip * float2(0.5, -0.5) + 0.5;
    return output;
}[shader("fragment")] float4 FragmentMain(Varying input)
    : SV_Target
{
    uint index = min(uint(input.Uv.x * 4), 3U);
#if defined(LUDUS_R11C_RUNTIME)
    index = BoundRuntimeBindingIndex(index);
#endif
    return Images[NonUniformResourceIndex(index)].SampleLevel(ImageSampler, input.Uv, 0) * SettingsBuffer.Selection;
}
#if defined(LUDUS_R11C_RUNTIME)
[shader("fragment")] float4 ConstantArrayMain(Varying input) : SV_Target
{
    float4 color = input.Uv.x < 0.25   ? Images[BoundRuntimeBindingIndex(0)].SampleLevel(ImageSampler, input.Uv, 0)
                   : input.Uv.x < 0.5  ? Images[BoundRuntimeBindingIndex(1)].SampleLevel(ImageSampler, input.Uv, 0)
                   : input.Uv.x < 0.75 ? Images[BoundRuntimeBindingIndex(2)].SampleLevel(ImageSampler, input.Uv, 0)
                                       : Images[BoundRuntimeBindingIndex(3)].SampleLevel(ImageSampler, input.Uv, 0);
    return color * SettingsBuffer.Selection;
}
#endif
