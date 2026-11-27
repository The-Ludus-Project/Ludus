// Texture and sampler bindings of the LUSL lit kernels (renderer_lit.lusl, LUSL L5d wave 2), which renderer_lit.lusl
// reaches through `extern slang`. They keep renderer_lit.slang's manual slots, which the renderer validates and binds
// by number (Renderer::prewarmProgram, lighting_layout.h). A slot's resource depends on the configuration (slot 7 is
// the clustered surface texture, the cluster sampler or the forward environment texture), and the realtime forward
// profile omits the bake texture at slot 3 (BAKE_TEXTURE_BINDING) while the later slots keep their numbers. LUSL
// numbers bindings in declaration order and can express neither. renderer_lit.lusl declares the material constant
// buffer at slot 0. The lowered module defines every configuration macro, 0 or 1, before including this file.
#pragma once

#include "raster_bindings.slang"

LUDUS_SHADER_BINDING(1) Texture2D<float4> colorTexture : register(t1);
LUDUS_SHADER_BINDING(2) SamplerState colorSampler : register(s2);
#if LUDUS_GRAPH_REALTIME == 0
LUDUS_SHADER_BINDING(3) Texture2D<float4> bakeTexture : register(t3);
#endif
LUDUS_SHADER_BINDING(4) Texture2D<float4> shadowTexture : register(t4);
LUDUS_SHADER_BINDING(5) SamplerState shadowSampler : register(s5);
#if LUDUS_CLUSTERED != 0
LUDUS_SHADER_BINDING(6) Texture2D<float4> clusterTexture : register(t6);
#    if LUDUS_SURFACE_FIDELITY != 0
LUDUS_SHADER_BINDING(7) Texture2D<float4> surfaceTexture : register(t7);
#    else
LUDUS_SHADER_BINDING(7) SamplerState clusterSampler : register(s7);
#    endif
#elif LUDUS_SURFACE_FIDELITY != 0
LUDUS_SHADER_BINDING(6) Texture2D<float4> surfaceTexture : register(t6);
#endif
#if LUDUS_AUTHORED_INPUTS != 0
#    if LUDUS_CLUSTERED != 0
LUDUS_SHADER_BINDING(8) Texture2D<float4> environmentTexture : register(t8);
LUDUS_SHADER_BINDING(9) SamplerState environmentSampler : register(s9);
LUDUS_SHADER_BINDING(10) Texture2D<float4> emissionTexture : register(t10);
#    else
LUDUS_SHADER_BINDING(7) Texture2D<float4> environmentTexture : register(t7);
LUDUS_SHADER_BINDING(8) SamplerState environmentSampler : register(s8);
LUDUS_SHADER_BINDING(9) Texture2D<float4> emissionTexture : register(t9);
#    endif
#endif
