#include "raster_bindings.slang"

LUDUS_SHADER_BINDING(0) RWStructuredBuffer<uint> Counter LUDUS_SHADER_STORAGE_REGISTER(0);
LUDUS_SHADER_BINDING(1) StructuredBuffer<uint> CounterInput LUDUS_SHADER_TEXTURE_REGISTER(1);
LUDUS_SHADER_BINDING(2) RWStructuredBuffer<float2> Vertices LUDUS_SHADER_STORAGE_REGISTER(2);
#define INDIRECT_ARGUMENT_SLOT 3
LUDUS_SHADER_BINDING(INDIRECT_ARGUMENT_SLOT)
RWStructuredBuffer<uint> Arguments LUDUS_SHADER_STORAGE_REGISTER(INDIRECT_ARGUMENT_SLOT);

static const uint EXPECTED_COUNTER = 42;
static const uint TRIANGLE_INDEX_COUNT = 3;
static const uint BASE_VERTEX_WORD = 3;
static const uint FIRST_INSTANCE_WORD = 4;
static const float OVERSCAN_EDGE = 3.0;

[shader("compute")][numthreads(1, 1, 1)] void AdvanceCounter() { Counter[0] += 1; }

    [shader("compute")][numthreads(1, 1, 1)] void PrepareDraw()
{
    Vertices[0] = float2(-1, -1);
    Vertices[1] = float2(-1, OVERSCAN_EDGE);
    Vertices[2] = float2(OVERSCAN_EDGE, -1);
    Arguments[0] = TRIANGLE_INDEX_COUNT;
    Arguments[1] = CounterInput[0] == EXPECTED_COUNTER ? 1 : 0;
    Arguments[2] = 0;
    Arguments[BASE_VERTEX_WORD] = 0;
    Arguments[FIRST_INSTANCE_WORD] = 0;
}

struct VertexInput
{
    LUDUS_SHADER_LOCATION(0) float2 PositionClip : POSITION;
};

[shader("vertex")] float4 DrawVertex(VertexInput input) : SV_Position
{
    return float4(input.PositionClip, 0.5, 1);
}

[shader("fragment")] float4 DrawFragment() : SV_Target0
{
    return float4(0, 1, 0, 1);
}
