#pragma once

// Slang translates these explicit descriptor slots into SPIR-V, Metal, WGSL
// and GLES interfaces. "vk" is Slang's slot annotation, not a Vulkan runtime
// branch. Keep target-specific interface vocabulary in this RHI-owned boundary.
#define LUDUS_SHADER_BINDING(slot) [[vk::binding(slot, 0)]]
#define LUDUS_SHADER_LOCATION(slot) [[vk::location(slot)]]

// Vertex index of a non-indexed draw. Slang lowers SV_VulkanVertexID to SPIR-V
// VertexIndex, WGSL vertex_index and GLSL ES gl_VertexID without base-vertex draw
// parameters, but passes it to HLSL unchanged, so the DXIL build (LUDUS_DXIL, set
// by ludus_compile_shader) uses SV_VertexID. Both count from the draw's first
// vertex, which the fullscreen compatibility draw fixes at zero.
#if defined(LUDUS_DXIL)
#    define LUDUS_SHADER_VERTEX_ID SV_VertexID
#else
#    define LUDUS_SHADER_VERTEX_ID SV_VulkanVertexID
#endif

// Native Metal reflection follows HLSL registers; portable targets follow the
// descriptor annotation above. Expand one owned numeric slot into its register
// spelling so these two interfaces cannot acquire independent slot literals.
#define LUDUS_SHADER_REGISTER_TOKEN_INNER(prefix, slot) prefix##slot
#define LUDUS_SHADER_REGISTER_TOKEN(prefix, slot) LUDUS_SHADER_REGISTER_TOKEN_INNER(prefix, slot)
#define LUDUS_SHADER_TEXTURE_REGISTER(slot) : register(LUDUS_SHADER_REGISTER_TOKEN(t, slot))
#define LUDUS_SHADER_UNIFORM_REGISTER(slot) : register(LUDUS_SHADER_REGISTER_TOKEN(b, slot))
#define LUDUS_SHADER_STORAGE_REGISTER(slot) : register(LUDUS_SHADER_REGISTER_TOKEN(u, slot))
