// Slang equivalent of barriers.lusl.
#include "raster_bindings.slang"

static const uint GROUP_SIZE = 64;
static const uint SCALE = 3;

LUDUS_SHADER_BINDING(0) RWStructuredBuffer<uint> sums LUDUS_SHADER_STORAGE_REGISTER(0);
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<uint> exchanged LUDUS_SHADER_STORAGE_REGISTER(1);
LUDUS_SHADER_BINDING(2) RWStructuredBuffer<uint> neighbors LUDUS_SHADER_STORAGE_REGISTER(2);
groupshared uint partial[GROUP_SIZE];

[shader("compute")][numthreads(64, 1, 1)] void
Main(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex, uint3 group : SV_GroupID) {
    partial[groupIndex] = id.x + 1;
    exchanged[id.x] = id.x * SCALE;
    AllMemoryBarrierWithGroupSync();
    neighbors[id.x] = exchanged[id.x ^ 1];
    for (uint stride = GROUP_SIZE / 2; stride > 0; stride >>= 1)
    {
        if (groupIndex < stride)
        {
            partial[groupIndex] += partial[groupIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (groupIndex == 0)
    {
        sums[group.x] = partial[0];
    }
}
