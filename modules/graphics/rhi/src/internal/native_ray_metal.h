#pragma once

#include "../../shaders/metal_ray_pipeline_layout.h"
#import <Metal/Metal.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::rhi::backend
{
using MetalRayIntersectionTable = id<MTLIntersectionFunctionTable>;
struct MetalRayFunctions final
{
    foundation::StaticArray<id<MTLFunction>, LUDUS_METAL_RAY_STAGE_COUNT> Stages{};
    id<MTLFunction> IntersectionOrNull = nil;
};
struct MetalRayPipeline final
{
    id<MTLVisibleFunctionTable> StagesOrNull = nil;
    id<MTLFunctionHandle> IntersectionOrNull = nil;
};
[[nodiscard]] bool LoadMetalRayFunctions(id<MTLLibrary> library, MetalRayFunctions& outFunctions) noexcept;
[[nodiscard]] bool CreateMetalRayPipeline(id<MTLComputePipelineState> pipeline,
                                          const MetalRayFunctions& functions,
                                          MetalRayPipeline& outPipeline) noexcept;
[[nodiscard]] MetalRayIntersectionTable CreateMetalRayIntersections(id<MTLComputePipelineState> pipeline,
                                                                    const MetalRayPipeline& functions) noexcept;
} // namespace ludus::graphics::rhi::backend
