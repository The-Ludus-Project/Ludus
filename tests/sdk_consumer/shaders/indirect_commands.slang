#include "raster_bindings.slang"

// R12a portable indirect-command fixture: one kernel writes every command, so no argument
// is ever read back to the CPU before it is consumed. The C++ journeys compare the resulting
// image with the direct-draw oracle and read back the indirect dispatch marks. The native
// conformance case (modules/graphics/rhi/tests/portable_raster_conformance_tests.cpp) and the
// installed-SDK browser fixture (tests/sdk_consumer/indirect.cpp) share this source.
//
// LUDUS_INDIRECT_ZERO_FIRST_INSTANCE writes zero FirstInstance values for a profile without
// IndirectCapabilities::HasFirstInstance. The consumer then selects each color through its
// instance-stream slice instead and orders the non-indexed draw first, so an ignored stride or an
// executed zero-instance command still covers the right half with the left-half color.
#define ARGUMENT_SLOT 0
#define MARK_SLOT 1

LUDUS_SHADER_BINDING(ARGUMENT_SLOT)
RWStructuredBuffer<uint> Arguments LUDUS_SHADER_STORAGE_REGISTER(ARGUMENT_SLOT);
LUDUS_SHADER_BINDING(MARK_SLOT) RWStructuredBuffer<uint> Marks LUDUS_SHADER_STORAGE_REGISTER(MARK_SLOT);

// Word offsets and values must match portable_raster_conformance_tests.cpp and indirect.cpp.
static const uint INDEXED_STRIDE_WORDS = 10;
static const uint POISON_WORD = 5;
static const uint NON_INDEXED_WORD = 16;
static const uint DISPATCH_WORD = 20;
static const uint EMPTY_DISPATCH_WORD = 24;
static const uint QUAD_INDEX_COUNT = 6;
static const uint QUAD_FIRST_INDEX = 3;
static const uint LEFT_BASE_VERTEX = 4;
static const uint RIGHT_FIRST_VERTEX = 8;
static const uint FULL_BASE_VERTEX = 14;
static const uint RED_INSTANCE = 0;
#if defined(LUDUS_INDIRECT_ZERO_FIRST_INSTANCE)
static const uint GREEN_INSTANCE = 0;
static const uint BLUE_INSTANCE = 0;
#else
static const uint GREEN_INSTANCE = 1;
static const uint BLUE_INSTANCE = 2;
#endif
static const uint MARKED_GROUPS = 3;
static const uint MARK_BASE = 100;

void WriteIndexed(uint word, uint instanceCount, uint baseVertex, uint firstInstance)
{
    Arguments[word] = QUAD_INDEX_COUNT;
    Arguments[word + 1] = instanceCount;
    Arguments[word + 2] = QUAD_FIRST_INDEX;
    Arguments[word + 3] = baseVertex;
    Arguments[word + 4] = firstInstance;
}

[shader("compute")][numthreads(1, 1, 1)] void PrepareCommands() {
    // Indexed command 0: the left quad, offset by FirstIndex and BaseVertex, with the green instance.
    WriteIndexed(0, 1, LEFT_BASE_VERTEX, GREEN_INSTANCE);
    // Read only by a backend that ignored the 40-byte stride: a red full-image quad.
    WriteIndexed(POISON_WORD, 1, FULL_BASE_VERTEX, RED_INSTANCE);
    // Indexed command 1: zero instances, so the red full-image quad is culled.
    WriteIndexed(INDEXED_STRIDE_WORDS, 0, FULL_BASE_VERTEX, RED_INSTANCE);
    // Non-indexed command: the right quad's six vertices with the blue instance.
    Arguments[NON_INDEXED_WORD] = QUAD_INDEX_COUNT;
    Arguments[NON_INDEXED_WORD + 1] = 1;
    Arguments[NON_INDEXED_WORD + 2] = RIGHT_FIRST_VERTEX;
    Arguments[NON_INDEXED_WORD + 3] = BLUE_INSTANCE;
    // A three-group dispatch and an empty dispatch.
    Arguments[DISPATCH_WORD] = MARKED_GROUPS;
    Arguments[DISPATCH_WORD + 1] = 1;
    Arguments[DISPATCH_WORD + 2] = 1;
    Arguments[EMPTY_DISPATCH_WORD] = 0;
    Arguments[EMPTY_DISPATCH_WORD + 1] = 1;
    Arguments[EMPTY_DISPATCH_WORD + 2] = 1;
}

    [shader("compute")][numthreads(1, 1, 1)] void MarkGroups(uint3 groupId : SV_GroupID)
{
    Marks[groupId.x] = MARK_BASE + groupId.x;
}

struct CommandInput
{
    LUDUS_SHADER_LOCATION(0) float2 PositionClip : POSITION;
    LUDUS_SHADER_LOCATION(1) float4 Color : COLOR;
};

struct CommandVarying
{
    float4 PositionClip : SV_Position;
    float4 Color : COLOR;
};

[shader("vertex")] CommandVarying CommandVertex(CommandInput input) {
    CommandVarying output;
    output.PositionClip = float4(input.PositionClip, 0.5, 1);
    output.Color = input.Color;
    return output;
}

    [shader("fragment")] float4 CommandFragment(CommandVarying input)
    : SV_Target0
{
    return input.Color;
}
