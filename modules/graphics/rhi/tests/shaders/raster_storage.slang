#include "raster_bindings.slang"

// R12b portable raster-storage fixture: one source supplies every native target.
// The vertex stage reads positions from read-only storage; the fragment stage writes
// a coverage buffer and a write-only storage texture; compute writes a second texture;
// a later pass samples the fragment-written texture (storage write before sampled read).
#define POSITION_SLOT 0
#define COVERAGE_SLOT 1
#define FRAGMENT_IMAGE_SLOT 2
#define SAMPLED_IMAGE_SLOT 3
#define COMPUTE_IMAGE_SLOT 4

LUDUS_SHADER_BINDING(POSITION_SLOT)
StructuredBuffer<float2> Positions LUDUS_SHADER_TEXTURE_REGISTER(POSITION_SLOT);
LUDUS_SHADER_BINDING(COVERAGE_SLOT)
RWStructuredBuffer<uint> Coverage LUDUS_SHADER_STORAGE_REGISTER(COVERAGE_SLOT);
[format("rgba8")] LUDUS_SHADER_BINDING(FRAGMENT_IMAGE_SLOT) WTexture2D<float4> FragmentImage
    LUDUS_SHADER_STORAGE_REGISTER(FRAGMENT_IMAGE_SLOT);
LUDUS_SHADER_BINDING(SAMPLED_IMAGE_SLOT)
Texture2D<float4> SampledImage LUDUS_SHADER_TEXTURE_REGISTER(SAMPLED_IMAGE_SLOT);
[format("rgba8")] LUDUS_SHADER_BINDING(COMPUTE_IMAGE_SLOT) WTexture2D<float4> ComputeImage
    LUDUS_SHADER_STORAGE_REGISTER(COMPUTE_IMAGE_SLOT);

// Must match the C++ fixture extent and coverage mark.
static const uint IMAGE_WIDTH = 64;
static const uint COVERAGE_MARK = 7;

[shader("vertex")] float4 StorageVertex(uint vertexIndex : LUDUS_SHADER_VERTEX_ID) : SV_Position
{
    return float4(Positions[vertexIndex], 0.5, 1);
}

[shader("fragment")] float4 StorageFragment(float4 positionPixel : SV_Position) : SV_Target0
{
    const uint2 pixel = uint2(positionPixel.xy);
    Coverage[pixel.y * IMAGE_WIDTH + pixel.x] = COVERAGE_MARK;
    FragmentImage.Store(pixel, float4(1, 0, 0, 1));
    return float4(0, 1, 0, 1);
}

[shader("fragment")] float4 SampleFragment(float4 positionPixel : SV_Position) : SV_Target0
{
    return SampledImage.Load(int3(int2(positionPixel.xy), 0));
}

[shader("compute")][numthreads(8, 8, 1)] void StorageCompute(uint3 thread : SV_DispatchThreadID) {
    ComputeImage.Store(thread.xy, float4(0, 0, 1, 1));
}
