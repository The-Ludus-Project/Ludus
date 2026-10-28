#include "internal/native_ray_metal.h"

// Thanks to Apple, “Discover ray tracing with Metal”, WWDC20 session 10012,
// https://developer.apple.com/videos/play/wwdc2020/10012/ and “Get to know Metal function
// pointers”, session 10013, https://developer.apple.com/videos/play/wwdc2020/10013/.
// Stage tables are immutable; intersection tables are created per dispatch so
// queued jobs never observe later edits to alpha-test resource bindings.
namespace ludus::graphics::rhi::backend
{
bool LoadMetalRayFunctions(id<MTLLibrary> library, MetalRayFunctions& outFunctions) noexcept
{
    MetalRayFunctions candidate;
    candidate.Stages[0] = [library newFunctionWithName:@LUDUS_METAL_RAY_GENERATION];
    candidate.Stages[1] = [library newFunctionWithName:@LUDUS_METAL_RAY_CLOSEST_HIT];
    candidate.Stages[2] = [library newFunctionWithName:@LUDUS_METAL_RAY_MISS];
    candidate.IntersectionOrNull = [library newFunctionWithName:@LUDUS_METAL_RAY_INTERSECTION];
    for (id<MTLFunction> stageOrNull : candidate.Stages)
    {
        if (stageOrNull == nil || stageOrNull.functionType != MTLFunctionTypeVisible)
        {
            return false;
        }
    }
    if (candidate.IntersectionOrNull == nil || candidate.IntersectionOrNull.functionType != MTLFunctionTypeIntersection)
    {
        return false;
    }
    outFunctions = candidate;
    return true;
}
bool CreateMetalRayPipeline(id<MTLComputePipelineState> pipeline,
                            const MetalRayFunctions& functions,
                            MetalRayPipeline& outPipeline) noexcept
{
    MTLVisibleFunctionTableDescriptor* description = [MTLVisibleFunctionTableDescriptor visibleFunctionTableDescriptor];
    description.functionCount = LUDUS_METAL_RAY_STAGE_COUNT;
    MetalRayPipeline candidate;
    candidate.StagesOrNull = [pipeline newVisibleFunctionTableWithDescriptor:description];
    candidate.IntersectionOrNull = [pipeline functionHandleWithFunction:functions.IntersectionOrNull];
    if (candidate.StagesOrNull == nil || candidate.IntersectionOrNull == nil)
    {
        return false;
    }
    for (foundation::usize index = 0; index < functions.Stages.GetSize(); ++index)
    {
        id<MTLFunctionHandle> handleOrNull = [pipeline functionHandleWithFunction:functions.Stages[index]];
        if (handleOrNull == nil)
        {
            return false;
        }
        [candidate.StagesOrNull setFunction:handleOrNull atIndex:index];
    }
    outPipeline = candidate;
    return true;
}
MetalRayIntersectionTable CreateMetalRayIntersections(id<MTLComputePipelineState> pipeline,
                                                      const MetalRayPipeline& functions) noexcept
{
    MTLIntersectionFunctionTableDescriptor* description =
        [MTLIntersectionFunctionTableDescriptor intersectionFunctionTableDescriptor];
    description.functionCount = 1;
    id<MTLIntersectionFunctionTable> tableOrNull = [pipeline newIntersectionFunctionTableWithDescriptor:description];
    [tableOrNull setFunction:functions.IntersectionOrNull atIndex:0];
    return tableOrNull;
}
} // namespace ludus::graphics::rhi::backend
