#include "raster_bindings.slang"

struct Camera
{
    // Clip-space scale, LOD in the original allocation, then view base mip.
    float4 Projection;
};

LUDUS_SHADER_BINDING(0) ConstantBuffer<Camera> CameraSettings : register(b0);
LUDUS_SHADER_BINDING(1) Texture2D<float4> Image : register(t1);
LUDUS_SHADER_BINDING(2) SamplerState ImageSampler : register(s2);

struct Vertex
{
    LUDUS_SHADER_LOCATION(0) float2 PositionMesh : POSITION;
    LUDUS_SHADER_LOCATION(1) float2 Uv : TEXCOORD0;
};

struct Varying
{
    float4 PositionClip : SV_Position;
    float2 Uv : TEXCOORD0;
};

[shader("vertex")] Varying VertexMain(Vertex vertex) {
    Varying output;
    output.PositionClip = float4(vertex.PositionMesh * CameraSettings.Projection.x, 0, 1);
    output.Uv = vertex.Uv;
    return output;
}

    [shader("fragment")] float4 FragmentMain(Varying input)
    : SV_Target
{
    float lod = CameraSettings.Projection.y - CameraSettings.Projection.z;
    uint width;
    uint height;
    uint mipCount;
    Image.GetDimensions(0, width, height, mipCount);
    static const float MAX_UNORM_CHANNEL = 255.0;
    float2 color = Image.SampleLevel(ImageSampler, input.Uv, lod).rg;
    return float4(color, float(mipCount) / MAX_UNORM_CHANNEL, 1);
}
