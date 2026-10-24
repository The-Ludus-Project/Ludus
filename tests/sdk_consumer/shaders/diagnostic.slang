#include "raster_bindings.slang"
#include "uniforms.slang"
#ifndef DIAGNOSTIC_SCALE
#define DIAGNOSTIC_SCALE 1.0
#endif
[[vk::binding(0, 0)]] ConstantBuffer<DiagnosticUniforms> settings;
[shader("vertex")]
// The RHI vertex ID maps to the plain Vulkan VertexIndex / WGSL vertex_index /
// GLSL ES gl_VertexID without base-vertex (SPV_KHR_shader_draw_parameters),
// which the WebGL 2 (GLSL ES) backend cannot express, and to DXIL SV_VertexID.
// Values are identical for the non-indexed, zero-base-vertex fullscreen draw
// across all backends.
float4 vertexMain(uint id : LUDUS_SHADER_VERTEX_ID) : SV_Position
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}
[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target0
{
#if LUDUS_GLSL_ES
    // WebGL fragment coordinates start at the bottom; our authored scene uses top-left pixels.
    position.y = settings.resolution.y - position.y;
#endif
    float2 uv = position.xy / settings.resolution;
    float2 centered = (position.xy - settings.resolution * 0.5) / settings.resolution.y;
    float circle = dot(centered, centered) < 0.04 ? 1.0 : 0.0;
    return float4(settings.tint.rgb * DIAGNOSTIC_SCALE * float3(uv.x, uv.y, circle) + settings.direction * settings.elapsedTime * 0.05, settings.tint.a);
}
