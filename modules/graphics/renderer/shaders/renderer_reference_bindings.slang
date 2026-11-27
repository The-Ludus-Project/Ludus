// Bindings and Slang libraries of the LUSL reference kernels (renderer_reference.lusl and its diagnostic and
// optical variants, LUSL L5d wave 2). They keep renderer_reference.slang's manual layout
// (renderer_reference_layout.h), which reference_compute.cpp binds: the shared libraries below read the Parameters
// fields as Slang globals (ray_bvh_traversal.slang reads Counts and Offsets, reference_medium.slang Tile,
// reference_light_sampling.slang Sampling), and ltc_table.slang and ggx_albedo_table.slang declare their table at a
// slot defined before their inclusion. Neither can be expressed by LUSL bindings until wave 5 ports these libraries.
// The kernels reach this file through `extern slang` (renderer_reference_bindings.luslh); the libraries call the
// kernels' SceneValue and IsBvhCandidateAccepted, which Slang resolves in any order.
#pragma once

#include "coated_diffuse.slang"
#include "dielectric.slang"
#include "lighting_layout.h"
#include "material_scattering.slang"
#include "math.slang"
#include "raster_bindings.slang"
#include "renderer_atmosphere_celestial.slang"
#include "renderer_gpu_preview_layout.h"
#include "renderer_reference_layout.h"
#include "renderer_reference_sky_layout.h"
#include "renderer_surface_functions.slang"
#include "spectral_sensor.slang"

LUDUS_SHADER_BINDING(LUDUS_REFERENCE_PARAMETERS_BINDING) cbuffer Parameters
{
    column_major float4x4 ClipToEye; // clip to eye-relative world: world axes, origin at CameraPositionWorld.xyz
    float4 CameraPositionWorld;      // w: orthographic flag
    uint4 Dimensions;                // width, height, maximum depth, roulette depth
    uint4 Offsets;                   // nodes, order, emitters, surfaces
    uint4 Counts;                    // triangles, nodes, emitters, analytic lights
    uint4 Sampling;                  // key low, key high, flags (reverse/infinite/center/opaque), target
    float4 Environment;
    float4 Options; // secondary range, unused exposure, instance count bits, scene data bits
    float4 BoundsMinimumWorld;
    float4 BoundsMaximumWorld;
    uint4 SurfaceLayout; // surface stride, analytic light offset, profile version, environment offset + 1
    uint4 Tile;          // directional prefix length, medium offset/count, optional sky offset + 1
};
LUDUS_SHADER_BINDING(LUDUS_REFERENCE_SCENE_BINDING) StructuredBuffer<uint4> sceneWords;
LUDUS_SHADER_BINDING(LUDUS_REFERENCE_ACCUMULATION_BINDING) RWStructuredBuffer<float4> accumulation;
LUDUS_SHADER_BINDING(LUDUS_REFERENCE_STATUS_BINDING) RWStructuredBuffer<uint4> status;
LUDUS_SHADER_BINDING(LUDUS_REFERENCE_TEXELS_BINDING) StructuredBuffer<uint4> textureWords;
// The renderer's LTC table, read only by the resampled ReferenceLightSampling modes. Declared after every
// lower binding: Slang numbers Metal buffers in declaration order.
#define LUDUS_LTC_TABLE_BINDING LUDUS_REFERENCE_LTC_TABLE_BINDING
#include "linearly_transformed_cosine.slang"
#include "ltc_table.slang"
// RT1's GGX albedo table, read only by ReferenceMaterialProfile::MultipleScatteringGgx; declared after the LTC table.
#define LUDUS_GGX_ALBEDO_TABLE_BINDING LUDUS_REFERENCE_GGX_ALBEDO_TABLE_BINDING
#include "ggx_albedo_table.slang"
#include "material_program.slang"
#include "reference_light_sampling.slang"
#include "weighted_reservoir.slang"
// The node walk and watertight triangle test are shared with render-graph ComputeBvh passes.
#include "ray_bvh_traversal.slang"
#include "reference_medium.slang"
