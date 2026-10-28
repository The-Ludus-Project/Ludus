#pragma once
#include "raster_bindings.slang"

struct DeferredInput
{
    LUDUS_SHADER_LOCATION(0) float4 PositionUv : POSITION;
};
struct DeferredVarying
{
    float4 PositionClip : SV_Position;
};
[shader("vertex")] DeferredVarying DeferredVertexMain(in DeferredInput input) {
    DeferredVarying output;
    output.PositionClip = float4(input.PositionUv.xy, 0, 1);
    return output;
}
