#include "reservoir.slang"

// The selector consumes emitter modes and helpers declared by reservoir.slang.
#include "initial_emitter_sampling.slang"

void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    Reservoir result = MakeReceiverReservoir(scene, payload.Launch, hit);
    float weightSum = 0;
    MaterialSample material = EvaluateMaterial(scene, hit);
    float3 surfaceEmission = EvaluateEmission(scene, hit, result.ViewDirectionWorld);
    if (material.IsValid == true && any(surfaceEmission > 0) == false && GetEmitterCount(scene) > 0)
    {
        InitialEmitterSelection selections[RESTIR_INITIAL_CANDIDATE_COUNT];
        const bool hasSelections = TrySelectInitialEmitters(scene, hit, queries, selections);
        [loop] for (uint i = 0; hasSelections == true && i < RESTIR_INITIAL_CANDIDATE_COUNT; ++i)
        {
            const uint emitterIndex = selections[i].EmitterIndex;
            const float selectionPmf = selections[i].SelectionPmf;
            TriangleData triangle;
            float conditionalPdfArea;
            if (TrySampleEmitterTriangle(scene, emitterIndex, queries, triangle, conditionalPdfArea) == false)
            {
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_LIGHT_SAMPLE);
                break;
            }
            float u = SampleSceneRandom(queries);
            float v = SampleSceneRandom(queries);
            float root = sqrt(u);
            float2 barycentric = float2(root * (1 - v), root * v);
            float proposalPdfArea = selectionPmf * conditionalPdfArea;
            if (isfinite(proposalPdfArea) == false || proposalPdfArea <= 0)
            {
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
                break;
            }
            float weight = EvaluateSampleTarget(scene, hit, triangle.TriangleIndex, barycentric) / proposalPdfArea;
            StreamReservoir(triangle.TriangleIndex,
                            barycentric,
                            weight,
                            1,
                            SampleSceneRandom(queries),
                            result,
                            weightSum,
                            queries);
        }
        FinalizeReservoir(scene, hit, weightSum, result, queries);
    }
    if (queries.HasFailed == false)
    {
        StoreReservoir(queries.RandomPixel, false, result);
    }
}
