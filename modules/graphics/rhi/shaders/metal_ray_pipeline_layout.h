#pragma once

// Private Metal ray-stage ABI, shared by the offline adapter and native backend.
// The public ray binding layout ends at diagnostic binding 10. Function tables
// occupy separate, backend-owned slots and never become user buffer bindings.
#define LUDUS_METAL_RAY_FUNCTION_TABLE_BINDING 11
#define LUDUS_METAL_RAY_INTERSECTION_TABLE_BINDING 12
#define LUDUS_METAL_RAY_STAGE_COUNT 3
// Generation + at most eight surface queries + one alpha intersection callback.
#define LUDUS_METAL_RAY_MAX_CALL_DEPTH 10
#define LUDUS_METAL_RAY_GENERATION "LudusNativeRayGeneration"
#define LUDUS_METAL_RAY_CLOSEST_HIT "LudusNativeClosestHit"
#define LUDUS_METAL_RAY_MISS "LudusNativeMiss"
#define LUDUS_METAL_RAY_INTERSECTION "LudusNativeIntersection"

#define LUDUS_METAL_RAY_ALPHA_TEXTURE_BINDING 4
