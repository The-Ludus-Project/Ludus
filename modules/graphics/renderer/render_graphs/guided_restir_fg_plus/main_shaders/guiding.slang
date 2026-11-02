#include "storage.slang"

bool HasPreviousFgPool(SceneContext scene, RayLaunch launch)
{
    return launch.SampleIndex > 0 && all(GraphBuffer0[0] == uint4(scene.Generation, launch.ExtentPixel)) == true &&
           all(GraphBuffer0[1] == uint4(launch.Seed, launch.SampleIndex, FG_VERSION)) == true;
}
float3 UpdateGuideDirections(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasFgPoolCapacity(scene, launch, queries) == false)
        return float3(0);
    bool hasHistory = HasPreviousFgPool(scene, launch);
    uint lightCount = GetLightCount(scene);
    for (uint light = GetFgPixel(launch); light < lightCount; light += GetFgPixelCount(launch))
    {
        uint address = GetFgLightAddress(light);
        uint total = 0;
        if (hasHistory == true)
            for (uint cell = 0; cell < FG_DIRECTION_CELLS; ++cell)
                total += GraphBuffer0[address + 1 + cell].y;
        for (uint cell = 0; cell < FG_DIRECTION_CELLS; ++cell)
        {
            uint4 entry = GraphBuffer0[address + 1 + cell];
            float probability = hasHistory == true ? asfloat(entry.x) : 1 / float(FG_DIRECTION_CELLS);
            if (total > 0)
                probability = lerp(probability, float(entry.y) / float(total), FG_GUIDING_ALPHA);
            GraphBuffer0[address + 1 + cell] = uint4(asuint(probability), 0, 0, 0);
        }
        uint4 header = GraphBuffer0[address];
        GraphBuffer0[address] = uint4(hasHistory == true ? header.x : 0, 0, total, hasHistory == true ? 1 : 0);
    }
    return float3(0);
}
// One serial reduction/prefix over at most FG_MAXIMUM_LIGHTS light entries. The directions
// update is parallel; the prefix is deliberately simple and bounded, not a performance claim.
float3 UpdateGuideLights(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasFgPoolCapacity(scene, launch, queries) == false || GetFgPixel(launch) != 0)
        return float3(0);
    uint lightCount = GetLightCount(scene);
    uint finiteCount = 0;
    uint totalContribution = 0;
    for (uint light = 0; light < lightCount; ++light)
    {
        uint kind = GetLightInfo(scene, light).Kind;
        if (kind == SCENE_LIGHT_MESH || kind == SCENE_LIGHT_POINT || kind == SCENE_LIGHT_SPOT)
        {
            ++finiteCount;
            totalContribution += GraphBuffer0[GetFgLightAddress(light)].z;
        }
    }
    uint requestedPaths = max(finiteCount, min(FG_PHOTON_PATHS, GetFgPixelCount(launch)));
    if (requestedPaths > GetFgPixelCount(launch))
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_PHOTON_CAPACITY);
        return float3(0);
    }
    uint cumulative = 0;
    for (uint light = 0; light < lightCount; ++light)
    {
        uint address = GetFgLightAddress(light);
        uint4 header = GraphBuffer0[address];
        uint kind = GetLightInfo(scene, light).Kind;
        bool isFinite = kind == SCENE_LIGHT_MESH || kind == SCENE_LIGHT_POINT || kind == SCENE_LIGHT_SPOT;
        float probability = isFinite == true ? 1 / float(finiteCount) : 0;
        if (header.w != 0 && IS_FG_GUIDING_ENABLED == true && isFinite == true)
        {
            probability = asfloat(header.x);
            if (totalContribution > 0)
                probability = lerp(probability, float(header.z) / float(totalContribution), FG_GUIDING_ALPHA);
        }
        uint count = isFinite == true ? uint(floor(1 + probability * float(requestedPaths - finiteCount))) : 0;
        cumulative += count;
        GraphBuffer0[address] = uint4(asuint(probability), cumulative, count, 0);
    }
    GraphBuffer0[0] = uint4(scene.Generation, launch.ExtentPixel);
    GraphBuffer0[1] = uint4(launch.Seed, launch.SampleIndex + 1, FG_VERSION);
    GraphBuffer0[2] = uint4(lightCount, cumulative, 0, 0);
    GraphBuffer0[3] = uint4(0);
    return float3(0);
}
