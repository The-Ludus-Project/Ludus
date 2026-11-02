#include "initial_reservoir.slang"

void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    GenerateInitialReservoir(scene, hit, payload.Launch, queries);
}
