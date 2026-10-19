// Texture viewer depth readback: copy every Depth32Float texel's IEEE bits into the four bytes
// of an RGBA8Unorm attachment (least significant byte in red), so the editor's RGBA8 readback
// returns depth exactly. Each byte k is written as k / 255, which unorm conversion rounds back
// to k. See apps/editor/src/internal/render_target_reader.h.
#include "raster_bindings.slang"

LUDUS_SHADER_BINDING(0) Texture2D<float> DepthImage : register(t0);

static const uint BYTE_MASK = 0xFF;
static const uint BITS_PER_BYTE = 8;
static const float UNORM8_MAXIMUM = 255.0;

struct Input
{
    LUDUS_SHADER_LOCATION(0) float4 PositionClip : POSITION;
};
struct Varying
{
    float4 PositionClip : SV_Position;
};

Varying VertexMain(Input value)
{
    Varying result;
    result.PositionClip = value.PositionClip;
    return result;
}

// Load, not Sample: one stored texel per output pixel, without filtering or comparison.
float4 FragmentMain(in float4 positionPixel : SV_Position) : SV_Target
{
    const uint bits = asuint(DepthImage.Load(int3(int2(positionPixel.xy), 0)));
    const uint4 bytes = uint4(bits & BYTE_MASK,
                              (bits >> BITS_PER_BYTE) & BYTE_MASK,
                              (bits >> (BITS_PER_BYTE * 2)) & BYTE_MASK,
                              bits >> (BITS_PER_BYTE * 3));
    return float4(bytes) / UNORM8_MAXIMUM;
}
