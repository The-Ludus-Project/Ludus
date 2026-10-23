#include "math.slang"
#include "raster_bindings.slang"

static const uint RESTIR_INITIAL_CANDIDATE_COUNT = 8;
static const uint SELECTION_RECORD_COUNT = RESTIR_INITIAL_CANDIDATE_COUNT + 1;
#define LUDUS_EMITTER_SAMPLING_MODE_UNIFORM 0
#define LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED (1)
#define LUDUS_EMITTER_SAMPLING_MODE LUDUS_EMITTER_SAMPLING_MODE_DISTANCE_WEIGHTED
LUDUS_SHADER_BINDING(0) RWStructuredBuffer<uint4> Results;

// A deterministic scene facade isolates the production selector. Observed weight
// calls prove geometric evaluation is shared; returned PMFs are checked independently.
struct SceneContext
{
    uint Invocation;
    uint EmitterCount;
};
struct SurfaceHit
{
    float3 PositionWorld;
};
struct RayQueryContext
{
    bool HasFailed;
    uint Invocation;
    uint Dimension;
};
uint GetEmitterCount(SceneContext scene)
{
    return scene.EmitterCount;
}
float ComputeSceneEmitterWeight(SceneContext scene, uint emitter, SurfaceHit receiver)
{
    const uint address = scene.Invocation * SELECTION_RECORD_COUNT;
    Results[address].w += 1;
    const uint scenario = scene.Invocation % 5;
    if (scenario == 1)
    {
        return 0;
    }
    if (scenario == 2)
    {
        return -1;
    }
    if (scenario == 3)
    {
        return asfloat(0x7fc00000u);
    }
    return emitter == 1 ? 1 : emitter == 2 ? 3 : emitter == 4 ? 7 : 0;
}
float SampleSceneRandom(inout RayQueryContext queries)
{
    const RandomAddress address = MakeRandomAddress(uint2(queries.Invocation, 0), 0, queries.Dimension++);
    return SampleRandomFloat01(uint2(1, 0), address);
}
uint SampleSceneRandom(inout RayQueryContext queries, uint minimumInclusive, uint maximumExclusive)
{
    const RandomAddress address = MakeRandomAddress(uint2(queries.Invocation, 0), 0, queries.Dimension++);
    uint value;
    if (TrySampleRandomBounded(uint2(1, 0), address, maximumExclusive - minimumInclusive, value) == false)
    {
        queries.HasFailed = true;
        return minimumInclusive;
    }
    return minimumInclusive + value;
}
#include "initial_emitter_sampling.slang"

[numthreads(64, 1, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    SceneContext scene;
    scene.Invocation = dispatchThreadId.x;
    scene.EmitterCount = scene.Invocation % 5 == 4 ? 17 : 5;
    SurfaceHit receiver = (SurfaceHit)0;
    RayQueryContext queries = (RayQueryContext)0;
    queries.Invocation = dispatchThreadId.x;
    InitialEmitterSelection selections[RESTIR_INITIAL_CANDIDATE_COUNT];
    const bool hasSelections = TrySelectInitialEmitters(scene, receiver, queries, selections);
    const uint address = scene.Invocation * SELECTION_RECORD_COUNT;
    Results[address].xyz = uint3(hasSelections ? 1 : 0, queries.HasFailed ? 1 : 0, queries.Dimension);
    for (uint candidate = 0; candidate < RESTIR_INITIAL_CANDIDATE_COUNT; ++candidate)
    {
        Results[address + candidate + 1] = uint4(selections[candidate].EmitterIndex,
                                               asuint(selections[candidate].SelectionPmf), 0, 0);
    }
}
