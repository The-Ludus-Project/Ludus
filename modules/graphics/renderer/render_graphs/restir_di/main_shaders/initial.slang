#include "initial_reservoir.slang"

void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    SpecularChain chain = FollowReceiverGlass(scene, payload.Launch, queries, hit);
    if (chain.IsSurface == true)
        GenerateInitialReservoir(scene, chain.Hit, payload.Launch, queries);
}
