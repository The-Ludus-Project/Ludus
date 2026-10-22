#include "reservoir.slang"

void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload payload)
{
    Reservoir empty = (Reservoir)0;
    StoreReservoir(queries.RandomPixel, GetGraphPassIndex() == SPATIAL_PASS_INDEX, empty);
    payload.Radiance = float3(0);
}
