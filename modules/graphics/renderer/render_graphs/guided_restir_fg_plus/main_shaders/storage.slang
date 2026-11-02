#pragma once
#include "transport.slang"

// Pool: identity (4 words), 17 words/light (light + 4x4 direction histogram),
// per-pixel hash and backprojection heads, then one 5-word photon per path depth.
// Every pass that accesses the pool binds it as GraphBuffer0. Per-pixel path passes
// bind source and destination as GraphBuffer0/1. Caustic banks are double buffered.
struct FgPhoton
{
    uint Triangle;
    float2 Barycentric;
    uint Flags;
    float3 Flux;
    uint NextGrid;
    float3 IncomingWorld;
    uint NextCaustic;
    uint4 RandomKey;
    float2 DirectionUv;
    float FluxScale;
    uint Light;
    uint Cell;
};
struct FgPathReservoir
{
    FgPhoton Photon;
    uint GatherTriangle;
    float2 GatherBarycentric;
    float Weight;
    uint ReceiverTriangle;
    float2 ReceiverBarycentric;
    uint Confidence;
    uint4 RandomKey;
    uint4 Identity;
    float3 ViewDirectionWorld;
    uint CameraDepth;
    float3 Throughput;
    uint Flags;
};
uint4 LoadFgWord(bool isSecond, uint address)
{
    return isSecond == true ? GraphBuffer1[address] : GraphBuffer0[address];
}
void StoreFgWord(bool isSecond, uint address, uint4 value)
{
    if (isSecond == true)
        GraphBuffer1[address] = value;
    else
        GraphBuffer0[address] = value;
}
[noinline] FgPhoton LoadFgPhoton(bool isSecond, uint address) {
    uint4 a = LoadFgWord(isSecond, address);
    uint4 b = LoadFgWord(isSecond, address + 1);
    uint4 c = LoadFgWord(isSecond, address + 2);
    uint4 d = LoadFgWord(isSecond, address + 4);
    FgPhoton photon;
    photon.Triangle = a.x;
    photon.Barycentric = asfloat(a.yz);
    photon.Flags = a.w;
    photon.Flux = asfloat(b.xyz);
    photon.NextGrid = b.w;
    photon.IncomingWorld = asfloat(c.xyz);
    photon.NextCaustic = c.w;
    photon.RandomKey = LoadFgWord(isSecond, address + 3);
    photon.DirectionUv = asfloat(d.xy);
    photon.FluxScale = asfloat(d.z);
    photon.Light = d.w;
    photon.Cell = GetFgDirectionCell(photon.DirectionUv);
    return photon;
}[noinline] void StoreFgPhoton(bool isSecond, uint address, FgPhoton photon)
{
    StoreFgWord(isSecond, address, uint4(photon.Triangle, asuint(photon.Barycentric), photon.Flags));
    StoreFgWord(isSecond, address + 1, uint4(asuint(photon.Flux), photon.NextGrid));
    StoreFgWord(isSecond, address + 2, uint4(asuint(photon.IncomingWorld), photon.NextCaustic));
    StoreFgWord(isSecond, address + 3, photon.RandomKey);
    StoreFgWord(isSecond, address + 4, uint4(asuint(photon.DirectionUv), asuint(photon.FluxScale), photon.Light));
}
uint4 MakeFgIdentity(SceneContext scene, RayLaunch launch)
{
    return uint4(scene.Generation, launch.SampleIndex + 1, FG_VERSION);
}
FgPathReservoir MakeFgPath(SceneContext scene, RayLaunch launch)
{
    FgPathReservoir result = (FgPathReservoir)0;
    result.Photon.Triangle = FG_INVALID;
    result.GatherTriangle = FG_INVALID;
    result.ReceiverTriangle = FG_INVALID;
    result.Identity = MakeFgIdentity(scene, launch);
    result.RandomKey = uint4(launch.Seed, launch.SampleIndex, GetFgPixel(launch));
    return result;
}
[noinline] FgPathReservoir LoadFgPath(bool isSecond, uint pixel) {
    uint address = pixel * FG_PATH_WORDS;
    FgPathReservoir result = (FgPathReservoir)0;
    result.Photon = LoadFgPhoton(isSecond, address);
    uint4 a = LoadFgWord(isSecond, address + FG_PATH_GATHER_WORD);
    uint4 b = LoadFgWord(isSecond, address + FG_PATH_RECEIVER_WORD);
    uint4 c = LoadFgWord(isSecond, address + FG_PATH_VIEW_WORD);
    uint4 d = LoadFgWord(isSecond, address + FG_PATH_THROUGHPUT_WORD);
    result.GatherTriangle = a.x;
    result.GatherBarycentric = asfloat(a.yz);
    result.Weight = asfloat(a.w);
    result.ReceiverTriangle = b.x;
    result.ReceiverBarycentric = asfloat(b.yz);
    result.Confidence = b.w;
    result.RandomKey = LoadFgWord(isSecond, address + FG_PATH_RANDOM_WORD);
    result.Identity = LoadFgWord(isSecond, address + FG_PATH_IDENTITY_WORD);
    result.ViewDirectionWorld = asfloat(c.xyz);
    result.CameraDepth = c.w;
    result.Throughput = asfloat(d.xyz);
    result.Flags = d.w;
    return result;
}[noinline] void StoreFgPath(bool isSecond, uint pixel, FgPathReservoir reservoir)
{
    uint address = pixel * FG_PATH_WORDS;
    StoreFgPhoton(isSecond, address, reservoir.Photon);
    StoreFgWord(isSecond,
                address + FG_PATH_GATHER_WORD,
                uint4(reservoir.GatherTriangle, asuint(reservoir.GatherBarycentric), asuint(reservoir.Weight)));
    StoreFgWord(isSecond,
                address + FG_PATH_RECEIVER_WORD,
                uint4(reservoir.ReceiverTriangle, asuint(reservoir.ReceiverBarycentric), reservoir.Confidence));
    StoreFgWord(isSecond, address + FG_PATH_RANDOM_WORD, reservoir.RandomKey);
    StoreFgWord(isSecond, address + FG_PATH_IDENTITY_WORD, reservoir.Identity);
    StoreFgWord(isSecond,
                address + FG_PATH_VIEW_WORD,
                uint4(asuint(reservoir.ViewDirectionWorld), reservoir.CameraDepth));
    StoreFgWord(isSecond, address + FG_PATH_THROUGHPUT_WORD, uint4(asuint(reservoir.Throughput), reservoir.Flags));
}
bool HasFgPathCapacity(RayLaunch launch)
{
    uint firstCount;
    uint firstStride;
    uint secondCount;
    uint secondStride;
    GraphBuffer0.GetDimensions(firstCount, firstStride);
    GraphBuffer1.GetDimensions(secondCount, secondStride);
    return GetFgPixelCount(launch) <= firstCount / FG_PATH_WORDS &&
           GetFgPixelCount(launch) <= secondCount / FG_PATH_WORDS;
}
bool HasFgIdentity(uint4 identity, SceneContext scene, RayLaunch launch, bool isHistory)
{
    return all(identity.xy == scene.Generation) == true && identity.w == FG_VERSION &&
           identity.z == (isHistory == true ? launch.SampleIndex : launch.SampleIndex + 1) &&
           (isHistory == false || launch.SampleIndex > 0);
}
bool HasFgPoolCapacity(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (launch.IsFullImageDispatch == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_PHOTON_DISPATCH);
        return false;
    }
    uint count;
    uint stride;
    GraphBuffer0.GetDimensions(count, stride);
    if (count < FG_POOL_WORDS || GetFgPixelCount(launch) > FG_MAXIMUM_PIXELS ||
        GetLightCount(scene) > FG_MAXIMUM_LIGHTS)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_PHOTON_CAPACITY);
        return false;
    }
    return true;
}
bool HasCurrentFgPool(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasFgPoolCapacity(scene, launch, queries) == false)
        return false;
    if (all(GraphBuffer0[0] == uint4(scene.Generation, launch.ExtentPixel)) == false ||
        all(GraphBuffer0[1] == uint4(launch.Seed, launch.SampleIndex + 1, FG_VERSION)) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return false;
    }
    if (GraphBuffer0[3].x != 0)
    {
        FailSceneQuery(queries, GraphBuffer0[3].x - 1);
        return false;
    }
    return true;
}
void RecordFgPoolFailure(RayQueryContext queries)
{
    if (queries.HasFailed == true)
    {
        uint previous;
        InterlockedCompareExchange(GraphBuffer0[3].x, 0, queries.FailureReason + 1, previous);
    }
}
