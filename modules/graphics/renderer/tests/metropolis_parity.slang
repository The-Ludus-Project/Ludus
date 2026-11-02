#define LUDUS_RENDER_GRAPH_RAY_PIPELINE 1
#define LUDUS_SOFTWARE_RAY_TRAVERSAL 1
float3 EvaluateGraphRay(uint pixel);
#include "metropolis_parity_layout.h"
#include "renderer_gpu_preview.slang"
#include "scene_shader.slang"

static const uint MH_SCATTERING_DEPTH = 3;
static const uint MH_BURN_IN = 32;
static const float MH_LARGE_STEP_PROBABILITY = 0.3f;
static const float MH_SMALL_STEP_RADIUS = 0.05f;
static const float MH_TARGET_FLOOR = 1;
LUDUS_SHADER_BINDING(6) RWStructuredBuffer<uint4> GraphBuffer0;
LUDUS_SHADER_BINDING(7) RWStructuredBuffer<uint4> GraphBuffer1;
LUDUS_SHADER_BINDING(8) RWStructuredBuffer<uint4> Results;
#include "chain.slang"

float3 EvaluateGraphRay(uint pixel)
{
    return float3(0);
}

[numthreads(LUDUS_MH_PARITY_GROUP_SIZE, 1, 1)] void MetropolisParityMain(uint3 dispatchPixel : SV_DispatchThreadID) {
    uint pixel = dispatchPixel.x;
    if (pixel >= LUDUS_MH_PARITY_INVOCATIONS)
        return;
    SceneContext scene;
    scene.Version = SCENE_INTERFACE_VERSION;
    scene.Generation = uint2(1, 0);
    RayLaunch launch = (RayLaunch)0;
    launch.PositionPixel = uint2(pixel, 0);
    launch.ExtentPixel = uint2(LUDUS_MH_PARITY_INVOCATIONS, 1);
    launch.CameraClipToWorld = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    // Odd pixels miss the floor through a shorter camera interval, exercising primary environment emission.
    launch.CameraClipToWorld[2][2] = pixel % 2 == 1 ? 0.25f : 1;
    launch.IsOrthographic = true;
    launch.Seed = Sampling.xy;
    float3 radiances[MH_ESTIMATOR_COUNT];
    uint failures[MH_ESTIMATOR_COUNT];
    [unroll] for (uint estimator = 0; estimator < MH_ESTIMATOR_COUNT; ++estimator)
    {
        radiances[estimator] = float3(0);
        failures[estimator] = 0;
    }
    [loop] for (uint iteration = 0; iteration < LUDUS_MH_PARITY_ITERATIONS; ++iteration)
    {
        launch.SampleIndex = iteration;
        [unroll] for (uint pass = 0; pass < MH_ESTIMATOR_COUNT; ++pass)
        {
            // Production order: RB advances the common chain, then standard and waste recycling resolve it.
            uint estimator = pass == 0 ? MH_RAO_BLACKWELL : pass - 1;
            RayQueryContext queries = (RayQueryContext)0;
            queries.MaximumSurfaceQueries = 8;
            radiances[estimator] = AdvanceMetropolisChain(scene, launch, queries, estimator);
            failures[estimator] += queries.HasFailed == true ? 1 : 0;
        }
    }
    [unroll] for (uint estimator = 0; estimator < MH_ESTIMATOR_COUNT; ++estimator)
        Results[pixel * LUDUS_MH_PARITY_RESULT_WORDS + estimator] =
            uint4(asuint(radiances[estimator]), failures[estimator]);
    // A rejected proposal must not publish chain identity or weighted film state.
    uint chainBase = pixel * MH_RECORD_WORDS;
    uint filmBase = pixel * LUDUS_MH_FILM_WORDS;
    uint4 previousIdentity = GraphBuffer0[chainBase + MH_IDENTITY_WORD];
    uint4 previousFilm = GraphBuffer1[filmBase];
    PrimarySamples primary;
    [unroll] for (uint dimension = 0; dimension < MH_PRIMARY_DIMENSIONS; ++dimension) primary.Values[dimension] =
        asfloat(GraphBuffer0[chainBase + dimension / MH_COMPONENTS_PER_WORD][dimension % MH_COMPONENTS_PER_WORD]);
    RayQueryContext aovQueries = (RayQueryContext)0;
    aovQueries.MaximumSurfaceQueries = 8;
    MetropolisPathAovs aovs;
    EvaluateMetropolisPath(scene, GenerateMetropolisCameraRay(launch, primary), primary, aovQueries, aovs);
    uint resultBase = pixel * LUDUS_MH_PARITY_RESULT_WORDS;
    Results[resultBase + LUDUS_MH_PARITY_EMISSION_WORD] =
        uint4(asuint(aovs.Emission), aovQueries.HasFailed == true ? 1 : 0);
    Results[resultBase + LUDUS_MH_PARITY_DIRECT_WORD] = uint4(asuint(aovs.Direct), 0);
    Results[resultBase + LUDUS_MH_PARITY_INDIRECT_WORD] = uint4(asuint(aovs.Indirect), 0);
    launch.SampleIndex = LUDUS_MH_PARITY_ITERATIONS;
    RayQueryContext failedQueries = (RayQueryContext)0;
    AdvanceMetropolisChain(scene, launch, failedQueries, MH_RAO_BLACKWELL);
    uint lifecycleFailures = failedQueries.HasFailed == true &&
                                     all(GraphBuffer0[chainBase + MH_IDENTITY_WORD] == previousIdentity) == true &&
                                     all(GraphBuffer1[filmBase] == previousFilm) == true
                                 ? 0
                                 : 1;
    // Even pixels model a camera reset with an unchanged scene/seed; odd pixels a World replacement.
    scene.Generation.x += pixel % 2;
    launch.SampleIndex = 0;
    RayQueryContext resetQueries = (RayQueryContext)0;
    resetQueries.MaximumSurfaceQueries = 8;
    float3 resetRadiance = AdvanceMetropolisChain(scene, launch, resetQueries, MH_RAO_BLACKWELL);
    lifecycleFailures += resetQueries.HasFailed == true ? 1 : 0;
    // Exercise the same primitives on exact inputs, independently of transport rounding.
    MetropolisTour tour;
    tour.RejectionProduct = 1;
    tour.HoldingWeight = 1;
    TryAdvanceMetropolisTour(0.5f, tour);
    TryAdvanceMetropolisTour(0.5f, tour);
    Results[pixel * LUDUS_MH_PARITY_RESULT_WORDS + MH_ESTIMATOR_COUNT] =
        uint4(asuint(tour.RejectionProduct), asuint(tour.HoldingWeight), lifecycleFailures, asuint(resetRadiance.x));
}
